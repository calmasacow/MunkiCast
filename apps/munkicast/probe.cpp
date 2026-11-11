// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `munkicast probe` — report everything about this machine that affects capture, composite and
// encode. This is the permanent bug-report tool (ADR-0005), so it must be dependency-light,
// never crash, and degrade to "unknown" rather than failing.

#include "probe.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <xf86drm.h>
#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <pipewire/pipewire.h>
}

#include <gio/gio.h>

namespace munkicast {
namespace {

// ---------------------------------------------------------------------------- output helpers

template <typename... Args> void out(std::format_string<Args...> fmt, Args&&... args) {
    const std::string s = std::format(fmt, std::forward<Args>(args)...);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
}

void section(std::string_view title) {
    out("");
    out("== {} {}", title, std::string(title.size() < 60 ? 60 - title.size() : 0, '='));
}

std::string env_or(const char* name, std::string_view fallback = "(unset)") {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string(fallback);
}

/// Runs a command and returns its first line of stdout, trimmed. Empty on any failure.
std::string capture_cmd(const char* cmd) {
    std::FILE* p = ::popen(cmd, "r");
    if (p == nullptr)
        return {};
    std::array<char, 512> buf{};
    std::string first;
    if (std::fgets(buf.data(), static_cast<int>(buf.size()), p) != nullptr)
        first = buf.data();
    ::pclose(p);
    while (!first.empty() && (first.back() == '\n' || first.back() == '\r' || first.back() == ' '))
        first.pop_back();
    return first;
}

bool have_exe(const char* name) {
    const std::string cmd = std::format("command -v {} 2>/dev/null", name);
    return !capture_cmd(cmd.c_str()).empty();
}

struct Findings {
    bool on_xwayland = false;
    bool wayland_ok = false;
    int warnings = 0;
};

void warn_line(Findings& f, std::string_view msg) {
    ++f.warnings;
    out("  !! {}", msg);
}

// What the hardware sections established, kept so the encoder table can cite it. The encoder
// table runs last for this reason: libavcodec only knows what it was compiled with, and on argus
// that made `av1_nvenc` and every `*_qsv` read "present" while none of them could run (ADR-0008).
// A name in the table is only as good as the evidence printed next to it.
struct VulkanEncode {
    std::uint32_t index = 0;
    std::string name;
    std::uint32_t vendor = 0;
    bool encode_queue = false;
    bool h264 = false;
    bool h265 = false;
    bool av1 = false;
};

struct VaapiNode {
    std::string path;
    bool h264_encode = false;
};

struct QsvNode {
    std::string path;
    bool ok = false;
    bool needs_ihd = false;
};

struct Evidence {
    bool vulkan_ran = false;
    std::vector<VulkanEncode> vulkan;
    bool vainfo_ran = false;
    std::vector<VaapiNode> vaapi;
    bool qsv_ran = false;
    std::vector<QsvNode> qsv;
};

// ---------------------------------------------------------------------------- session / display

void probe_session(Findings& f) {
    section("session");

    const std::string session_type = env_or("XDG_SESSION_TYPE");
    const std::string wl_display = env_or("WAYLAND_DISPLAY");
    const std::string x_display = env_or("DISPLAY");
    const std::string qt_platform = env_or("QT_QPA_PLATFORM");
    const std::string desktop = env_or("XDG_CURRENT_DESKTOP");

    out("  XDG_SESSION_TYPE    {}", session_type);
    out("  XDG_CURRENT_DESKTOP {}", desktop);
    out("  WAYLAND_DISPLAY     {}", wl_display);
    out("  DISPLAY             {}  (XWayland, for other clients)", x_display);
    out("  QT_QPA_PLATFORM     {}", qt_platform);
    out("  QT_QPA_PLATFORMTHEME {}", env_or("QT_QPA_PLATFORMTHEME"));

    f.wayland_ok = (session_type == "wayland") && (wl_display != "(unset)");

    // ADR-0003: we must never *be* an X11 client. A trailing ";xcb" in QT_QPA_PLATFORM lets Qt
    // silently fall back to XWayland, which is how one ends up debugging an X11 app believed
    // to be native. Flag it here so it can never regress unnoticed.
    if (qt_platform.find("xcb") != std::string::npos) {
        warn_line(f, std::format(
            "QT_QPA_PLATFORM contains 'xcb' ({}): Qt may silently fall back to XWayland. "
            "MunkiCast forces QT_QPA_PLATFORM=wayland with no fallback.", qt_platform));
    }

    if (!f.wayland_ok) {
        f.on_xwayland = true;
        out("  !! not a Wayland session - MunkiCast is Wayland-native with no X11 backend (ADR-0003)");
    }

    const std::string hypr = env_or("HYPRLAND_INSTANCE_SIGNATURE", "");
    if (!hypr.empty() && have_exe("hyprctl"))
        out("  compositor          Hyprland {}", capture_cmd("hyprctl version -j 2>/dev/null | "
            "sed -n 's/.*\"tag\": *\"\\([^\"]*\\)\".*/\\1/p' | head -1"));

    out("  AQ_DRM_DEVICES      {}", env_or("AQ_DRM_DEVICES"));
}

// ---------------------------------------------------------------------------- DRM devices

struct DrmNode {
    std::string path;      // /dev/dri/renderD128
    std::string driver;    // nvidia-drm, i915, ...
    unsigned dev_major = 0;
    unsigned dev_minor = 0;
    bool is_render = false;
};

std::vector<DrmNode> probe_drm() {
    section("DRM devices");
    std::vector<DrmNode> nodes;

    std::array<drmDevicePtr, 16> devs{};
    const int n = drmGetDevices2(0, devs.data(), static_cast<int>(devs.size()));
    if (n <= 0) {
        out("  (drmGetDevices2 returned {}; no DRM devices visible)", n);
        return nodes;
    }

    for (int i = 0; i < n; ++i) {
        drmDevicePtr d = devs[static_cast<std::size_t>(i)];
        std::string bus = "(non-PCI)";
        if (d->bustype == DRM_BUS_PCI && d->businfo.pci != nullptr) {
            bus = std::format("pci {:04x}:{:02x}:{:02x}.{} vendor {:04x} device {:04x}",
                              d->businfo.pci->domain, d->businfo.pci->bus,
                              d->businfo.pci->dev, d->businfo.pci->func,
                              d->deviceinfo.pci->vendor_id, d->deviceinfo.pci->device_id);
        }

        for (int t = 0; t < DRM_NODE_MAX; ++t) {
            if ((d->available_nodes & (1 << t)) == 0)
                continue;
            const char* path = d->nodes[t];
            if (path == nullptr)
                continue;

            DrmNode node;
            node.path = path;
            node.is_render = (t == DRM_NODE_RENDER);

            struct stat st{};
            if (::stat(path, &st) == 0) {
                node.dev_major = major(st.st_rdev);
                node.dev_minor = minor(st.st_rdev);
            }

            const int fd = ::open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                if (drmVersionPtr ver = drmGetVersion(fd); ver != nullptr) {
                    node.driver = (ver->name != nullptr) ? ver->name : "?";
                    drmFreeVersion(ver);
                }
                ::close(fd);
            } else {
                node.driver = std::format("(open failed: {})", std::strerror(errno));
            }

            out("  {:<22} {}:{:<3} driver={:<12} {}", node.path, node.dev_major, node.dev_minor,
                node.driver.empty() ? "?" : node.driver, bus);
            nodes.push_back(std::move(node));
        }
    }
    drmFreeDevices(devs.data(), n);
    return nodes;
}

// ---------------------------------------------------------------------------- Vulkan

bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
    default:                                     return "other";
    }
}

void probe_vulkan(Findings& f, Evidence& ev, const std::vector<DrmNode>& drm_nodes) {
    section("Vulkan");

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "munkicast-probe";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (VkResult r = vkCreateInstance(&ici, nullptr, &inst); r != VK_SUCCESS) {
        warn_line(f, std::format("vkCreateInstance failed ({}) - no Vulkan 1.3 instance",
                                 static_cast<int>(r)));
        return;
    }

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    if (count == 0) {
        warn_line(f, "no Vulkan physical devices");
        vkDestroyInstance(inst, nullptr);
        return;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(inst, &count, devices.data());

    ev.vulkan_ran = true;

    // Extensions that the capture->composite->encode path actually depends on. The per-codec
    // encode extensions are listed because the queue extension alone says nothing about which
    // codecs the silicon has: the RTX 2070 has the queue and no AV1 encoder.
    static constexpr std::array<const char*, 10> kWanted{
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
        "VK_KHR_video_encode_queue",
        "VK_KHR_video_encode_h264",
        "VK_KHR_video_encode_h265",
        "VK_KHR_video_encode_av1",
    };

    for (std::uint32_t i = 0; i < count; ++i) {
        VkPhysicalDevice dev = devices[i];

        VkPhysicalDeviceDrmPropertiesEXT drm{};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &drm;
        vkGetPhysicalDeviceProperties2(dev, &props2);
        const VkPhysicalDeviceProperties& p = props2.properties;

        out("");
        out("  [{}] {}", i, p.deviceName);
        out("      type={} api={}.{}.{} driver=0x{:x} vendor=0x{:04x} device=0x{:04x}",
            device_type_name(p.deviceType),
            VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion),
            p.driverVersion, p.vendorID, p.deviceID);

        std::uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, exts.data());

        // DRM node identity: this is how we pick the compositing device from a dmabuf's owner
        // instead of from config (see the plan, section 3).
        if (has_ext(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
            if (drm.hasPrimary != 0u)
                out("      drm primary {}:{}", drm.primaryMajor, drm.primaryMinor);
            if (drm.hasRender != 0u)
                out("      drm render  {}:{}", drm.renderMajor, drm.renderMinor);

            for (const DrmNode& n : drm_nodes) {
                const bool match_primary = drm.hasPrimary != 0u &&
                    n.dev_major == static_cast<unsigned>(drm.primaryMajor) &&
                    n.dev_minor == static_cast<unsigned>(drm.primaryMinor);
                const bool match_render = drm.hasRender != 0u &&
                    n.dev_major == static_cast<unsigned>(drm.renderMajor) &&
                    n.dev_minor == static_cast<unsigned>(drm.renderMinor);
                if (match_primary || match_render)
                    out("      -> {} ({})", n.path, n.driver);
            }
        } else {
            out("      drm        (VK_EXT_physical_device_drm unsupported: cannot match to a "
                "DRM node, so buffer-derived device selection is unavailable here)");
        }

        for (const char* want : kWanted)
            out("      {:<46} {}", want, has_ext(exts, want) ? "yes" : "NO");

        ev.vulkan.push_back(VulkanEncode{
            .index = i,
            .name = p.deviceName,
            .vendor = p.vendorID,
            .encode_queue = has_ext(exts, "VK_KHR_video_encode_queue"),
            .h264 = has_ext(exts, "VK_KHR_video_encode_h264"),
            .h265 = has_ext(exts, "VK_KHR_video_encode_h265"),
            .av1 = has_ext(exts, "VK_KHR_video_encode_av1"),
        });
    }

    vkDestroyInstance(inst, nullptr);
}

// ---------------------------------------------------------------------------- encoders

enum class Family { Vulkan, Nvenc, Vaapi, Qsv, Software };
enum class Codec { H264, Hevc, Av1, Audio };

struct Verdict {
    const char* status;
    std::string why;
};

const char* codec_ext(Codec c) {
    switch (c) {
    case Codec::H264: return "VK_KHR_video_encode_h264";
    case Codec::Hevc: return "VK_KHR_video_encode_h265";
    case Codec::Av1:  return "VK_KHR_video_encode_av1";
    default:          return "";
    }
}

bool claims(const VulkanEncode& d, Codec c) {
    switch (c) {
    case Codec::H264: return d.encode_queue && d.h264;
    case Codec::Hevc: return d.encode_queue && d.h265;
    case Codec::Av1:  return d.encode_queue && d.av1;
    default:          return false;
    }
}

std::string join(const std::vector<std::string>& parts) {
    std::string s;
    for (const std::string& p : parts)
        s += (s.empty() ? "" : ", ") + p;
    return s;
}

std::string label(const VulkanEncode& d) { return std::format("[{}] {}", d.index, d.name); }

Verdict judge_vulkan(const Evidence& ev, Codec c) {
    if (!ev.vulkan_ran)
        return {"unknown", "no Vulkan instance (see Vulkan section)"};
    std::vector<std::string> yes;
    bool any_queue = false;
    for (const VulkanEncode& d : ev.vulkan) {
        any_queue = any_queue || d.encode_queue;
        if (claims(d, c))
            yes.push_back(label(d));
    }
    if (!yes.empty())
        return {"claimed", std::format("{} advertises {}", join(yes), codec_ext(c))};
    if (!any_queue)
        return {"no", "no Vulkan device has VK_KHR_video_encode_queue"};
    return {"no", std::format("no Vulkan device advertises {}", codec_ext(c))};
}

// NVENC is never opened here, but on NVIDIA the Vulkan video encode extensions are the same
// hardware block reported by the same driver, so they are the cheapest honest witness to which
// codecs the silicon has. The one way this can be wrong is a driver old enough to expose the
// queue but predate a codec's extension, which is why "no" names the missing extension rather
// than asserting anything about the GPU.
Verdict judge_nvenc(const Evidence& ev, const std::vector<DrmNode>& drm_nodes, Codec c) {
    std::vector<const VulkanEncode*> nvidia;
    for (const VulkanEncode& d : ev.vulkan)
        if (d.vendor == 0x10de)
            nvidia.push_back(&d);

    if (nvidia.empty()) {
        const bool nvidia_drm = std::any_of(drm_nodes.begin(), drm_nodes.end(),
            [](const DrmNode& n) { return n.driver == "nvidia-drm"; });
        return nvidia_drm ? Verdict{"unknown", "NVIDIA GPU present but not visible to Vulkan"}
                          : Verdict{"no", "no NVIDIA GPU on this machine"};
    }

    std::vector<std::string> yes;
    std::vector<std::string> lacking;
    for (const VulkanEncode* d : nvidia) {
        if (claims(*d, c))
            yes.push_back(label(*d));
        else if (d->encode_queue)
            lacking.push_back(label(*d));
    }
    if (!yes.empty())
        return {"claimed", std::format("{} advertises {}", join(yes), codec_ext(c))};
    if (!lacking.empty())
        return {"no", std::format("{} lacks {}", join(lacking), codec_ext(c))};
    return {"unknown", "NVIDIA driver exposes no Vulkan video encode to read codecs from"};
}

Verdict judge_vaapi(const Evidence& ev, Codec c) {
    if (!ev.vainfo_ran)
        return {"unknown", "vainfo not installed, so no entrypoint was read"};
    if (c != Codec::H264)
        return {"unknown", "only the H.264 entrypoint is checked (VAAPI section)"};
    std::vector<std::string> yes;
    for (const VaapiNode& n : ev.vaapi)
        if (n.h264_encode)
            yes.push_back(n.path);
    if (!yes.empty())
        return {"claimed", std::format("{} advertises an H.264 encode entrypoint", join(yes))};
    return {"no", "no render node advertises an H.264 encode entrypoint"};
}

// The QSV section really does encode -- one second of H.264 -- so h264_qsv is the one encoder in
// this table the probe has run. Session creation comes before any codec is chosen, so when it
// fails it fails for HEVC and AV1 as well; that is a measured failure, not an inference.
Verdict judge_qsv(const Evidence& ev, Codec c) {
    if (!ev.qsv_ran)
        return {"unknown", "ffmpeg CLI not found, so no QSV session was tried"};
    if (ev.qsv.empty())
        return {"no", "no Intel render node"};

    std::vector<std::string> ok;
    std::vector<std::string> failed;
    for (const QsvNode& n : ev.qsv) {
        if (n.ok)
            ok.push_back(n.needs_ihd ? n.path + " (with LIBVA_DRIVER_NAME=iHD)" : n.path);
        else
            failed.push_back(n.path);
    }
    if (ok.empty()) {
        return c == Codec::H264
            ? Verdict{"FAILED", std::format("tried on {}: no QSV session (QSV section)", join(failed))}
            : Verdict{"FAILED", std::format("no QSV session on {}, so no QSV codec can run",
                                            join(failed))};
    }
    if (c == Codec::H264)
        return {"ran", std::format("encoded 1 s of H.264 on {}", join(ok))};
    return {"unknown", std::format("QSV session works on {}; only H.264 was run", join(ok))};
}

void probe_encoders(const Evidence& ev, const std::vector<DrmNode>& drm_nodes) {
    section("encoders (libavcodec)");
    out("  libavcodec {}.{}.{}", LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR,
        LIBAVCODEC_VERSION_MICRO);
    out("");
    out("  \"built\" means only that this libavcodec was compiled with the encoder. It is a fact");
    out("  about the ffmpeg build, NOT a claim that the encoder works on this machine (ADR-0008).");
    out("  \"this machine\" says what the sections above found, and how:");
    out("    ran      the probe encoded frames with it here");
    out("    FAILED   the probe tried it, or what it needs, here and it failed");
    out("    no       a driver reports no hardware encoder for this codec here");
    out("    claimed  a driver advertises it; the probe never opened it, so it is unproven");
    out("    unknown  nothing in this probe can tell");
    out("    software CPU encoder; no hardware to check");
    out("");

    struct Entry {
        const char* name;
        Family family;
        Codec codec;
    };
    static constexpr std::array<Entry, 15> kEncoders{{
        {"h264_vulkan", Family::Vulkan,   Codec::H264},
        {"hevc_vulkan", Family::Vulkan,   Codec::Hevc},
        {"av1_vulkan",  Family::Vulkan,   Codec::Av1},
        {"h264_nvenc",  Family::Nvenc,    Codec::H264},
        {"hevc_nvenc",  Family::Nvenc,    Codec::Hevc},
        {"av1_nvenc",   Family::Nvenc,    Codec::Av1},
        {"h264_vaapi",  Family::Vaapi,    Codec::H264},
        {"hevc_vaapi",  Family::Vaapi,    Codec::Hevc},
        {"av1_vaapi",   Family::Vaapi,    Codec::Av1},
        {"h264_qsv",    Family::Qsv,      Codec::H264},
        {"hevc_qsv",    Family::Qsv,      Codec::Hevc},
        {"av1_qsv",     Family::Qsv,      Codec::Av1},
        {"libx264",     Family::Software, Codec::H264},
        {"libsvtav1",   Family::Software, Codec::Av1},
        {"aac",         Family::Software, Codec::Audio},
    }};

    out("  {:<13} {:<6} {}", "encoder", "built", "this machine");
    for (const Entry& e : kEncoders) {
        if (avcodec_find_encoder_by_name(e.name) == nullptr) {
            out("  {:<13} {:<6} {:<8} {}", e.name, "no", "-", "not compiled into this libavcodec");
            continue;
        }
        Verdict v{"software", "CPU encoder; no hardware to check"};
        switch (e.family) {
        case Family::Vulkan:   v = judge_vulkan(ev, e.codec); break;
        case Family::Nvenc:    v = judge_nvenc(ev, drm_nodes, e.codec); break;
        case Family::Vaapi:    v = judge_vaapi(ev, e.codec); break;
        case Family::Qsv:      v = judge_qsv(ev, e.codec); break;
        case Family::Software: break;
        }
        out("  {:<13} {:<6} {:<8} {}", e.name, "yes", v.status, v.why);
    }
    out("");
    out("  note: *_nvenc is judged from the NVIDIA driver's Vulkan video extensions, which report");
    out("        the same encoder hardware. NVENC itself is never opened by the probe.");
    out("  note: av1_vulkan writes bitstreams dav1d cannot parse even where a driver claims AV1");
    out("        (ADR-0006). Do not read \"claimed\" there as usable.");
    out("  note: QSV is not a MunkiCast encode path (ADR-0008); it is listed because a working-");
    out("        looking QSV entry is exactly the confusion that ADR records.");
    out("  note: NVENC concurrent-session capacity is NOT probed yet. Per the plan it must be");
    out("        MEASURED (not hardcoded) before per-destination multistream encode is offered.");
}

// ---------------------------------------------------------------------------- PipeWire + portal

void probe_pipewire() {
    section("PipeWire");
    pw_init(nullptr, nullptr);
    out("  headers {}", pw_get_headers_version());
    out("  library {}", pw_get_library_version());
    pw_deinit();
}

void probe_portal(Findings& f) {
    section("xdg-desktop-portal");

    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (bus == nullptr) {
        warn_line(f, std::format("no session bus: {}",
                                 (err != nullptr && err->message != nullptr) ? err->message : "?"));
        if (err != nullptr) g_error_free(err);
        return;
    }

    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.DBus.Introspectable", "Introspect", nullptr, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &err);

    if (reply == nullptr) {
        warn_line(f, std::format("portal introspect failed: {}",
                                 (err != nullptr && err->message != nullptr) ? err->message : "?"));
        if (err != nullptr) g_error_free(err);
        g_object_unref(bus);
        return;
    }

    const char* xml = nullptr;
    g_variant_get(reply, "(&s)", &xml);
    const std::string_view doc = (xml != nullptr) ? std::string_view(xml) : std::string_view{};

    static constexpr std::array<const char*, 5> kIfaces{
        "org.freedesktop.portal.ScreenCast",
        "org.freedesktop.portal.GlobalShortcuts",
        "org.freedesktop.portal.Screenshot",
        "org.freedesktop.portal.Inhibit",
        "org.freedesktop.portal.RemoteDesktop",
    };
    for (const char* iface : kIfaces) {
        const bool present = doc.find(iface) != std::string_view::npos;
        out("  {:<42} {}", iface, present ? "yes" : "NO");
        if (!present && (std::strcmp(iface, "org.freedesktop.portal.ScreenCast") == 0 ||
                         std::strcmp(iface, "org.freedesktop.portal.GlobalShortcuts") == 0)) {
            warn_line(f, std::format("{} missing - required (ADR-0003)", iface));
        }
    }

    g_variant_unref(reply);
    g_object_unref(bus);
}

// ---------------------------------------------------------------------------- VAAPI drivers

// On a dual-GPU box, nvidia-vaapi-driver can claim VAAPI for *every* render node, including the
// Intel one, and NVDEC is decode-only. That surfaces as "No usable encoding entrypoint found".
// See ADR-0006. We must therefore pick the driver per device rather than trusting libva's default.
void probe_vaapi(Findings& f, Evidence& ev, const std::vector<DrmNode>& nodes) {
    section("VAAPI drivers per render node");

    if (!have_exe("vainfo")) {
        out("  vainfo not installed - cannot verify VAAPI driver binding (install libva-utils)");
        return;
    }
    ev.vainfo_ran = true;

    bool any_encode = false;
    for (const DrmNode& n : nodes) {
        if (!n.is_render)
            continue;

        const std::string drv = capture_cmd(std::format(
            "vainfo --display drm --device {} 2>&1 | sed -n 's/.*Driver version: *//p' | head -1",
            n.path).c_str());
        const std::string enc = capture_cmd(std::format(
            "vainfo --display drm --device {} 2>&1 | grep -c 'VAProfileH264High.*VAEntrypointEncSlice'",
            n.path).c_str());
        const bool has_enc = (!enc.empty() && enc != "0");
        any_encode = any_encode || has_enc;

        out("  {:<22} default driver : {}", n.path, drv.empty() ? "(none)" : drv);
        out("  {:<22} H264 encode    : {}", "", has_enc ? "yes" : "NO");
        VaapiNode& rec = ev.vaapi.emplace_back(VaapiNode{.path = n.path, .h264_encode = has_enc});

        // If the default binding cannot encode, try the vendor-appropriate driver explicitly.
        if (!has_enc) {
            const char* forced = (n.driver == "i915" || n.driver == "xe") ? "iHD"
                               : (n.driver == "amdgpu") ? "radeonsi" : nullptr;
            if (forced != nullptr) {
                const std::string fdrv = capture_cmd(std::format(
                    "LIBVA_DRIVER_NAME={} vainfo --display drm --device {} 2>&1 "
                    "| sed -n 's/.*Driver version: *//p' | head -1", forced, n.path).c_str());
                const std::string fenc = capture_cmd(std::format(
                    "LIBVA_DRIVER_NAME={} vainfo --display drm --device {} 2>&1 "
                    "| grep -c 'VAProfileH264High.*VAEntrypointEncSlice'", forced, n.path).c_str());
                if (!fenc.empty() && fenc != "0") {
                    any_encode = true;
                    rec.h264_encode = true;
                    out("  {:<22} with LIBVA_DRIVER_NAME={}: {} -- H264 encode yes", "", forced,
                        fdrv);
                    warn_line(f, std::format(
                        "{}: VAAPI default binding cannot encode, but LIBVA_DRIVER_NAME={} can. "
                        "MunkiCast must set the driver explicitly per device (ADR-0006).",
                        n.path, forced));
                }
            }
        }
    }

    if (!any_encode)
        out("  note: no VAAPI encode entrypoint found on any node; the VAAPI fallback tier is "
            "unavailable here.");
}

// ---------------------------------------------------------------------------- QSV sessions

// QSV sits on oneVPL over VAAPI, so the nvidia-vaapi-driver hijack (ADR-0006/0007) stops it from
// even creating a session. This is the author's production Twitch encoder, so it is probed directly.
void probe_qsv(Findings& f, Evidence& ev, const std::vector<DrmNode>& nodes) {
    section("QSV encode sessions");

    if (!have_exe("ffmpeg")) {
        out("  ffmpeg CLI not found - skipping QSV session check");
        return;
    }
    ev.qsv_ran = true;

    // A one-second 640x360 encode to /dev/null is enough to prove a session can be created.
    const auto qsv_cmd = [](const std::string& node, const char* env_prefix) {
        return std::string(env_prefix) +
               "ffmpeg -hide_banner -loglevel error -y -init_hw_device qsv=hw:" + node +
               " -filter_hw_device hw -f lavfi -i testsrc=size=640x360:rate=30:duration=1"
               " -vf format=nv12,hwupload=extra_hw_frames=16 -c:v h264_qsv -f null - 2>&1 | head -1";
    };

    bool probed_any = false;
    for (const DrmNode& n : nodes) {
        if (!n.is_render || (n.driver != "i915" && n.driver != "xe"))
            continue;
        probed_any = true;

        QsvNode& rec = ev.qsv.emplace_back(QsvNode{.path = n.path});
        const std::string plain = capture_cmd(qsv_cmd(n.path, "").c_str());
        if (plain.empty()) {
            rec.ok = true;
            out("  {:<22} QSV session (default env)      : OK", n.path);
            continue;
        }
        out("  {:<22} QSV session (default env)      : FAILED", n.path);
        out("  {:<22}   {}", "", plain);

        const std::string forced = capture_cmd(qsv_cmd(n.path, "LIBVA_DRIVER_NAME=iHD ").c_str());
        if (forced.empty()) {
            rec.ok = true;
            rec.needs_ihd = true;
            out("  {:<22} QSV with LIBVA_DRIVER_NAME=iHD : OK", "");
            warn_line(f, std::format(
                "{}: QSV requires LIBVA_DRIVER_NAME=iHD. MunkiCast sets this per encode worker "
                "process (ADR-0007) rather than relying on the session environment.", n.path));
        } else {
            out("  {:<22} QSV with LIBVA_DRIVER_NAME=iHD : FAILED", "");
            out("  {:<22}   {}", "", forced);
            warn_line(f, std::format("{}: QSV unavailable on this node", n.path));
        }
    }

    if (!probed_any)
        out("  no Intel render node present; QSV not applicable");
}

// ---------------------------------------------------------------------------- Omarchy theme

void probe_theme() {
    section("Omarchy theme");

    if (!have_exe("omarchy")) {
        out("  omarchy not found - the fallback theme provider will be used");
        return;
    }
    // Plan/ADR: /usr/share/omarchy/version is stale (reports 4.0.0.alpha on a 4.0.4-1 install).
    // Note: the subcommand is 'omarchy version'; 'omarchy --version' is not a valid command (rc=127).
    out("  omarchy version     {}", capture_cmd("omarchy version 2>/dev/null"));

    const std::string home = env_or("HOME", "");
    const std::string state = home + "/.local/state/omarchy/current";
    out("  theme.name          {}",
        capture_cmd(std::format("cat {}/theme.name 2>/dev/null", state).c_str()));
    out("  colors.toml mode    {}",
        capture_cmd(std::format("omarchy-theme-color --file {}/theme/colors.toml mode 2>/dev/null",
                                state).c_str()));
    out("  accent              {}", capture_cmd("omarchy-theme-color accent 2>/dev/null"));
    out("  background          {}", capture_cmd("omarchy-theme-color background 2>/dev/null"));
    out("  resolved keys       {}",
        capture_cmd("omarchy-theme-color --all 2>/dev/null | wc -l"));
    out("  background image    {}",
        capture_cmd(std::format("readlink -f {}/background 2>/dev/null", state).c_str()));
    out("");
    out("  hook dir            {}/.config/omarchy/hooks/theme-set.d/", home);
    out("  template dir        {}/.config/omarchy/themed/", home);
}

} // namespace

int probe_main(bool strict) {
    Findings f;

    out("munkicast probe - environment report");
    out("(capture/composite/encode relevant facts; attach this to any bug report)");

    Evidence ev;
    probe_session(f);
    const std::vector<DrmNode> drm_nodes = probe_drm();
    probe_vulkan(f, ev, drm_nodes);
    probe_vaapi(f, ev, drm_nodes);
    probe_qsv(f, ev, drm_nodes);
    probe_encoders(ev, drm_nodes);
    probe_pipewire();
    probe_portal(f);
    probe_theme();

    section("verdict");
    if (f.on_xwayland) {
        out("  FAIL: not a native Wayland session.");
        out("  MunkiCast is Wayland-native and ships no X11 backend (ADR-0003). Running under");
        out("  XWayland is refused deliberately rather than silently degrading.");
        return 2;
    }
    out("  Wayland session OK; {} warning(s).", f.warnings);
    if (strict && f.warnings > 0) {
        out("  FAIL: --strict was given and warnings were found.");
        return 1;
    }
    return 0;
}

} // namespace munkicast

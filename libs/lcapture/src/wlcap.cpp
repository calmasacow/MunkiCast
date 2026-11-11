// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ext-image-copy-capture-v1 capture backend.
//
// Why this exists alongside the portal path: see docs/adr/0011. The short version is that the
// portal path failed on this machine for reasons Omarchy's own recorder documents and routes
// around, and a capture failure cannot be attributed without a second, independent path.
//
// The structural difference from PipeWire is that *the client allocates the buffers*. That inverts
// the modifier problem in our favour: we choose a modifier from the intersection of what the
// compositor accepts and what our Vulkan device can use, allocate with it, and hand the compositor
// a buffer it can only either render into or reject outright. There is no "negotiated but
// unallocatable" state, which is exactly the trap ADR-0010 documents.

#include <lcapture/wlcap.hpp>

#include <lcore/log.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <poll.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

namespace munkicast::capture {

struct WlCap {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    ext_output_image_capture_source_manager_v1* source_mgr = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* window_mgr = nullptr;
    ext_foreign_toplevel_list_v1* window_list = nullptr;
    struct Window {
        WlWindowInfo info;
        ext_foreign_toplevel_handle_v1* handle = nullptr;
        bool closed = false;
    };
    std::vector<std::unique_ptr<Window>> windows;
    ext_image_copy_capture_manager_v1* capture_mgr = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;

    std::vector<WlOutputInfo> outputs;
    std::vector<wl_output*> output_proxies;

    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;

    // Session description, accumulated until `done`.
    WlCapCaps caps;
    bool caps_done = false;
    bool stopped = false;

    std::vector<wl_buffer*> slots;

    // Per-frame state.
    /// Written to by wlcap_wake() to break a blocking wait. Without it a thread parked on a
    /// still screen cannot be stopped: the compositor is correct to send nothing, so there is no
    /// event coming to notice a shutdown flag with.
    int wake_r = -1;
    int wake_w = -1;
    bool frame_ready = false;
    bool frame_failed = false;
    std::uint32_t frame_failure_reason = 0;
    std::uint64_t frame_presented_ns = 0;
};

namespace {

// ---------------------------------------------------------------------------- wl_output

void on_output_geometry(void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                        std::int32_t, const char*, const char*, std::int32_t) {}

void on_output_mode(void* data, wl_output* out, std::uint32_t flags, std::int32_t w, std::int32_t h,
                    std::int32_t refresh) {
    auto* cap = static_cast<WlCap*>(data);
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
        return;
    for (std::size_t i = 0; i < cap->output_proxies.size(); ++i) {
        if (cap->output_proxies[i] == out) {
            cap->outputs[i].width = w;
            cap->outputs[i].height = h;
            cap->outputs[i].refresh_mhz = refresh;
        }
    }
}

void on_output_done(void*, wl_output*) {}
void on_output_scale(void*, wl_output*, std::int32_t) {}

void on_output_name(void* data, wl_output* out, const char* name) {
    auto* cap = static_cast<WlCap*>(data);
    for (std::size_t i = 0; i < cap->output_proxies.size(); ++i)
        if (cap->output_proxies[i] == out)
            cap->outputs[i].name = name != nullptr ? name : "";
}

void on_output_description(void* data, wl_output* out, const char* desc) {
    auto* cap = static_cast<WlCap*>(data);
    for (std::size_t i = 0; i < cap->output_proxies.size(); ++i)
        if (cap->output_proxies[i] == out)
            cap->outputs[i].description = desc != nullptr ? desc : "";
}

constexpr wl_output_listener kOutputListener = {
    .geometry = on_output_geometry,
    .mode = on_output_mode,
    .done = on_output_done,
    .scale = on_output_scale,
    .name = on_output_name,
    .description = on_output_description,
};

// ---------------------------------------------------------------------------- capture session

void on_session_buffer_size(void* data, ext_image_copy_capture_session_v1*, std::uint32_t w,
                            std::uint32_t h) {
    auto* cap = static_cast<WlCap*>(data);
    cap->caps.width = w;
    cap->caps.height = h;
}

void on_session_shm_format(void* data, ext_image_copy_capture_session_v1*, std::uint32_t fmt) {
    static_cast<WlCap*>(data)->caps.shm_formats.push_back(fmt);
}

void on_session_dmabuf_device(void* data, ext_image_copy_capture_session_v1*, wl_array* dev) {
    auto* cap = static_cast<WlCap*>(data);
    // The device arrives as the raw bytes of a dev_t, not as a string or a pair of ints.
    if (dev != nullptr && dev->size >= sizeof(dev_t)) {
        dev_t d{};
        std::memcpy(&d, dev->data, sizeof d);
        cap->caps.dmabuf_device = static_cast<std::uint64_t>(d);
    }
}

void on_session_dmabuf_format(void* data, ext_image_copy_capture_session_v1*, std::uint32_t fmt,
                              wl_array* modifiers) {
    auto* cap = static_cast<WlCap*>(data);
    WlCapFormat f;
    f.drm_format = fmt;
    if (modifiers != nullptr) {
        // A packed array of uint64 modifiers. Reading it as anything else silently yields garbage
        // modifiers that then fail to allocate, so be explicit about the element size.
        const std::size_t n = modifiers->size / sizeof(std::uint64_t);
        const auto* vals = static_cast<const std::uint64_t*>(modifiers->data);
        f.modifiers.assign(vals, vals + n);
    }
    cap->caps.formats.push_back(std::move(f));
}

void on_session_done(void* data, ext_image_copy_capture_session_v1*) {
    static_cast<WlCap*>(data)->caps_done = true;
}

void on_session_stopped(void* data, ext_image_copy_capture_session_v1*) {
    auto* cap = static_cast<WlCap*>(data);
    cap->stopped = true;
    cap->caps_done = true;
}

constexpr ext_image_copy_capture_session_v1_listener kSessionListener = {
    .buffer_size = on_session_buffer_size,
    .shm_format = on_session_shm_format,
    .dmabuf_device = on_session_dmabuf_device,
    .dmabuf_format = on_session_dmabuf_format,
    .done = on_session_done,
    .stopped = on_session_stopped,
};

// ---------------------------------------------------------------------------- frame

void on_frame_transform(void*, ext_image_copy_capture_frame_v1*, std::uint32_t) {}
void on_frame_damage(void*, ext_image_copy_capture_frame_v1*, std::int32_t, std::int32_t,
                     std::int32_t, std::int32_t) {}
void on_frame_presentation_time(void* data, ext_image_copy_capture_frame_v1*,
                                std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo,
                                std::uint32_t tv_nsec) {
    auto* cap = static_cast<WlCap*>(data);
    const std::uint64_t sec = (static_cast<std::uint64_t>(tv_sec_hi) << 32) | tv_sec_lo;
    cap->frame_presented_ns = sec * 1'000'000'000ULL + tv_nsec;
}

void on_frame_ready(void* data, ext_image_copy_capture_frame_v1*) {
    static_cast<WlCap*>(data)->frame_ready = true;
}

void on_frame_failed(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t reason) {
    auto* cap = static_cast<WlCap*>(data);
    cap->frame_failed = true;
    cap->frame_failure_reason = reason;
}

constexpr ext_image_copy_capture_frame_v1_listener kFrameListener = {
    .transform = on_frame_transform,
    .damage = on_frame_damage,
    .presentation_time = on_frame_presentation_time,
    .ready = on_frame_ready,
    .failed = on_frame_failed,
};

const char* frame_failure_str(std::uint32_t reason) {
    switch (reason) {
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN: return "unknown";
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
        return "buffer constraints: the buffer we supplied is not acceptable";
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED: return "session stopped";
    default: return "unrecognised reason";
    }
}

// ---------------------------------------------------------------------------- registry

void window_closed(void* data, ext_foreign_toplevel_handle_v1*) {
    static_cast<WlCap::Window*>(data)->closed = true;
}
void window_done(void*, ext_foreign_toplevel_handle_v1*) {}
void window_title(void* data, ext_foreign_toplevel_handle_v1*, const char* title) {
    static_cast<WlCap::Window*>(data)->info.title = title;
}
void window_app(void* data, ext_foreign_toplevel_handle_v1*, const char* app) {
    static_cast<WlCap::Window*>(data)->info.app_id = app;
}
void window_id(void* data, ext_foreign_toplevel_handle_v1*, const char* id) {
    static_cast<WlCap::Window*>(data)->info.id = id;
}
constexpr ext_foreign_toplevel_handle_v1_listener kWindowListener = {
    window_closed, window_done, window_title, window_app, window_id};
void window_added(void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
    auto window = std::make_unique<WlCap::Window>();
    window->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &kWindowListener, window.get());
    static_cast<WlCap*>(data)->windows.push_back(std::move(window));
}
void window_list_done(void*, ext_foreign_toplevel_list_v1*) {}
constexpr ext_foreign_toplevel_list_v1_listener kWindowListListener = {window_added, window_list_done};

void on_global(void* data, wl_registry* reg, std::uint32_t id, const char* iface,
               std::uint32_t version) {
    auto* cap = static_cast<WlCap*>(data);
    const std::string_view name(iface);

    if (name == wl_output_interface.name) {
        // Version 4 is where wl_output gained `name`, which is what lets us address an output as
        // "DP-1" instead of by bind order. Bind order is not stable across compositor restarts.
        const std::uint32_t v = std::min(version, 4u);
        auto* out = static_cast<wl_output*>(wl_registry_bind(reg, id, &wl_output_interface, v));
        cap->output_proxies.push_back(out);
        cap->outputs.emplace_back();
        wl_output_add_listener(out, &kOutputListener, cap);
    } else if (name == ext_output_image_capture_source_manager_v1_interface.name) {
        cap->source_mgr = static_cast<ext_output_image_capture_source_manager_v1*>(
            wl_registry_bind(reg, id, &ext_output_image_capture_source_manager_v1_interface, 1));
    } else if (name == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
        cap->window_mgr = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
            wl_registry_bind(reg, id, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    } else if (name == ext_foreign_toplevel_list_v1_interface.name) {
        cap->window_list = static_cast<ext_foreign_toplevel_list_v1*>(
            wl_registry_bind(reg, id, &ext_foreign_toplevel_list_v1_interface, 1));
        ext_foreign_toplevel_list_v1_add_listener(cap->window_list, &kWindowListListener, cap);
    } else if (name == ext_image_copy_capture_manager_v1_interface.name) {
        cap->capture_mgr = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(reg, id, &ext_image_copy_capture_manager_v1_interface, 1));
    } else if (name == zwp_linux_dmabuf_v1_interface.name) {
        cap->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, std::min(version, 4u)));
    }
}

void on_global_remove(void*, wl_registry*, std::uint32_t) {}

constexpr wl_registry_listener kRegistryListener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

} // namespace

void wlcap_wake(WlCap& cap) {
    if (cap.wake_w < 0)
        return;
    const char byte = 1;
    // Deliberately unchecked: the only failures are "the pipe is full", which means a wake is
    // already pending and a second one would change nothing, and a closed pipe during teardown.
    const ssize_t ignored = ::write(cap.wake_w, &byte, 1);
    (void)ignored;
}

std::string wlcap_idle_reason(std::uint32_t idle_timeout_ms) {
    return std::format(
        "no new frame in {:.1f} s. The screen has not changed: capture is damage-driven "
        "(ADR-0013), so the compositor only sends a frame when something on the output redraws. "
        "On an idle desktop that is expected, not a fault - put something moving on the output "
        "(a terminal running a print loop is enough)",
        static_cast<double>(idle_timeout_ms) / 1000.0);
}

std::string wlcap_device_str(std::uint64_t dev) {
    const auto d = static_cast<dev_t>(dev);
    return std::format("{}:{}", major(d), minor(d));
}

WlCapPtr wlcap_open(std::string& error) {
    // The wake pipe is made before anything can block on the display, so a capture stopped
    // immediately after opening still has a way to interrupt its own wait.
    WlCapPtr cap(new WlCap());
    {
        int fds[2] = {-1, -1};
        if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            cap->wake_r = fds[0];
            cap->wake_w = fds[1];
        }
        // A failed pipe is not fatal: capture still works, and only the ability to interrupt a
        // wait on a still screen is lost. Worth not failing the whole open over.
    }

    cap->display = wl_display_connect(nullptr);
    if (cap->display == nullptr) {
        error = "wl_display_connect failed (no WAYLAND_DISPLAY?)";
        return nullptr;
    }

    cap->registry = wl_display_get_registry(cap->display);
    wl_registry_add_listener(cap->registry, &kRegistryListener, cap.get());
    // Two roundtrips: the first delivers the globals, the second the wl_output events (name, mode)
    // that only arrive after we bind.
    wl_display_roundtrip(cap->display);
    wl_display_roundtrip(cap->display);

    if (cap->source_mgr == nullptr) {
        error = "compositor does not implement ext_output_image_capture_source_manager_v1";
        return nullptr;
    }
    if (cap->capture_mgr == nullptr) {
        error = "compositor does not implement ext_image_copy_capture_manager_v1";
        return nullptr;
    }
    if (cap->dmabuf == nullptr) {
        error = "compositor does not implement zwp_linux_dmabuf_v1";
        return nullptr;
    }
    return cap;
}

const std::vector<WlOutputInfo>& wlcap_outputs(WlCap& cap) { return cap.outputs; }

std::vector<WlWindowInfo> list_windows(std::string& error) {
    auto cap = wlcap_open(error);
    if (!cap) return {};
    if (!cap->window_mgr || !cap->window_list) {
        error = "compositor does not support window capture";
        return {};
    }
    wl_display_roundtrip(cap->display);
    std::vector<WlWindowInfo> result;
    for (const auto& window : cap->windows)
        if (!window->closed && !window->info.id.empty()) result.push_back(window->info);
    return result;
}

bool window_size(const std::string& id, int& width, int& height, std::string& error) {
    auto cap = wlcap_open(error);
    if (!cap) return false;
    WlCapCaps caps;
    if (!wlcap_begin_session(*cap, id, false, caps, error, true)) return false;
    width = static_cast<int>(caps.width);
    height = static_cast<int>(caps.height);
    return width > 0 && height > 0;
}

std::optional<std::size_t> wlcap_select_output(const std::vector<WlOutputInfo>& outputs,
                                               const std::string& name, std::string& error) {
    if (outputs.empty()) {
        error = "no outputs";
        return std::nullopt;
    }
    if (name.empty())
        return std::size_t{0};
    const auto it = std::find_if(outputs.begin(), outputs.end(),
                                 [&](const WlOutputInfo& o) { return o.name == name; });
    if (it == outputs.end()) {
        // Names what was asked for and what exists. An operator reading this has a monitor
        // plugged into something, and the connector they expected is the thing to compare.
        std::string had;
        for (const WlOutputInfo& o : outputs) {
            if (!had.empty())
                had += ", ";
            had += o.name;
        }
        error = std::format("no output named '{}' (have: {})", name, had);
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - outputs.begin());
}

bool wlcap_begin_session(WlCap& cap, const std::string& output_name, bool with_cursor,
                         WlCapCaps& out, std::string& error, bool window) {
    if (window) {
        if (!cap.window_mgr || !cap.window_list) {
            error = "compositor does not support window capture";
            return false;
        }
        wl_display_roundtrip(cap.display);
        auto found = std::find_if(cap.windows.begin(), cap.windows.end(), [&](const auto& item) {
            return !item->closed && item->info.id == output_name;
        });
        if (found == cap.windows.end()) { error = "selected window is no longer open"; return false; }
        cap.source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
            cap.window_mgr, (*found)->handle);
    } else {
    const std::optional<std::size_t> chosen =
        wlcap_select_output(cap.outputs, output_name, error);
    if (!chosen)
        return false;
    const std::size_t index = *chosen;
    if (cap.output_proxies.empty()) {
        error = "no outputs";
        return false;
    }

    cap.source = ext_output_image_capture_source_manager_v1_create_source(
        cap.source_mgr, cap.output_proxies[index]);
    }
    const std::uint32_t options =
        with_cursor
            ? static_cast<std::uint32_t>(EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS)
            : 0u;
    cap.session =
        ext_image_copy_capture_manager_v1_create_session(cap.capture_mgr, cap.source, options);
    if (cap.session == nullptr) {
        error = "create_session failed";
        return false;
    }
    ext_image_copy_capture_session_v1_add_listener(cap.session, &kSessionListener, &cap);

    cap.caps = WlCapCaps{};
    cap.caps_done = false;
    while (!cap.caps_done) {
        if (wl_display_dispatch(cap.display) < 0) {
            error = "wl_display_dispatch failed while waiting for session description";
            return false;
        }
    }
    if (cap.stopped) {
        error = "compositor stopped the session immediately";
        return false;
    }
    out = cap.caps;
    return true;
}

bool wlcap_capture_frame(WlCap& cap, const DmabufImage& img, std::uint32_t drm_format,
                         std::string& error) {
    if (cap.session == nullptr) {
        error = "no session";
        return false;
    }

    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(cap.dmabuf);
    for (std::uint32_t i = 0; i < img.n_planes; ++i) {
        zwp_linux_buffer_params_v1_add(params, img.dmabuf_fd, i, img.offsets[i], img.strides[i],
                                       static_cast<std::uint32_t>(img.modifier >> 32),
                                       static_cast<std::uint32_t>(img.modifier & 0xffffffffu));
    }
    // create_immed rather than create: a protocol error here should be a hard failure now, not an
    // asynchronous surprise after we have already asked for a capture.
    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
        params, static_cast<std::int32_t>(img.width), static_cast<std::int32_t>(img.height),
        drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (buffer == nullptr) {
        error = "zwp_linux_buffer_params_v1_create_immed failed";
        return false;
    }

    ext_image_copy_capture_frame_v1* frame =
        ext_image_copy_capture_session_v1_create_frame(cap.session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, &cap);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, static_cast<std::int32_t>(img.width),
                                                  static_cast<std::int32_t>(img.height));
    ext_image_copy_capture_frame_v1_capture(frame);

    cap.frame_ready = false;
    cap.frame_failed = false;
    bool ok = true;
    while (!cap.frame_ready && !cap.frame_failed) {
        if (wl_display_dispatch(cap.display) < 0) {
            error = "wl_display_dispatch failed while waiting for the frame";
            ok = false;
            break;
        }
    }
    if (ok && cap.frame_failed) {
        error = std::format("compositor failed the capture: {}",
                            frame_failure_str(cap.frame_failure_reason));
        ok = false;
    }

    ext_image_copy_capture_frame_v1_destroy(frame);
    wl_buffer_destroy(buffer);
    return ok;
}

bool wlcap_add_slot(WlCap& cap, const DmabufImage& img, std::uint32_t drm_format,
                    std::string& error) {
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(cap.dmabuf);
    for (std::uint32_t i = 0; i < img.n_planes; ++i)
        zwp_linux_buffer_params_v1_add(params, img.dmabuf_fd, i, img.offsets[i], img.strides[i],
                                       static_cast<std::uint32_t>(img.modifier >> 32),
                                       static_cast<std::uint32_t>(img.modifier & 0xffffffffu));
    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
        params, static_cast<std::int32_t>(img.width), static_cast<std::int32_t>(img.height),
        drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (buffer == nullptr) {
        error = "create_immed failed for buffer slot";
        return false;
    }
    cap.slots.push_back(buffer);
    return true;
}

namespace {

enum class Dispatch { Progress, TimedOut, Failed, Woken };

/// One round of event dispatch that gives up at `deadline` instead of blocking for ever.
///
/// wl_display_dispatch() has no timeout, so this is the prepare_read / poll / read_events dance
/// libwayland documents for exactly this purpose. A prepare_read that fails means events are
/// already queued; dispatching them is progress, and the caller re-checks its condition before
/// waiting again, so an event that was already here never costs a timeout.
/// Waits for an event with no deadline, but stays interruptible.
///
/// The plain `wl_display_dispatch` cannot be unblocked, which is fine for a tool that exits when
/// it is done and wrong for a capture that has to stop when its source is removed. Polling the
/// display's descriptor alongside a wake pipe keeps the wait unbounded -- ADR-0013 Consequence 4
/// is about not re-requesting, not about never returning -- while leaving a way out.
Dispatch dispatch_forever(wl_display* display, int wake_fd) {
    if (wl_display_prepare_read(display) != 0)
        return wl_display_dispatch_pending(display) < 0 ? Dispatch::Failed : Dispatch::Progress;
    wl_display_flush(display);
    for (;;) {
        pollfd fds[2] = {{wl_display_get_fd(display), POLLIN, 0}, {wake_fd, POLLIN, 0}};
        const int r = ::poll(fds, wake_fd >= 0 ? 2 : 1, -1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            wl_display_cancel_read(display);
            return Dispatch::Failed;
        }
        if (wake_fd >= 0 && (fds[1].revents & POLLIN) != 0) {
            wl_display_cancel_read(display);
            return Dispatch::Woken;
        }
        if ((fds[0].revents & POLLIN) == 0)
            continue;
        if (wl_display_read_events(display) < 0)
            return Dispatch::Failed;
        return wl_display_dispatch_pending(display) < 0 ? Dispatch::Failed : Dispatch::Progress;
    }
}

Dispatch dispatch_until(wl_display* display, std::chrono::steady_clock::time_point deadline) {
    if (wl_display_prepare_read(display) != 0)
        return wl_display_dispatch_pending(display) < 0 ? Dispatch::Failed : Dispatch::Progress;
    wl_display_flush(display);
    for (;;) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            wl_display_cancel_read(display);
            return Dispatch::TimedOut;
        }
        pollfd pfd{wl_display_get_fd(display), POLLIN, 0};
        const int r = ::poll(&pfd, 1, static_cast<int>(left.count()));
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            wl_display_cancel_read(display);
            return Dispatch::Failed;
        }
        if (r == 0)
            continue; // re-checked against the deadline above, which is the one source of truth
        if (wl_display_read_events(display) < 0)
            return Dispatch::Failed;
        return wl_display_dispatch_pending(display) < 0 ? Dispatch::Failed : Dispatch::Progress;
    }
}

} // namespace

bool wlcap_capture_slot(WlCap& cap, std::size_t index, std::uint32_t width, std::uint32_t height,
                        std::uint64_t* out_presented_ns, std::string& error,
                        std::uint32_t idle_timeout_ms, bool* out_idle) {
    if (out_idle != nullptr)
        *out_idle = false;
    if (index >= cap.slots.size()) {
        error = "slot index out of range";
        return false;
    }
    ext_image_copy_capture_frame_v1* frame =
        ext_image_copy_capture_session_v1_create_frame(cap.session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, &cap);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, cap.slots[index]);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, static_cast<std::int32_t>(width),
                                                  static_cast<std::int32_t>(height));
    ext_image_copy_capture_frame_v1_capture(frame);

    cap.frame_ready = false;
    cap.frame_failed = false;
    cap.frame_presented_ns = 0;
    bool ok = true;
    // The 0 path keeps the plain blocking dispatch rather than an infinite deadline, so "wait for
    // ever" is literally the code that ran before the timeout existed.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(idle_timeout_ms);
    while (!cap.frame_ready && !cap.frame_failed) {
        const Dispatch d = idle_timeout_ms == 0
                               ? dispatch_forever(cap.display, cap.wake_r)
                               : dispatch_until(cap.display, deadline);
        if (d == Dispatch::Woken) {
            error = "capture was stopped";
            ok = false;
            break;
        }
        if (d == Dispatch::Failed) {
            error = "wl_display_dispatch failed while waiting for the frame";
            ok = false;
            break;
        }
        if (d == Dispatch::TimedOut) {
            // Destroying a frame that is still pending is allowed at any time by the protocol and
            // the session survives it - but see the header before asking it for another frame.
            error = wlcap_idle_reason(idle_timeout_ms);
            if (out_idle != nullptr)
                *out_idle = true;
            ok = false;
            break;
        }
    }
    if (ok && cap.frame_failed) {
        error = std::format("compositor failed the capture: {}",
                            frame_failure_str(cap.frame_failure_reason));
        ok = false;
    }
    if (out_presented_ns != nullptr)
        *out_presented_ns = cap.frame_presented_ns;
    ext_image_copy_capture_frame_v1_destroy(frame);
    return ok;
}

void wlcap_close(WlCap* cap) {
    if (cap == nullptr)
        return;
    if (cap->wake_r >= 0)
        ::close(cap->wake_r);
    if (cap->wake_w >= 0)
        ::close(cap->wake_w);
    for (wl_buffer* b : cap->slots)
        wl_buffer_destroy(b);
    if (cap->session != nullptr)
        ext_image_copy_capture_session_v1_destroy(cap->session);
    if (cap->source != nullptr)
        ext_image_capture_source_v1_destroy(cap->source);
    if (cap->source_mgr != nullptr)
        ext_output_image_capture_source_manager_v1_destroy(cap->source_mgr);
    for (const auto& window : cap->windows)
        ext_foreign_toplevel_handle_v1_destroy(window->handle);
    if (cap->window_list) ext_foreign_toplevel_list_v1_destroy(cap->window_list);
    if (cap->window_mgr) ext_foreign_toplevel_image_capture_source_manager_v1_destroy(cap->window_mgr);
    if (cap->capture_mgr != nullptr)
        ext_image_copy_capture_manager_v1_destroy(cap->capture_mgr);
    if (cap->dmabuf != nullptr)
        zwp_linux_dmabuf_v1_destroy(cap->dmabuf);
    for (wl_output* o : cap->output_proxies)
        wl_output_destroy(o);
    if (cap->registry != nullptr)
        wl_registry_destroy(cap->registry);
    if (cap->display != nullptr) {
        wl_display_flush(cap->display);
        wl_display_disconnect(cap->display);
    }
    delete cap;
}

} // namespace munkicast::capture

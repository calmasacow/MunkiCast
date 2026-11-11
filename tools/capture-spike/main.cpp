// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase-0 capture spike. Staged deliberately so each stage is verifiable on its own:
//   stage 1  portal  : negotiate a ScreenCast session, print node id + PipeWire fd
//   stage 2  stream  : connect to the node, report the negotiated format/modifier, count frames
//   stage 3  import  : import each dmabuf into a VkImage and prove correctness by readback
// Run with --stage N (default: the highest implemented stage).

#include "portal.hpp"
#include "stream.hpp"
#include <lrhi/vulkan.hpp>
#include <lcapture/wlcap.hpp>
#include "v4l2cap.hpp"

#include <linux/videodev2.h>

#include <lcore/log.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <functional>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <cstdio>

#include <sys/stat.h>
#include <unistd.h>

namespace {

void usage() {
    std::puts(R"(capture-spike - Phase-0 portal/PipeWire/Vulkan capture probe

usage:
  capture-spike [--stage N] [--window] [--no-cursor] [--restore-token TOK]

  --stage N            stop after stage N (1=portal, 2=stream, 3=vulkan import)
  --no-restore         ignore the cached restore token and always show the picker
  --no-probe           skip the node readiness probe and connect immediately
  --wayland, --wl      use the ext-image-copy-capture-v1 backend (no portal, NO PICKER)
  --output NAME        capture this output by connector name, e.g. DP-1 (implies --wayland)
  --second-gpu MAJ:MIN transfer the frame to a second GPU and import it there (ADR-0007)
  --scale WxH          downscale on the compositing GPU before the cross-GPU transfer
  --frames N           capture N frames (default 3); use a few hundred to measure throughput
  --slots N            rotate between N capture buffers (default 4; 1 serializes the compositor)
  --idle-timeout SECS  give up after SECS with no new frame (default 10; 0 waits for ever).
                       Capture is damage-driven, so a still screen sends nothing; exits 7
  --v4l2 DEVICE        capture from a V4L2 device (capture card, webcam) and report its timing
  --v4l2-size WxH      request this capture size      --v4l2-format NV12   request this fourcc
  --v4l2-fps N         request this frame rate        --v4l2-dmabuf        test VIDIOC_EXPBUF
  --v4l2-range full|limited   request a quantization range (drivers may ignore it)
  --forget             delete the cached restore token and exit
  --sweep              try every modifier variant inside ONE portal session (one picker click)
  --drm-node MAJ:MIN   select the Vulkan device by DRM node (never by index)
  --mirror             advertise one fully-fixed format matching the source (bisect helper)
  --dump-formats       print the EnumFormat params the source node offers, then exit
  --shm-only           advertise no modifiers (shm path only) - for bisecting negotiation
  --modifiers LIST     comma-separated DRM modifiers to advertise, e.g. 0x0,0x300000000e08014
  --window             request window sources instead of monitors
  --no-cursor          request a hidden cursor instead of embedded
  --restore-token TOK  reuse a previous session and skip the picker

A restore token is cached automatically once frames have actually flowed, so the picker appears
only on the first run (or after a token goes stale). A session whose node turns out not to exist
is discarded and retried with a fresh picker - see docs/adr/0009.)");
}

const char* source_type_name(std::uint32_t t) {
    switch (t) {
    case 1: return "monitor";
    case 2: return "window";
    case 4: return "virtual";
    default: return "unknown";
    }
}

} // namespace

namespace {

std::string g_session_handle; // for the signal handler

void on_fatal_signal(int sig) {
    // `timeout` sends SIGTERM and destructors do not run, so without this every timed-out run
    // leaked a live capture session on the compositor.
    if (!g_session_handle.empty())
        munkicast::spike::close_screencast_session(g_session_handle);
    std::_Exit(128 + sig);
}

/// Closes the portal session on any exit path. Leaking one leaves a live capture stream on the
/// compositor and poisons subsequent sessions (see docs/adr/0009).
struct SessionCloser {
    const std::string* handle = nullptr;
    ~SessionCloser() {
        if (handle != nullptr && !handle->empty())
            munkicast::spike::close_screencast_session(*handle);
    }
};

// ------------------------------------------------------------------ restore-token cache
//
// The spike caches its own token so repeat runs do not need a picker click, and so the
// stale-token path is exercised by ordinary use rather than only by a hand-passed argument.
// Per ADR-0009 the token is written ONLY after frames have actually flowed: a token that merely
// produced a session handle has not been shown to work.

std::string token_cache_path() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string dir;
    if (xdg != nullptr && xdg[0] != '\0') {
        dir = xdg;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        dir = std::string(home) + "/.cache";
    } else {
        return {};
    }
    dir += "/munkicast";
    ::mkdir(dir.c_str(), 0700);
    return dir + "/capture-spike.token";
}

std::string load_cached_token() {
    const std::string path = token_cache_path();
    if (path.empty())
        return {};
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr)
        return {};
    std::string tok;
    char buf[512];
    while (const std::size_t n = std::fread(buf, 1, sizeof buf, fp))
        tok.append(buf, n);
    std::fclose(fp);
    while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r'))
        tok.pop_back();
    return tok;
}

void save_cached_token(const std::string& tok) {
    const std::string path = token_cache_path();
    if (path.empty() || tok.empty())
        return;
    if (std::FILE* fp = std::fopen(path.c_str(), "wb"); fp != nullptr) {
        std::fwrite(tok.data(), 1, tok.size(), fp);
        std::fputc('\n', fp);
        std::fclose(fp);
        ::chmod(path.c_str(), 0600);
    }
}

void forget_cached_token() {
    const std::string path = token_cache_path();
    if (!path.empty())
        ::unlink(path.c_str());
}

// ------------------------------------------------------------------ validated acquisition

/// A capture session that has been proven to have a live, format-advertising node behind it.
struct Acquired {
    munkicast::spike::PortalSession session;
    munkicast::spike::PwSessionPtr pw;
    munkicast::spike::NodeFormats formats; ///< what the node advertised at validation time
    bool restored = false; ///< true if a restore token was used and accepted
};

/// Orders the modifiers we can import by what the source actually offers, its own preference
/// first, and drops anything the source never mentioned.
///
/// The compositor's preference is the single most useful signal available: Hyprland advertises
/// NVIDIA(0x...606010-015) for DP-1 but publishes 0x...606014 as its fixed layout, and fixating
/// 0x...606015 negotiates, reaches `streaming`, and then delivers no frames at all. Trying our own
/// enumeration order first means walking into that on purpose. Ordering by the source's preference
/// is also what keeps the fallback ladder cheap - and possibly what keeps it workable, since a
/// failed attempt appears to leave the node with no formats to retry against.
std::vector<std::uint64_t> order_candidates(const std::vector<std::uint64_t>& ours,
                                            const munkicast::spike::NodeFormats& src) {
    if (src.offered.empty())
        return ours; // a source with no modifier property at all: shm path, nothing to intersect
    std::vector<std::uint64_t> out;
    const auto take = [&](std::uint64_t m) {
        if (m == munkicast::spike::kModifierInvalid)
            return; // wildcard: appended last, never preferred
        if (std::find(ours.begin(), ours.end(), m) == ours.end())
            return;
        if (std::find(out.begin(), out.end(), m) == out.end())
            out.push_back(m);
    };
    for (std::uint64_t m : src.preferred)
        take(m);
    for (std::uint64_t m : src.offered)
        take(m);
    // DRM_FORMAT_MOD_INVALID is deliberately never added. It is the absence of a modifier, not a
    // modifier, and the ladder's implicit/shm rung is how we express "no explicit modifier".
    return out;
}

/// Negotiates a ScreenCast session and *validates* it before handing it back, which is the
/// behaviour ADR-0009 requires of the real capture worker:
///
///   1. ScreenCast.Start can report success while returning a node id that PipeWire never
///      registers. That happens with a stale restore token and is indistinguishable downstream
///      from a modifier incompatibility, so it must be caught here.
///   2. A restored session that fails validation is discarded - token dropped, portal session
///      closed - and retried as a fresh picker session, rather than surfaced as a capture error.
///   3. A *fresh* session that fails validation is a real fault and is reported as one. Retrying
///      it would just reprompt the user forever.
bool acquire_capture_session(munkicast::spike::PortalRequest req, Acquired& out,
                             const std::function<bool(Acquired&, std::string&)>& verify,
                             bool probe_node, bool persist_token, std::string& error) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool restoring = !req.restore_token.empty();
        if (restoring)
            lc::info("portal: restoring previous session (no picker expected)");
        else
            lc::info("portal: requesting a fresh session (a picker will appear)");

        munkicast::spike::PortalSession session;
        if (!munkicast::spike::negotiate_screencast(req, session, error))
            return false; // includes user cancellation; never retry over the user

        if (session.streams.empty()) {
            error = "portal returned a session with no streams";
            munkicast::spike::close_screencast_session(session.session_handle);
            return false;
        }

        // Log the source NOW. The offered formats differ per output, so a run whose capture source
        // is unknown is not a measurement - and the summary block below only prints on success.
        for (const auto& st : session.streams)
            lc::info("CAPTURE SOURCE: {} ({}) {}x{} at ({},{}) node {}",
                     st.mapping_id.empty() ? std::string("(no mapping_id)") : st.mapping_id,
                     source_type_name(st.source_type), st.width, st.height, st.pos_x, st.pos_y,
                     st.node_id);

        // From here on the session must be closed on every failure path.
        const std::uint32_t node_id = session.streams.front().node_id;
        std::string serr;
        munkicast::spike::PwSessionPtr pw =
            munkicast::spike::pw_session_open(session.pipewire_fd, serr);
        session.pipewire_fd = -1; // ownership moved into the PipeWire session
        if (!pw) {
            error = std::format("pw_session_open failed: {}", serr);
            munkicast::spike::close_screencast_session(session.session_handle);
            return false;
        }

        // --no-probe exists to test whether the readiness probe is itself the problem. It binds
        // the node over the registry and enumerates its params, and the only stage-2 run that
        // ever delivered frames was the one written before the probe existed.
        munkicast::spike::NodeFormats formats;
        const munkicast::spike::NodeReadiness r =
            probe_node ? munkicast::spike::pw_probe_node(*pw, node_id, &formats)
                       : munkicast::spike::NodeReadiness::Ready;
        std::string why;
        if (r == munkicast::spike::NodeReadiness::Ready && probe_node) {
            lc::info("node {} offers {} modifier(s) before we connect:", node_id,
                     formats.offered.size());
            for (std::uint64_t m : formats.offered)
                lc::info("  offered: {}{}", munkicast::spike::modifier_str(m),
                         std::find(formats.preferred.begin(), formats.preferred.end(), m) !=
                                 formats.preferred.end()
                             ? "   <- published as a fixed value (its current layout)"
                             : "");
        }
        if (r == munkicast::spike::NodeReadiness::Ready) {
            // Persist the token HERE, on validation, not after the whole run succeeds. The token
            // records that the user authorised this source; whether our pipeline then works is a
            // separate question. Saving it only on full success meant every debugging run cost
            // another picker click - the opposite of what the policy was for.
            if (!session.restore_token.empty() && persist_token)
                save_cached_token(session.restore_token);
            out.session = std::move(session);
            out.pw = std::move(pw);
            out.formats = std::move(formats);
            out.restored = restoring;
            // Readiness is necessary but NOT sufficient: a restored session can hand back a node
            // that is still in the registry, still answers a format query, and is dead by the
            // time we connect. The only proof a session works is frames flowing through it, so
            // the caller's real attempt is the last validation step.
            if (!verify || verify(out, why))
                return true;
            lc::warn("session rejected: node {} passed readiness but failed in use ({})", node_id,
                     why);
            session = std::move(out.session);
            pw = std::move(out.pw);
        } else {
            why = munkicast::spike::node_readiness_str(r);
            lc::warn("session rejected: node {} is unusable ({})", node_id, why);
        }

        pw.reset();
        munkicast::spike::close_screencast_session(session.session_handle);

        if (!restoring) {
            error = std::format("a freshly picked source is unusable: node {}: {}", node_id, why);
            return false;
        }

        // Discard the token only on definitive evidence that it is bad - a node that does not
        // exist, or exists with no formats. A failure *in use* is far more likely to be a bug of
        // ours, and throwing the token away for that means every debugging run costs the user
        // another picker click, which is exactly backwards.
        if (r != munkicast::spike::NodeReadiness::Ready) {
            lc::warn("discarding the restore token: the node it names is unusable");
            forget_cached_token();
        } else {
            lc::warn("keeping the restore token (the node was valid; the failure was in use)");
        }
        lc::warn("falling back to a fresh picker session");
        req.restore_token.clear();
    }
    error = "could not acquire a usable capture session";
    return false;
}

/// Captures from a V4L2 device and reports timing from the DRIVER's timestamps.
///
/// Two things this answers that nothing else can: whether a capture card is actually receiving a
/// signal (it streams black frames at full rate when it is not), and what rate the source is
/// really sending - which need not be what either machine was configured for.
int run_v4l2(const munkicast::spike::V4l2Config& cfg) {
    munkicast::spike::V4l2Result r;
    std::string err;
    if (!munkicast::spike::v4l2_capture(cfg, r, err)) {
        lc::error("v4l2 capture failed: {}", err);
        return 3;
    }
    if (!err.empty())
        lc::warn("capture ended early: {}", err);

    std::printf("\n=== %s ===\n", cfg.device.c_str());
    std::printf("  negotiated    : %ux%u %s\n", r.width, r.height,
                munkicast::spike::v4l2_fourcc_str(r.fourcc).c_str());
    std::printf("  colour        : %s / %s / %s / quantization %s\n",
                munkicast::spike::v4l2_colorspace_str(r.colorspace).c_str(),
                munkicast::spike::v4l2_ycbcr_enc_str(r.ycbcr_enc).c_str(),
                munkicast::spike::v4l2_xfer_func_str(r.xfer_func).c_str(),
                munkicast::spike::v4l2_quantization_str(r.quantization).c_str());
    std::printf("  frames        : %u\n", r.captured);
    std::printf("  dmabuf export : %s\n", r.dmabuf_exported ? "yes (zero-copy possible)"
                                                             : "no (mmap copy required)");

    const auto stats = [](std::vector<double> v, const char* label) {
        if (v.empty())
            return;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v)
            sum += x;
        const double mean = sum / static_cast<double>(v.size());
        std::printf("  %-13s : mean %.3f ms (%.2f fps)  p50 %.3f  p99 %.3f  max %.3f\n", label,
                    mean, mean > 0 ? 1000.0 / mean : 0.0, v[v.size() / 2],
                    v[(v.size() * 99) / 100], v.back());
    };
    stats(r.driver_ms, "driver gap");
    stats(r.wall_ms, "wall gap");

    // The rate the DRIVER actually ran at, over the whole run rather than frame to frame. A
    // capture card has its own crystal; it is not the source's and it is not the encoder's, and
    // the difference only becomes visible over a long span.
    if (r.span_ms > 0 && r.captured > 1) {
        const double produced = static_cast<double>(r.seq_last - r.seq_first);
        std::printf("  measured rate : %.4f fps over %.2f s  (dequeued %.4f fps)\n",
                    produced * 1000.0 / r.span_ms, r.span_ms / 1000.0,
                    static_cast<double>(r.captured - 1) * 1000.0 / r.span_ms);
    }
    std::printf("  driver seq    : %u..%u, %u break(s), %llu frame(s) never delivered to us\n",
                r.seq_first, r.seq_last, r.seq_breaks,
                static_cast<unsigned long long>(r.seq_missing));

    // Signal detection, because this device has no ioctl for it.
    //
    // Measured on the 4K X: with no input it streams Y=0 everywhere (and neutral chroma 128). With
    // a valid input it is limited-range Rec.709, so even true video black is Y=16. That gap is the
    // discriminator - but only on the luma plane, and only after the device has settled.
    const std::uint32_t settle = r.luma.size() > 8 ? 4u : 0u;
    std::uint32_t startup_black = 0;
    for (std::size_t i = 0; i < r.luma_max.size() && r.luma_max[i] == 0; ++i)
        ++startup_black;

    double lmean = 0;
    unsigned lo = 255;
    unsigned hi = 0;
    std::size_t n = 0;
    for (std::size_t i = settle; i < r.luma.size(); ++i) {
        lmean += r.luma[i];
        lo = std::min<unsigned>(lo, r.luma_min[i]);
        hi = std::max<unsigned>(hi, r.luma_max[i]);
        ++n;
    }
    lmean = n > 0 ? lmean / static_cast<double>(n) : 0.0;

    std::printf("  luma (plane 0): mean %.2f  min %u  max %u   [first %zu frames skipped]\n", lmean,
                lo, hi, static_cast<std::size_t>(settle));
    std::printf("  startup black : %u frame(s) of Y=0 before content\n", startup_black);

    // Cross-check the DECLARED range against the pixels actually delivered. The 4K X accepts a
    // request for full range, echoes V4L2_QUANTIZATION_FULL_RANGE back in the format reply, and
    // then sends limited-range pixels anyway. Comparing the reply to the request cannot catch
    // that - only the pixels can. Limited range clamps luma into [16,235]; full range does not.
    const char* declared = r.quantization == V4L2_QUANTIZATION_FULL_RANGE   ? "full"
                           : r.quantization == V4L2_QUANTIZATION_LIM_RANGE ? "limited"
                                                                           : "default";
    const bool observed_full = lo < 16 || hi > 235;
    if (r.quantization == V4L2_QUANTIZATION_FULL_RANGE && !observed_full) {
        lc::warn("device DECLARES full range but delivers luma clamped to [{},{}] - the "
                 "declaration is false; range must come from configuration, not the device",
                 lo, hi);
    } else if (r.quantization == V4L2_QUANTIZATION_LIM_RANGE && observed_full) {
        lc::warn("device declares limited range but delivers luma [{},{}] outside [16,235]", lo,
                 hi);
    }
    std::printf("  range         : declared %s, observed %s\n", declared,
                observed_full ? "full" : "limited (luma inside [16,235])");

    const bool signal = hi > 0;
    if (!signal) {
        std::printf("  SIGNAL        : ABSENT - luma is identically 0. The card streams at full "
                    "rate with no input, so frame flow alone proves nothing.\n");
    } else if (hi <= 16) {
        std::printf("  SIGNAL        : PRESENT, picture is black (luma pinned at limited-range "
                    "black, not 0 - the source is sending, the content is dark)\n");
    } else {
        std::printf("  SIGNAL        : PRESENT  (%s range)\n", lo < 16 ? "full" : "limited");
    }
    return signal ? 0 : 6;
}

/// Hands a captured frame to a *second* GPU, the way a second encoder would receive it.
///
/// This is the load-bearing risk in the dual-encode design: the show streams 1440p from NVENC to
/// YouTube and 1080p from VAAPI on the Intel iGPU to Twitch, which only works if a buffer the
/// NVIDIA GPU produced can be read by Intel. A vendor-specific block-linear modifier cannot cross,
/// so the transfer buffer is LINEAR - and the downscale happens here, on the compositing GPU,
/// because moving full-resolution pixels across the bus to shrink them on the far side spends the
/// bandwidth that is the actual scarce resource.
bool run_cross_gpu(munkicast::rhi::VkCtx& src_ctx, const munkicast::rhi::VkExportedImage& captured,
                   std::uint32_t drm_format, const std::string& second_node, std::uint32_t scale_w,
                   std::uint32_t scale_h) {
    std::string err;
    const std::uint32_t w = scale_w != 0 ? scale_w : captured.width;
    const std::uint32_t h = scale_h != 0 ? scale_h : captured.height;

    std::printf("\n=== cross-GPU transfer: %ux%u -> %ux%u, then import on %s ===\n",
                captured.width, captured.height, w, h, second_node.c_str());

    munkicast::rhi::VkCtxPtr dst_ctx = munkicast::rhi::vk_init(second_node, err);
    if (!dst_ctx) {
        lc::error("second device init failed: {}", err);
        return false;
    }
    lc::info("second device: {}", munkicast::rhi::vk_device_name(*dst_ctx));

    const std::vector<std::uint64_t> dst_mods =
        munkicast::rhi::vk_supported_modifiers(*dst_ctx, drm_format);
    const bool dst_takes_linear =
        std::find(dst_mods.begin(), dst_mods.end(), munkicast::spike::kModifierLinear) !=
        dst_mods.end();
    std::printf("  second device can import %zu modifier(s) for %s; LINEAR: %s\n", dst_mods.size(),
                munkicast::spike::fourcc_str(drm_format).c_str(), dst_takes_linear ? "yes" : "NO");
    if (!dst_takes_linear) {
        lc::error("the second device cannot import LINEAR, so there is no common layout");
        return false;
    }

    // Allocate the transfer buffer on the SOURCE device, LINEAR, so both vendors can address it.
    munkicast::rhi::VkExportedImage transfer;
    // host_memory = true: see vk_create_exportable's contract. A VRAM-backed dmabuf imports
    // cleanly on the second card and then fails at submit, which is a miserable thing to debug.
    if (!munkicast::rhi::vk_create_exportable(src_ctx, w, h, drm_format,
                                               {munkicast::spike::kModifierLinear}, true, transfer,
                                               err)) {
        lc::error("could not allocate a LINEAR transfer image on the source device: {}", err);
        return false;
    }
    lc::info("transfer buffer: {}x{} modifier {} stride {}", transfer.width, transfer.height,
             munkicast::spike::modifier_str(transfer.modifier), transfer.strides[0]);

    if (!munkicast::rhi::vk_blit_exported(src_ctx, captured, true, transfer, err)) {
        lc::error("blit/scale on the source device failed: {}", err);
        munkicast::rhi::vk_destroy_exportable(src_ctx, transfer);
        return false;
    }

    // Import on the second device through the ordinary import path - the same code the portal
    // backend uses, so this exercises it too.
    const munkicast::spike::CapturedFrame frame =
        munkicast::rhi::vk_exported_as_frame(transfer, drm_format);
    std::vector<std::uint8_t> pixels;
    const bool ok = munkicast::rhi::vk_import_and_readback(*dst_ctx, frame, pixels, err);
    if (!ok) {
        lc::error("import on the second device failed: {}", err);
        munkicast::rhi::vk_destroy_exportable(src_ctx, transfer);
        return false;
    }

    std::uint32_t nonzero = 0;
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        checksum = checksum * 1099511628211ULL ^ pixels[i];
        if ((pixels[i] | pixels[i + 1] | pixels[i + 2]) != 0)
            ++nonzero;
    }
    const std::uint32_t total = w * h;
    std::printf("  imported on second device: %zu bytes, non-black %u / %u (%.1f%%)\n",
                pixels.size(), nonzero, total, total > 0 ? 100.0 * nonzero / total : 0.0);
    std::printf("  fnv checksum  : 0x%016lx\n", checksum);

    if (std::FILE* fp = std::fopen("/tmp/capture-spike-crossgpu.ppm", "wb"); fp != nullptr) {
        std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
            const std::uint8_t rgb[3] = {pixels[i + 2], pixels[i + 1], pixels[i]};
            std::fwrite(rgb, 1, 3, fp);
        }
        std::fclose(fp);
        std::printf("  wrote         : /tmp/capture-spike-crossgpu.ppm\n");
    }

    munkicast::rhi::vk_destroy_exportable(src_ctx, transfer);
    const bool pass = nonzero > 0;
    std::printf("  CROSS-GPU: %s\n", pass ? "PASS" : "FAIL - imported but every pixel is black");
    return pass;
}

/// The ext-image-copy-capture-v1 path, end to end: enumerate outputs, open a session, intersect
/// modifiers, allocate the buffer ourselves, let the compositor render into it, read it back.
///
/// No picker and no portal, so unlike the PipeWire path this can be run as many times as needed.
int run_wayland_backend(const std::string& output_name, bool with_cursor, bool want_cursor_set,
                        const std::string& prefer_drm_node, std::uint32_t frames,
                        const std::string& second_node, std::uint32_t scale_w,
                        std::uint32_t scale_h, std::uint32_t slots, std::uint32_t idle_timeout_ms) {
    std::string err;
    munkicast::capture::WlCapPtr cap = munkicast::capture::wlcap_open(err);
    if (!cap) {
        lc::error("ext-image-copy-capture unavailable: {}", err);
        return 2;
    }

    std::printf("\n=== outputs (no picker needed) ===\n");
    for (const auto& o : munkicast::capture::wlcap_outputs(*cap))
        std::printf("  %-10s %5dx%-5d @ %.3f Hz  %s\n", o.name.c_str(), o.width, o.height,
                    o.refresh_mhz / 1000.0, o.description.c_str());

    munkicast::capture::WlCapCaps caps;
    if (!munkicast::capture::wlcap_begin_session(*cap, output_name, want_cursor_set && with_cursor,
                                              caps, err)) {
        lc::error("session failed: {}", err);
        return 3;
    }

    std::printf("\n=== what the COMPOSITOR itself offers ===\n");
    std::printf("  buffer size   : %ux%u\n", caps.width, caps.height);
    std::printf("  dmabuf device : %s\n",
                munkicast::capture::wlcap_device_str(caps.dmabuf_device).c_str());
    for (const auto& f : caps.formats) {
        std::printf("  format %s (%zu modifiers)\n",
                    munkicast::spike::fourcc_str(f.drm_format).c_str(), f.modifiers.size());
        for (std::uint64_t m : f.modifiers)
            std::printf("      %s\n", munkicast::spike::modifier_str(m).c_str());
    }

    munkicast::rhi::VkCtxPtr vk = munkicast::rhi::vk_init(prefer_drm_node, err);
    if (!vk) {
        lc::error("vulkan init failed: {}", err);
        return 4;
    }
    lc::info("vulkan device: {}", munkicast::rhi::vk_device_name(*vk));

    // Pick the first offered format our device can also use, and intersect the modifier sets.
    // Because *we* allocate, this intersection is the whole negotiation - there is no second round
    // and no way for the compositor to accept a modifier it cannot then write to.
    for (const auto& f : caps.formats) {
        const std::vector<std::uint64_t> ours = munkicast::rhi::vk_supported_modifiers(
            *vk, f.drm_format);
        std::vector<std::uint64_t> common;
        for (std::uint64_t m : f.modifiers)
            if (std::find(ours.begin(), ours.end(), m) != ours.end())
                common.push_back(m);

        std::printf("\n=== %s: %zu common modifier(s) ===\n",
                    munkicast::spike::fourcc_str(f.drm_format).c_str(), common.size());
        for (std::uint64_t m : common)
            std::printf("  %s\n", munkicast::spike::modifier_str(m).c_str());
        if (common.empty())
            continue;

        // A ring of buffers, not one. With a single destination the compositor is serialized
        // against us - it cannot draw frame N+1 while we hold its only target - so a one-buffer
        // measurement both understates the rate and invents a latency tail.
        std::vector<munkicast::rhi::VkExportedImage> ring(slots);
        bool alloc_ok = true;
        for (std::uint32_t si = 0; si < slots; ++si) {
            if (!munkicast::rhi::vk_create_exportable(*vk, caps.width, caps.height, f.drm_format,
                                                       common, false, ring[si], err)) {
                lc::warn("could not allocate exportable image {} for {}: {}", si,
                         munkicast::spike::fourcc_str(f.drm_format), err);
                alloc_ok = false;
                break;
            }
            if (!munkicast::capture::wlcap_add_slot(*cap, ring[si], f.drm_format, err)) {
                lc::warn("could not register buffer slot {}: {}", si, err);
                alloc_ok = false;
                break;
            }
        }
        if (!alloc_ok) {
            for (auto& r : ring)
                munkicast::rhi::vk_destroy_exportable(*vk, r);
            continue;
        }
        munkicast::rhi::VkExportedImage& img = ring.front();
        lc::info("allocated {} slot(s) of {}x{} {} modifier {} planes {} stride {}, fd {}", slots,
                 img.width,
                 img.height, munkicast::spike::fourcc_str(f.drm_format),
                 munkicast::spike::modifier_str(img.modifier), img.n_planes, img.strides[0],
                 img.dmabuf_fd);

        std::uint32_t captured = 0;
        std::vector<std::uint8_t> pixels;
        std::vector<double> frame_ms;   // wall time between delivered frames
        std::vector<double> present_ms; // compositor-reported time between the frames themselves
        frame_ms.reserve(frames);
        std::uint64_t prev_present = 0;
        const auto t0 = std::chrono::steady_clock::now();
        auto prev = t0;
        for (std::uint32_t i = 0; i < frames; ++i) {
            std::uint64_t presented = 0;
            bool idle = false;
            if (!munkicast::capture::wlcap_capture_slot(*cap, i % slots, caps.width, caps.height,
                                                     &presented, err, idle_timeout_ms, &idle)) {
                if (idle) {
                    // Not a capture fault, so it must not fall into the paths below: a failure
                    // with no frames moves on to the next format, which would blame the format for
                    // the screen being still. Stop here, say why, and exit with a code of its own
                    // so a script can tell "nothing moved" from "capture is broken".
                    lc::error("gave up after {} of {} frame(s): {}. --idle-timeout 0 waits for "
                              "ever",
                              captured, frames, err);
                    for (auto& r : ring)
                        munkicast::rhi::vk_destroy_exportable(*vk, r);
                    return 7;
                }
                lc::error("capture failed on frame {}: {}", i, err);
                break;
            }
            ++captured;
            const auto now = std::chrono::steady_clock::now();
            frame_ms.push_back(std::chrono::duration<double, std::milli>(now - prev).count());
            prev = now;
            if (presented != 0 && prev_present != 0 && presented > prev_present)
                present_ms.push_back(static_cast<double>(presented - prev_present) / 1e6);
            if (presented != 0)
                prev_present = presented;
        }
        const double total_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (captured == 0) {
            for (auto& r : ring)
                munkicast::rhi::vk_destroy_exportable(*vk, r);
            continue;
        }

        munkicast::rhi::VkExportedImage& last = ring[(captured - 1) % slots];
        if (!munkicast::rhi::vk_readback_exported(*vk, last, true, pixels, err)) {
            lc::error("readback failed: {}", err);
            for (auto& r : ring)
                munkicast::rhi::vk_destroy_exportable(*vk, r);
            return 5;
        }

        std::uint32_t nonzero = 0;
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
            checksum = checksum * 1099511628211ULL ^ pixels[i];
            if ((pixels[i] | pixels[i + 1] | pixels[i + 2]) != 0)
                ++nonzero;
        }
        const std::uint32_t total = last.width * last.height;

        // Report the distribution, not just the mean: a capture path that averages 60 fps but
        // stalls for 40 ms once a second drops frames on a live stream, and an average hides that
        // completely.
        std::printf("\n=== result ===\n");
        std::printf("  frames        : %u captured in %.1f ms\n", captured, total_ms);
        if (!frame_ms.empty()) {
            std::vector<double> sorted = frame_ms;
            std::sort(sorted.begin(), sorted.end());
            const double mean = total_ms / static_cast<double>(frame_ms.size());
            std::printf("  per frame     : mean %.2f ms (%.1f fps)  min %.2f  p50 %.2f  p99 %.2f  "
                        "max %.2f\n",
                        mean, mean > 0 ? 1000.0 / mean : 0.0, sorted.front(),
                        sorted[sorted.size() / 2], sorted[(sorted.size() * 99) / 100],
                        sorted.back());
            if (!present_ms.empty()) {
                std::vector<double> ps = present_ms;
                std::sort(ps.begin(), ps.end());
                double sum = 0;
                for (double v : ps)
                    sum += v;
                const double pmean = sum / static_cast<double>(ps.size());
                std::printf("  presented gap : mean %.2f ms (%.1f fps)  p50 %.2f  p99 %.2f\n",
                            pmean, pmean > 0 ? 1000.0 / pmean : 0.0, ps[ps.size() / 2],
                            ps[(ps.size() * 99) / 100]);
                std::printf("      ^ this is how often the SCREEN CHANGED. Inter-arrival time\n");
                std::printf("        above cannot be faster than this, so on a still desktop it\n");
                std::printf("        measures the content, not the capture path.\n");
            }
            const double budget = 1000.0 / 60.0;
            const std::size_t over = static_cast<std::size_t>(
                std::count_if(sorted.begin(), sorted.end(),
                              [budget](double v) { return v > budget; }));
            std::printf("  over 60fps budget (%.2f ms): %zu / %zu frames\n", budget, over,
                        sorted.size());
        }
        std::printf("  modifier      : %s\n", munkicast::spike::modifier_str(img.modifier).c_str());
        std::printf("  readback      : %zu bytes (%ux%u)\n", pixels.size(), last.width, last.height);
        std::printf("  non-black px  : %u / %u (%.1f%%)\n", nonzero, total,
                    total > 0 ? 100.0 * nonzero / total : 0.0);
        std::printf("  fnv checksum  : 0x%016lx\n", checksum);

        if (std::FILE* fp = std::fopen("/tmp/capture-spike-frame.ppm", "wb"); fp != nullptr) {
            std::fprintf(fp, "P6\n%u %u\n255\n", last.width, last.height);
            for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                const std::uint8_t rgb[3] = {pixels[i + 2], pixels[i + 1], pixels[i]};
                std::fwrite(rgb, 1, 3, fp);
            }
            std::fclose(fp);
            std::printf("  wrote         : /tmp/capture-spike-frame.ppm\n");
        }

        // ---- ADR-0007's cross-GPU transfer, measured rather than assumed.
        bool cross_ok = true;
        if (!second_node.empty()) {
            cross_ok = run_cross_gpu(*vk, last, f.drm_format, second_node, scale_w, scale_h);
        }

        for (auto& r : ring)
            munkicast::rhi::vk_destroy_exportable(*vk, r);
        const bool pass = nonzero > 0 && cross_ok;
        std::printf("\n  VERDICT: %s\n\n",
                    pass ? "PASS - compositor rendered into our Vulkan image, real pixel content"
                         : "FAIL - capture succeeded but every pixel is black");
        return pass ? 0 : 5;
    }

    lc::error("no offered format had a modifier this device can use");
    return 5;
}

} // namespace

int main(int argc, char** argv) {
    lc::init_from_env();
    // Logs go to stderr and results to stdout; without this the two interleave out of order and
    // the output reads as though the error preceded its own diagnostics.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    int stage = 3; // raised as later stages land
    bool shm_only = false;
    std::vector<std::uint64_t> explicit_mods;
    bool dump_formats = false;
    bool mirror = false;
    bool sweep = false;
    bool no_restore = false;
    bool no_probe = false;
    bool use_wayland = false;
    bool cursor_set = false;
    std::string wl_output;
    std::string second_node;
    std::uint32_t wl_frames = 3;
    std::uint32_t wl_slots = 4;
    // Ten seconds is far beyond any frame interval a moving screen produces - even a 1 Hz clock
    // redraws ten times over - so on a busy output this never fires, and on a still one the run
    // explains itself before anyone reaches for kill.
    std::uint32_t idle_timeout_ms = 10'000;
    munkicast::spike::V4l2Config v4l2cfg;
    bool use_v4l2 = false;
    std::uint32_t scale_w = 0;
    std::uint32_t scale_h = 0;
    bool explicit_token = false;
    std::string prefer_drm_node;
    munkicast::spike::PortalRequest req;
    // Monitors only unless --window is given: mixing monitor and window sources made earlier
    // measurements non-comparable.
    req.source_types = static_cast<std::uint32_t>(munkicast::spike::SourceType::Monitor);

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--stage" && i + 1 < args.size()) {
            stage = std::atoi(std::string(args[++i]).c_str());
        } else if (a == "--window") {
            req.source_types = static_cast<std::uint32_t>(munkicast::spike::SourceType::Window);
        } else if (a == "--no-cursor") {
            req.cursor_mode = munkicast::spike::CursorMode::Hidden;
            cursor_set = true;
        } else if (a == "--modifiers" && i + 1 < args.size()) {
            // Comma-separated, hex (0x..) or decimal. Lets us probe what a compositor accepts
            // without a rebuild.
            const std::string list(args[++i]);
            std::size_t pos = 0;
            while (pos <= list.size()) {
                const std::size_t comma = list.find(',', pos);
                const std::string tok = list.substr(pos, comma == std::string::npos
                                                            ? std::string::npos
                                                            : comma - pos);
                if (!tok.empty())
                    explicit_mods.push_back(std::strtoull(tok.c_str(), nullptr, 0));
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
        } else if (a == "--drm-node" && i + 1 < args.size()) {
            prefer_drm_node = std::string(args[++i]);
        } else if (a == "--sweep") {
            sweep = true;
        } else if (a == "--mirror") {
            mirror = true;
        } else if (a == "--dump-formats") {
            dump_formats = true;
        } else if (a == "--shm-only") {
            shm_only = true;
        } else if (a == "--restore-token" && i + 1 < args.size()) {
            req.restore_token = std::string(args[++i]);
            explicit_token = true;
        } else if (a == "--no-restore") {
            no_restore = true;
        } else if (a == "--no-probe") {
            no_probe = true;
        } else if (a == "--wayland" || a == "--wl") {
            use_wayland = true;
        } else if (a == "--second-gpu" && i + 1 < args.size()) {
            second_node = std::string(args[++i]);
            use_wayland = true;
        } else if (a == "--v4l2" && i + 1 < args.size()) {
            v4l2cfg.device = std::string(args[++i]);
            use_v4l2 = true;
        } else if (a == "--v4l2-size" && i + 1 < args.size()) {
            const std::string v(args[++i]);
            if (const std::size_t x = v.find('x'); x != std::string::npos) {
                v4l2cfg.width = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                v4l2cfg.height =
                    static_cast<std::uint32_t>(std::strtoul(v.c_str() + x + 1, nullptr, 10));
            }
        } else if (a == "--v4l2-format" && i + 1 < args.size()) {
            v4l2cfg.fourcc = munkicast::spike::v4l2_fourcc_parse(std::string(args[++i]));
        } else if (a == "--v4l2-fps" && i + 1 < args.size()) {
            v4l2cfg.fps_num =
                static_cast<std::uint32_t>(std::strtoul(std::string(args[++i]).c_str(), nullptr, 10));
        } else if (a == "--v4l2-range" && i + 1 < args.size()) {
            const std::string v(args[++i]);
            v4l2cfg.quantization =
                v == "full" ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
        } else if (a == "--v4l2-dmabuf") {
            v4l2cfg.export_dmabuf = true;
        } else if (a == "--slots" && i + 1 < args.size()) {
            wl_slots = static_cast<std::uint32_t>(
                std::strtoul(std::string(args[++i]).c_str(), nullptr, 10));
            if (wl_slots == 0)
                wl_slots = 1;
        } else if (a == "--idle-timeout" && i + 1 < args.size()) {
            const std::string raw{args[++i]};
            char* end = nullptr;
            const double secs = std::strtod(raw.c_str(), &end);
            // The end pointer is the point: strtod does not fail, it returns 0 and converts
            // nothing. Without this, `--idle-timeout nope` would read as 0 -- "wait for ever" --
            // and a typo would restore the exact hang this flag exists to end. The negated range
            // test also catches NaN and inf; the upper bound keeps the conversion to
            // milliseconds inside uint32 (about 49 days).
            if (end == raw.c_str() || *end != '\0' || !(secs >= 0.0 && secs <= 4'000'000.0)) {
                lc::error("--idle-timeout takes seconds, 0 or more (0 waits for ever), got '{}'",
                          raw);
                return 1;
            }
            // A positive value always means a timeout. Truncating 0.0004 s to 0 ms would turn
            // the smallest timeout anyone could type into "never", which is the one value it
            // cannot be allowed to mean.
            const double ms = secs * 1000.0;
            idle_timeout_ms = secs > 0.0 && ms < 1.0 ? 1u : static_cast<std::uint32_t>(ms);
        } else if (a == "--frames" && i + 1 < args.size()) {
            wl_frames = static_cast<std::uint32_t>(std::strtoul(std::string(args[++i]).c_str(),
                                                                nullptr, 10));
        } else if (a == "--scale" && i + 1 < args.size()) {
            const std::string v(args[++i]);
            if (const std::size_t x = v.find('x'); x != std::string::npos) {
                scale_w = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                scale_h = static_cast<std::uint32_t>(std::strtoul(v.c_str() + x + 1, nullptr, 10));
            }
        } else if (a == "--output" && i + 1 < args.size()) {
            wl_output = std::string(args[++i]);
            use_wayland = true;
        } else if (a == "--forget") {
            forget_cached_token();
            lc::info("cached restore token removed ({})", token_cache_path());
            return 0;
        } else {
            lc::error("unknown argument '{}' (try --help)", a);
            return 1;
        }
    }

    if (use_v4l2) {
        v4l2cfg.frames = wl_frames > 3 ? wl_frames : 120;
        return run_v4l2(v4l2cfg);
    }

    // The Wayland backend is a different path end to end, so it returns rather than falling
    // through into the portal flow (ADR-0011).
    if (use_wayland)
        return run_wayland_backend(wl_output, req.cursor_mode == munkicast::spike::CursorMode::Embedded,
                                   cursor_set, prefer_drm_node, wl_frames, second_node, scale_w, scale_h,
                                   wl_slots, idle_timeout_ms);

    // -------------------------------------------------------- stage 0: Vulkan, before any click
    // Bring Vulkan up first so negotiation can advertise the modifiers this device can really
    // import, and - just as important - so a Vulkan failure happens *before* the user is asked to
    // click a picker rather than after.
    munkicast::rhi::VkCtxPtr vk;
    std::vector<std::uint64_t> vk_mods;
    if (stage >= 3 || sweep) {
        std::string verr;
        vk = munkicast::rhi::vk_init(prefer_drm_node, verr);
        if (!vk) {
            lc::error("vulkan init failed: {}", verr);
            return 4;
        }
        lc::info("vulkan device: {}", munkicast::rhi::vk_device_name(*vk));
        // ARGB8888 is what a BGRA SPA format maps to.
        vk_mods = munkicast::rhi::vk_supported_modifiers(*vk, 0x34325241 /* DRM_FORMAT_ARGB8888 */);
        std::printf("\n=== modifiers this device can import for ARGB8888 (%zu) ===\n",
                    vk_mods.size());
        for (std::uint64_t m : vk_mods)
            std::printf("  %s\n", munkicast::spike::modifier_str(m).c_str());
        std::printf("\n");
    }

    // ---------------------------------------------------------------- stage 1: portal
    lc::info("stage 1: negotiating portal ScreenCast session");
    if (req.restore_token.empty() && !no_restore) {
        req.restore_token = load_cached_token();
        if (!req.restore_token.empty())
            lc::info("using cached restore token from {}", token_cache_path());
    } else if (no_restore && !explicit_token) {
        req.restore_token.clear();
    }

    munkicast::spike::StreamConfig sc;
    sc.max_frames = 30;
    // Stage 2 advertises a broad list to learn what the compositor actually offers. Stage 3
    // replaces this with the set Vulkan reports as importable for the chosen format.
    // What this device can import, before intersecting with any particular source.
    std::vector<std::uint64_t> importable;
    if (!shm_only) {
        if (!explicit_mods.empty())
            importable = explicit_mods;
        else if (!vk_mods.empty())
            importable = vk_mods; // the device's real capability
        else
            importable = {munkicast::spike::kModifierInvalid, munkicast::spike::kModifierLinear};
    }

    struct Stats {
        std::uint32_t dmabuf = 0;
        std::uint32_t shm = 0;
        bool printed_detail = false;
        std::uint32_t imported = 0;
        std::uint32_t import_failed = 0;
        std::string first_import_error;
        std::uint64_t checksum = 0;
        std::uint32_t nonzero_pixels = 0;
        std::vector<std::uint8_t> first_image;
        std::uint32_t img_w = 0;
        std::uint32_t img_h = 0;
    };
    Stats stats;

    sc.on_frame = [&stats](const munkicast::spike::CapturedFrame& f) {
        if (f.is_dmabuf) ++stats.dmabuf; else ++stats.shm;
        if (!stats.printed_detail) {
            stats.printed_detail = true;
            std::printf("\n=== first frame ===\n");
            std::printf("  %ux%u  drm_format=%s  modifier=%s\n", f.width, f.height,
                        munkicast::spike::fourcc_str(f.drm_format).c_str(),
                        munkicast::spike::modifier_str(f.modifier).c_str());
            std::printf("  buffer type   : %s\n", f.is_dmabuf ? "DmaBuf" : "shm (MemPtr/MemFd)");
            std::printf("  planes        : %zu\n", f.planes.size());
            for (std::size_t i = 0; i < f.planes.size(); ++i) {
                const auto& p = f.planes[i];
                std::printf("    [%zu] fd=%-4d offset=%-8u stride=%-8u maxsize=%ld\n", i, p.fd,
                            p.offset, p.stride, static_cast<long>(p.size));
            }
            std::printf("\n");
        }
    };

    // Chain Vulkan import onto the frame callback when stage 3 is active.
    if (stage >= 3 && vk) {
        auto base = sc.on_frame;
        munkicast::rhi::VkCtx* vkp = vk.get();
        sc.on_frame = [&stats, base, vkp](const munkicast::spike::CapturedFrame& f) {
            if (base)
                base(f);
            if (!f.is_dmabuf)
                return;
            // Import only the first few frames: this is a correctness proof, not a benchmark.
            if (stats.imported + stats.import_failed >= 3)
                return;
            std::vector<std::uint8_t> pixels;
            std::string ierr;
            if (!munkicast::rhi::vk_import_and_readback(*vkp, f, pixels, ierr)) {
                ++stats.import_failed;
                if (stats.first_import_error.empty())
                    stats.first_import_error = ierr;
                return;
            }
            ++stats.imported;
            if (stats.first_image.empty()) {
                stats.first_image = pixels;
                stats.img_w = f.width;
                stats.img_h = f.height;
                for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                    stats.checksum = stats.checksum * 1099511628211ULL ^ pixels[i];
                    if ((pixels[i] | pixels[i + 1] | pixels[i + 2]) != 0)
                        ++stats.nonzero_pixels;
                }
            }
        };
    }

    // The real capture attempt, used as the final validation step of acquisition: a session that
    // cannot deliver frames is not a session, whatever its node said when asked.
    munkicast::spike::NegotiatedFormat fmt;
    const auto stream_once = [&](Acquired& a, std::string& serr) {
        stats = Stats{};
        sc.node_id = a.session.streams.front().node_id;
        if (mirror) {
            sc.mirror_width = static_cast<std::uint32_t>(a.session.streams.front().width);
            sc.mirror_height = static_cast<std::uint32_t>(a.session.streams.front().height);
        }
        lc::info("stage 2: connecting PipeWire stream to node {}", sc.node_id);

        // Intersect against this source before connecting. An empty intersection is a real,
        // nameable condition - this device cannot import anything this source can produce - and
        // saying so here beats discovering it as "no more input formats" three callbacks later.
        if (!explicit_mods.empty() || shm_only) {
            sc.modifiers = importable; // diagnostic overrides are passed through untouched
        } else {
            sc.modifiers = order_candidates(importable, a.formats);
            if (sc.modifiers.empty() && !a.formats.offered.empty()) {
                std::string off;
                for (std::uint64_t m : a.formats.offered)
                    off += (off.empty() ? "" : " ") + munkicast::spike::modifier_str(m);
                std::string ours;
                for (std::uint64_t m : importable)
                    ours += (ours.empty() ? "" : " ") + munkicast::spike::modifier_str(m);
                serr = std::format("no common DRM format modifier: source offers [{}], this "
                                   "device can import [{}]", off, ours);
                lc::error("{}", serr);
                return false;
            }
            lc::info("candidates after intersecting with the source: {} (source prefers {})",
                     sc.modifiers.size(),
                     a.formats.preferred.empty()
                         ? std::string("nothing in particular")
                         : munkicast::spike::modifier_str(a.formats.preferred.front()));
        }

        // ADR-0007's fallback ladder, as an actual loop rather than a promise.
        //
        // A compositor can advertise a modifier it cannot then allocate with: negotiation
        // succeeds, the stream reaches `streaming`, and buffer allocation fails. Measured on this
        // box - Hyprland offers NVIDIA(0x...606010-015) for DP-1 but its own fixed preference is
        // 0x...606014, and fixating 0x...606015 reaches streaming and dies with no frames.
        //
        // So a failure is not final while candidates remain: drop the modifier that was actually
        // fixated and retry on the same session. The ladder ends at LINEAR and then at shm, which
        // is the degradation order the design calls for - a stream that degrades beats one that
        // dies. Each rung is logged, because silently taking a slower path is how performance
        // problems become unexplainable.
        std::vector<std::uint64_t> candidates = sc.modifiers;
        bool tried_implicit = false;
        for (int rung = 0;; ++rung) {
            sc.modifiers = candidates;
            // 8s is a no-progress watchdog, not a total budget: it is re-armed on every frame, so
            // a negotiation failure is still reported in seconds while stage 3's per-frame GPU
            // readback gets as long as it needs.
            if (munkicast::spike::pw_try_variant(*a.pw, sc, fmt, serr, 8000)) {
                if (rung > 0)
                    lc::warn("succeeded on fallback rung {} with modifier {}", rung,
                             fmt.have_modifier ? munkicast::spike::modifier_str(fmt.modifier)
                                               : std::string("none (shm)"));
                return true;
            }
            // Exhaust the explicit candidates first: only a modifier that was actually
            // fixated can be blamed, so drop that one and try the next.
            bool dropped = false;
            if (fmt.have_modifier) {
                const auto it = std::find(candidates.begin(), candidates.end(), fmt.modifier);
                if (it != candidates.end()) {
                    lc::warn("modifier {} negotiated but could not deliver frames ({}); "
                             "dropping it and retrying",
                             munkicast::spike::modifier_str(fmt.modifier), serr);
                    candidates.erase(it);
                    dropped = !candidates.empty();
                }
            }
            // Last rung: no modifier property at all - the implicit/shm path. This is the
            // bottom of ADR-0007's ladder and the one configuration proven to deliver frames on
            // this machine. It is NOT "{INVALID, LINEAR}": INVALID is the absence of a modifier,
            // and advertising the sentinel as though it were one is what produced a stream that
            // reached `streaming` and then starved.
            if (!dropped) {
                if (tried_implicit || shm_only || !explicit_mods.empty())
                    break;
                tried_implicit = true;
                candidates.clear();
                lc::warn("explicit modifiers exhausted; falling back to the implicit/shm path "
                         "(no modifier property) - zero-copy is given up, the stream survives");
            }
            stats = Stats{};
        }
        lc::error("stream failed: {}", serr);
        // Always show what the source was actually offering. Without this the failure is just a
        // string, and the one question that matters - did our advertised modifiers intersect
        // theirs at all? - needs a whole extra session to answer.
        std::printf("\n=== we advertised %zu modifier(s) ===\n", sc.modifiers.size());
        for (std::uint64_t m : sc.modifiers)
            std::printf("  %s\n", munkicast::spike::modifier_str(m).c_str());
        munkicast::spike::pw_dump_formats(*a.pw, sc.node_id);
        return false;
    };

    Acquired acq;
    std::string error;
    // stage 1, --dump-formats and --sweep deliberately pass no verifier: they are diagnostics that
    // must report what they find rather than retry around it.
    const bool stream_verifies = stage >= 2 && !sweep && !dump_formats;
    std::function<bool(Acquired&, std::string&)> verifier;
    if (stream_verifies)
        verifier = stream_once;
    if (!acquire_capture_session(req, acq, verifier, !no_probe, !no_restore, error)) {
        lc::error("portal negotiation failed: {}", error);
        return 2;
    }
    munkicast::spike::PortalSession& session = acq.session;

    const SessionCloser closer{&session.session_handle};
    g_session_handle = session.session_handle;
    std::signal(SIGTERM, on_fatal_signal);
    std::signal(SIGINT, on_fatal_signal);
    std::signal(SIGHUP, on_fatal_signal);

    std::printf("\n=== portal session ===\n");
    std::printf("  source        : %s\n", acq.restored ? "restored session (no picker)"
                                                        : "freshly picked");
    std::printf("  streams       : %zu\n", session.streams.size());
    if (!session.streams.empty())
        lc::info("CAPTURE SOURCE: {} ({}) {}x{} node {}",
                 session.streams.front().mapping_id.empty()
                     ? std::string("(no mapping_id)")
                     : session.streams.front().mapping_id,
                 source_type_name(session.streams.front().source_type),
                 session.streams.front().width, session.streams.front().height,
                 session.streams.front().node_id);
    for (const auto& s : session.streams) {
        std::printf("    node %u  %dx%d  at (%d,%d)  type=%s%s%s\n", s.node_id, s.width, s.height,
                    s.pos_x, s.pos_y, source_type_name(s.source_type),
                    s.mapping_id.empty() ? "" : "  mapping_id=", s.mapping_id.c_str());
    }
    std::printf("  node          : VALIDATED (exists and advertises formats)\n");
    if (!session.restore_token.empty())
        std::printf("  restore token : %s\n", session.restore_token.c_str());
    std::printf("\n");

    if (stage <= 1) {
        lc::info("stage 1 complete");
        return 0;
    }

    if (dump_formats) {
        munkicast::spike::pw_dump_formats(*acq.pw, session.streams.front().node_id);
        return 0;
    }

    if (sweep) {
        struct Variant {
            const char* label;
            std::vector<std::uint64_t> mods;
        };
        // Known-good first: a failed negotiation may poison the session for later streams on the
        // same core, so ordering is part of the experiment.
        std::vector<Variant> variants{
            {"INVALID + LINEAR (known good)", {munkicast::spike::kModifierInvalid,
                                               munkicast::spike::kModifierLinear}},
        };
        if (!vk_mods.empty()) {
            std::vector<std::uint64_t> with_invalid = vk_mods;
            with_invalid.insert(with_invalid.begin(), munkicast::spike::kModifierInvalid);
            variants.push_back({"INVALID + Vulkan list", with_invalid});
            variants.push_back({"Vulkan-supported list", vk_mods});
        }
        variants.push_back({"INVALID", {munkicast::spike::kModifierInvalid}});
        variants.push_back({"LINEAR", {munkicast::spike::kModifierLinear}});
        variants.push_back({"shm only (no modifier prop)", {}});

        std::printf("=== sweep: %zu variants against ONE session (node %u, %s) ===\n\n",
                    variants.size(), session.streams.front().node_id,
                    session.streams.front().mapping_id.c_str());

        // One PipeWire connection for the whole sweep: pw_init/pw_deinit are not reliably
        // re-entrant and the portal fd is single-use, so dup()-ing per attempt does not work.
        // The connection was opened and its node validated during acquisition.
        munkicast::spike::PwSession* pws = acq.pw.get();

        bool dumped_on_failure = false;
        for (const Variant& v : variants) {
            munkicast::spike::StreamConfig vc;
            vc.node_id = session.streams.front().node_id;
            vc.max_frames = 5;
            vc.modifiers = v.mods;

            std::uint32_t dma = 0;
            std::uint32_t shm = 0;
            vc.on_frame = [&dma, &shm](const munkicast::spike::CapturedFrame& f) {
                if (f.is_dmabuf) ++dma; else ++shm;
            };

            munkicast::spike::NegotiatedFormat vf;
            std::string verr2;
            const bool ok = munkicast::spike::pw_try_variant(*pws, vc, vf, verr2);

            std::printf("  %-26s : ", v.label);
            if (!ok) {
                std::printf("FAIL  (%s)\n", verr2.c_str());
                if (!dumped_on_failure) {
                    dumped_on_failure = true;
                    std::fflush(stdout);
                    munkicast::spike::pw_dump_formats(*pws, vc.node_id);
                }
            } else {
                std::printf("OK  %ux%u %s mod=%-34s %u dmabuf %u shm\n", vf.width, vf.height,
                            munkicast::spike::fourcc_str(vf.drm_format).c_str(),
                            vf.have_modifier
                                ? munkicast::spike::modifier_str(vf.modifier).c_str()
                                : "none",
                            dma, shm);
            }
            std::fflush(stdout);
        }
        std::printf("\n");
        return 0;
    }

    // ---------------------------------------------------------------- stage 2: result
    std::printf("=== stage 2 result ===\n");
    std::printf("  frames        : %u dmabuf, %u shm\n", stats.dmabuf, stats.shm);
    std::printf("  negotiated    : %ux%u  drm=%s  modifier=%s\n", fmt.width, fmt.height,
                munkicast::spike::fourcc_str(fmt.drm_format).c_str(),
                fmt.have_modifier ? munkicast::spike::modifier_str(fmt.modifier).c_str()
                                  : "none (shm)");
    std::printf("  zero-copy path: %s\n\n",
                stats.dmabuf > 0 ? "AVAILABLE (dmabuf frames received)"
                                 : "NOT TAKEN (shm fallback only)");

    if (stage <= 2) {
        lc::info("stage 2 complete");
        return 0;
    }

    // ---------------------------------------------------------------- stage 3: Vulkan import
    std::printf("=== stage 3 result: Vulkan dmabuf import ===\n");
    std::printf("  device        : %s\n", munkicast::rhi::vk_device_name(*vk).c_str());
    std::printf("  imported      : %u ok, %u failed\n", stats.imported, stats.import_failed);
    if (!stats.first_import_error.empty())
        std::printf("  first error   : %s\n", stats.first_import_error.c_str());
    if (stats.imported > 0) {
        const std::uint32_t total = stats.img_w * stats.img_h;
        std::printf("  readback      : %zu bytes (%ux%u)\n", stats.first_image.size(),
                    stats.img_w, stats.img_h);
        std::printf("  non-black px  : %u / %u (%.1f%%)\n", stats.nonzero_pixels, total,
                    total > 0 ? 100.0 * stats.nonzero_pixels / total : 0.0);
        std::printf("  fnv checksum  : 0x%016lx\n", stats.checksum);

        // Write a PPM so the frame can be eyeballed - the only honest proof of correctness.
        if (const char* out_path = "/tmp/capture-spike-frame.ppm"; stats.img_w > 0) {
            if (std::FILE* fp = std::fopen(out_path, "wb"); fp != nullptr) {
                std::fprintf(fp, "P6\n%u %u\n255\n", stats.img_w, stats.img_h);
                for (std::size_t i = 0; i + 3 < stats.first_image.size(); i += 4) {
                    // readback is B,G,R,A in memory for VK_FORMAT_B8G8R8A8_UNORM
                    const std::uint8_t rgb[3] = {stats.first_image[i + 2],
                                                 stats.first_image[i + 1],
                                                 stats.first_image[i]};
                    std::fwrite(rgb, 1, 3, fp);
                }
                std::fclose(fp);
                std::printf("  wrote         : %s\n", out_path);
            }
        }
    }
    std::printf("\n");

    const bool pass = stats.imported > 0 && stats.nonzero_pixels > 0;
    std::printf("  VERDICT: %s\n\n",
                pass ? "PASS - dmabuf imported into Vulkan with real pixel content"
                     : "FAIL - see errors above");
    return pass ? 0 : 5;
}

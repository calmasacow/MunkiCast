// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Phase-0 go/no-go gate: capture -> colour convert -> encode -> mux, with synced audio,
// producing a playable file on this machine.
//
// This is the bottom rung of ADR-0006's ladder on purpose. It reads frames back to host memory and
// converts on the CPU, which is the slow path - and measuring exactly how slow is the point, since
// that cost is what the Vulkan tier exists to remove. A gate that only passes on the fast path
// proves nothing about what happens when the fast path is unavailable.

#include "audio.hpp"
#include "lencode/encoder.hpp"

#include <vk_convert.hpp>
#include <lrhi/vulkan.hpp>
#include <lcapture/wlcap.hpp>

#include <lcore/log.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

void usage() {
    std::printf(R"(encode-spike - the Phase-0 gate: capture -> convert -> encode -> mux

usage:
  --out FILE           output file (default /tmp/munkicast-gate.mp4)
  --output NAME        which output to capture (default: the first)
  --frames N           capture and encode N frames (default 300)
  --fps N              declared output rate (default 60)
  --bitrate KBPS       video bitrate in kbps (default 12000)
  --tier NAME          pin an encoder tier: h264_nvenc | libx264
  --convert gpu|cpu    where BGRA->NV12 happens (default gpu; cpu is the old swscale path)
  --range limited|full output range, applied in our shader - never detected (default limited)
  --no-audio           video only - useful to isolate, but NOT the gate
  --audio-source       capture a microphone rather than the desktop's output
  --drm-node MAJ:MIN   select the Vulkan device by DRM node, never by index
  --slots N            capture buffer ring size (default 4)
  --idle-timeout SECS  how long with no new capture before saying so (default 10; 0 = never).
                       Before the first frame this is fatal; after it, encoding carries on
)");
}

void report(const char* label, std::vector<double> v) {
    if (v.empty())
        return;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v)
        sum += x;
    const double mean = sum / static_cast<double>(v.size());
    std::printf("  %-16s mean %7.3f ms   p50 %7.3f   p99 %7.3f   max %7.3f\n", label, mean,
                v[v.size() / 2], v[(v.size() * 99) / 100], v.back());
}

} // namespace

int main(int argc, char** argv) {
    lc::init_from_env();

    std::string out_path = "/tmp/munkicast-gate.mp4";
    std::string output_name;
    std::string drm_node;
    std::string tier;
    std::uint32_t frames = 300;
    std::uint32_t slots = 4;
    // The same ten seconds the first-frame wait always used, now one setting. It is far beyond
    // any gap a moving screen leaves, so a busy run never sees it.
    std::uint32_t idle_timeout_ms = 10'000;
    std::int32_t fps = 60;
    std::int64_t bitrate_kbps = 12000;
    bool want_audio = true;
    bool capture_sink = true;
    // GPU convert is the default now: ADR-0019 measured the CPU path at 4.351 ms a frame plus
    // 2.7x the PCIe traffic, for work a shader does for free. --convert cpu keeps the old path
    // so the two stay comparable.
    bool gpu_convert = true;
    munkicast::spike::ColorCfg color{};

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--out" && i + 1 < args.size()) {
            out_path = std::string(args[++i]);
        } else if (a == "--output" && i + 1 < args.size()) {
            output_name = std::string(args[++i]);
        } else if (a == "--drm-node" && i + 1 < args.size()) {
            drm_node = std::string(args[++i]);
        } else if (a == "--tier" && i + 1 < args.size()) {
            tier = std::string(args[++i]);
        } else if (a == "--frames" && i + 1 < args.size()) {
            frames = static_cast<std::uint32_t>(std::strtoul(std::string(args[++i]).c_str(), nullptr, 10));
        } else if (a == "--slots" && i + 1 < args.size()) {
            slots = static_cast<std::uint32_t>(std::strtoul(std::string(args[++i]).c_str(), nullptr, 10));
        } else if (a == "--idle-timeout" && i + 1 < args.size()) {
            const std::string raw{args[++i]};
            char* end = nullptr;
            const double secs = std::strtod(raw.c_str(), &end);
            // strtod returns 0 for text it cannot read, so without the end pointer a typo would
            // mean "never time out" -- the hang this flag exists to end. The range test also
            // catches NaN and inf and keeps milliseconds inside uint32.
            if (end == raw.c_str() || *end != '\0' || !(secs >= 0.0 && secs <= 4'000'000.0)) {
                lc::error("--idle-timeout takes seconds, 0 or more (0 = never), got '{}'", raw);
                return 2;
            }
            // A positive value always means a timeout. Truncating 0.0004 s to 0 ms would turn
            // the smallest timeout anyone could type into "never", which is the one value it
            // cannot be allowed to mean.
            const double ms = secs * 1000.0;
            idle_timeout_ms = secs > 0.0 && ms < 1.0 ? 1u : static_cast<std::uint32_t>(ms);
        } else if (a == "--fps" && i + 1 < args.size()) {
            fps = static_cast<std::int32_t>(std::strtol(std::string(args[++i]).c_str(), nullptr, 10));
        } else if (a == "--bitrate" && i + 1 < args.size()) {
            bitrate_kbps = std::strtoll(std::string(args[++i]).c_str(), nullptr, 10);
        } else if (a == "--convert" && i + 1 < args.size()) {
            const std::string_view v = args[++i];
            gpu_convert = (v == "gpu");
            if (v != "gpu" && v != "cpu") {
                lc::error("--convert takes 'gpu' or 'cpu', got '{}'", v);
                return 2;
            }
        } else if (a == "--range" && i + 1 < args.size()) {
            const std::string_view v = args[++i];
            color.full_range = (v == "full");
            if (v != "full" && v != "limited") {
                lc::error("--range takes 'full' or 'limited', got '{}'", v);
                return 2;
            }
        } else if (a == "--no-audio") {
            want_audio = false;
        } else if (a == "--audio-source") {
            capture_sink = false;
        } else {
            lc::error("unknown argument: {}", a);
            usage();
            return 1;
        }
    }

    std::string err;

    // --- capture (ADR-0011: ext-image-copy-capture-v1, no portal, no picker) ---
    munkicast::capture::WlCapPtr cap = munkicast::capture::wlcap_open(err);
    if (!cap) {
        lc::error("ext-image-copy-capture unavailable: {}", err);
        return 2;
    }
    munkicast::capture::WlCapCaps caps;
    if (!munkicast::capture::wlcap_begin_session(*cap, output_name, false, caps, err)) {
        lc::error("capture session failed: {}", err);
        return 2;
    }

    munkicast::rhi::VkCtxPtr vk = munkicast::rhi::vk_init(drm_node, err);
    if (!vk) {
        lc::error("vulkan init failed: {}", err);
        return 2;
    }
    lc::info("vulkan device: {}", munkicast::rhi::vk_device_name(*vk));
    lc::info("capture: {}x{} from {}", caps.width, caps.height,
             output_name.empty() ? "the first output" : output_name.c_str());

    // Allocate the ring from the intersection of what the compositor offers and what our device
    // can use. Because we allocate, this intersection IS the negotiation (ADR-0011).
    std::vector<munkicast::rhi::VkExportedImage> ring;
    std::uint32_t chosen_format = 0;
    for (const auto& f : caps.formats) {
        const std::vector<std::uint64_t> ours = munkicast::rhi::vk_supported_modifiers(*vk, f.drm_format);
        std::vector<std::uint64_t> common;
        for (std::uint64_t m : f.modifiers)
            if (std::find(ours.begin(), ours.end(), m) != ours.end())
                common.push_back(m);
        if (common.empty())
            continue;

        ring.assign(slots, {});
        bool ok = true;
        for (std::uint32_t s = 0; s < slots; ++s) {
            if (!munkicast::rhi::vk_create_exportable(*vk, caps.width, caps.height, f.drm_format,
                                                       common, false, ring[s], err) ||
                !munkicast::capture::wlcap_add_slot(*cap, ring[s], f.drm_format, err)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            chosen_format = f.drm_format;
            break;
        }
        for (auto& r : ring)
            munkicast::rhi::vk_destroy_exportable(*vk, r);
        ring.clear();
    }
    if (chosen_format == 0) {
        lc::error("no format/modifier the compositor and this GPU both accept");
        return 2;
    }

    // --- audio ---
    munkicast::spike::AudioPtr audio;
    if (want_audio) {
        munkicast::spike::AudioConfig acfg;
        acfg.capture_sink = capture_sink;
        audio = munkicast::spike::audio_open(acfg, err);
        if (!audio) {
            lc::error("audio capture failed: {}", err);
            return 2;
        }
    }

    // --- encoder ---
    munkicast::encode::RuntimeConfig ecfg;
    ecfg.path = out_path;
    ecfg.width = caps.width;
    ecfg.height = caps.height;
    ecfg.fps_num = fps;
    ecfg.bitrate = bitrate_kbps * 1000;
    ecfg.force_tier = tier;
    ecfg.audio_channels = want_audio ? 2 : 0;
    munkicast::encode::EncoderPtr enc = munkicast::encode::encoder_open(ecfg, err);
    if (!enc) {
        lc::error("encoder open failed: {}", err);
        return 2;
    }

    // ---- the encoder's clock ----
    //
    // ADR-0013: capture is damage-driven, so frames arrive when the screen changes and at no
    // other time. A loop that encodes one frame per captured frame therefore produces a file whose
    // rate is set by how busy the desktop happened to be - and on a still desktop it simply stops.
    // That was the first version of this loop and it hung, which is the ADR being right.
    //
    // So capture runs free on its own thread, publishing the newest frame it has, and the encoder
    // ticks at the configured output rate, taking whatever is current. When nothing changed it
    // re-encodes the last frame, which is exactly what a streaming encoder must do: the viewer is
    // owed a constant frame rate whether or not the screen moved.

    struct Latest {
        std::mutex mu;
        std::vector<std::uint8_t> pixels;
        munkicast::spike::Nv12Frame nv12;
        std::uint64_t presented_ns = 0;
        std::uint64_t seq = 0;
        bool stop = false;
        std::string err;
    } latest;

    std::vector<double> capture_ms;
    std::vector<double> readback_ms;
    std::vector<double> gpu_convert_ms;
    /// The whole producer iteration, against which the named stages are a breakdown.
    std::vector<double> capture_loop_ms;
    /// The whole consumer iteration, excluding the deliberate wait for the frame deadline.
    std::vector<double> encode_loop_ms;
    double first_frame_ms = 0.0;
    // A stall that cannot be attributed cannot be fixed, so the worst frame keeps its breakdown
    // rather than just its total (ADR-0019's unexplained 61 ms).
    struct Worst {
        double total = 0.0;
        munkicast::spike::ConvertTiming t{};
        std::uint64_t frame = 0;
    } worst;
    std::uint32_t over_5ms = 0;
    std::uint32_t over_budget = 0;
    std::mutex stats_mu;

    std::thread capture_thread([&] {
        std::vector<std::uint8_t> local;
        munkicast::spike::Nv12Frame local_nv12;
        munkicast::spike::ConvertTiming ct{};
        for (std::uint64_t i = 0;; ++i) {
            {
                std::lock_guard<std::mutex> lk(latest.mu);
                if (latest.stop)
                    return;
            }
            const std::size_t slot = static_cast<std::size_t>(i % ring.size());
            std::uint64_t presented_ns = 0;
            std::string e;

            // Wall clock over the whole iteration, so that what the named stages do not account
            // for shows up as a gap instead of vanishing.
            //
            // Three separate times during this spike's life the expensive thing turned out to be
            // outside the instrumentation: swscale outside the "encode+convert" timer, the
            // readback hidden behind a two-sample mean, and two full-frame buffer copies sitting
            // between the stage timers. Each quoted figure was true and the total it implied was
            // not. Bracketing named stages does not measure a pipeline; wall clock over the loop
            // does, and the difference is the part nobody thought to time.
            const auto iteration_begin = std::chrono::steady_clock::now();
            auto t = iteration_begin;
            if (!munkicast::capture::wlcap_capture_slot(*cap, slot, caps.width, caps.height,
                                                     &presented_ns, e)) {
                std::lock_guard<std::mutex> lk(latest.mu);
                latest.err = e;
                return;
            }
            const double cms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();

            t = std::chrono::steady_clock::now();
            if (gpu_convert) {
                // Convert on the GPU and read back NV12: 1.5 bytes/pixel instead of 4, and no
                // CPU colour conversion at all (ADR-0019).
                if (!munkicast::spike::vk_convert_nv12(*vk, ring[slot], true, color, local_nv12,
                                                      e, &ct)) {
                    std::lock_guard<std::mutex> lk(latest.mu);
                    latest.err = e;
                    return;
                }
            } else if (!munkicast::rhi::vk_readback_exported(*vk, ring[slot], true, local, e)) {
                std::lock_guard<std::mutex> lk(latest.mu);
                latest.err = e;
                return;
            }
            const double rms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();

            // The first frame pays one-time costs in both paths - the staging allocation, and
            // for the GPU path the compute pipeline build - so it is excluded from the per-frame
            // figures rather than left to drag the mean somewhere unrepresentative. It is
            // reported on its own line instead of being quietly dropped.
            if (i == 0) {
                std::lock_guard<std::mutex> lk(stats_mu);
                first_frame_ms = cms + rms;
            } else {
                std::lock_guard<std::mutex> lk(stats_mu);
                capture_ms.push_back(cms);
                if (gpu_convert) {
                    gpu_convert_ms.push_back(rms);
                    if (rms > worst.total) {
                        worst.total = rms;
                        worst.t = ct;
                        worst.frame = i;
                    }
                    if (rms > 5.0)
                        ++over_5ms;
                    if (rms > 16.667)
                        ++over_budget;
                } else {
                    readback_ms.push_back(rms);
                }
            }
            {
                std::lock_guard<std::mutex> lk(latest.mu);
                // Swap, never assign. These are 5.27 MiB (NV12) or 14.06 MiB (BGRA) vectors,
                // and assigning them copies the lot. Swapping hands the filled buffer over and
                // takes the stale one back to fill next time, so the buffers rotate between the
                // two threads instead of being duplicated. Four copies per frame become two.
                if (gpu_convert)
                    std::swap(latest.nv12, local_nv12);
                else
                    std::swap(latest.pixels, local);
                latest.presented_ns = presented_ns;
                ++latest.seq;
            }

            if (i != 0) {
                const double iteration_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - iteration_begin)
                        .count();
                std::lock_guard<std::mutex> lk(stats_mu);
                capture_loop_ms.push_back(iteration_ms);
            }
        }
    });

    // Wait for the first real frame; without one there is nothing to duplicate from.
    std::vector<std::uint8_t> pixels;
    munkicast::spike::Nv12Frame nv12;
    std::uint64_t last_seq = 0;
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(idle_timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(latest.mu);
                if (latest.seq > 0) {
                    if (gpu_convert)
                        std::swap(nv12, latest.nv12);
                    else
                        std::swap(pixels, latest.pixels);
                    last_seq = latest.seq;
                    break;
                }
                if (!latest.err.empty()) {
                    lc::error("capture thread failed: {}", latest.err);
                    { std::lock_guard<std::mutex> l2(latest.mu); latest.stop = true; }
                    capture_thread.join();
                    return 3;
                }
            }
            if (idle_timeout_ms != 0 && std::chrono::steady_clock::now() > deadline) {
                lc::error("gave up with 0 frames captured: {}",
                          munkicast::capture::wlcap_idle_reason(idle_timeout_ms));
                { std::lock_guard<std::mutex> lk(latest.mu); latest.stop = true; }
                capture_thread.detach();
                enc.reset();
                std::fflush(nullptr);
                std::_Exit(3); // see the note at the end of main
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Discard everything audio captured up to this instant. The two streams must share one
    // origin, and the origin is the first encoder tick - not encoder_open, because the wait for
    // the first captured frame sits between the two and audio keeps running through it. Draining
    // earlier leaves exactly that gap of pre-roll in the file, labelled as concurrent with t=0,
    // which plays as a fixed A/V offset that looks like drift.
    if (audio) {
        std::vector<float> junk;
        std::uint32_t jn = 0;
        munkicast::spike::audio_drain(*audio, junk, jn);
    }

    std::uint64_t audio_frames_fed = 0;
    std::uint32_t duplicated = 0;

    // Dry spells are noticed HERE, by the encoder seeing no new capture, and not in the capture
    // thread by putting a timeout on its wait and asking again. That was the first version, and
    // measured on argus it invents frames: once a pending ext-image-copy-capture frame is
    // destroyed, Hyprland answers the next request at once with unchanged content. A 1 s timeout
    // on a screen that really redrew every ~5 s reported a capture every 1002 ms - each one the
    // timeout plus 2 ms - and quietly filled the capture timings with the timeout's length. The
    // capture thread is therefore left exactly as it was; only the watcher is new.
    std::uint32_t dry_spells = 0;
    bool in_dry_spell = false;
    const auto run_begin = std::chrono::steady_clock::now();
    auto last_fresh = run_begin; // the first frame was taken an instant ago
    const std::int64_t tick_ns = 1'000'000'000LL * ecfg.fps_den / ecfg.fps_num;

    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto deadline = run_begin + std::chrono::nanoseconds(tick_ns * i);
        std::this_thread::sleep_until(deadline);

        // After the sleep: waiting for the frame deadline is the point of the loop, not work.
        const auto iteration_begin = std::chrono::steady_clock::now();
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lk(latest.mu);
            if (!latest.err.empty()) {
                lc::warn("capture ended early: {}", latest.err);
                break;
            }
            if (latest.seq != last_seq) {
                // Swap rather than copy: see the note on the producer side.
                if (gpu_convert)
                    std::swap(nv12, latest.nv12);
                else
                    std::swap(pixels, latest.pixels);
                last_seq = latest.seq;
                fresh = true;
            }
        }
        if (!fresh)
            ++duplicated;
        // One line when captures stop and one when they come back, rather than one per timeout,
        // so a long still stretch reads as a single event. It is a warning and not an exit: the
        // encoder duplicating the last frame is correct (ADR-0013) and the file stays valid.
        if (fresh) {
            if (in_dry_spell)
                lc::info("captures resumed after {:.1f} s",
                         std::chrono::duration<double>(iteration_begin - last_fresh).count());
            in_dry_spell = false;
            last_fresh = iteration_begin;
        } else if (idle_timeout_ms != 0 && !in_dry_spell &&
                   iteration_begin - last_fresh >= std::chrono::milliseconds(idle_timeout_ms)) {
            in_dry_spell = true;
            ++dry_spells;
            lc::warn("captures have dried up after {} frame(s): {}. Encoding carries on, "
                     "duplicating the last frame",
                     last_seq, munkicast::capture::wlcap_idle_reason(idle_timeout_ms));
        }

        // The pts is the ENCODER's clock, not the compositor's. The output timeline is ours and
        // is constant-rate; the compositor's presentation time only tells us whether the content
        // is new.
        const std::int64_t pts_ns = tick_ns * i;

        const bool sent =
            gpu_convert
                ? munkicast::encode::encoder_video_nv12(
                      *enc, nv12.data.data(), static_cast<int>(nv12.y_stride),
                      nv12.data.data() + nv12.uv_offset, static_cast<int>(nv12.uv_stride), pts_ns,
                      err)
                : munkicast::encode::encoder_video(*enc, pixels.data(),
                                                 static_cast<std::size_t>(caps.width) * 4, pts_ns,
                                                 err);
        if (!sent) {
            lc::error("video encode failed at frame {}: {}", i, err);
            break;
        }

        if (audio) {
            std::vector<float> samples;
            std::uint32_t n = 0;
            munkicast::spike::audio_drain(*audio, samples, n);
            if (n > 0) {
                if (!munkicast::encode::encoder_audio(*enc, samples.data(), n, pts_ns, err)) {
                    lc::error("audio encode failed: {}", err);
                    break;
                }
                audio_frames_fed += n;
            }
        }

        encode_loop_ms.push_back(std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - iteration_begin)
                                     .count());
    }

    { std::lock_guard<std::mutex> lk(latest.mu); latest.stop = true; }
    // The capture thread may be parked inside a blocking roundtrip waiting for damage that will
    // never come, so it is detached rather than joined. A real worker gets a wakeup pipe; a spike
    // that is about to exit does not need one.
    capture_thread.detach();

    // Trailing audio, so the tail is not cut short of the video.
    if (audio) {
        std::vector<float> samples;
        std::uint32_t n = 0;
        munkicast::spike::audio_drain(*audio, samples, n);
        if (n > 0 && munkicast::encode::encoder_audio(*enc, samples.data(), n, 0, err))
            audio_frames_fed += n;
    }

    if (!munkicast::encode::encoder_finish(*enc, err)) {
        lc::error("finish failed: {}", err);
        return 3;
    }
    const double wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - run_begin).count();

    const auto& st = munkicast::encode::encoder_stats(*enc);
    std::printf("\n=== Phase-0 gate: %s ===\n", out_path.c_str());
    std::printf("  video         : %s, %u frames, %.2f MiB\n", st.video_encoder.c_str(),
                static_cast<unsigned>(st.video_frames),
                static_cast<double>(st.video_bytes) / (1024.0 * 1024.0));
    if (audio) {
        std::printf("  audio         : %s (%s), %u packets, %.2f MiB, %llu sample-frames fed\n",
                    st.audio_encoder.c_str(), munkicast::spike::audio_format_str(*audio).c_str(),
                    static_cast<unsigned>(st.audio_frames),
                    static_cast<double>(st.audio_bytes) / (1024.0 * 1024.0),
                    static_cast<unsigned long long>(audio_frames_fed));
        const double audio_s = static_cast<double>(audio_frames_fed) / 48000.0;
        std::printf("  audio span    : %.3f s of audio for %.3f s of wall time (%+.1f ms)\n",
                    audio_s, wall_s, (audio_s - wall_s) * 1000.0);
    }
    std::printf("  duplicated    : %u of %u output frames had no new capture (%.1f%%) - the screen did "
                "not change; re-encoding the last frame is correct, not a fault\n",
                duplicated, static_cast<unsigned>(st.video_frames),
                st.video_frames > 0 ? 100.0 * duplicated / static_cast<double>(st.video_frames) : 0.0);
    if (dry_spells > 0)
        std::printf("  dry spells    : %u stretch(es) of %.1f s or more with no capture at all - "
                    "the capture figures below are not a measurement of a busy screen\n",
                    dry_spells, static_cast<double>(idle_timeout_ms) / 1000.0);
    std::printf("  wall          : %.2f s for %u frames (%.2f fps)\n", wall_s,
                static_cast<unsigned>(st.video_frames),
                wall_s > 0 ? static_cast<double>(st.video_frames) / wall_s : 0.0);
    std::printf("\n  per-stage cost (sw path; the Vulkan tier removes readback and convert)\n");
    {
        std::lock_guard<std::mutex> lk(stats_mu);
        std::printf("  captures delivered by the compositor: %zu (+1 excluded: first frame "
                    "pays one-time setup, %.3f ms)\n",
                    capture_ms.size(), first_frame_ms);
        std::printf("  %s\n", gpu_convert ? munkicast::spike::vk_convert_note(*vk).c_str()
                                          : munkicast::rhi::vk_readback_note(*vk).c_str());
        report("capture", capture_ms);
        report("readback", readback_ms);
        report("convert+readback", gpu_convert_ms);
        if (gpu_convert && !gpu_convert_ms.empty()) {
            std::printf("  tail          : %u over 5 ms, %u over the 16.667 ms budget, of %zu\n",
                        over_5ms, over_budget, gpu_convert_ms.size());
            std::printf("  worst convert : frame %llu, %.3f ms = record %.3f + submit %.3f + "
                        "gpu-wait %.3f + copy %.3f\n",
                        static_cast<unsigned long long>(worst.frame), worst.total,
                        worst.t.record_ms, worst.t.submit_ms, worst.t.wait_ms, worst.t.copy_ms);
        }
    }
    report("bgra->nv12 (cpu)", st.convert_ms);
    report("nv12 plane copy", st.copy_ms);
    report("encode", st.video_encode_ms);

    // Each loop's wall clock is the known total; the named stages are a breakdown *within* it,
    // and whatever they do not account for is printed rather than left to be inferred from a sum
    // that was never checked. Three times in this spike's life the expensive thing lived in that
    // gap, so it gets a line of its own.
    const auto mean_of = [](const std::vector<double>& v) {
        if (v.empty())
            return 0.0;
        double sum = 0.0;
        for (const double x : v)
            sum += x;
        return sum / static_cast<double>(v.size());
    };
    const auto budget = [&](const char* name, const std::vector<double>& total,
                            std::initializer_list<const std::vector<double>*> parts) {
        if (total.empty())
            return;
        const double whole = mean_of(total);
        double named = 0.0;
        for (const std::vector<double>* p : parts)
            named += mean_of(*p);
        const double gap = whole - named;
        std::printf("  %-22s wall %7.3f ms = named %7.3f + unaccounted %7.3f (%.0f%%)  n=%zu\n",
                    name, whole, named, gap, whole > 0.0 ? 100.0 * gap / whole : 0.0,
                    total.size());
    };

    std::printf("\n  --- where the time actually goes ---\n");
    budget("capture loop", capture_loop_ms,
           {&capture_ms, gpu_convert ? &gpu_convert_ms : &readback_ms});
    budget("encode loop", encode_loop_ms,
           {&st.convert_ms, &st.copy_ms, &st.video_encode_ms});

    // The capture thread is detached and may be parked inside a blocking roundtrip waiting for
    // damage that will never come, so there is no point at which it is safe to destroy the
    // Vulkan context, the capture connection, the image ring or `latest` - it is still reading
    // all four. Returning normally ran those destructors underneath it and dumped core at exit.
    //
    // So the encoder is closed explicitly, because that is what flushes and closes the output
    // file, and then the process exits without unwinding. The kernel reclaims the rest. A real
    // worker gets a wakeup pipe and joins its thread properly; a spike that is one instruction
    // from exiting does not need one, but it does need to not crash.
    enc.reset();
    std::fflush(nullptr);
    std::_Exit(0);
}

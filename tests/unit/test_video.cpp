// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <lcapture/video.hpp>
#include <lcapture/video_buffer.hpp>
#include "../../libs/lcapture/src/video_signal.hpp"
#include "ltest.hpp"
#include <cstdint>
#include <optional>
#include <vector>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sys/ioctl.h>
#include <unistd.h>

LTEST_CASE(automatic_capture_reports_unavailable_input_timing_without_guessing_a_rate) {
    const char* device = std::getenv("MUNKICAST_TEST_SIGNAL_DEVICE");
    if (!device) return;
    const int fd = ::open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    REQUIRE(fd >= 0);
    v4l2_dv_timings timing{};
    const bool available = ::ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &timing) == 0;
    ::close(fd);
    if (available) return; // Devices exposing timing exercise the negotiation path instead.
    std::string error;
    auto capture = munkicast::capture::VideoCapture::open(device, "Match incoming signal",
        "Match incoming signal", "Default", error);
    CHECK(!capture);
    CHECK(error.starts_with("Incoming signal timing unavailable"));
}

LTEST_CASE(incoming_signal_rate_uses_full_frame_timing_and_rejects_invalid_signals) {
    using namespace munkicast::capture::detail;
    v4l2_dv_timings timing{};
    timing.type = V4L2_DV_BT_656_1120;
    timing.bt.width = 1920;
    timing.bt.height = 1080;
    timing.bt.hfrontporch = 88;
    timing.bt.hsync = 44;
    timing.bt.hbackporch = 148;
    timing.bt.vfrontporch = 4;
    timing.bt.vsync = 5;
    timing.bt.vbackporch = 36;
    timing.bt.pixelclock = 148500000;
    auto signal = signal_mode(timing);
    REQUIRE(signal);
    CHECK_EQ(signal->width, 1920);
    CHECK_EQ(signal->height, 1080);
    CHECK_EQ(signal->fps(), 60.0);
    CHECK(matches_signal_rate(*signal, 60, 1));
    CHECK(!matches_signal_rate(*signal, 30, 1));
    CHECK(!matches_signal_rate(*signal, 0, 1));
    CHECK(!matches_signal_rate(*signal, 60, 0));
    timing.bt.pixelclock = 148351648;
    signal = signal_mode(timing);
    REQUIRE(signal);
    CHECK_NEAR(signal->fps(), 60000.0 / 1001, 0.000001);
    CHECK(matches_signal_rate(*signal, 60000, 1001));
    CHECK(matches_signal_rate(SignalMode{1920, 1080, 12017, 100}, 120, 1));
    CHECK(!matches_signal_rate(SignalMode{1920, 1080, 12017, 100}, 60, 1));
    timing.bt.interlaced = 1;
    CHECK(!signal_mode(timing));
    timing.bt.interlaced = 0;
    timing.bt.pixelclock = 0;
    CHECK(!signal_mode(timing));
    timing.bt.pixelclock = 148500000;
    timing.bt.hfrontporch = 0xffffffff;
    CHECK(!signal_mode(timing));
    timing.type = 0xffffffff;
    CHECK(!signal_mode(timing));
}

LTEST_CASE(hdr_signals_are_classified_rather_than_refused) {
    using namespace munkicast::capture::detail;
    // Capture used to fail outright on PQ or BT.2020 (ADR-0071). It now classifies the signal,
    // and the three cases need telling apart: HLG has no transfer code of its own, so it cannot
    // be distinguished from wide-gamut SDR by `colorspace` alone.
    const auto plain = signal_colour(V4L2_COLORSPACE_REC709, V4L2_XFER_FUNC_709);
    CHECK(!plain.hdr());
    CHECK(!plain.wide);

    const auto pq = signal_colour(V4L2_COLORSPACE_BT2020, V4L2_XFER_FUNC_SMPTE2084);
    CHECK(pq.hdr());
    CHECK(pq.pq);
    CHECK(!pq.hlg);
    CHECK(pq.wide); // needs the 2020 matrix as well as being HDR

    // PQ is PQ even when the card does not say BT.2020 alongside it.
    const auto bare_pq = signal_colour(V4L2_COLORSPACE_REC709, V4L2_XFER_FUNC_SMPTE2084);
    CHECK(bare_pq.hdr());
    CHECK(bare_pq.pq);

    // BT.2020 with no stated curve is how HLG arrives.
    const auto hlg = signal_colour(V4L2_COLORSPACE_BT2020, V4L2_XFER_FUNC_NONE);
    CHECK(hlg.hdr());
    CHECK(hlg.hlg);
    CHECK(!hlg.pq);

    // Wide primaries with an ordinary curve is SDR, and must not be called HDR -- it is the
    // case that would otherwise be tone-mapped for no reason.
    const auto wide_sdr = signal_colour(V4L2_COLORSPACE_BT2020, V4L2_XFER_FUNC_709);
    CHECK(!wide_sdr.hdr());
    CHECK(wide_sdr.wide);

    // A card stating both is PQ, not both: the log would otherwise name the wrong curve.
    const auto both = signal_colour(V4L2_COLORSPACE_BT2020, V4L2_XFER_FUNC_SMPTE2084);
    CHECK(both.pq);
    CHECK(!both.hlg);
}

LTEST_CASE(unavailable_video_is_an_error_not_an_invented_frame) {
    std::string error;
    CHECK(!munkicast::capture::VideoCapture::open("/dev/video999999", "", "", "", error));
    CHECK(!error.empty());
}

LTEST_CASE(video_options_reject_invalid_timeouts_and_nondevices) {
    for (int frames : {0, 1, 121}) {
        std::string error;
        munkicast::capture::VideoOptions options;
        options.timeout_frames = frames;
        CHECK(!munkicast::capture::VideoCapture::open("/dev/video999999", "", "", "", error, options));
        CHECK(error.find("2 to 120") != std::string::npos);
    }
    CHECK(munkicast::capture::list_video_formats("/etc/passwd").empty());
}

LTEST_CASE(device_formats_and_both_buffering_modes_produce_real_frames) {
    const char* device = std::getenv("MUNKICAST_TEST_VIDEO");
    if (!device) return;
    const auto formats = munkicast::capture::list_video_formats(device);
    REQUIRE(!formats.empty());
    for (const auto& format : formats) {
        for (bool buffering : {true, false}) {
            munkicast::capture::VideoOptions options;
            options.format = format.code;
            options.buffering = buffering;
            options.auto_reset = true;
            // Elgato offers these formats at 1080p30; higher modes can exceed USB bandwidth.
            options.timeout_frames = 120;
            std::string error;
            auto capture = munkicast::capture::VideoCapture::open(device, "1920x1080", "30", "Default", error, options);
            REQUIRE_MSG(capture != nullptr, "{} {}: {}", format.code, buffering, error);
            CHECK_EQ(capture->negotiated_fps(), 30.0);
            std::mutex mu;
            std::condition_variable changed;
            int frames = 0;
            capture->start([&](auto frame) {
                if (!frame) return;
                std::lock_guard lock(mu);
                if (frame->width == 1920 && frame->height == 1080 &&
                    frame->pixels.size() == std::size_t{1920 * 1080 * 4}) ++frames;
                changed.notify_all();
            });
            {
                std::unique_lock lock(mu);
                changed.wait_for(lock, std::chrono::seconds(6), [&] { return frames >= 3; });
            }
            capture->stop();
            CHECK_MSG(frames >= 3, "{} {}: {} decoded frames", format.code, buffering, frames);
        }
    }
}

LTEST_CASE(elgato_streams_1440p60_and_held_frames_are_immutable) {
    const char* device = std::getenv("MUNKICAST_TEST_VIDEO");
    if (!device) return;
    std::string error;
    auto capture = munkicast::capture::VideoCapture::open(device, "2560x1440", "60", "Default", error);
    REQUIRE_MSG(capture != nullptr, "{}", error);
    CHECK_EQ(capture->negotiated_fps(), 60.0);
    std::mutex mu;
    std::condition_variable changed;
    int count = 0;
    std::shared_ptr<const munkicast::capture::VideoFrame> held;
    std::vector<std::uint8_t> original;
    std::chrono::steady_clock::time_point first, last;
    capture->start([&](auto frame) {
        if (!frame) return;
        std::lock_guard lock(mu);
        if (!held) {
            held = frame;
            original = frame->pixels;
            first = std::chrono::steady_clock::now();
        }
        last = std::chrono::steady_clock::now();
        ++count;
        changed.notify_all();
    });
    {
        std::unique_lock lock(mu);
        changed.wait_for(lock, std::chrono::seconds(8), [&] { return count >= 180; });
    }
    capture->stop();
    REQUIRE(held != nullptr);
    CHECK_EQ(held->width, 2560);
    CHECK_EQ(held->height, 1440);
    CHECK_EQ(held->pixels.size(), std::size_t{2560 * 1440 * 4});
    CHECK(held->pixels == original);
    REQUIRE(count >= 180);
    const double fps = static_cast<double>(count - 1) / std::chrono::duration<double>(last - first).count();
    std::fprintf(stderr, "Elgato capture + BGRA conversion: %.2f fps (%d frames)\n", fps, count);
    CHECK(fps > 55.0);
}

LTEST_CASE(buffered_video_smooths_ordered_bursts_without_waiting) {
    using namespace std::chrono_literals;
    using munkicast::capture::VideoBuffer;
    using munkicast::capture::VideoFrame;
    VideoBuffer buffer(10ms);
    const auto start = VideoBuffer::Clock::time_point{} + 1s;
    constexpr int jitter[] = {0, 8, 2, 12, 4};
    int delivered = 0;
    for (int output = 0; output < 100; ++output) {
        const auto tick = start + output * 10ms + 21ms;
        while (start + delivered * 10ms + jitter[delivered % 5] * 1ms <= tick) {
            auto frame = std::make_shared<VideoFrame>();
            frame->sequence = static_cast<std::uint32_t>(delivered);
            buffer.push(frame, static_cast<std::uint64_t>(delivered + 1),
                        start + delivered * 10ms + jitter[delivered % 5] * 1ms);
            ++delivered;
        }
        CHECK(buffer.ready(tick));
        const auto sample = buffer.sample(tick);
        REQUIRE(sample.frame);
        CHECK_EQ(sample.frame->sequence, static_cast<std::uint32_t>(output));
        CHECK_EQ(buffer.sample(tick).frame, sample.frame);
        CHECK(!buffer.ready(tick));
    }
    CHECK_EQ(buffer.delay(), 20ms);
}

LTEST_CASE(buffered_video_bounds_leases_and_recovers_from_disconnect_reset_and_wrap) {
    using namespace std::chrono_literals;
    using munkicast::capture::VideoBuffer;
    using munkicast::capture::VideoFrame;
    VideoBuffer buffer(10ms);
    auto now = VideoBuffer::Clock::time_point{} + 1s;
    std::vector<std::weak_ptr<const VideoFrame>> leases;
    auto push = [&](std::uint32_t driver, std::uint64_t callback) {
        auto frame = std::make_shared<VideoFrame>();
        frame->sequence = driver;
        leases.push_back(frame);
        buffer.push(frame, callback, now);
        now += 10ms;
    };
    push(0xfffffffeu, 1);
    push(0xffffffffu, 2);
    push(0, 3);
    CHECK_EQ(buffer.sample(now + 1ms).sequence, 2u);
    for (std::uint32_t i = 1; i <= 100; ++i) push(i, i + 3);
    CHECK(std::count_if(leases.begin(), leases.end(), [](const auto& p) {
        return !p.expired();
    }) <= 5);
    push(0, 104); // Driver restarted.
    CHECK_EQ(buffer.sample(now + 20ms).sequence, 104u);
    buffer.push({}, 105, now);
    CHECK(!buffer.ready(now + 1s));
    CHECK(!buffer.sample(now + 1s).frame);
    CHECK(std::all_of(leases.begin(), leases.end(), [](const auto& p) { return p.expired(); }));
    now += 2s;
    push(0, 106);
    CHECK_EQ(buffer.sample(now + 20ms).sequence, 106u);
    now += 2s; // A long delivery stall reanchors instead of permanently running behind.
    push(1, 107);
    CHECK_EQ(buffer.sample(now + 20ms).sequence, 107u);
    buffer.clear();
    CHECK(!buffer.sample(now).frame);
    bool rejected = false;
    try { VideoBuffer invalid(0ns); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

LTEST_CASE(buffered_video_uses_actual_camera_clock_without_periodic_refills) {
    using namespace std::chrono_literals;
    using munkicast::capture::VideoBuffer;
    using munkicast::capture::VideoFrame;
    constexpr auto output_period = 16666666ns;
    constexpr auto camera_period = 16311900ns; // Measured Guermok rate despite nominal 60 fps.
    VideoBuffer buffer(output_period);
    const auto start = VideoBuffer::Clock::time_point{} + 1s;
    constexpr int jitter[] = {0, 8, 2};
    int delivered = 0, skipped = 0;
    std::uint64_t previous = 0;
    for (int output = 0; output < 1200; ++output) {
        const auto tick = start + 15ms + output * output_period + buffer.delay() + 1ms;
        while (start + delivered * camera_period + 15ms + jitter[delivered % 3] * 1ms <= tick) {
            auto frame = std::make_shared<VideoFrame>();
            frame->sequence = static_cast<std::uint32_t>(delivered);
            frame->captured_at = start + delivered * camera_period;
            buffer.push(frame, static_cast<std::uint64_t>(delivered + 1),
                        frame->captured_at + 15ms + jitter[delivered % 3] * 1ms);
            ++delivered;
        }
        const auto sample = buffer.sample(tick);
        REQUIRE(sample.frame);
        CHECK_MSG(sample.sequence > previous, "output {} repeated sequence {}", output, previous);
        if (previous) skipped += static_cast<int>(sample.sequence - previous - 1);
        previous = sample.sequence;
    }
    CHECK(skipped >= 20 && skipped <= 30); // Resample faster input; never manufacture a hold.
}

LTEST_CASE(buffered_video_does_not_alternate_holds_and_skips_at_timestamp_boundaries) {
    using namespace std::chrono_literals;
    using munkicast::capture::VideoBuffer;
    using munkicast::capture::VideoFrame;
    // Positive jitter on an even driver frame also exercises 120 -> 60 fps decimation.
    constexpr int jitter[] = {0, 30, 20, 15, -30, 5};
    const auto start = VideoBuffer::Clock::time_point{} + 10s;
    for (const auto period : {16666666ns, 8333333ns}) {
        for (const int stride : {1, 2, 3, 4}) {
            VideoBuffer buffer(period);
            int delivered = 0;
            for (int output = 0; output < 1200; ++output) {
                // The output crosses the timestamp threshold with two frames already buffered.
                const auto tick = start + 8ms + buffer.delay() + output * period * stride;
                while (start + delivered * period + jitter[delivered % 6] * 1us + 8ms <= tick) {
                    auto frame = std::make_shared<VideoFrame>();
                    frame->sequence = static_cast<std::uint32_t>(delivered);
                    frame->captured_at = start + delivered * period + jitter[delivered % 6] * 1us;
                    buffer.push(frame, static_cast<std::uint64_t>(delivered + 1),
                                frame->captured_at + 8ms);
                    ++delivered;
                }
                const auto sample = buffer.sample(tick);
                REQUIRE(sample.frame);
                REQUIRE_MSG(sample.frame->sequence == static_cast<std::uint32_t>(output * stride),
                            "stride {} tick {} selected driver {} despite buffered input", stride,
                            output, sample.frame->sequence);
                CHECK_EQ(buffer.sample(tick).frame, sample.frame);
            }
        }
    }
}

LTEST_CASE(buffered_video_hysteresis_still_resamples_real_clock_drift) {
    using namespace std::chrono_literals;
    using munkicast::capture::VideoBuffer;
    using munkicast::capture::VideoFrame;
    constexpr auto output_period = 16666666ns;
    constexpr int jitter[] = {0, 30, -20, 15, -30, 5};
    const auto start = VideoBuffer::Clock::time_point{} + 10s;
    for (const int stride : {1, 2}) {
        const auto period = output_period / stride;
        for (const auto drift : {-666ns / stride, 666ns / stride}) {
            VideoBuffer buffer(period);
            const auto camera_period = period + drift;
            int delivered = 0, repeated = 0, skipped = 0;
            std::uint64_t previous = 0;
            // Simulate 25 minutes without sleeping: clock drift must eventually resample.
            for (int output = 0; output < 90000; ++output) {
                const auto tick = start + 8ms + buffer.delay() + output * output_period;
                while (start + delivered * camera_period + jitter[delivered % 6] * 1us + 8ms <=
                       tick) {
                    auto frame = std::make_shared<VideoFrame>();
                    frame->sequence = static_cast<std::uint32_t>(delivered);
                    frame->captured_at =
                        start + delivered * camera_period + jitter[delivered % 6] * 1us;
                    buffer.push(frame, static_cast<std::uint64_t>(delivered + 1),
                                frame->captured_at + 8ms);
                    ++delivered;
                }
                const auto sample = buffer.sample(tick);
                REQUIRE(sample.frame);
                if (previous) {
                    const auto advance = static_cast<int>(sample.sequence - previous);
                    repeated += std::max(0, stride - advance);
                    skipped += std::max(0, advance - stride);
                    if (stride == 2)
                        CHECK(advance > 0); // 120 -> 60 drift may choose one/three, never hold.
                }
                previous = sample.sequence;
            }
            if (drift < 0ns) {
                CHECK_EQ(repeated, 0);
                CHECK(skipped >= 2 * stride && skipped <= 5 * stride);
            } else {
                CHECK_EQ(skipped, 0);
                CHECK(repeated >= 2 * stride && repeated <= 5 * stride);
            }
        }
    }
}

LTEST_CASE(buffered_video_rejects_invalid_driver_timestamps_without_flushing_good_frames) {
    using namespace std::chrono_literals;
    using munkicast::capture::VideoBuffer;
    using munkicast::capture::VideoFrame;
    VideoBuffer buffer(10ms);
    const auto start = VideoBuffer::Clock::time_point{} + 10s;
    int delivered = 0;
    for (int output = 0; output < 100; ++output) {
        const auto tick = start + output * 10ms + 21ms;
        while (start + delivered * 10ms <= tick) {
            auto frame = std::make_shared<VideoFrame>();
            frame->sequence = static_cast<std::uint32_t>(delivered);
            const auto arrival = start + delivered * 10ms;
            frame->captured_at = arrival;
            if (delivered % 17 == 3) frame->captured_at = arrival - 20ms; // Backward.
            if (delivered % 17 == 5) frame->captured_at = arrival + 1s; // Future.
            if (delivered % 17 == 7) frame->captured_at = arrival - 3s; // Stale.
            buffer.push(frame, static_cast<std::uint64_t>(delivered + 1), arrival);
            ++delivered;
        }
        const auto sample = buffer.sample(tick);
        REQUIRE(sample.frame);
        CHECK_EQ(sample.frame->sequence, static_cast<std::uint32_t>(output));
    }
}

// ---- HDR capture (issue #20) ----------------------------------------------------------------
//
// Measured against an Elgato 4K X fed a continuous HDR 10-bit loop, because the premise the
// earlier HDR work was built on does not hold for that card: it reports Colorspace sRGB and
// Transfer Function Rec.709 even on a PQ signal, so `signal_colour` sees plain SDR and every
// colorimetry-keyed branch is dead. The numbers below are from that card's own frames.

LTEST_CASE(pq_code_values_decode_to_the_luminances_the_standard_defines) {
    using munkicast::capture::detail::pq_nits;
    // ST 2084's own anchors.
    CHECK_NEAR(pq_nits(0.0), 0.0, 1e-9);
    CHECK_NEAR(pq_nits(1.0), 10000.0, 1.0);
    // 0.5 is a touch under SDR white, which is what makes 100 nits sit near the middle of the
    // range rather than at the top: there is room above it for highlights.
    CHECK(pq_nits(0.5) > 85.0);
    CHECK(pq_nits(0.5) < 100.0);
    // The measured frame: median luma 518 of 1023, peak 593. Read as PQ these are 98 and 201
    // nits -- ordinary picture content against a 100-nit SDR white, which is how the signal was
    // identified as genuinely PQ rather than mislabelled SDR.
    CHECK_NEAR(pq_nits(518.0 / 1023.0), 98.3, 1.0);
    CHECK_NEAR(pq_nits(593.0 / 1023.0), 201.0, 2.0);
}

LTEST_CASE(tone_mapping_pq_to_sdr_never_makes_a_brighter_input_darker) {
    using munkicast::capture::detail::pq_to_sdr;
    // The bug this guards against was written and measured: a linear ramp below reference with a
    // Reinhard shoulder above it is not monotonic. 100 nits came out at 0.999 and 201 nits at
    // 0.682, so on a real frame a highlight would have turned into a dark patch.
    double previous = -1.0;
    for (int step = 0; step <= 1023; ++step) {
        const double mapped = pq_to_sdr(static_cast<double>(step) / 1023.0);
        CHECK_MSG(mapped >= previous, "code {} fell from {} to {}", step, previous, mapped);
        CHECK(mapped >= 0.0 && mapped <= 1.0);
        previous = mapped;
    }
}

LTEST_CASE(tone_mapping_output_is_a_display_signal_not_linear_light) {
    using munkicast::capture::detail::linear_to_display;
    using munkicast::capture::detail::pq_to_sdr;
    CHECK_NEAR(pq_to_sdr(0.0), 0.0, 1e-9);
    // The OETF is the half that was missing at first, and the live feed is what caught it. With
    // the roll off alone the tone-mapped frame put 50% of its pixels in the bottom eighth of the
    // range, against 14.6% for the card's own 8-bit conversion of the same signal, and its mean
    // luma came out 50 where the card gave 72. The output is a display signal, so a 100-nit
    // input belongs near 0.7, not at the 0.5 that linear light would put it at.
    const double white = pq_to_sdr(518.0 / 1023.0);
    CHECK(white > 0.66 && white < 0.75);
    // The frame's own peak is brighter than white but short of clipping: it has somewhere to go.
    const double peak = pq_to_sdr(593.0 / 1023.0);
    CHECK(peak > white);
    CHECK(peak < 0.95);
    // Far above the assumed source peak everything lands on white, which is a roll off reaching
    // its end rather than content being thrown away.
    CHECK_NEAR(pq_to_sdr(0.95), 1.0, 0.01);
    // The curve's own anchors, so a change to it has to be deliberate rather than incidental.
    CHECK_NEAR(linear_to_display(0.0), 0.0, 1e-9);
    CHECK_NEAR(linear_to_display(1.0), 1.0, 1e-6);
    CHECK(linear_to_display(0.5) > 0.70 && linear_to_display(0.5) < 0.76);
}

LTEST_CASE(a_ten_bit_plane_is_told_from_eight_bit_values_padded_into_one) {
    using munkicast::capture::detail::plane_is_truly_ten_bit;
    // Why this exists: the signal's bit depth cannot be asked for. The 4K X answers
    // VIDIOC_QUERY_DV_TIMINGS with ENOTTY and enumerates P010 from a static USB descriptor at
    // every size whatever is plugged in, so only the data itself says what arrived.
    std::vector<std::uint16_t> padded(4096);
    for (std::size_t i = 0; i < padded.size(); ++i)
        // An 8-bit value widened into P010's layout: ten significant bits at the top, and the
        // low two of those ten always zero because the source had no such detail.
        padded[i] = static_cast<std::uint16_t>((i % 256) << 8);
    CHECK_EQ(plane_is_truly_ten_bit(padded.data(), padded.size()), std::optional<bool>(false));

    std::vector<std::uint16_t> real(4096);
    for (std::size_t i = 0; i < real.size(); ++i)
        real[i] = static_cast<std::uint16_t>((((i * 7) % 1024) & 0x3ffu) << 6);
    CHECK_EQ(plane_is_truly_ten_bit(real.data(), real.size()), std::optional<bool>(true));

    // Nothing to look at is no answer at all, rather than an answer of "8-bit".
    CHECK(!plane_is_truly_ten_bit(nullptr, 0).has_value());
    CHECK(!plane_is_truly_ten_bit(real.data(), 0).has_value());

    // The bug the live feed caught: this card sends two blank frames before the first real one,
    // and a black frame's low bits are zero at either depth. Deciding on it answered "8-bit" for
    // every signal, PQ included, so a flat frame must abstain and let the next one answer.
    const std::vector<std::uint16_t> black(4096, 0);
    CHECK(!plane_is_truly_ten_bit(black.data(), black.size()).has_value());
    const std::vector<std::uint16_t> flat_grey(4096, static_cast<std::uint16_t>(512u << 6));
    CHECK(!plane_is_truly_ten_bit(flat_grey.data(), flat_grey.size()).has_value());
}

LTEST_MAIN("video")

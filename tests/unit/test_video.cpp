// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <lcapture/video.hpp>
#include <lcapture/video_buffer.hpp>
#include "../../libs/lcapture/src/video_signal.hpp"
#include "ltest.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sys/ioctl.h>
#include <unistd.h>

LTEST_CASE(automatic_capture_reports_unavailable_input_timing_without_guessing_a_rate) {
    const char* device = std::getenv("LIBERTAS_TEST_SIGNAL_DEVICE");
    if (!device) return;
    const int fd = ::open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    REQUIRE(fd >= 0);
    v4l2_dv_timings timing{};
    const bool available = ::ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &timing) == 0;
    ::close(fd);
    if (available) return; // Devices exposing timing exercise the negotiation path instead.
    std::string error;
    auto capture = libertas::capture::VideoCapture::open(device, "Match incoming signal",
        "Match incoming signal", "Default", error);
    CHECK(!capture);
    CHECK(error.starts_with("Incoming signal timing unavailable"));
}

LTEST_CASE(incoming_signal_rate_uses_full_frame_timing_and_rejects_invalid_signals) {
    using namespace libertas::capture::detail;
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

LTEST_CASE(unavailable_video_is_an_error_not_an_invented_frame) {
    std::string error;
    CHECK(!libertas::capture::VideoCapture::open("/dev/video999999", "", "", "", error));
    CHECK(!error.empty());
}

LTEST_CASE(video_options_reject_invalid_timeouts_and_nondevices) {
    for (int frames : {0, 1, 121}) {
        std::string error;
        libertas::capture::VideoOptions options;
        options.timeout_frames = frames;
        CHECK(!libertas::capture::VideoCapture::open("/dev/video999999", "", "", "", error, options));
        CHECK(error.find("2 to 120") != std::string::npos);
    }
    CHECK(libertas::capture::list_video_formats("/etc/passwd").empty());
}

LTEST_CASE(device_formats_and_both_buffering_modes_produce_real_frames) {
    const char* device = std::getenv("LIBERTAS_TEST_VIDEO");
    if (!device) return;
    const auto formats = libertas::capture::list_video_formats(device);
    REQUIRE(!formats.empty());
    for (const auto& format : formats) {
        for (bool buffering : {true, false}) {
            libertas::capture::VideoOptions options;
            options.format = format.code;
            options.buffering = buffering;
            options.auto_reset = true;
            // Elgato offers these formats at 1080p30; higher modes can exceed USB bandwidth.
            options.timeout_frames = 120;
            std::string error;
            auto capture = libertas::capture::VideoCapture::open(device, "1920x1080", "30", "Default", error, options);
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
    const char* device = std::getenv("LIBERTAS_TEST_VIDEO");
    if (!device) return;
    std::string error;
    auto capture = libertas::capture::VideoCapture::open(device, "2560x1440", "60", "Default", error);
    REQUIRE_MSG(capture != nullptr, "{}", error);
    CHECK_EQ(capture->negotiated_fps(), 60.0);
    std::mutex mu;
    std::condition_variable changed;
    int count = 0;
    std::shared_ptr<const libertas::capture::VideoFrame> held;
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
    using libertas::capture::VideoBuffer;
    using libertas::capture::VideoFrame;
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
    using libertas::capture::VideoBuffer;
    using libertas::capture::VideoFrame;
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
    using libertas::capture::VideoBuffer;
    using libertas::capture::VideoFrame;
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
    using libertas::capture::VideoBuffer;
    using libertas::capture::VideoFrame;
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
    using libertas::capture::VideoBuffer;
    using libertas::capture::VideoFrame;
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
    using libertas::capture::VideoBuffer;
    using libertas::capture::VideoFrame;
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

LTEST_MAIN("video")

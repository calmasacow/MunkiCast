// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <lcapture/video.hpp>
#include "../../libs/lcapture/src/video_signal.hpp"
#include "ltest.hpp"
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

LTEST_MAIN("video")

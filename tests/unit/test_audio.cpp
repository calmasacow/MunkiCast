// SPDX-License-Identifier: GPL-3.0-or-later
#include "ltest.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <lcapture/audio.hpp>
#include <mutex>

LTEST_CASE(audio_captures_real_stereo_pcm) {
    const char* device = std::getenv("MUNKICAST_TEST_AUDIO");
    if (!device)
        return;
    std::mutex mu;
    std::condition_variable ready;
    std::size_t samples = 0;
    bool finite = true;
    std::string error;
    auto capture = munkicast::capture::AudioCapture::open(
        device, false,
        [&](auto data) {
            std::lock_guard lock(mu);
            samples += data.size();
            for (float sample : data)
                finite = finite && std::isfinite(sample);
            ready.notify_all();
        },
        error);
    REQUIRE_MSG(capture != nullptr, "{}", error);
    {
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(4), [&] { return samples >= 96000; });
    }
    capture.reset();
    CHECK(samples >= 96000);
    CHECK(finite);
    CHECK_EQ(samples % 2, std::size_t{0});
}
LTEST_MAIN("audio")

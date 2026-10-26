// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../apps/libertas-render/source_worker.hpp"
#include "ltest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fstream>
#include <lcomp/countdown.hpp>
#include <mutex>

namespace {
ldoc::Source source(const char* type) {
    ldoc::Source out;
    out.id = ldoc::Id::generate();
    out.type = type;
    out.name = "worker check";
    out.settings["width"] = std::int64_t{64};
    out.settings["height"] = std::int64_t{32};
    return out;
}

std::vector<lcomp::PreviewFrame> capture(const ldoc::Source& src, int count = 1) {
    std::mutex mu;
    std::condition_variable ready;
    std::vector<lcomp::PreviewFrame> frames;
    {
        SourceWorker worker(src, [&](auto frame) {
            std::lock_guard lock(mu);
            if (frame &&
                (frames.empty() || !std::ranges::equal(frames.back().pixels, frame->bytes()))) {
                frames.push_back(*frame);
                frames.back().pixels.assign(frame->bytes().begin(), frame->bytes().end());
                frames.back().mapped = {};
                frames.back().storage.reset();
                ready.notify_all();
            }
        });
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(8),
                       [&] { return static_cast<int>(frames.size()) >= count; });
    }
    return frames;
}
} // namespace

LTEST_CASE(text_worker_publishes_real_glyphs_with_transparency) {
    auto src = source("source.text");
    src.settings["text"] = std::string("Hi");
    src.settings["size"] = std::int64_t{22};
    auto frames = capture(src);
    REQUIRE(!frames.empty());
    CHECK_EQ(frames[0].width, 64);
    bool ink = false, clear = false;
    for (std::size_t i = 3; i < frames[0].pixels.size(); i += 4) {
        ink = ink || frames[0].pixels[i] > 0;
        clear = clear || frames[0].pixels[i] == 0;
    }
    CHECK(ink);
    CHECK(clear);
}

LTEST_CASE(concurrent_text_workers_draw_without_settings_changes) {
    std::mutex mu;
    std::condition_variable ready;
    std::array<bool, 16> drawn{};
    std::vector<std::unique_ptr<SourceWorker>> workers;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        auto src = source("source.text");
        src.settings["text"] = std::string("Hi");
        src.settings["font"] = std::string("Sans");
        src.settings["size"] = std::int64_t{22};
        workers.push_back(std::make_unique<SourceWorker>(src, [&, i](auto frame) {
            if (!frame)
                return;
            std::lock_guard lock(mu);
            for (std::size_t j = 3; j < frame->bytes().size(); j += 4)
                drawn[i] = drawn[i] || frame->bytes()[j] > 0;
            ready.notify_all();
        }));
    }
    std::unique_lock lock(mu);
    ready.wait_for(lock, std::chrono::seconds(8),
                   [&] { return std::ranges::all_of(drawn, [](bool value) { return value; }); });
    CHECK(std::ranges::all_of(drawn, [](bool value) { return value; }));
}

LTEST_CASE(text_worker_recovers_after_process_exit_without_settings_changes) {
    auto src = source("source.text");
    src.settings["text"] = std::string("Hi");
    src.settings["size"] = std::int64_t{22};
    std::mutex mu;
    std::condition_variable ready;
    int pictures = 0;
    SourceWorker worker(src, [&](auto frame) {
        if (!frame)
            return;
        std::lock_guard lock(mu);
        ++pictures;
        ready.notify_all();
    });
    {
        std::unique_lock lock(mu);
        REQUIRE(ready.wait_for(lock, std::chrono::seconds(8), [&] { return pictures > 0; }));
    }
    std::vector<pid_t> children;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream input(task.path() / "children");
        for (pid_t pid; input >> pid;)
            children.push_back(pid);
    }
    REQUIRE(children.size() == 1);
    REQUIRE(::kill(children.front(), SIGKILL) == 0);
    std::unique_lock lock(mu);
    CHECK(ready.wait_for(lock, std::chrono::seconds(10), [&] { return pictures > 1; }));
}

LTEST_CASE(slideshow_worker_advances_between_real_images) {
    auto src = source("source.slideshow");
    src.settings["files"] =
        std::string(LIBERTAS_IMAGE_FIXTURES) + "/two.png\n" + LIBERTAS_IMAGE_FIXTURES + "/red.jpg";
    src.settings["intervalMs"] = std::int64_t{100};
    auto frames = capture(src, 2);
    REQUIRE(frames.size() >= 2);
    CHECK(frames[0].pixels != frames[1].pixels);
}

LTEST_CASE(media_worker_decodes_video_in_its_process) {
    auto src = source("source.media");
    src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion.mkv";
    for (bool hardware : {false, true}) {
        src.settings["hardwareDecode"] = hardware;
        auto frames = capture(src, 2);
        REQUIRE(frames.size() >= 2);
        CHECK(frames[0].pixels != frames[1].pixels);
    }
}

LTEST_CASE(media_workers_preserve_webm_and_webp_alpha) {
    for (const auto* file : {"/alpha.webm", "/alpha.webp", "/animated-alpha.webp"}) {
        auto src = source("source.media");
        src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + file;
        const int count = std::string_view(file) == "/alpha.webp" ? 1 : 12;
        auto frames = capture(src, count);
        REQUIRE(static_cast<int>(frames.size()) >= count);
        const auto& pixels = frames.front().pixels;
        bool transparent = false, translucent = false;
        for (std::size_t i = 3; i < pixels.size(); i += 4) {
            transparent = transparent || pixels[i] == 0;
            translucent = translucent || (pixels[i] > 100 && pixels[i] < 160);
        }
        CHECK_MSG(transparent && translucent, "{} retains transparent and half-alpha pixels", file);
    }
}

LTEST_CASE(immutable_worker_frames_survive_later_frames_and_worker_shutdown) {
    auto src = source("source.media");
    src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-60.webm";
    std::mutex mu;
    std::condition_variable ready;
    std::shared_ptr<const lcomp::PreviewFrame> first;
    std::vector<std::uint8_t> snapshot;
    int count = 0;
    {
        SourceWorker worker(src, [&](auto frame) {
            if (!frame)
                return;
            std::lock_guard lock(mu);
            if (!first) {
                first = frame;
                snapshot.assign(frame->bytes().begin(), frame->bytes().end());
            }
            ++count;
            ready.notify_all();
        });
        std::unique_lock lock(mu);
        REQUIRE(ready.wait_for(lock, std::chrono::seconds(8), [&] { return count >= 8; }));
    }
    REQUIRE(first);
    CHECK(!first->mapped.empty());
    CHECK(first->storage != nullptr);
    CHECK(std::ranges::equal(first->bytes(), snapshot));
}

LTEST_CASE(alpha_video_delivers_smooth_frames_between_player_position_updates) {
    auto src = source("source.media");
    src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-60.webm";
    std::mutex mu;
    std::condition_variable ready;
    std::vector<std::uint8_t> previous;
    std::vector<std::chrono::steady_clock::time_point> times;
    SourceWorker worker(src, [&](auto frame) {
        if (!frame)
            return;
        std::lock_guard lock(mu);
        if (!std::ranges::equal(frame->bytes(), previous)) {
            previous.assign(frame->bytes().begin(), frame->bytes().end());
            times.push_back(std::chrono::steady_clock::now());
            ready.notify_all();
        }
    });
    std::unique_lock lock(mu);
    REQUIRE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return times.size() >= 30; }));
    const auto elapsed = times[29] - times[0];
    CHECK(elapsed >= std::chrono::milliseconds(300));
    CHECK(elapsed < std::chrono::milliseconds(1000));
}

LTEST_CASE(media_worker_retains_native_alpha_pixels_for_compositor_scaling) {
    for (int height : {64, 128}) {
        auto src = source("source.media");
        src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha.webm";
        src.settings["width"] = std::int64_t{128};
        src.settings["height"] = static_cast<std::int64_t>(height);
        auto frames = capture(src);
        REQUIRE(!frames.empty());
        const auto& frame = frames.front();
        CHECK_EQ(frame.width, 32);
        CHECK_EQ(frame.height, 24);
        bool translucent = false;
        for (std::size_t i = 3; i < frame.pixels.size(); i += 4)
            translucent = translucent || (frame.pixels[i] > 100 && frame.pixels[i] < 160);
        CHECK(translucent);
    }
}

LTEST_CASE(portrait_alpha_video_decodes_without_corrupting_frame_storage) {
    auto src = source("source.media");
    src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-portrait.webm";
    src.settings["ignoreAudio"] = true;
    const auto frames = capture(src, 8);
    REQUIRE(frames.size() >= 8);
    for (const auto& frame : frames) {
        CHECK_EQ(frame.width, 1080);
        CHECK_EQ(frame.height, 1920);
        CHECK_EQ(frame.stride, 1080u * 4u);
        const auto alpha = [&](int x, int y) {
            return frame.pixels[static_cast<std::size_t>(y) * frame.stride +
                                static_cast<std::size_t>(x) * 4 + 3];
        };
        CHECK_EQ(alpha(1079, 1919), 0);
        CHECK(alpha(540, 960) > 100 && alpha(540, 960) < 160);
    }
}

LTEST_CASE(browser_worker_renders_local_html) {
    auto src = source("source.browser");
    src.settings["url"] = std::string("file://") + LIBERTAS_IMAGE_FIXTURES + "/browser.html";
    auto frames = capture(src);
    REQUIRE(!frames.empty());
    const auto& pixels = frames[0].pixels;
    CHECK(std::abs(int(pixels[0]) - 220) <= 2);
    CHECK(std::abs(int(pixels[1]) - 30) <= 2);
    const std::size_t background = (20 * 64 + 30) * 4;
    CHECK(std::abs(int(pixels[background]) - 20) <= 2);
    CHECK(std::abs(int(pixels[background + 2]) - 160) <= 2);
}

LTEST_CASE(browser_worker_obeys_background_transparency_and_preserves_content_alpha) {
    for (bool transparent : {true, false}) {
        auto src = source("source.browser");
        src.settings["url"] = std::string("file://") + LIBERTAS_IMAGE_FIXTURES + "/browser-alpha.html";
        src.settings["ignoreAudio"] = true;
        src.settings["transparentBackground"] = transparent;
        auto frames = capture(src);
        REQUIRE(!frames.empty());
        const auto& pixels = frames.front().pixels;
        CHECK_EQ(pixels[3], 255);
        CHECK(std::abs(int(pixels[0]) - 220) <= 2);
        const auto background = (20 * 64 + 40) * 4;
        CHECK_EQ(pixels[background + 3], transparent ? 0 : 255);
        const auto translucent = (8 * 64 + 24) * 4;
        CHECK(std::abs(int(pixels[translucent + 3]) - (transparent ? 128 : 255)) <= 1);
        CHECK(std::abs(int(pixels[translucent + 2]) - (transparent ? 255 : 128)) <= 2);
    }
}

LTEST_CASE(unavailable_blackmagic_worker_exits_without_inventing_frames) {
    auto src = source("source.blackmagic");
    src.settings["deviceIndex"] = std::int64_t{999};
    std::mutex mu;
    std::condition_variable done;
    bool stopped = false, picture = false;
    {
        SourceWorker worker(src, [&](auto frame) {
            std::lock_guard lock(mu);
            picture = picture || bool(frame);
            stopped = !frame;
            done.notify_all();
        });
        std::unique_lock lock(mu);
        done.wait_for(lock, std::chrono::seconds(4), [&] { return stopped; });
        CHECK(stopped);
        CHECK(!picture);
    }
}

LTEST_CASE(alpha_webm_preserves_audio_alongside_transparency) {
    auto src = source("source.media");
    src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-tone.webm";
    std::mutex mu;
    std::condition_variable ready;
    bool tone = false, translucent = false;
    {
        SourceWorker worker(
            src,
            [&](auto frame) {
                std::lock_guard lock(mu);
                if (frame)
                    for (std::size_t i = 3; i < frame->bytes().size(); i += 4)
                        translucent =
                            translucent || (frame->bytes()[i] > 100 && frame->bytes()[i] < 160);
                ready.notify_all();
            },
            [&](auto samples) {
                std::lock_guard lock(mu);
                for (float sample : samples)
                    tone = tone || std::abs(sample) > 0.001f;
                ready.notify_all();
            });
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(8), [&] { return tone && translucent; });
    }
    CHECK(tone && translucent);
}

LTEST_CASE(browser_audio_reaches_pcm_transport) {
    if (!std::getenv("LIBERTAS_TEST_BROWSER_AUDIO")) return;
    auto src = source("source.browser");
    src.settings["url"] = std::string("file://") + LIBERTAS_IMAGE_FIXTURES + "/browser-tone.html";
    std::mutex mu;
    std::condition_variable ready;
    bool tone = false;
    {
        SourceWorker worker(src, [](auto) {}, [&](auto samples) {
            std::lock_guard lock(mu);
            for (float sample : samples) tone = tone || std::abs(sample) > 0.001f;
            ready.notify_all();
        });
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(8), [&] { return tone; });
    }
    CHECK(tone);
}

LTEST_CASE(browser_ignore_audio_keeps_video_without_pcm) {
    auto src = source("source.browser");
    src.settings["url"] = std::string("file://") + LIBERTAS_IMAGE_FIXTURES + "/browser-tone.html";
    src.settings["ignoreAudio"] = true;
    std::mutex mu;
    std::condition_variable ready;
    bool picture = false, audio = false;
    {
        SourceWorker worker(src, [&](auto frame) {
            std::lock_guard lock(mu);
            picture = picture || bool(frame);
            ready.notify_all();
        }, [&](auto samples) {
            std::lock_guard lock(mu);
            audio = audio || !samples.empty();
            ready.notify_all();
        });
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(8), [&] { return picture; });
        ready.wait_for(lock, std::chrono::milliseconds(250), [&] { return audio; });
    }
    CHECK(picture);
    CHECK(!audio);
}

LTEST_CASE(countdown_text_worker_draws_running_and_paused_timer) {
    auto src = source("source.text");
    src.settings["size"] = std::int64_t{20};
    src.settings["countdown.enabled"] = true;
    src.settings["countdown.durationMs"] = std::int64_t{2000};
    src.settings["countdown.running"] = true;
    src.settings["countdown.startedAtMs"] = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto frames = capture(src, 3);
    REQUIRE(frames.size() >= 3);
    CHECK(frames.front().pixels != frames.back().pixels);
    src.settings["countdown.running"] = false;
    src.settings["countdown.elapsedMs"] = std::int64_t{1000};
    const auto paused = lcomp::countdown(src.settings, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    CHECK_EQ(paused.text, std::string("00:01"));
    CHECK_EQ(lcomp::countdown(src.settings, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 5000).text, paused.text);
    REQUIRE(!capture(src).empty());
    src.settings["countdown.elapsedMs"] = std::int64_t{0};
    CHECK_EQ(lcomp::countdown(src.settings, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()).text, std::string("00:02"));
    REQUIRE(!capture(src).empty());
}

LTEST_CASE(countdown_digits_keep_their_place_as_the_time_changes) {
    // A clock must not move while it runs. Two things used to make it: a proportional "1" is
    // narrower than an "8", and the shrink-to-fit was measured against the current caption, so
    // a narrower one picked a bigger size. Both are settled per configure now, so the ink keeps
    // the same extent whatever the digits say.
    const auto extent = [](const char* elapsed_from, std::int64_t elapsed) {
        auto src = source("source.text");
        src.settings["width"] = std::int64_t{128};
        src.settings["height"] = std::int64_t{48};
        src.settings["size"] = std::int64_t{32};
        src.settings["countdown.enabled"] = true;
        src.settings["countdown.periodMinutes"] = std::int64_t{18};
        src.settings["countdown.periodSeconds"] = std::int64_t{51};
        src.settings["countdown.elapsedMs"] = elapsed;
        (void)elapsed_from;
        auto frames = capture(src);
        REQUIRE(!frames.empty());
        const auto& pixels = frames.front().pixels;
        const int width = frames.front().width;
        int left = width, right = -1, top = 1 << 30, bottom = -1;
        const int height = static_cast<int>(pixels.size() / 4) / width;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x)) * 4 + 3] > 0) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
        return std::array<int, 4>{left, right, top, bottom};
    };
    // "18:51" against "11:11": same digit count, but all-ones is the narrowest a
    // proportional face draws.
    const auto eights = extent("18:51", 0);
    const auto ones = extent("11:11", 460000);
    // Same baseline and same cap height: the caption is not being re-sized.
    CHECK_EQ(eights[2], ones[2]);
    CHECK_EQ(eights[3], ones[3]);
    // Same starting edge, and the same total width to within a pixel of hinting.
    CHECK_EQ(eights[0], ones[0]);
    CHECK(std::abs((eights[1] - eights[0]) - (ones[1] - ones[0])) <= 1);
}

LTEST_CASE(countdown_end_message_is_shrunk_to_fit_rather_than_cropped) {
    // The source is sized for the digits it normally shows, so a wordy end message is routinely
    // wider than the box. It must be made to fit rather than run off the edge.
    auto src = source("source.text");
    src.settings["width"] = std::int64_t{96};
    src.settings["height"] = std::int64_t{32};
    src.settings["size"] = std::int64_t{28};
    src.settings["countdown.enabled"] = true;
    src.settings["countdown.periodSeconds"] = std::int64_t{0};
    src.settings["countdown.showEndMessage"] = true;
    src.settings["countdown.endMessage"] = std::string("WE WILL BE RIGHT BACK");
    auto frames = capture(src);
    REQUIRE(!frames.empty());
    const auto& pixels = frames.front().pixels;
    const int width = frames.front().width;
    const int height = static_cast<int>(pixels.size() / 4) / width;
    REQUIRE(height > 0);
    const auto inked = [&](int x, int y) {
        return pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)) *
                          4 +
                      3] > 0;
    };
    bool any = false, touches_edge = false;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (inked(x, y)) {
                any = true;
                // Ink in the last column or row means the glyphs ran out of the box.
                if (x == width - 1 || y == height - 1)
                    touches_edge = true;
            }
    CHECK(any);
    CHECK(!touches_edge);
}

LTEST_CASE(countdown_colour_rule_reaches_the_drawn_pixels) {
    auto src = source("source.text");
    src.settings["size"] = std::int64_t{20};
    src.settings["countdown.enabled"] = true;
    src.settings["countdown.periodSeconds"] = std::int64_t{5};
    src.settings["countdown.useTextColour"] = true;
    // Pure blue under ten seconds, so the rule and the default white cannot be confused.
    src.settings["countdown.colourRules"] = std::string("0,10000,0,0,255,255");
    auto frames = capture(src);
    REQUIRE(!frames.empty());
    const auto& pixels = frames.front().pixels;
    bool blue = false;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
        if (pixels[i + 3] > 200 && pixels[i + 2] > 200 && pixels[i] < 80 && pixels[i + 1] < 80)
            blue = true;
    CHECK(blue);
}

LTEST_CASE(countdown_formats_the_way_the_obs_plugin_does) {
    ldoc::ParamMap settings;
    settings["countdown.enabled"] = true;
    settings["countdown.periodMinutes"] = std::int64_t{1};
    settings["countdown.periodSeconds"] = std::int64_t{5};
    // Four fields rather than one duration, and the two spellings agree.
    CHECK_EQ(lcomp::countdown_period_ms(settings), std::int64_t{65000});
    CHECK_EQ(lcomp::countdown(settings, 0).text, std::string("01:05"));

    // Every part token the plugin substitutes, bare and zero-padded.
    settings["countdown.useFormattedOutput"] = true;
    settings["countdown.format"] =
        std::string("%days%/%hours%/%minutes%/%seconds% %days2%:%hours2%:%minutes2%:%seconds2% [%time%]");
    CHECK_EQ(lcomp::countdown(settings, 0).text,
             std::string("0/0/1/5 00:00:01:05 [01:05]"));

    // An unknown token is left alone, so a literal percentage survives a format string.
    settings["countdown.format"] = std::string("100% done in %time%");
    CHECK_EQ(lcomp::countdown(settings, 0).text, std::string("100% done in 01:05"));

    // Every unit switched off is a misconfiguration the operator can see, not a blank caption.
    settings["countdown.format"] = std::string("%time%");
    settings["countdown.showMinutes"] = false;
    settings["countdown.showSeconds"] = false;
    CHECK_EQ(lcomp::countdown(settings, 0).text, std::string("Nothing selected!"));
    settings["countdown.showMinutes"] = true;
    settings["countdown.showSeconds"] = true;

    // Counting up shows how far it has come and ends at the period.
    settings["countdown.countUp"] = true;
    settings["countdown.elapsedMs"] = std::int64_t{5000};
    CHECK_EQ(lcomp::countdown(settings, 0).text, std::string("00:05"));
    CHECK(!lcomp::countdown(settings, 0).ended);
    settings["countdown.elapsedMs"] = std::int64_t{65000};
    CHECK(lcomp::countdown(settings, 0).ended);
    settings["countdown.countUp"] = false;
    settings["countdown.elapsedMs"] = std::int64_t{0};

    // A duration written on its own still opens, and fills the four fields back in.
    ldoc::ParamMap legacy;
    legacy["countdown.durationMs"] = std::int64_t{90000};
    CHECK_EQ(lcomp::countdown_period_ms(legacy), std::int64_t{90000});
    CHECK_EQ(lcomp::countdown(legacy, 0).text, std::string("01:30"));
}

LTEST_CASE(countdown_colour_rules_pick_the_first_matching_range) {
    ldoc::ParamMap settings;
    settings["countdown.enabled"] = true;
    settings["countdown.periodSeconds"] = std::int64_t{30};
    settings["countdown.useTextColour"] = true;
    settings["countdown.mainTextColour"] = ldoc::Rgba::from_srgb8(255, 255, 255);
    // Red under ten seconds, amber under twenty; white otherwise.
    settings["countdown.colourRules"] = std::string("0,10000,255,0,0,255;0,20000,255,170,0,255");

    const auto at = [&](std::int64_t left) {
        auto copy = settings;
        copy["countdown.periodSeconds"] = left / 1000;
        return lcomp::countdown(copy, 0);
    };
    CHECK(at(30000).has_colour);
    CHECK_EQ(at(30000).colour.to_srgb8()[0], std::uint8_t{255});
    CHECK_EQ(at(30000).colour.to_srgb8()[1], std::uint8_t{255});
    // Fifteen seconds misses the first rule and takes the second.
    CHECK_EQ(at(15000).colour.to_srgb8()[1], std::uint8_t{170});
    // Five seconds matches both; the first one listed wins, as the plugin's list does.
    CHECK_EQ(at(5000).colour.to_srgb8()[0], std::uint8_t{255});
    CHECK_EQ(at(5000).colour.to_srgb8()[1], std::uint8_t{0});

    // Switched off, nothing is coloured at all.
    settings["countdown.useTextColour"] = false;
    CHECK(!lcomp::countdown(settings, 0).has_colour);

    // A malformed rule costs that rule and not the list.
    settings["countdown.useTextColour"] = true;
    settings["countdown.colourRules"] = std::string("junk;0,10000,0,0,255,255");
    const auto rules = lcomp::countdown_colour_rules(settings);
    CHECK_EQ(rules.size(), std::size_t{1});
    CHECK_EQ(rules.front().colour.to_srgb8()[2], std::uint8_t{255});
}

LTEST_CASE(preloaded_media_reactivates_without_restarting_worker) {
    for (const char* file : {"motion.mkv", "alpha-60.webm"}) {
        auto src = source("source.media");
        src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/" + file;
        src.settings["loop"] = true;
        std::mutex mu;
        std::condition_variable ready;
        std::shared_ptr<const lcomp::PreviewFrame> latest;
        int count = 0;
        SourceWorker worker(src, [&](auto frame) {
            if (!frame) return;
            std::lock_guard lock(mu);
            latest = std::move(frame);
            ++count;
            ready.notify_all();
        });
        worker.set_active(false);
        std::unique_lock lock(mu);
        REQUIRE(ready.wait_for(lock, std::chrono::seconds(8), [&] { return count > 0; }));
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        lock.lock();
        const auto retained = latest;
        const int before = count;
        worker.set_active(true);
        REQUIRE(ready.wait_for(lock, std::chrono::milliseconds(350), [&] { return count > before; }));
        CHECK(latest);
        CHECK(retained && !retained->bytes().empty());
        lock.unlock();
    }
}

LTEST_CASE(visible_media_bypasses_busy_inactive_preload_queue) {
    auto invalid = source("source.media");
    invalid.settings["localFile"] = std::string("/nonexistent/libertas-preload.mp4");
    SourceWorker first(invalid, [](auto) {}, {}, false);
    invalid.id = ldoc::Id::generate();
    SourceWorker second(invalid, [](auto) {}, {}, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto src = source("source.media");
    src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion.mkv";
    std::mutex mu;
    std::condition_variable ready;
    bool received = false;
    SourceWorker foreground(src, [&](auto frame) {
        if (!frame) return;
        std::lock_guard lock(mu);
        received = true;
        ready.notify_all();
    }, {}, false);
    foreground.set_active(true);
    std::unique_lock lock(mu);
    CHECK(ready.wait_for(lock, std::chrono::seconds(4), [&] { return received; }));
    lock.unlock();
    first.stop(); second.stop(); foreground.stop();
}

LTEST_CASE(measure_media_first_frame) {
    const auto* path = std::getenv("LIBERTAS_BENCH_MEDIA");
    if (!path) return;
    auto src = source("source.media");
    src.settings["localFile"] = std::string(path);
    const auto start = std::chrono::steady_clock::now();
    auto frames = capture(src);
    REQUIRE(!frames.empty());
    std::fprintf(stderr, "media first frame including worker teardown: %.1f ms\n",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

LTEST_MAIN("source workers")

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../apps/munkicast-render/source_worker.hpp"
#include "ltest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fstream>
#include <lcomp/countdown.hpp>
#include <QFontDatabase>
#include <QGuiApplication>
#include <lcomp/text_metrics.hpp>
#include <mutex>
#include <utility>
#include <vector>

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
    // The surface is what the text measures, not the 64x32 the helper sets for every other
    // source type -- so "Hi" at 22px comes back about 21 px wide. Checked as a range rather
    // than a number, because the exact advance belongs to whichever face "Sans" resolves to on
    // the machine running this, and pinning it would make the test a font check.
    CHECK_MSG(frames[0].width > 8 && frames[0].width < 64,
              "'Hi' at 22px published a {}px-wide surface; expected it to fit the glyphs",
              frames[0].width);
    CHECK_MSG(frames[0].height > 8 && frames[0].height < 64,
              "'Hi' at 22px published a {}px-tall surface", frames[0].height);
    bool ink = false, clear = false;
    for (std::size_t i = 3; i < frames[0].pixels.size(); i += 4) {
        ink = ink || frames[0].pixels[i] > 0;
        clear = clear || frames[0].pixels[i] == 0;
    }
    CHECK(ink);
    CHECK(clear);
}

/// A QFontMetrics needs a QGuiApplication, and this suite is a plain console binary. One is
/// brought up offscreen for the measuring test only -- the same thing the shell and the worker
/// each have, without this suite needing a display.
struct MeasuringApp {
    MeasuringApp() {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        app = std::make_unique<QGuiApplication>(argc, argv);
    }
    QSize measure(const ldoc::ParamMap& settings) const {
        return lcomp::text_surface(settings).bounds.size();
    }
    int argc = 1;
    char name[8] = "ltest";
    char* argv[2] = {name, nullptr};
    std::unique_ptr<QGuiApplication> app;
};

/// Fixed width holds the layer still, measured on a face whose digits genuinely differ.
///
/// This is the test that was missing when the fault was reported. The existing digit test used
/// "Sans", which resolves to Liberation Sans -- and every stock font on a Linux box, checked,
/// draws all ten digits at one width. On such a face the fixed-width path cannot be told from
/// doing nothing, so it was never really exercised. A face is synthesised here instead of
/// trusting one to be installed: the rule under test is the layout, not anybody's typography.
LTEST_CASE(fixed_width_holds_a_timer_still_on_a_face_with_uneven_digits) {
    const MeasuringApp gui;
    const QFont uneven = [] {
        // Found rather than assumed: the first installed family whose digits are not all one
        // width. Skipped, not failed, when the machine has none -- a CI box with three stock
        // fonts is not a broken build.
        for (const QString& family : QFontDatabase::families()) {
            QFont font(family);
            font.setPixelSize(64);
            const QFontMetrics metrics(font);
            int lo = 1 << 30, hi = 0;
            for (char digit = '0'; digit <= '9'; ++digit) {
                const int advance = metrics.horizontalAdvance(QLatin1Char(digit));
                lo = std::min(lo, advance);
                hi = std::max(hi, advance);
            }
            if (lo > 0 && hi > lo + hi / 8)
                return font;
        }
        return QFont{};
    }();
    if (uneven.family().isEmpty()) {
        CHECK_MSG(true, "no installed face has uneven digits; nothing to measure here");
        return;
    }

    const auto measure = [&](const char* caption, bool fixed) {
        ldoc::ParamMap settings;
        settings["font"] = uneven.family().toStdString();
        settings["size"] = std::int64_t{64};
        settings["text"] = std::string(caption);
        settings["countdown.enabled"] = true;
        settings["countdown.periodMinutes"] = std::int64_t{18};
        settings["countdown.periodSeconds"] = std::int64_t{51};
        settings["countdown.format"] = std::string(caption);
        settings["countdown.useFormattedOutput"] = true;
        if (fixed)
            settings["countdown.fixedWidth"] = true;
        return gui.measure(settings).width();
    };

    // Off: the layer follows the caption, so the narrower digits give a narrower layer. This is
    // the collapse that was reported, and it is the honest behaviour of plain text.
    const int loose_wide = measure("18:88", false);
    const int loose_narrow = measure("11:11", false);
    CHECK_MSG(loose_narrow < loose_wide,
              "on {}, '11:11' should lay out narrower than '18:88' without fixed width; "
              "got {}px and {}px",
              uneven.family().toStdString(), loose_narrow, loose_wide);

    // On: every digit gets the widest digit's cell, so both captions occupy the same layer and
    // nothing moves as the clock ticks.
    const int fixed_wide = measure("18:88", true);
    const int fixed_narrow = measure("11:11", true);
    CHECK_MSG(fixed_wide == fixed_narrow,
              "on {}, fixed width should give '18:88' and '11:11' the same layer; "
              "got {}px and {}px",
              uneven.family().toStdString(), fixed_wide, fixed_narrow);
}

/// The levelled digits reach the glyphs, not just the measurement.
///
/// The measurement test above passed throughout a bug where no caption was ever levelled: the
/// surface was the right size and the digits inside it were not spaced. Only the published
/// pixels could have caught that, so this reads them.
LTEST_CASE(fixed_width_actually_spaces_the_drawn_digits) {
    const MeasuringApp gui;
    QString uneven;
    int narrow = 0, wide = 0;
    for (const QString& family : QFontDatabase::families()) {
        QFont font(family);
        font.setPixelSize(64);
        const QFontMetrics metrics(font);
        const int one = metrics.horizontalAdvance(QLatin1Char('1'));
        int widest = 0;
        for (char digit = '0'; digit <= '9'; ++digit)
            widest = std::max(widest, metrics.horizontalAdvance(QLatin1Char(digit)));
        if (!(one > 0 && widest > one + widest / 4))
            continue;
        uneven = family;
        narrow = one;
        wide = widest;
        break;
    }
    if (uneven.isEmpty()) {
        CHECK_MSG(true, "no installed face draws '1' much narrower than its widest digit");
        return;
    }

    auto src = source("source.text");
    src.settings["font"] = uneven.toStdString();
    src.settings["size"] = std::int64_t{64};
    src.settings["countdown.enabled"] = true;
    src.settings["countdown.periodSeconds"] = std::int64_t{11};
    src.settings["countdown.fixedWidth"] = true;
    auto frames = capture(src);
    REQUIRE(!frames.empty());

    // The pitch between the two "1"s of "00:11", which is the whole point of the feature.
    //
    // Not the widest gap in the frame: that is the space beside the colon, and it is wide on
    // both paths, so a test keyed on it passes whether or not the digits were levelled --
    // checked, and the mutation survived. Levelling moves the second "1" a full cell from the
    // first instead of its own narrow advance, so the pitch is what tells the paths apart.
    const auto& pixels = frames.front().pixels;
    const int width = static_cast<int>(frames.front().width);
    const int height = static_cast<int>(pixels.size() / 4) / width;
    const auto inked_column = [&](int x) {
        for (int y = 0; y < height; ++y)
            if (pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                        static_cast<std::size_t>(x)) * 4 + 3] > 0)
                return true;
        return false;
    };
    // The left edge of each inked run. "00:11" gives five runs for a face whose colon is one
    // mark; the last two are the digits being measured.
    std::vector<int> runs;
    bool in_run = false;
    for (int x = 0; x < width; ++x) {
        const bool ink = inked_column(x);
        if (ink && !in_run)
            runs.push_back(x);
        in_run = ink;
    }
    REQUIRE(runs.size() >= 2);
    const int pitch = runs.back() - runs[runs.size() - 2];
    // A levelled digit starts one full cell after the one before it, give or take the centring
    // of a narrow glyph inside its cell. An unlevelled "1" starts one narrow advance along.
    CHECK_MSG(pitch > (narrow + wide) / 2,
              "on {}, '1' advances {}px and the widest digit {}px, so levelled digits should be "
              "about {}px apart; the published frame has them {}px apart",
              uneven.toStdString(), narrow, wide, wide, pitch);
}

LTEST_CASE(the_shell_measures_a_text_surface_the_same_size_the_worker_publishes) {
    const MeasuringApp gui;
    // The bug this covers: the worker sized its surface to the laid-out text while the shell
    // still read the old `width`/`height` settings and defaulted to 1920x1080, so the selection
    // handles sat in the middle of the canvas and the glyphs were somewhere else. One
    // measurement, in `lcomp::text_surface`, is the fix -- this is what says the worker uses it.
    for (const auto& [caption, size] : std::initializer_list<std::pair<const char*, int>>{
             {"Hi", 22}, {"SOON", 120}, {"The quick brown fox", 48}}) {
        auto src = source("source.text");
        src.settings["text"] = std::string(caption);
        src.settings["size"] = static_cast<std::int64_t>(size);
        const QSize measured = gui.measure(src.settings);
        auto frames = capture(src);
        REQUIRE(!frames.empty());
        CHECK_MSG(static_cast<int>(frames[0].width) == measured.width() &&
                      static_cast<int>(frames[0].height) == measured.height(),
                  "'{}' at {}px: the worker published {}x{} and the shell measured {}x{}",
                  caption, size, frames[0].width, frames[0].height, measured.width(),
                  measured.height());
    }
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
        std::string(MUNKICAST_IMAGE_FIXTURES) + "/two.png\n" + MUNKICAST_IMAGE_FIXTURES + "/red.jpg";
    src.settings["intervalMs"] = std::int64_t{100};
    auto frames = capture(src, 2);
    REQUIRE(frames.size() >= 2);
    CHECK(frames[0].pixels != frames[1].pixels);
}

LTEST_CASE(media_worker_decodes_video_in_its_process) {
    auto src = source("source.media");
    src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/motion.mkv";
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
        src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + file;
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
    src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/alpha-60.webm";
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
    src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/alpha-60.webm";
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
        src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/alpha.webm";
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
    src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/alpha-portrait.webm";
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
    src.settings["url"] = std::string("file://") + MUNKICAST_IMAGE_FIXTURES + "/browser.html";
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
        src.settings["url"] = std::string("file://") + MUNKICAST_IMAGE_FIXTURES + "/browser-alpha.html";
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
    src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/alpha-tone.webm";
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
    if (!std::getenv("MUNKICAST_TEST_BROWSER_AUDIO")) return;
    auto src = source("source.browser");
    src.settings["url"] = std::string("file://") + MUNKICAST_IMAGE_FIXTURES + "/browser-tone.html";
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
    src.settings["url"] = std::string("file://") + MUNKICAST_IMAGE_FIXTURES + "/browser-tone.html";
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

LTEST_CASE(countdown_digits_keep_their_place_only_when_fixed_width_is_asked_for) {
    // A proportional "1" is narrower than an "8", so a running clock shuffles as the digits
    // change. Levelling the digit advances stops that -- but it is a departure from the face
    // the operator chose, so it is a checkbox rather than something a countdown does to its
    // text by itself. Both halves of that switch are checked here: without the flag the text
    // behaves like any other text, with it the ink keeps its extent.
    const auto extent = [](std::int64_t elapsed, bool fixed) {
        auto src = source("source.text");
        src.settings["size"] = std::int64_t{32};
        src.settings["countdown.enabled"] = true;
        src.settings["countdown.periodMinutes"] = std::int64_t{18};
        src.settings["countdown.periodSeconds"] = std::int64_t{51};
        src.settings["countdown.elapsedMs"] = elapsed;
        if (fixed)
            src.settings["countdown.fixedWidth"] = true;
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
    // "18:51" against "11:11": same digit count, but all-ones is the narrowest a proportional
    // face draws.
    const auto eights = extent(0, true);
    const auto ones = extent(460000, true);
    // Same baseline and same cap height: the caption is not being re-sized.
    CHECK_EQ(eights[2], ones[2]);
    CHECK_EQ(eights[3], ones[3]);
    // Same starting edge, and the same total width to within a pixel of hinting.
    CHECK_EQ(eights[0], ones[0]);
    CHECK_MSG(std::abs((eights[1] - eights[0]) - (ones[1] - ones[0])) <= 1,
              "with fixed width on, '18:51' inked {}px and '11:11' inked {}px",
              eights[1] - eights[0], ones[1] - ones[0]);

    // Off, which is the default: the timer is ordinary text, so the narrower caption is
    // narrower. Asserted rather than merely allowed, because "the flag does nothing" would
    // otherwise pass the half of this test above.
    const auto loose_eights = extent(0, false);
    const auto loose_ones = extent(460000, false);
    CHECK_MSG((loose_ones[1] - loose_ones[0]) < (loose_eights[1] - loose_eights[0]),
              "without fixed width, '11:11' should ink narrower than '18:51'; got {}px and {}px",
              loose_ones[1] - loose_ones[0], loose_eights[1] - loose_eights[0]);
}

LTEST_CASE(a_countdown_surface_follows_the_caption_on_screen) {
    // The surface used to be measured against the widest caption the timer would ever produce,
    // so it stayed the same size as the digits ticked. That made the layer bigger than its text
    // at every other moment -- and because the node's anchor acts on the layer, anchoring a
    // timer appeared to do nothing. The surface now follows the caption, so a long end message
    // gets a wider layer rather than smaller type, and the digits get a layer their own size.
    const auto published = [](const char* message, std::int64_t elapsed) {
        auto src = source("source.text");
        src.settings["size"] = std::int64_t{28};
        src.settings["countdown.enabled"] = true;
        src.settings["countdown.periodSeconds"] = std::int64_t{30};
        src.settings["countdown.elapsedMs"] = elapsed;
        src.settings["countdown.showEndMessage"] = true;
        src.settings["countdown.endMessage"] = std::string(message);
        auto frames = capture(src);
        REQUIRE(!frames.empty());
        return frames.front().width;
    };
    const auto counting = published("WE WILL BE RIGHT BACK", 0);
    const auto ended = published("WE WILL BE RIGHT BACK", 30000);
    CHECK_MSG(ended > counting,
              "the end message is far longer than '00:30', so its layer should be wider; "
              "counting published {}px and ended published {}px",
              counting, ended);

    // And the type is not shrunk to reach that: the same message at twice the size is wider.
    auto big = source("source.text");
    big.settings["size"] = std::int64_t{56};
    big.settings["countdown.enabled"] = true;
    big.settings["countdown.periodSeconds"] = std::int64_t{30};
    big.settings["countdown.elapsedMs"] = std::int64_t{30000};
    big.settings["countdown.showEndMessage"] = true;
    big.settings["countdown.endMessage"] = std::string("WE WILL BE RIGHT BACK");
    auto frames = capture(big);
    REQUIRE(!frames.empty());
    CHECK_MSG(static_cast<int>(frames.front().width) > ended,
              "56px should lay out wider than 28px; got {}px against {}px",
              frames.front().width, ended);
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
        src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/" + file;
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
    invalid.settings["localFile"] = std::string("/nonexistent/munkicast-preload.mp4");
    SourceWorker first(invalid, [](auto) {}, {}, false);
    invalid.id = ldoc::Id::generate();
    SourceWorker second(invalid, [](auto) {}, {}, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto src = source("source.media");
    src.settings["localFile"] = std::string(MUNKICAST_IMAGE_FIXTURES) + "/motion.mkv";
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
    const auto* path = std::getenv("MUNKICAST_BENCH_MEDIA");
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

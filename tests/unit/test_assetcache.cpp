// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which prepared form an asset should take (ADR-0069 stage 3), and the settings reading that
// decides what gets prepared at all.
//
// The decision is here rather than in the shell self-check because it is a fact about a file
// and a canvas, not about the UI: a table of sizes exercises every branch in milliseconds,
// where driving the same cases through a build would be a transcode each. What the self-check
// still owns is the parts that need the real shell -- the indicator, the menu, the index on
// disk, path substitution into the published project.

#include "assetcache.hpp"

#include "ltest.hpp"

using namespace munkicast::ui;

namespace {

/// A video of this size. Duration is what tells `probe_asset` a video from a still, so a
/// fixture without one would be testing the still path by accident.
Probe video(int w, int h) { return Probe{QSize(w, h), QStringLiteral("h264"), 4.0}; }

const QSize k1440p{2560, 1440};
const QSize k1080p{1920, 1080};

ldoc::Source source_of(std::string type, ldoc::ParamMap settings) {
    ldoc::Source source;
    source.type = std::move(type);
    source.settings = std::move(settings);
    return source;
}

} // namespace

LTEST_CASE(oversized_video_is_downscaled_to_the_canvas) {
    // The measured case: the reference collection's 4K trailer against a 1440p canvas. 2.25x
    // less per-frame RGBA conversion is the whole reason this stage exists.
    const auto recipe = recipe_for(video(3840, 2160), k1440p);
    CHECK_EQ(recipe.name, QStringLiteral("downscale"));
    CHECK_EQ(recipe.target, k1440p);
}

LTEST_CASE(video_at_or_under_canvas_size_is_copied) {
    // NVDEC already handles these, and re-encoding would spend quality for no gain.
    for (const auto& probe : {video(2560, 1440), video(1920, 1080), video(640, 480)})
        CHECK_EQ(recipe_for(probe, k1440p).name, QStringLiteral("copy"));
}

LTEST_CASE(aspect_ratio_is_kept_rather_than_filling_the_canvas) {
    // A 4:3 clip on a 16:9 canvas is letterboxed by the compositor. Encoding it at the
    // canvas's own aspect would bake in a stretch no later setting could undo.
    const auto recipe = recipe_for(video(4000, 3000), k1080p);
    CHECK_EQ(recipe.name, QStringLiteral("downscale"));
    CHECK_EQ(recipe.target.height(), 1080);
    CHECK_EQ(recipe.target.width(), 1440);
}

LTEST_CASE(a_clip_oversized_in_one_direction_only_is_still_downscaled) {
    // Taller than the canvas, narrower than it: the rows the canvas cannot show are still
    // rows being converted on every frame.
    const auto recipe = recipe_for(video(1000, 2000), k1080p);
    CHECK_EQ(recipe.name, QStringLiteral("downscale"));
    CHECK(recipe.target.height() <= 1080);
    CHECK(recipe.target.width() <= 1920);
}

LTEST_CASE(encoded_dimensions_are_even) {
    // H.264 chroma is subsampled; an odd dimension is either refused or silently rounded,
    // depending on the encoder.
    for (const auto& probe : {video(1999, 1777), video(3333, 2221), video(2001, 1081)}) {
        const auto target = recipe_for(probe, k1080p).target;
        CHECK_EQ(target.width() % 2, 0);
        CHECK_EQ(target.height() % 2, 0);
    }
}

LTEST_CASE(stills_and_unprobeable_files_are_copied) {
    // A still has a video stream and a size too. Duration is what separates them, and a
    // probe that failed entirely must not be guessed at.
    CHECK_EQ(recipe_for(Probe{QSize(8000, 6000), QStringLiteral("png"), 0.0}, k1080p).name,
             QStringLiteral("copy"));
    CHECK_EQ(recipe_for(Probe{}, k1080p).name, QStringLiteral("copy"));
}

LTEST_CASE(no_canvas_means_no_transcode) {
    // Before the first document arrives the canvas is 0x0. Downscaling to that would be
    // destroying the asset.
    CHECK_EQ(recipe_for(video(3840, 2160), QSize{}).name, QStringLiteral("copy"));
    CHECK_EQ(recipe_for(video(3840, 2160), QSize(0, 0)).name, QStringLiteral("copy"));
}

LTEST_CASE(only_file_backed_types_are_cacheable) {
    CHECK(source_type_cacheable("source.media"));
    CHECK(source_type_cacheable("source.image"));
    CHECK(source_type_cacheable("source.playlist"));
    CHECK(source_type_cacheable("source.slideshow"));
    // A device node is not a file to copy, and a colour has nothing to prepare.
    CHECK(!source_type_cacheable("source.v4l2"));
    CHECK(!source_type_cacheable("source.color"));
    CHECK(!source_type_cacheable("source.browser"));
    CHECK(!source_type_cacheable("source.screen"));
}

LTEST_CASE(use_cache_defaults_on_for_a_source_that_predates_the_setting) {
    CHECK(source_uses_cache(source_of("source.image", {{"file", std::string{"/a.png"}}})));
    CHECK(source_uses_cache(source_of("source.image", {{"useCache", true}})));
    CHECK(!source_uses_cache(source_of("source.image", {{"useCache", false}})));
}

LTEST_CASE(asset_paths_come_from_the_key_that_names_the_file) {
    CHECK_EQ(source_asset_paths(source_of("source.media", {{"localFile", std::string{"/v.mp4"}}})),
             QStringList{QStringLiteral("/v.mp4")});
    CHECK_EQ(source_asset_paths(source_of("source.image", {{"file", std::string{"/s.png"}}})),
             QStringList{QStringLiteral("/s.png")});
    // A list type keeps order and count: a playlist is an ordered thing.
    const auto many = source_asset_paths(
        source_of("source.playlist", {{"files", std::string{"/a.mp4\n/b.mp4\n/c.mp4"}}}));
    CHECK_EQ(many.size(), 3);
    CHECK_EQ(many.at(0), QStringLiteral("/a.mp4"));
    CHECK_EQ(many.at(2), QStringLiteral("/c.mp4"));
    // Typed in by hand, so a trailing newline is not an asset called "".
    const auto messy = source_asset_paths(
        source_of("source.slideshow", {{"files", std::string{"  /a.png  \n\n/b.png\n"}}}));
    CHECK_EQ(messy, (QStringList{QStringLiteral("/a.png"), QStringLiteral("/b.png")}));
    // Nothing to ask about for a type with no file, or a field left empty.
    CHECK(source_asset_paths(source_of("source.v4l2", {{"device", std::string{"/dev/video0"}}}))
              .isEmpty());
    CHECK(source_asset_paths(source_of("source.image", {})).isEmpty());
}

LTEST_MAIN("assetcache")

// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcomp/composite.hpp"
#include "lcomp/filters.hpp"
#include "ltest.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

using lcomp::GapReason;
using lcomp::Picture;
using lcomp::PictureStatus;

namespace {

ldoc::Source colour_source(std::uint8_t r, std::uint8_t g, std::uint8_t b, int w, int h) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Colour";
    source.type = "source.color";
    source.settings["color"] = ldoc::Rgba::from_srgb8(r, g, b);
    source.settings["width"] = static_cast<std::int64_t>(w);
    source.settings["height"] = static_cast<std::int64_t>(h);
    return source;
}

ldoc::Node bound_to(const ldoc::Source& source) {
    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, source.name);
    std::get<ldoc::SourceData>(node.data).source = source.id;
    return node;
}

/// A project whose one canvas is `edge` pixels square and whose program scene is `scene`.
ldoc::Project show(int edge, ldoc::Scene scene, std::vector<ldoc::Source> sources = {}) {
    ldoc::Project project = ldoc::make_default_project("show");
    project.canvases[0].width = edge;
    project.canvases[0].height = edge;
    scene.canvas = project.canvases[0].id;
    project.decks[0].program_scene = scene.id;
    project.scenes = {std::move(scene)};
    project.sources = std::move(sources);
    return project;
}

ldoc::Scene scene_named(std::string name) {
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.name = std::move(name);
    return scene;
}

ldoc::Source image_source(std::string path) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Still";
    source.type = "source.image";
    source.settings["file"] = std::move(path);
    return source;
}

std::filesystem::path image_fixture(const char* name) {
    return std::filesystem::path{LIBERTAS_IMAGE_FIXTURES} / name;
}

ldoc::Source screen_source(std::string connector) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Screen";
    source.type = "source.screen";
    source.settings["output"] = std::move(connector);
    return source;
}

/// Backing store for a captured frame. `CapturedPicture` borrows rather than owns -- a 4K frame
/// is 33 MB and copying it per composite cost more than the scaling -- so the test holds the
/// buffer alive for as long as the walk is reading it.
using FrameBuffer = std::vector<std::uint8_t>;

/// One flat colour in the order the compositor actually delivers: **BGRA**. Written that way on
/// purpose. The scaler swaps as it samples, and a test that handed it RGBA would pass whether or
/// not the swap happened.
FrameBuffer flat_bgra(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    FrameBuffer px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = b;
        px[i + 1] = g;
        px[i + 2] = r;
        px[i + 3] = 255;
    }
    return px;
}

lcomp::CapturedPicture view_of(const FrameBuffer& px, int w, int h) {
    lcomp::CapturedPicture frame;
    frame.width = w;
    frame.height = h;
    frame.pixels = px.data();
    frame.size = px.size();
    return frame;
}

std::tuple<int, int, int, int> pixel(const Picture& picture, int x, int y) {
    const std::size_t i =
        static_cast<std::size_t>(y) * picture.stride + static_cast<std::size_t>(x) * 4u;
    return {picture.pixels[i], picture.pixels[i + 1], picture.pixels[i + 2], picture.pixels[i + 3]};
}

} // namespace

LTEST_CASE(red_covers_the_canvas_and_black_is_what_nothing_draws) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(red)};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);

    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(255, 0, 0, 255));

    project.scenes[0].roots.clear();
    const Picture empty = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(empty.status, PictureStatus::Empty);
    CHECK_EQ(pixel(empty, 1, 1), std::tuple(0, 0, 0, 255));

    project.decks[0].program_scene.reset();
    const Picture none = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(none.status, PictureStatus::NoProgram);
    CHECK_EQ(pixel(none, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(the_top_of_the_list_is_drawn_last) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    red.name = "Red";
    ldoc::Source blue = colour_source(0, 0, 255, 4, 4);
    blue.name = "Blue";
    ldoc::Scene scene = scene_named("Scene");
    // roots[0] is the topmost node. Red in front of blue is red.
    scene.roots = {bound_to(red), bound_to(blue)};
    ldoc::Project project = show(4, scene, {red, blue});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 2, 2), std::tuple(255, 0, 0, 255));
}

LTEST_CASE(a_hidden_node_is_not_a_gap_and_is_not_drawn) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Node node = bound_to(red);
    node.visible = false;
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {node};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Empty);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(half_opacity_blends_in_linear_light) {
    // 50% opaque red over the black backdrop. Linear 0.5 encodes to sRGB 188.
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Node node = bound_to(red);
    node.blend.opacity = 0.5;
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {node};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 1, 2), std::tuple(188, 0, 0, 255));
}

LTEST_CASE(seven_blend_modes_render_real_sources_with_alpha_and_opacity) {
    using M = ldoc::BlendMode;
    const std::array modes{M::Normal,   M::LinearDodge, M::Subtract, M::Screen,
                           M::Multiply, M::Lighten,     M::Darken};
    const std::array<std::array<double, 3>, 7> blended{{{0.6, 0.2, 0.4},
                                                        {0.9, 0.7, 1.0},
                                                        {0.0, 0.3, 0.3},
                                                        {0.72, 0.6, 0.82},
                                                        {0.18, 0.1, 0.28},
                                                        {0.6, 0.5, 0.7},
                                                        {0.3, 0.2, 0.4}}};
    auto front = colour_source(0, 0, 0, 4, 4);
    auto back = colour_source(0, 0, 0, 4, 4);
    back.settings["color"] = ldoc::Rgba{0.3, 0.5, 0.7, 1};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        for (const double alpha : {0.0, 0.5, 1.0}) {
            front.settings["color"] = ldoc::Rgba{0.6, 0.2, 0.4, alpha};
            auto scene = scene_named("blended scene");
            auto node = bound_to(front);
            node.blend.mode = modes[i];
            node.blend.opacity = 0.4;
            scene.roots = {node, bound_to(back)};
            auto project = show(4, scene, {front, back});
            const auto picture = lcomp::composite_canvas(project, project.canvases[0].id);
            REQUIRE(picture.status == PictureStatus::Complete);
            const double coverage = alpha * 0.4;
            const auto expected = ldoc::Rgba{0.3 * (1 - coverage) + blended[i][0] * coverage,
                                             0.5 * (1 - coverage) + blended[i][1] * coverage,
                                             0.7 * (1 - coverage) + blended[i][2] * coverage, 1}
                                      .to_srgb8();
            for (std::size_t c = 0; c < 4; ++c)
                CHECK(std::abs(int(picture.pixels[c]) - int(expected[c])) <= 1);
        }
    }
}

LTEST_CASE(group_blend_flattens_children_and_applies_opacity_once) {
    auto red = colour_source(255, 0, 0, 4, 4);
    auto white = colour_source(255, 255, 255, 4, 4);
    auto group = ldoc::make_node(ldoc::NodeKind::Group, "Multiply group");
    group.blend.mode = ldoc::BlendMode::Multiply;
    group.blend.opacity = 0.5;
    std::get<ldoc::GroupData>(group.data).children = {bound_to(red), bound_to(red)};
    auto scene = scene_named("group scene");
    scene.roots = {group, bound_to(white)};
    auto project = show(4, scene, {red, white});
    const auto picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 188, 188, 255));
    project.scenes[0].roots[0].blend.pass_through_group = true;
    const auto refused = lcomp::composite_canvas(project, project.canvases[0].id);
    REQUIRE(!refused.gaps.empty());
    CHECK_EQ(refused.gaps[0].reason, GapReason::UnsupportedBlend);
}

LTEST_CASE(a_groups_opacity_reaches_its_children) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Node child = bound_to(red);
    ldoc::Node group = ldoc::make_node(ldoc::NodeKind::Group, "Group");
    group.blend.opacity = 0.5;
    std::get<ldoc::GroupData>(group.data).children = {child};
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {group};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(188, 0, 0, 255));

    project.scenes[0].roots[0].visible = false;
    const Picture hidden = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(hidden.status, PictureStatus::Empty);
    CHECK_EQ(pixel(hidden, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(group_transform_places_its_children_as_one_picture) {
    auto red = colour_source(255, 0, 0, 4, 4);
    auto group = ldoc::make_node(ldoc::NodeKind::Group, "Scaled group");
    std::get<ldoc::GroupData>(group.data).children = {bound_to(red)};
    group.transform.pivot = {0, 0};
    group.transform.position = {1, 1};
    group.transform.scale = {0.5, 0.5};
    auto scene = scene_named("Scene");
    scene.roots = {group};
    auto project = show(4, scene, {red});
    const auto picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 0, 0, 255));
    CHECK_EQ(pixel(picture, 1, 1), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 2, 2), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(contain_letterboxes_a_colour) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 2);
    ldoc::Node node = bound_to(red);
    node.layout.fit = ldoc::FitMode::Contain;
    node.layout.size = {4.0, 4.0};
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {node};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 0, 0, 255));
    CHECK_EQ(pixel(picture, 0, 1), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 2), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(what_cannot_be_drawn_is_a_gap_and_the_rest_still_is) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Source camera;
    camera.id = ldoc::Id::generate();
    camera.name = "Desk Cam";
    camera.type = "source.v4l2";
    ldoc::Node cam = bound_to(camera);
    ldoc::Node card = bound_to(red);
    // The camera is on top. It contributes nothing, so the colour behind it is the picture.
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {cam, card};

    ldoc::Node unbound = ldoc::make_node(ldoc::NodeKind::Source, "Loose");
    scene.roots.insert(scene.roots.begin(), unbound);

    ldoc::Node turned = bound_to(red);
    turned.name = "Turned";
    turned.transform.rotation_deg = 15.0;
    scene.roots.push_back(turned);

    ldoc::Source mic;
    mic.id = ldoc::Id::generate();
    mic.name = "Mic";
    mic.type = "source.audio-input";
    scene.roots.push_back(bound_to(mic));

    ldoc::Project project = show(4, scene, {red, camera, mic});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));

    int cameras = 0;
    int loose = 0;
    int turned_n = 0;
    for (const lcomp::Gap& gap : picture.gaps) {
        if (gap.reason == GapReason::UnsupportedType && gap.name == "Desk Cam")
            ++cameras;
        if (gap.reason == GapReason::NoSource)
            ++loose;
        if (gap.reason == GapReason::UnsupportedTransform)
            ++turned_n;
    }
    CHECK_EQ(cameras, 1);
    CHECK_EQ(loose, 1);
    CHECK_EQ(turned_n, 1);
    // The microphone is not a picture, so it is not a failure to draw one.
    CHECK_EQ(picture.gaps.size(), 3u);
}

LTEST_CASE(a_precomp_draws_the_other_scene_and_a_cycle_stops) {
    ldoc::Source blue = colour_source(0, 0, 255, 4, 4);
    blue.name = "Blue";
    ldoc::Scene inner = scene_named("Inner");
    inner.roots = {bound_to(blue)};

    ldoc::Node pre = ldoc::make_node(ldoc::NodeKind::Precomp, "Inner");
    std::get<ldoc::PrecompData>(pre.data).scene = inner.id;
    std::get<ldoc::PrecompData>(pre.data).size = {4.0, 4.0};
    ldoc::Scene outer = scene_named("Outer");
    outer.roots = {pre};

    ldoc::Project project = ldoc::make_default_project("show");
    project.canvases[0].width = 4;
    project.canvases[0].height = 4;
    inner.canvas = project.canvases[0].id;
    outer.canvas = project.canvases[0].id;
    project.decks[0].program_scene = outer.id;
    project.scenes = {outer, inner};
    project.sources = {blue};

    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 1, 1), std::tuple(0, 0, 255, 255));

    CHECK(!lcomp::plan_canvas(project, project.canvases[0].id).ops.front().nested);
    project.scenes[0].roots[0].blend.opacity = 0.5;
    CHECK(lcomp::plan_canvas(project, project.canvases[0].id).ops.front().nested != nullptr);
    project.scenes[0].roots[0].blend.opacity = 1;
    project.scenes[1].roots[0].blend.mode = ldoc::BlendMode::Multiply;
    CHECK(lcomp::plan_canvas(project, project.canvases[0].id).ops.front().nested != nullptr);
    project.scenes[1].roots[0].blend.mode = ldoc::BlendMode::Normal;
    ldoc::Effect correction;
    correction.id = ldoc::Id::generate();
    correction.type = "filter.color-correction";
    project.scenes[1].effects = {correction};
    CHECK(lcomp::plan_canvas(project, project.canvases[0].id).ops.front().nested != nullptr);
    CHECK(lcomp::composite_canvas(project, project.canvases[0].id).pixels == picture.pixels);
    project.scenes[1].effects.clear();

    // Point the inner scene back at the outer one. Drawing has to finish, and say why.
    ldoc::Node back = ldoc::make_node(ldoc::NodeKind::Precomp, "Back");
    std::get<ldoc::PrecompData>(back.data).scene = outer.id;
    project.scenes[1].roots.push_back(back);
    const Picture cycled = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(cycled.status, PictureStatus::Partial);
    bool saw_cycle = false;
    for (const lcomp::Gap& gap : cycled.gaps)
        if (gap.reason == GapReason::Cycle)
            saw_cycle = true;
    CHECK(saw_cycle);
    CHECK_EQ(pixel(cycled, 0, 0), std::tuple(0, 0, 255, 255));
}

LTEST_CASE(a_colour_with_no_colour_stored_is_not_invented) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Bare";
    source.type = "source.color";
    source.settings["width"] = static_cast<std::int64_t>(4);
    source.settings["height"] = static_cast<std::int64_t>(4);
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(source)};
    ldoc::Project project = show(4, scene, {source});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::NoColour);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(a_png_is_drawn_at_its_own_size_and_a_clear_pixel_stays_black) {
    ldoc::Source still = image_source(image_fixture("two.png").string());
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(still)};
    ldoc::Project project = show(4, scene, {still});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 1, 0), std::tuple(0, 255, 0, 255));
    CHECK_EQ(pixel(picture, 0, 1), std::tuple(0, 0, 255, 255));
    CHECK_EQ(pixel(picture, 1, 1), std::tuple(0, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(a_jpeg_and_a_webp_are_drawn) {
    ldoc::Source jpeg = image_source(image_fixture("red.jpg").string());
    jpeg.name = "Jpeg";
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(jpeg)};
    ldoc::Project project = show(2, scene, {jpeg});
    const Picture jpg = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(jpg.status, PictureStatus::Complete);
    // cjpeg -quality 100 of solid red, decoded by the same libjpeg, is 254.
    CHECK_EQ(pixel(jpg, 0, 0), std::tuple(254, 0, 0, 255));
    CHECK_EQ(pixel(jpg, 1, 1), std::tuple(254, 0, 0, 255));

    ldoc::Source webp = image_source(image_fixture("red.webp").string());
    project.sources = {webp};
    project.scenes[0].roots = {bound_to(webp)};
    const Picture web = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(web.status, PictureStatus::Complete);
    CHECK_EQ(pixel(web, 0, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(web, 1, 1), std::tuple(255, 0, 0, 255));
}

LTEST_CASE(an_image_that_cannot_be_read_leaves_what_is_behind_it) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Source missing = image_source("/no/such/libertas-still.png");
    missing.name = "Missing";
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(missing), bound_to(red)};
    ldoc::Project project = show(4, scene, {red, missing});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::Unreadable);
    CHECK_EQ(picture.gaps.front().name, "Missing");

    ldoc::Source blank = image_source("");
    project.sources = {blank};
    project.scenes[0].roots = {bound_to(blank)};
    const Picture empty = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(empty.status, PictureStatus::Partial);
    CHECK_EQ(empty.gaps.front().reason, GapReason::Unreadable);
    CHECK_EQ(pixel(empty, 0, 0), std::tuple(0, 0, 0, 255));

    const auto junk = std::filesystem::temp_directory_path() / "libertas-not-an-image";
    {
        std::ofstream out(junk);
        out << "not a picture\n";
    }
    ldoc::Source garbage = image_source(junk.string());
    project.sources = {garbage};
    project.scenes[0].roots = {bound_to(garbage)};
    const Picture refused = lcomp::composite_canvas(project, project.canvases[0].id);
    std::filesystem::remove(junk);
    CHECK_EQ(refused.gaps.front().reason, GapReason::Unreadable);

    ldoc::Source huge = image_source(image_fixture("huge.png").string());
    project.sources = {huge};
    project.scenes[0].roots = {bound_to(huge)};
    const Picture oversized = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(oversized.gaps.front().reason, GapReason::Unreadable);
    CHECK_EQ(pixel(oversized, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(a_quarter_turn_and_a_flip_are_drawn_and_a_slant_is_not) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 2);
    ldoc::Node card = bound_to(red);
    card.transform.position = {2.0, 3.0};
    card.transform.rotation_deg = 90.0;
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {card};
    ldoc::Project project = show(8, scene, {red});
    const Picture turned = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(turned.status, PictureStatus::Complete);
    CHECK(turned.gaps.empty());
    // 4 by 2 at (2, 3), pivot in the middle, turned clockwise: a 2 by 4 at (3, 2).
    CHECK_EQ(pixel(turned, 3, 2), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(turned, 4, 5), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(turned, 2, 3), std::tuple(0, 0, 0, 255));
    CHECK_EQ(pixel(turned, 5, 2), std::tuple(0, 0, 0, 255));

    ldoc::Source still = image_source(image_fixture("two.png").string());
    ldoc::Node photo = bound_to(still);
    photo.transform.rotation_deg = 90.0;
    project.sources = {still};
    project.scenes[0].roots = {photo};
    const Picture spun = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(spun.status, PictureStatus::Complete);
    // two.png is red, green / blue, clear. Clockwise, that is blue, red / clear, green.
    CHECK_EQ(pixel(spun, 0, 0), std::tuple(0, 0, 255, 255));
    CHECK_EQ(pixel(spun, 1, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(spun, 1, 1), std::tuple(0, 255, 0, 255));

    photo.transform.rotation_deg = 0.0;
    photo.transform.scale.x = -1.0;
    project.scenes[0].roots = {photo};
    const Picture flipped = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(flipped.status, PictureStatus::Complete);
    CHECK_EQ(pixel(flipped, 0, 0), std::tuple(0, 255, 0, 255));
    CHECK_EQ(pixel(flipped, 1, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(flipped, 1, 1), std::tuple(0, 0, 255, 255));

    photo.transform.scale = {1.0, 1.0};
    photo.transform.rotation_deg = 15.0;
    project.scenes[0].roots = {photo};
    const Picture slanted = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(slanted.status, PictureStatus::Partial);
    REQUIRE(!slanted.gaps.empty());
    CHECK_EQ(slanted.gaps.front().reason, GapReason::UnsupportedTransform);

    ldoc::Node plain = bound_to(red);
    GapReason why = GapReason::NoSize;
    const auto solved = lcomp::solve_drawn_rect(plain, 4.0, 2.0, true, 0.0, 0.0, 2.0, 1.0);
    REQUIRE(solved.has_value());
    CHECK(std::abs(solved->scale_x - 0.5) < 1e-9);
    CHECK(std::abs(solved->scale_y - 0.5) < 1e-9);
    plain.transform.position = {solved->x, solved->y};
    plain.transform.scale = {solved->scale_x, solved->scale_y};
    const auto frame = lcomp::drawn_frame(plain, 4.0, 2.0, true, why);
    REQUIRE(frame.has_value());
    CHECK(std::abs(frame->x) < 1e-6);
    CHECK(std::abs(frame->y) < 1e-6);
    CHECK(std::abs(frame->width - 2.0) < 1e-6);
    CHECK(std::abs(frame->height - 1.0) < 1e-6);
}

LTEST_CASE(a_screen_source_draws_the_frame_it_is_handed) {
    // The whole point of the commit: a captured frame reaches the picture. Green, because black
    // is what an undrawn canvas already is and a test that cannot tell those apart proves nothing.
    const ldoc::Source screen = screen_source("DP-1");
    ldoc::Scene scene = scene_named("program");
    scene.roots = {bound_to(screen)};
    const ldoc::Project project = show(4, scene, {screen});

    std::string asked_for = "<never asked>";
    const FrameBuffer green = flat_bgra(4, 4, 0, 200, 0);
    const lcomp::FrameSource frames = [&](const ldoc::Source& source) {
        const auto it = source.settings.find("output");
        const auto connector = it == source.settings.end() ? std::string{} : std::get<std::string>(it->second);
        asked_for = connector;
        return view_of(green, 4, 4);
    };

    const Picture picture =
        lcomp::composite_canvas(project, project.canvases[0].id, frames);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK(picture.gaps.empty());
    // It asked for the connector the source names, not for whatever was first.
    CHECK_EQ(asked_for, std::string{"DP-1"});
    const auto [r, g, b, a] = pixel(picture, 2, 2);
    CHECK_EQ(r, 0);
    CHECK_EQ(g, 200);
    CHECK_EQ(b, 0);
    CHECK_EQ(a, 255);
}

LTEST_CASE(a_screen_with_no_frame_yet_is_a_gap_and_not_a_black_rectangle) {
    // ADR-0013: capture is damage-driven, so a still screen sends nothing and that is correct
    // behaviour rather than a fault. ADR-0040: an absence belongs in the caption, not the pixels.
    const ldoc::Source screen = screen_source("DP-1");
    ldoc::Scene scene = scene_named("program");
    scene.roots = {bound_to(screen)};
    const ldoc::Project project = show(4, scene, {screen});

    const lcomp::FrameSource none = [](const ldoc::Source&) { return lcomp::CapturedPicture{}; };
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id, none);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::NoFrame);
}

LTEST_CASE(shared_enabled_unknown_filters_report_gaps_and_disabled_filters_draw) {
    auto source = colour_source(255, 0, 0, 8, 8);
    auto scene = scene_named("Filters");
    scene.roots.push_back(bound_to(source));
    auto project = show(8, scene, {source});
    ldoc::Effect effect;
    effect.id = ldoc::Id::generate();
    effect.type = "unknown.filter";
    for (const auto owner : {source.id, scene.id}) {
        auto* stack = project.effect_stack(owner);
        REQUIRE(stack);
        *stack = {effect};
        auto picture = lcomp::composite_canvas(project, project.canvases.front().id);
        REQUIRE(!picture.gaps.empty());
        CHECK(picture.gaps.front().reason == GapReason::UnsupportedEffect);
        stack->front().enabled = false;
        picture = lcomp::composite_canvas(project, project.canvases.front().id);
        CHECK(picture.gaps.empty());
        REQUIRE(!picture.pixels.empty());
        CHECK_EQ(picture.pixels.front(), 255);
        stack->clear();
    }
}

LTEST_CASE(shared_color_correction_changes_pixels_and_crop_changes_placement) {
    auto source = colour_source(0, 0, 0, 8, 8);
    auto scene = scene_named("Filtered");
    scene.roots.push_back(bound_to(source));
    auto project = show(8, scene, {source});
    ldoc::Effect effect;
    effect.id = ldoc::Id::generate();
    effect.type = "filter.color-correction";
    effect.params["brightness"] = 0.25;
    project.sources[0].effects = {effect};
    auto image = lcomp::composite_canvas(project, project.canvases[0].id);
    REQUIRE(image.gaps.empty());
    REQUIRE(!image.pixels.empty());
    CHECK_EQ(image.pixels[0], 64);
    project.sources[0].effects[0].enabled = false;
    image = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(image.pixels[0], 0);
    effect.type = "filter.crop";
    effect.params.clear();
    effect.params["left"] = std::int64_t{3};
    project.sources[0].effects = {effect};
    const auto plan = lcomp::plan_canvas(project, project.canvases[0].id);
    REQUIRE(plan.gaps.empty());
    REQUIRE(plan.ops.size() == 1);
    CHECK_EQ(plan.ops[0].rect.x1 - plan.ops[0].rect.x0, 5);
    effect.params["left"] = std::int64_t{100};
    project.sources[0].effects = {effect};
    CHECK(!lcomp::plan_canvas(project, project.canvases[0].id).gaps.empty());
}

LTEST_CASE(a_screen_is_still_an_unsupported_type_when_the_caller_has_no_device) {
    // The overload with no frame source is what every deviceless caller uses, and a screen source
    // has to stay the gap it has always been there -- not NoFrame, which would promise a frame is
    // coming when nothing is capturing at all.
    const ldoc::Source screen = screen_source("DP-1");
    ldoc::Scene scene = scene_named("program");
    scene.roots = {bound_to(screen)};
    const ldoc::Project project = show(4, scene, {screen});

    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::UnsupportedType);
}

LTEST_CASE(an_empty_connector_is_passed_through_as_empty) {
    // ADR-0041 puts "empty means the first screen" in the frame source, which is the half that
    // knows what the compositor advertised. The walk must not invent a connector on the way.
    const ldoc::Source screen = screen_source("");
    ldoc::Scene scene = scene_named("program");
    scene.roots = {bound_to(screen)};
    const ldoc::Project project = show(4, scene, {screen});

    std::string asked_for = "<never asked>";
    const FrameBuffer any = flat_bgra(4, 4, 10, 20, 30);
    const lcomp::FrameSource frames = [&](const ldoc::Source& source) {
        const auto it = source.settings.find("output");
        const auto connector = it == source.settings.end() ? std::string{} : std::get<std::string>(it->second);
        asked_for = connector;
        return view_of(any, 4, 4);
    };
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id, frames);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(asked_for, std::string{""});
}

LTEST_CASE(a_frame_shorter_than_it_claims_is_refused_rather_than_read_past) {
    // A buffer that does not hold the pixels its header promises is a bug on the device side.
    // Drawing from it would read off the end, so it is refused like any other missing frame.
    const ldoc::Source screen = screen_source("DP-1");
    ldoc::Scene scene = scene_named("program");
    scene.roots = {bound_to(screen)};
    const ldoc::Project project = show(4, scene, {screen});

    const FrameBuffer two_pixels = flat_bgra(1, 2, 255, 0, 0);
    const lcomp::FrameSource lying = [&](const ldoc::Source&) {
        // Claims 4x4, carries two pixels.
        return view_of(two_pixels, 4, 4);
    };
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id, lying);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::NoFrame);
}

LTEST_CASE(a_screen_inside_a_precomp_uses_the_same_frame_provider) {
    auto screen = screen_source("HDMI-A-2");
    auto inner = scene_named("inner");
    inner.roots = {bound_to(screen)};
    auto outer = scene_named("outer");
    auto precomp = ldoc::make_node(ldoc::NodeKind::Precomp, "nested");
    std::get<ldoc::PrecompData>(precomp.data).scene = inner.id;
    outer.roots = {precomp};
    auto project = show(4, outer, {screen});
    inner.canvas = project.canvases[0].id;
    project.scenes.push_back(inner);
    const auto pixels = flat_bgra(4, 4, 13, 117, 231);
    const Picture picture =
        lcomp::composite_canvas(project, project.canvases[0].id, [&](const ldoc::Source& source) {
        const auto it = source.settings.find("output");
        const auto connector = it == source.settings.end() ? std::string{} : std::get<std::string>(it->second);
            CHECK_EQ(connector, std::string("HDMI-A-2"));
            return view_of(pixels, 4, 4);
        });
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 2, 2), std::tuple(13, 117, 231, 255));
}

LTEST_CASE(extreme_placement_is_bounded_before_integer_conversion_or_axis_allocation) {
    auto screen = screen_source("screen");
    auto scene = scene_named("extreme transform");
    auto node = bound_to(screen);
    node.layout.fit = ldoc::FitMode::Stretch;
    node.layout.size = {1e7, 1e7};
    scene.roots = {node};
    auto project = show(4, scene, {screen});
    const auto pixels = flat_bgra(4, 4, 13, 117, 231);
    lcomp::FrameSource frames = [&](const ldoc::Source&) { return view_of(pixels, 4, 4); };
    auto picture = lcomp::composite_canvas(project, project.canvases[0].id, frames);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(13, 117, 231, 255));
    project.scenes[0].roots[0].transform.position.x = 1e100;
    picture = lcomp::composite_canvas(project, project.canvases[0].id, frames);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::UnsupportedTransform);
}

LTEST_CASE(cropping_samples_only_the_retained_source_pixels) {
    auto source = screen_source("crop");
    auto scene = scene_named("crop");
    auto node = bound_to(source);
    node.layout.crop = {1, 0, 1, 0};
    node.layout.fit = ldoc::FitMode::Stretch;
    node.layout.size = {4, 4};
    scene.roots = {node};
    auto project = show(4, scene, {source});
    const std::vector<std::uint8_t> pixels = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255};
    const auto picture = lcomp::composite_canvas(project, project.canvases[0].id,
        [&](const ldoc::Source&) { return lcomp::CapturedPicture{3, 1, pixels.data(), pixels.size()}; });
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 255, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(0, 255, 0, 255));
}

LTEST_CASE(source_clones_follow_shared_inputs_and_keep_independent_filters) {
    auto input = colour_source(0, 0, 0, 4, 4);
    ldoc::Effect correction;
    correction.id = ldoc::Id::generate();
    correction.type = "filter.color-correction";
    correction.params["brightness"] = 0.25;
    input.effects = {correction};
    auto clone = colour_source(0, 0, 0, 4, 4);
    clone.type = "source.clone";
    clone.settings["target"] = input.id.to_string();
    auto scene = scene_named("Clone only");
    scene.roots = {bound_to(clone)};
    auto project = show(4, scene, {input, clone});
    auto image = lcomp::composite_canvas(project, project.canvases[0].id);
    REQUIRE(image.gaps.empty());
    CHECK_EQ(image.pixels[0], 64);
    project.sources[1].settings["noFilters"] = true;
    image = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(image.pixels[0], 0);
    correction.id = ldoc::Id::generate();
    correction.params["brightness"] = 0.5;
    project.sources[1].effects = {correction};
    image = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(image.pixels[0], 128);
    project.sources[0].settings["color"] = ldoc::Rgba::from_srgb8(255, 0, 0);
    image = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(image.pixels[0], 255);
    project.sources[0].name = "Renamed input";
    CHECK(ldoc::resolve_source(project, project.sources[1]).has_value());
    project.sources.erase(project.sources.begin());
    image = lcomp::composite_canvas(project, project.canvases[0].id);
    REQUIRE(!image.gaps.empty());
    CHECK_EQ(image.gaps.front().reason, GapReason::NoSource);
}

LTEST_CASE(source_clones_share_capture_identity_and_reject_cycles) {
    auto input = screen_source("DP-1");
    auto clone = input;
    clone.id = ldoc::Id::generate();
    clone.type = "source.clone";
    clone.settings["target"] = input.id.to_string();
    auto scene = scene_named("Clone");
    scene.roots = {bound_to(clone)};
    auto project = show(4, scene, {input, clone});
    const auto pixels = flat_bgra(4, 4, 255, 0, 0);
    int requested = 0;
    const auto image = lcomp::composite_canvas(project, project.canvases[0].id,
        [&](const ldoc::Source& source) {
            CHECK_EQ(source.id, input.id);
            CHECK_EQ(source.type, input.type);
            ++requested;
            return view_of(pixels, 4, 4);
        });
    CHECK_EQ(requested, 1);
    CHECK_EQ(pixel(image, 0, 0), std::tuple(255, 0, 0, 255));
    project.sources[0].type = "source.clone";
    project.sources[0].settings["target"] = clone.id.to_string();
    CHECK(!ldoc::resolve_source(project, project.sources[1]));
    const auto cyclic = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK(!cyclic.gaps.empty());
    CHECK(!ldoc::has_audio(project.sources[1]));
    project.sources[1].settings["audio"] = true;
    CHECK(ldoc::has_audio(project.sources[1]));
}

LTEST_MAIN("composite")

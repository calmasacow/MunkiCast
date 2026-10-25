// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ltest.hpp"

#include <QGuiApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <fstream>
#include <json/json.h>
#include <lcomp/composite.hpp>
#include <lcomp/filters.hpp>
#include <ldoc/project_io.hpp>
#include <limport/obs.hpp>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using J = Json::Value;
namespace fs = std::filesystem;

J source(const char* name, const char* id, const char* uuid) {
    J s;
    s["name"] = name;
    s["id"] = id;
    s["uuid"] = uuid;
    s["settings"] = J(Json::objectValue);
    return s;
}

J item(const char* name, const char* uuid, int x = 0, int y = 0) {
    J i;
    i["name"] = name;
    i["source_uuid"] = uuid;
    i["align"] = 5;
    i["pos"]["x"] = x;
    i["pos"]["y"] = y;
    return i;
}

J collection() {
    J root;
    root["name"] = "Imported show";
    auto red = source("red", "color_source", "red");
    red["settings"]["color"] = Json::UInt(0xff0000ff);
    red["settings"]["width"] = 4;
    red["settings"]["height"] = 4;
    auto green = source("green", "color_source", "green");
    green["settings"]["color"] = Json::UInt(0xff00ff00);
    green["settings"]["width"] = 2;
    green["settings"]["height"] = 2;
    auto scene = source("Show", "scene", "show");
    scene["settings"]["items"].append(item("red", "red"));
    scene["settings"]["items"].append(item("green", "green", 1, 1));
    root["sources"].append(red);
    root["sources"].append(green);
    root["sources"].append(scene);
    J order;
    order["name"] = "Show";
    root["scene_order"].append(order);
    root["current_scene"] = "Show";
    root["current_program_scene"] = "Show";
    return root;
}

struct Scratch {
    QTemporaryDir temp;
    fs::path input = fs::path(temp.path().toStdString()) / "obs.json";
    fs::path output = fs::path(temp.path().toStdString()) / "Imported.libertas";

    void write(const J& root) { std::ofstream(input) << root; }

    auto convert() { return limport::import_obs(input, output, {4, 4, {60000, 1001}}); }
};

std::array<int, 3> pixel(const lcomp::Picture& p, int x, int y) {
    const auto offset = static_cast<std::size_t>(y) * p.stride + static_cast<std::size_t>(x) * 4;
    return {p.pixels[offset], p.pixels[offset + 1], p.pixels[offset + 2]};
}
} // namespace

LTEST_CASE(source_clone_import_export_preserves_input_identity_and_options) {
    Scratch scratch;
    auto root = collection();
    auto clone = source("Clone green", "source-clone", "clone-green");
    clone["settings"]["clone"] = "green";
    clone["settings"]["audio"] = true;
    clone["settings"]["no_filters"] = true;
    auto placement = item("Clone green", "clone-green", 2, 2);
    placement["align"] = 0;
    root["sources"][2]["settings"]["items"] = J(Json::arrayValue);
    root["sources"][2]["settings"]["items"].append(placement);
    root["sources"].append(clone);
    scratch.write(root);
    auto imported = scratch.convert();
    REQUIRE_MSG(imported.has_value(), "{}", imported.error());
    const auto find_clone = [](const ldoc::Project& project) -> const ldoc::Source* {
        for (const auto& source : project.sources)
            if (source.type == "source.clone") return &source;
        return nullptr;
    };
    const auto* native = find_clone(imported->project);
    REQUIRE(native);
    auto input = ldoc::resolve_source(imported->project, *native);
    REQUIRE(input);
    CHECK_EQ(input->name, std::string("green"));
    const auto picture = lcomp::composite_canvas(imported->project, imported->project.canvases[0].id);
    CHECK_EQ(pixel(picture, 2, 2), (std::array<int, 3>{0, 255, 0}));
    CHECK_EQ(pixel(picture, 0, 0), (std::array<int, 3>{0, 0, 0}));
    CHECK(ldoc::has_audio(*native));
    CHECK(std::get<bool>(native->settings.at("noFilters")));
    const auto output = scratch.output.string() + ".obs.json";
    auto reordered = imported->project;
    for (auto& source : reordered.sources)
        if (source.id == input->id) source.name = "Show";
    std::reverse(reordered.sources.begin(), reordered.sources.end());
    REQUIRE(limport::export_obs(reordered, scratch.output, output));
    const auto again = limport::import_obs(output, output + ".libertas", {4, 4, {60, 1}});
    REQUIRE_MSG(again.has_value(), "{}", again.error());
    native = find_clone(again->project);
    REQUIRE(native);
    input = ldoc::resolve_source(again->project, *native);
    REQUIRE(input);
    CHECK_EQ(input->name, std::string("Show (2)"));
    const auto roundtrip = lcomp::composite_canvas(again->project, again->project.canvases[0].id);
    CHECK_EQ(roundtrip.pixels, picture.pixels);
}

LTEST_CASE(import_creates_reopenable_project_and_preserves_stack_and_identity) {
    Scratch scratch;
    auto root = collection();
    auto other = source("Other", "scene", "other");
    other["settings"]["items"].append(item("red", "red"));
    root["sources"].append(other);
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE_MSG(converted.has_value(), "{}", converted.error());
    auto opened = ldoc::open_project(scratch.output);
    REQUIRE_MSG(opened.has_value(), "{}", opened.error());
    auto& p = opened->project;
    CHECK_EQ(p.sources.size(), 2u);
    CHECK_EQ(p.scenes.size(), 2u);
    CHECK_EQ(p.canvases[0].fps, (ldoc::Fps{60000, 1001}));
    CHECK_EQ(p.scenes[0].roots[0].name, std::string("green"));
    CHECK_EQ(std::get<ldoc::SourceData>(p.scenes[0].roots[1].data).source,
             std::get<ldoc::SourceData>(p.scenes[1].roots[0].data).source);
    for (const auto& problem : opened->problems)
        CHECK(problem.severity != ldoc::Severity::Error);
    const auto picture = lcomp::composite_canvas(p, p.canvases[0].id);
    REQUIRE(picture.status == lcomp::PictureStatus::Complete);
    CHECK(pixel(picture, 0, 0) == (std::array<int, 3>{255, 0, 0}));
    CHECK(pixel(picture, 1, 1) == (std::array<int, 3>{0, 255, 0}));
    CHECK(fs::exists(scratch.output / "obs-original.json"));
    CHECK(fs::exists(scratch.output / "obs-import-report.md"));
    CHECK_EQ(fs::file_size(scratch.input), fs::file_size(scratch.output / "obs-original.json"));
}

LTEST_CASE(file_backed_stinger_imports_timing_and_roundtrips_to_obs) {
    Scratch scratch;
    auto root = collection();
    auto transition = source("Stinger", "obs_stinger_transition", "stinger");
    transition["settings"]["path"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-tone.webm";
    transition["settings"]["transition_point"] = 400;
    root["transitions"].append(transition);
    root["current_transition"] = "Stinger";
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE(converted);
    const auto find = [](const auto& project) {
        return std::ranges::find(project.sources, std::string("Stinger"), &ldoc::Source::name);
    };
    const auto imported = find(converted->project);
    REQUIRE(imported != converted->project.sources.end());
    CHECK_EQ(std::get<std::int64_t>(imported->settings.at("stingerCutMs")), 400);
    CHECK_EQ(converted->project.decks.front().transition, "transition.stinger:" + imported->id.to_string());
    const auto output = fs::path(scratch.temp.path().toStdString()) / "stinger.json";
    REQUIRE(limport::export_obs(converted->project, scratch.output, output));
    const auto again = limport::import_obs(output, output.string() + ".libertas", {4, 4, {60, 1}});
    REQUIRE(again);
    const auto saved = find(again->project);
    REQUIRE(saved != again->project.sources.end());
    CHECK_EQ(std::get<std::int64_t>(saved->settings.at("stingerCutMs")), 400);
}

LTEST_CASE(media_hardware_decoding_policy_survives_obs_roundtrip) {
    for (bool hardware : {false, true}) {
        Scratch scratch;
        auto root = collection();
        auto media = source("Clip", "ffmpeg_source", "clip");
        media["settings"]["hw_decode"] = hardware;
        root["sources"].append(media);
        scratch.write(root);
        auto converted = scratch.convert();
        REQUIRE(converted);
        auto find_media = [](const auto& project) {
            return std::ranges::find(project.sources, std::string("source.media"),
                                     &ldoc::Source::type);
        };
        const auto imported = find_media(converted->project);
        REQUIRE(imported != converted->project.sources.end());
        CHECK_EQ(std::get<bool>(imported->settings.at("hardwareDecode")), hardware);
        CHECK_EQ(std::get<bool>(imported->settings.at("restartOnActivate")), true);
        CHECK_EQ(std::get<bool>(imported->settings.at("closeWhenInactive")), false);
        const auto output = fs::path(scratch.temp.path().toStdString()) / "export.json";
        REQUIRE(limport::export_obs(converted->project, scratch.output, output));
        const auto again =
            limport::import_obs(output, output.string() + ".libertas", {4, 4, {60, 1}});
        REQUIRE(again);
        const auto reimported = find_media(again->project);
        REQUIRE(reimported != again->project.sources.end());
        CHECK_EQ(std::get<bool>(reimported->settings.at("hardwareDecode")), hardware);
    }
}

LTEST_CASE(imported_color_filter_draws_and_exports_with_name_order_and_disabled_state) {
    Scratch scratch;
    auto root = collection();
    J effect;
    effect["id"] = "color_filter";
    effect["versioned_id"] = "color_filter_v2";
    effect["name"] = "Brighten";
    effect["enabled"] = true;
    effect["settings"]["brightness"] = 0.25;
    root["sources"][0]["filters"].append(effect);
    effect["name"] = "Disabled correction";
    effect["enabled"] = false;
    effect["settings"]["contrast"] = 3.73;
    root["sources"][0]["filters"].append(effect);
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE(converted);
    const auto& p = converted->project;
    const auto* red = &p.sources.front();
    REQUIRE(red->effects.size() == 2);
    CHECK_EQ(red->effects[0].name, std::string("Brighten"));
    CHECK(!red->effects[1].enabled);
    CHECK(lcomp::validate_filter(red->effects[1]));
    const auto image = lcomp::composite_canvas(p, p.canvases.front().id);
    REQUIRE(image.gaps.empty());
    CHECK(pixel(image, 0, 0)[1] > 40);
    const auto output = fs::path(scratch.temp.path().toStdString()) / "export.json";
    REQUIRE(limport::export_obs(p, scratch.output, output));
    const auto again = limport::import_obs(output, output.string() + ".libertas", {4, 4, {60, 1}});
    REQUIRE(again);
    REQUIRE(again->project.sources.front().effects.size() == 2);
    CHECK_EQ(again->project.sources.front().effects[0].name, std::string("Brighten"));
    CHECK(!again->project.sources.front().effects[1].enabled);
    const auto reopened = ldoc::open_project(scratch.output);
    REQUIRE(reopened);
    CHECK(reopened->project.sources.front().effects == red->effects);
}

LTEST_CASE(imported_scale_mask_and_scroll_preserve_settings_and_render_after_roundtrip) {
    Scratch scratch;
    auto root = collection();
    J scale;
    scale["id"] = "scale_filter";
    scale["settings"]["resolution"] = "16:9";
    scale["settings"]["sampling"] = "lanczos";
    root["sources"][0]["filters"].append(scale);
    J mask;
    mask["id"] = "mask_filter";
    mask["versioned_id"] = "mask_filter_v2";
    mask["settings"]["image_path"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/two.png";
    mask["settings"]["color"] = Json::UInt(0xff804020);
    mask["settings"]["stretch"] = false;
    root["sources"][0]["filters"].append(mask);
    J scroll;
    scroll["id"] = "scroll_filter";
    scroll["settings"]["limit_cx"] = true;
    scroll["settings"]["cx"] = 3;
    root["sources"][0]["filters"].append(scroll);
    scratch.write(root);
    auto imported = scratch.convert();
    REQUIRE(imported);
    const auto& effects = imported->project.sources[0].effects;
    REQUIRE(effects.size() == 3);
    CHECK(lcomp::filter_flag(effects[0], "aspect_only"));
    CHECK_EQ(lcomp::filter_text(effects[0], "sampling"), std::string("lanczos"));
    CHECK_EQ(lcomp::filter_number(effects[1], "color"), double(0x204080));
    CHECK(!lcomp::filter_flag(effects[1], "stretch"));
    CHECK(lcomp::filter_flag(effects[2], "limit_cx"));
    CHECK(!lcomp::filter_flag(effects[2], "limit_cy"));
    const auto before =
        lcomp::composite_canvas(imported->project, imported->project.canvases[0].id);
    REQUIRE(before.gaps.empty());
    const auto output = fs::path(scratch.temp.path().toStdString()) / "export.json";
    REQUIRE(limport::export_obs(imported->project, scratch.output, output));
    auto again = limport::import_obs(output, output.string() + ".libertas", {4, 4, {60, 1}});
    REQUIRE(again);
    const auto after = lcomp::composite_canvas(again->project, again->project.canvases[0].id);
    CHECK(after.gaps.empty());
    CHECK(before.pixels == after.pixels);
}

LTEST_CASE(invalid_lut_reports_a_gap_and_correcting_the_path_restores_rendering) {
    Scratch scratch;
    const auto invalid = fs::path(scratch.temp.path().toStdString()) / "invalid.cube";
    std::ofstream(invalid) << "LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n0 0 0\n";
    auto root = collection();
    J lut;
    lut["id"] = "clut_filter";
    lut["settings"]["image_path"] = invalid.string();
    root["sources"][0]["filters"].append(lut);
    scratch.write(root);
    auto imported = scratch.convert();
    REQUIRE(imported);
    auto& project = imported->project;
    const auto before = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK(!before.gaps.empty());
    project.sources[0].effects[0].params["path"] =
        std::string(LIBERTAS_IMAGE_FIXTURES) + "/identity.cube";
    const auto after = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK(after.gaps.empty());
    CHECK(pixel(after, 0, 0)[0] > 250);
}

LTEST_CASE(all_obs_blend_modes_survive_import_render_export_and_reimport) {
    using M = ldoc::BlendMode;
    const std::array modes{M::Normal,   M::LinearDodge, M::Subtract, M::Screen,
                           M::Multiply, M::Lighten,     M::Darken};
    const std::array names{"normal",   "additive", "subtract", "screen",
                           "multiply", "lighten",  "darken"};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        Scratch scratch;
        auto root = collection();
        root["sources"][2]["settings"]["items"][1]["blend_type"] = names[i];
        scratch.write(root);
        auto imported = scratch.convert();
        REQUIRE_MSG(imported, "{}", imported.error());
        const auto& project = imported->project;
        CHECK_EQ(project.scenes[0].roots[0].blend.mode, modes[i]);
        const auto before = lcomp::composite_canvas(project, project.canvases[0].id);
        CHECK(before.gaps.empty());
        const auto output = fs::path(scratch.temp.path().toStdString()) / "export.json";
        const auto exported = limport::export_obs(project, scratch.output, output);
        REQUIRE_MSG(exported, "{}", exported.error());
        const auto reimported =
            limport::import_obs(output, output.string() + ".libertas", {4, 4, {60, 1}});
        REQUIRE_MSG(reimported, "{}", reimported.error());
        CHECK_EQ(reimported->project.scenes[0].roots[0].blend.mode, modes[i]);
        const auto after =
            lcomp::composite_canvas(reimported->project, reimported->project.canvases[0].id);
        CHECK_EQ(before.pixels, after.pixels);
    }
}

LTEST_CASE(import_crops_actual_image_pixels_and_preserves_flipped_bounds) {
    Scratch scratch;
    auto root = collection();
    auto image = source("image", "image_source", "image");
    image["settings"]["file"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/two.png";
    root["sources"].append(image);
    auto i = item("image", "image");
    i["crop_left"] = 1;
    i["crop_bottom"] = 1;
    i["bounds_type"] = 1;
    i["bounds"]["x"] = 4;
    i["bounds"]["y"] = 4;
    i["scale"]["x"] = -1;
    i["scale"]["y"] = 1;
    root["sources"][2]["settings"]["items"] = J(Json::arrayValue);
    root["sources"][2]["settings"]["items"].append(i);
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE_MSG(converted.has_value(), "{}", converted.error());
    const auto& p = converted->project;
    const auto picture = lcomp::composite_canvas(p, p.canvases[0].id);
    REQUIRE(picture.status == lcomp::PictureStatus::Complete);
    // Only the green top-right pixel remains, stretched across the flipped bounds.
    CHECK(pixel(picture, 0, 0) == (std::array<int, 3>{0, 255, 0}));
    CHECK(pixel(picture, 3, 3) == (std::array<int, 3>{0, 255, 0}));
}

LTEST_CASE(import_group_skips_backup_items_and_renders_saved_tight_bounds) {
    Scratch scratch;
    auto root = collection();
    auto group = source("Group", "group", "group");
    group["settings"]["cx"] = 2;
    group["settings"]["cy"] = 2;
    group["settings"]["items"].append(item("green", "green"));
    root["groups"].append(group);
    auto backup = item("green", "green");
    backup["group_item_backup"] = true;
    auto placed = item("Group", "group", 2, 2);
    placed["locked"] = true;
    auto& items = root["sources"][2]["settings"]["items"];
    items = J(Json::arrayValue);
    items.append(backup);
    items.append(placed);
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE_MSG(converted.has_value(), "{}", converted.error());
    const auto& p = converted->project;
    REQUIRE(p.scenes.size() == 1);
    REQUIRE(p.scenes[0].roots.size() == 1);
    REQUIRE(p.scenes[0].roots[0].is_group());
    const auto& native = std::get<ldoc::GroupData>(p.scenes[0].roots[0].data);
    REQUIRE(native.size.has_value());
    CHECK(*native.size == (ldoc::Vec2{2, 2}));
    CHECK_EQ(native.children.size(), 1u);
    CHECK(p.scenes[0].roots[0].locked);
    const auto picture = lcomp::composite_canvas(p, p.canvases[0].id);
    REQUIRE(picture.status == lcomp::PictureStatus::Complete);
    CHECK(pixel(picture, 0, 0) == (std::array<int, 3>{0, 0, 0}));
    CHECK(pixel(picture, 3, 3) == (std::array<int, 3>{0, 255, 0}));
    auto reopened = ldoc::open_project(scratch.output);
    REQUIRE(reopened);
    CHECK(reopened->project.scenes[0].roots == p.scenes[0].roots);
}

LTEST_CASE(import_omits_global_audio_and_wrappers_but_keeps_scene_audio_and_groups) {
    Scratch scratch;
    auto root = collection();
    auto wrapper = source("Obsolete", "streamelements_transition_audio_wrapper_source", "obsolete");
    root["AuxAudioDevice1"] = wrapper;
    auto mic = source("Microphone", "pulse_input_capture", "mic");
    mic["settings"]["device_id"] = "default";
    root["sources"].append(mic);
    root["AuxAudioDevice2"] = source("Global mic", "pulse_input_capture", "global-mic");
    auto group = source("Group", "group", "group");
    group["settings"]["cx"] = 2;
    group["settings"]["cy"] = 2;
    group["settings"]["items"].append(item("green", "green"));
    group["settings"]["items"].append(item("Obsolete", "obsolete"));
    root["groups"].append(group);
    auto& items = root["sources"][2]["settings"]["items"];
    items = J(Json::arrayValue);
    items.append(item("Group", "group"));
    items.append(item("Group", "group", 2, 2));
    items.append(item("Obsolete", "obsolete"));
    items.insert(0, item("Microphone", "mic"));
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE_MSG(converted, "{}", converted.error());
    const auto& p = converted->project;
    REQUIRE(p.scenes.size() == 1);
    REQUIRE(p.scenes[0].roots.size() == 3);
    const auto& first = std::get<ldoc::GroupData>(p.scenes[0].roots[0].data);
    const auto& second = std::get<ldoc::GroupData>(p.scenes[0].roots[1].data);
    REQUIRE(first.children.size() == 1 && second.children.size() == 1);
    CHECK(first.children[0].id != second.children[0].id);
    CHECK(std::get<ldoc::SourceData>(first.children[0].data).source ==
          std::get<ldoc::SourceData>(second.children[0].data).source);
    CHECK_EQ(p.sources.size(), 3u);
    CHECK_EQ(p.buses.size(), 1u);
    CHECK_EQ(p.scenes[0].roots.back().name, std::string("Microphone"));
    const auto picture = lcomp::composite_canvas(p, p.canvases[0].id);
    REQUIRE(picture.status == lcomp::PictureStatus::Complete);
    CHECK(pixel(picture, 0, 0) == (std::array<int, 3>{0, 255, 0}));
    CHECK(pixel(picture, 3, 3) == (std::array<int, 3>{0, 255, 0}));
    CHECK(std::ranges::any_of(converted->warnings, [](const auto& warning) {
        return warning.find("StreamElements") != std::string::npos;
    }));
}

LTEST_CASE(import_rejects_group_expansion_and_surface_limits_before_writing) {
    for (const bool oversized : {false, true}) {
        Scratch scratch;
        auto root = collection();
        std::string previous = "green";
        for (int i = 0; i < (oversized ? 1 : 17); ++i) {
            const auto name = "group" + std::to_string(i);
            auto group = source(name.c_str(), "group", name.c_str());
            group["settings"]["cx"] = oversized ? 8192 : 2;
            group["settings"]["cy"] = oversized ? 8192 : 2;
            group["settings"]["items"].append(item(previous.c_str(), previous.c_str()));
            group["settings"]["items"].append(item(previous.c_str(), previous.c_str()));
            root["groups"].append(group);
            previous = name;
        }
        root["sources"][2]["settings"]["items"].append(item(previous.c_str(), previous.c_str()));
        scratch.write(root);
        const auto result = scratch.convert();
        REQUIRE(!result);
        CHECK(result.error().find(oversized ? "32M pixels" : "100000 nodes") != std::string::npos);
        CHECK(!fs::exists(scratch.output));
    }
}

LTEST_CASE(import_centered_quarter_turns_keep_the_obs_anchor) {
    for (int turn = 0; turn < 4; ++turn) {
        Scratch scratch;
        auto root = collection();
        auto i = item("green", "green", 2, 2);
        i["align"] = 0;
        i["rot"] = turn * 90;
        auto& items = root["sources"][2]["settings"]["items"];
        items = J(Json::arrayValue);
        items.append(i);
        scratch.write(root);
        auto converted = scratch.convert();
        REQUIRE_MSG(converted.has_value(), "{}", converted.error());
        const auto& p = converted->project;
        const auto picture = lcomp::composite_canvas(p, p.canvases[0].id);
        REQUIRE(picture.status == lcomp::PictureStatus::Complete);
        CHECK(pixel(picture, 0, 0) == (std::array<int, 3>{0, 0, 0}));
        CHECK(pixel(picture, 1, 1) == (std::array<int, 3>{0, 255, 0}));
        CHECK(pixel(picture, 2, 2) == (std::array<int, 3>{0, 255, 0}));
        CHECK(pixel(picture, 3, 3) == (std::array<int, 3>{0, 0, 0}));
    }
}

LTEST_CASE(import_resolves_main_scene_name_against_its_obs_canvas) {
    Scratch scratch;
    auto root = collection();
    root["sources"][2]["canvas_uuid"] = "6c69626f-6273-4c00-9d88-c5136d61696e";
    auto vertical = source("Show", "scene", "vertical");
    vertical["canvas_uuid"] = "another-canvas";
    root["sources"].append(vertical);
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE_MSG(converted.has_value(), "{}", converted.error());
    const auto& p = converted->project;
    CHECK_EQ(p.scenes.size(), 2u);
    CHECK_EQ(p.decks[0].program_scene, std::optional(p.scenes[0].id));
    CHECK_EQ(p.scenes[1].name, std::string("Show"));
    REQUIRE(p.canvases.size() == 2);
    CHECK_EQ(p.scenes[1].canvas, p.canvases[1].id);
    CHECK_EQ(p.decks[1].program_scene, std::optional(p.scenes[1].id));
}

LTEST_CASE(import_builds_independent_vertical_canvas_and_preserves_shared_sources) {
    Scratch scratch;
    auto root = collection();
    J canvas;
    canvas["info"]["uuid"] = "portrait";
    canvas["info"]["name"] = "Aitum Vertical";
    root["canvases"].append(canvas);
    auto vertical = source("Portrait scene", "scene", "portrait-scene");
    vertical["canvas_uuid"] = "portrait";
    vertical["settings"]["custom_size"] = true;
    vertical["settings"]["cx"] = 2;
    vertical["settings"]["cy"] = 6;
    vertical["settings"]["items"].append(item("red", "red"));
    root["sources"].append(vertical);
    scratch.write(root);
    auto imported = scratch.convert();
    REQUIRE_MSG(imported, "{}", imported.error());
    const auto reopened = ldoc::open_project(scratch.output);
    REQUIRE_MSG(reopened, "{}", reopened.error());
    const auto& p = reopened->project;
    REQUIRE(p.canvases.size() == 2);
    CHECK_EQ(p.canvases[1].name, std::string("Aitum Vertical"));
    CHECK_EQ(p.canvases[1].width, 2);
    CHECK_EQ(p.canvases[1].height, 6);
    CHECK_EQ(p.canvases[1].fps, p.canvases[0].fps);
    CHECK_EQ(p.scenes.back().canvas, p.canvases[1].id);
    CHECK_EQ(std::get<ldoc::SourceData>(p.scenes.back().roots[0].data).source,
             std::get<ldoc::SourceData>(p.scenes[0].roots.back().data).source);
    const auto portrait = lcomp::composite_canvas(p, p.canvases[1].id);
    CHECK_EQ(portrait.width, 2);
    CHECK_EQ(portrait.height, 6);
    CHECK(portrait.gaps.empty());
    CHECK(pixel(portrait, 1, 1) == (std::array<int, 3>{255, 0, 0}));
    CHECK(pixel(portrait, 1, 5) == (std::array<int, 3>{0, 0, 0}));
    const auto main = lcomp::composite_canvas(p, p.canvases[0].id);
    CHECK(pixel(main, 1, 1) == (std::array<int, 3>{0, 255, 0}));
}

LTEST_CASE(import_refuses_conflicting_and_oversized_extra_canvas_geometry) {
    for (const bool oversized : {false, true}) {
        Scratch scratch;
        auto root = collection();
        for (int i = 0; i < 2; ++i) {
            const auto name = "portrait" + std::to_string(i);
            auto scene = source(name.c_str(), "scene", name.c_str());
            scene["canvas_uuid"] = "portrait";
            scene["settings"]["custom_size"] = true;
            scene["settings"]["cx"] = oversized ? 8192 : 2 + i;
            scene["settings"]["cy"] = oversized ? 8192 : 6;
            root["sources"].append(scene);
        }
        scratch.write(root);
        CHECK(!scratch.convert());
        CHECK(!fs::exists(scratch.output));
    }
}

LTEST_CASE(import_maps_scene_sources_without_obs_mixer_or_global_audio_devices) {
    Scratch scratch;
    auto root = collection();
    const std::pair<const char*, const char*> mappings[] = {
        {"text_ft2_source", "source.text"},
        {"ffmpeg_source", "source.media"},
        {"vlc_source", "source.playlist"},
        {"slideshow", "source.slideshow"},
        {"browser_source", "source.browser"},
        {"v4l2_input", "source.v4l2"},
        {"pulse_input_capture", "source.audio-input"},
        {"pulse_output_capture", "source.audio-output"},
        {"pipewire-desktop-capture-source", "source.screen"},
        {"game_capture", "source.game"},
        {"xcomposite_input", "source.window"},
        {"decklink-input", "source.blackmagic"},
        {"vendor_plugin", "source.obs-unsupported"}};
    for (const auto& [obs, native] : mappings) {
        (void)native;
        root["sources"].append(source(obs, obs, obs));
    }
    auto global = source("Global microphone", "pulse_input_capture", "mic");
    global["volume"] = 0.5;
    global["muted"] = true;
    global["sync"] = 100000000;
    global["mixers"] = 5;
    root["sources"].append(global);
    auto& items = root["sources"][2]["settings"]["items"];
    items.insert(0, item("Global microphone", "mic"));
    root["AuxAudioDevice1"] = source("Excluded global", "pulse_input_capture", "global");
    root["DesktopAudioDevice1"] = source("Excluded desktop", "pulse_output_capture", "desktop");
    scratch.write(root);
    auto converted = scratch.convert();
    REQUIRE_MSG(converted.has_value(), "{}", converted.error());
    auto& p = converted->project;
    for (const auto& [obs, native] : mappings) {
        auto found = std::ranges::find(p.sources, std::string(obs), &ldoc::Source::name);
        REQUIRE(found != p.sources.end());
        CHECK_EQ(found->type, std::string(native));
        CHECK(found->settings.contains("obs.original"));
    }
    const auto& mic = std::get<ldoc::SourceData>(p.scenes[0].roots.back().data);
    REQUIRE(mic.audio.bus.has_value());
    const auto* bus = p.bus(*mic.audio.bus);
    REQUIRE(bus);
    CHECK_EQ(p.buses.size(), 1u);
    CHECK(bus->kind == ldoc::BusKind::Program);
    CHECK(mic.audio.mute);
    CHECK_NEAR(mic.audio.gain_db, -6.0206, 0.001);
    CHECK(mic.audio.sends.empty());
    CHECK(std::ranges::none_of(p.sources,
                               [](const auto& s) { return s.name.starts_with("Excluded"); }));
}

LTEST_CASE(import_preserves_video_format_buffering_and_timeout_settings) {
    for (double frames : {7.0, 1.0, 121.0, 2.5}) {
        Scratch scratch;
        auto root = collection();
        auto camera = source("Capture options", "v4l2_input", "capture-options");
        camera["settings"]["pixelformat"] = Json::UInt{0x3231564e}; // NV12
        camera["settings"]["buffering"] = false;
        camera["settings"]["auto_reset"] = true;
        camera["settings"]["timeout_frames"] = frames;
        root["sources"].append(camera);
        scratch.write(root);
        auto converted = scratch.convert();
        if (frames != 7.0) {
            CHECK(!converted.has_value());
            continue;
        }
        REQUIRE_MSG(converted.has_value(), "{}", converted.error());
        const auto& sources = converted->project.sources;
        const auto found = std::ranges::find(sources, std::string("Capture options"), &ldoc::Source::name);
        REQUIRE(found != sources.end());
        CHECK_EQ(std::get<std::string>(found->settings.at("videoFormat")), std::string("NV12"));
        CHECK(!std::get<bool>(found->settings.at("buffering")));
        CHECK(std::get<bool>(found->settings.at("autoReset")));
        CHECK_EQ(std::get<std::int64_t>(found->settings.at("timeoutFrames")), 7);
    }
}

LTEST_CASE(import_refuses_malformed_references_cycles_and_existing_destinations) {
    for (int mode = 0; mode < 7; ++mode) {
        Scratch scratch;
        auto root = collection();
        if (mode == 0)
            root["sources"][2]["settings"]["items"][0]["source_uuid"] = "absent";
        if (mode == 1)
            root["sources"][2]["settings"]["items"][0]["source_uuid"] = "show";
        if (mode == 2)
            root["sources"][0]["uuid"] = "green";
        if (mode == 3)
            root["sources"][0]["settings"]["width"] = -1;
        if (mode == 4)
            root["sources"][2]["settings"]["items"][0]["pos"]["x"] = "wrong";
        if (mode == 5)
            root["sources"][2]["settings"]["items"][0]["scale"]["x"] = 1e20;
        scratch.write(root);
        if (mode == 6)
            std::ofstream(scratch.input) << "{\"sources\":[],\"sources\":[]}";
        CHECK(!scratch.convert());
        CHECK(!fs::exists(scratch.output));
    }
    Scratch scratch;
    scratch.write(collection());
    fs::create_directory(scratch.output);
    std::ofstream(scratch.output / "keep") << "untouched";
    CHECK(!scratch.convert());
    CHECK_EQ(fs::file_size(scratch.output / "keep"), 9u);
    CHECK(!limport::import_obs(scratch.input, scratch.output / "new", {}));
}

LTEST_CASE(import_rejects_deep_scene_chains_even_in_leaf_first_order) {
    Scratch scratch;
    auto root = collection();
    std::string previous = "show";
    for (int i = 0; i < 70; ++i) {
        const auto name = "scene" + std::to_string(i);
        auto s = source(name.c_str(), "scene", name.c_str());
        s["settings"]["items"].append(item(previous.c_str(), previous.c_str()));
        root["sources"].append(s);
        previous = name;
    }
    scratch.write(root);
    CHECK(!scratch.convert());
    CHECK(!fs::exists(scratch.output));
}

LTEST_CASE(import_cli_creates_a_project_with_explicit_canvas_rate) {
    Scratch scratch;
    scratch.write(collection());
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        execl(LIBERTAS_CLI, LIBERTAS_CLI, "import-obs", scratch.input.c_str(),
              scratch.output.c_str(), "4", "4", "60", "1", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    CHECK(ldoc::open_project(scratch.output).has_value());
}

LTEST_CASE(export_roundtrip_preserves_pixels_stack_crop_pivot_flips_and_rotation) {
    Scratch scratch;
    scratch.write(collection());
    auto converted = scratch.convert();
    REQUIRE(converted);
    auto& project = converted->project;
    auto& node = project.scenes.front().roots.front();
    node.transform.position = {1, 1};
    node.transform.rotation_deg = 90;
    node.transform.scale = {-1, 1};
    node.transform.pivot = {0.5, 0.5};
    node.layout.crop.left = 1;
    const auto before = lcomp::composite_canvas(project, project.canvases.front().id);
    const auto output = fs::path(scratch.temp.path().toStdString()) / "export.json";
    auto exported = limport::export_obs(project, scratch.output, output);
    REQUIRE_MSG(exported, "{}", exported.error());
    CHECK(fs::exists(output.string() + ".report.md"));
    CHECK(!limport::export_obs(project, scratch.output, output));
    const auto imported = limport::import_obs(output, output.string() + ".libertas", {4, 4, {60000, 1001}});
    REQUIRE_MSG(imported, "{}", imported.error());
    const auto after = lcomp::composite_canvas(imported->project, imported->project.canvases.front().id);
    CHECK_EQ(before.pixels, after.pixels);
    CHECK_EQ(imported->project.sources.size(), project.sources.size());
    CHECK(exported->find("60000/1001") != std::string::npos);
}

LTEST_CASE(export_disambiguates_names_and_reports_unsupported_features_without_output_keys) {
    Scratch scratch;
    scratch.write(collection());
    auto converted = scratch.convert();
    REQUIRE(converted);
    auto& project = converted->project;
    project.sources.front().name = project.scenes.front().name;
    project.sources.back().type = "source.unknown";
    project.scenes.front().roots.front().blend.opacity = 0.5;
    const auto output = fs::path(scratch.temp.path().toStdString()) / "export.json";
    const auto exported = limport::export_obs(project, scratch.output, output);
    REQUIRE(exported);
    CHECK(exported->find("transparent placeholder") != std::string::npos);
    CHECK(exported->find("opacity") != std::string::npos);
    const auto imported = limport::import_obs(output, output.string() + ".libertas", {4, 4, {60, 1}});
    REQUIRE_MSG(imported, "{}", imported.error());
    CHECK_EQ(imported->project.scenes.size(), project.scenes.size());
    CHECK_EQ(imported->project.sources.size(), project.sources.size());
    CHECK(!limport::export_obs(project, scratch.output, scratch.temp.path().toStdString()));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    return ltest::run_all("obs_import");
}

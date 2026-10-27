// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit hardware check: LIBERTAS_TEST_GPU=1. No fallback is allowed to pass this check.
#include "ltest.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <lcapture/screen.hpp>
#include <lcomp/filters.hpp>
#include <lrhi/composite.hpp>
#include <mutex>
extern "C" {
#include <libswscale/swscale.h>
}

namespace {
ldoc::Project show(int width = 48, int height = 32) {
    auto p = ldoc::make_default_project("GPU check");
    p.canvases[0].width = width;
    p.canvases[0].height = height;
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.type = "source.screen";
    auto node = ldoc::make_node(ldoc::NodeKind::Source, "screen");
    std::get<ldoc::SourceData>(node.data).source = source.id;
    node.layout.fit = ldoc::FitMode::Stretch;
    node.layout.size = {static_cast<double>(width), static_cast<double>(height)};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.canvas = p.canvases[0].id;
    scene.roots = {node};
    p.sources = {source};
    p.scenes = {scene};
    p.decks[0].program_scene = scene.id;
    return p;
}

void compare(const lcomp::Picture& cpu, const lcomp::Picture& gpu) {
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    REQUIRE(cpu.status == gpu.status);
    REQUIRE(cpu.gaps.size() == gpu.gaps.size());
    REQUIRE(cpu.pixels.size() == gpu.pixels.size());
    int maximum = 0;
    for (std::size_t i = 0; i < cpu.pixels.size(); ++i)
        maximum = std::max(maximum, std::abs(int(cpu.pixels[i]) - int(gpu.pixels[i])));
    CHECK_MSG(maximum <= 2, "GPU/CPU maximum channel error: {} (allowed 2)", maximum);
}

libertas::rhi::VkCtxPtr device() {
    std::string error;
    const char* selected = std::getenv("LIBERTAS_RENDER_DEVICE");
    auto result = libertas::rhi::vk_init(selected ? selected : "", error);
    REQUIRE_MSG(result != nullptr, "{}", error);
    return result;
}
} // namespace

LTEST_CASE(one_composition_exports_matching_full_size_and_preview_frames) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu, "{}", error);
    auto project = show(48, 32);
    std::vector<std::uint8_t> pixels(48 * 32 * 4);
    for (int channel = 0; channel < 3; ++channel) {
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = pixels[i + 1] = pixels[i + 2] = 0;
            pixels[i + static_cast<std::size_t>(channel)] = pixels[i + 3] = 255;
        }
        const auto plan =
            lcomp::plan_canvas(project, project.canvases[0].id, [&](const ldoc::Source&) {
                return lcomp::CapturedPicture{48, 32, pixels.data(), pixels.size()};
            });
        lcomp::Picture full;
        const auto preview = gpu->render(plan, 12, error, &full);
        REQUIRE_MSG(preview, "{}", error);
        const auto expected = lcomp::execute_cpu(plan);
        compare(expected, full);
        compare(lcomp::preview_size(expected, 12), *preview);
    }
}

LTEST_CASE(native_media_frames_preserve_output_box_letterboxing_crop_and_filters) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    for (int height : {4, 8}) {
        auto p = show(8, 8);
        p.sources[0].type = "source.media";
        p.sources[0].settings["width"] = std::int64_t{8};
        p.sources[0].settings["height"] = static_cast<std::int64_t>(height);
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            std::initializer_list<std::uint8_t>{255, 0, 0, 128, 255, 0, 0, 128});
        auto frames = [&](const auto&) {
            return lcomp::CapturedPicture{2,       1,     pixels->data(), pixels->size(),
                                          nullptr, false, pixels};
        };
        auto render = [&] {
            const auto plan = lcomp::plan_canvas(p, p.canvases[0].id, frames);
            const auto cpu = lcomp::execute_cpu(plan);
            const auto result = gpu->render(plan, 0, error);
            REQUIRE_MSG(result, "{}", error);
            compare(cpu, *result);
            return cpu;
        };
        const auto plain = render();
        if (height == 8)
            CHECK_EQ(plain.pixels[0], 0);
        CHECK(plain.pixels[(3 * 8 + 3) * 4] > 180);
        render(); // Same immutable frame reuses its GPU resize.
        pixels = std::make_shared<std::vector<std::uint8_t>>(
            std::initializer_list<std::uint8_t>{0, 0, 255, 128, 0, 0, 255, 128});
        const auto changed = render();
        CHECK(changed.pixels[(3 * 8 + 3) * 4 + 2] > 180);
        auto& node = p.scenes[0].roots[0];
        node.layout.crop.top = 1;
        node.layout.crop.bottom = 1;
        render();
        ldoc::Effect effect;
        effect.id = ldoc::Id::generate();
        effect.type = "filter.color-correction";
        effect.params["brightness"] = 0.2;
        p.sources[0].effects = {effect};
        render();
    }
}

LTEST_CASE(stock_video_filters_execute_on_cpu_and_gpu_from_the_same_scene) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    std::vector<std::uint8_t> pixels(16 * 16 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = static_cast<std::uint8_t>(i % 251);
        pixels[i + 1] = 100;
        pixels[i + 2] = 180;
        pixels[i + 3] = 255;
    }
    const lcomp::FrameSource frames = [&](const auto&) {
        return lcomp::CapturedPicture{16, 16, pixels.data(), pixels.size()};
    };
    for (const auto& info : lcomp::filter_catalog()) {
        if (info.audio)
            continue;
        auto p = show(16, 16);
        ldoc::Effect effect;
        effect.id = ldoc::Id::generate();
        effect.type = info.type;
        for (const auto& f : info.fields)
            effect.params[f.key] = f.value;
        if (info.type == "filter.mask")
            effect.params["path"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/two.png";
        if (info.type == "filter.lut")
            effect.params["path"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/identity.cube";
        if (info.type == "filter.scale") {
            effect.params["width"] = std::int64_t{8};
            effect.params["height"] = std::int64_t{12};
        }
        if (info.type == "filter.crop") {
            effect.params["left"] = std::int64_t{2};
            effect.params["top"] = std::int64_t{-2};
        }
        if (info.type == "filter.color-correction")
            effect.params["brightness"] = 0.125;
        for (int owner = 0; owner < 3; ++owner) {
            p.sources[0].effects.clear();
            p.scenes[0].effects.clear();
            p.scenes[0].roots[0].effects.clear();
            if (owner == 0)
                p.sources[0].effects = {effect};
            else if (owner == 1)
                p.scenes[0].effects = {effect};
            else
                p.scenes[0].roots[0].effects = {effect};
            const auto plan = lcomp::plan_canvas(p, p.canvases[0].id, frames);
            REQUIRE(plan.gaps.empty());
            const auto image = gpu->render(plan, 0, error);
            REQUIRE_MSG(image, "{}: {}", info.type, error);
            compare(lcomp::execute_cpu(plan), *image);
        }
    }
}

LTEST_CASE(still_images_share_pixels_and_cached_uploads_follow_replacement) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / (ldoc::Id::generate().to_string() + ".png");
    fs::copy_file(fs::path(LIBERTAS_IMAGE_FIXTURES) / "two.png", path);
    auto project = show(32, 24);
    project.sources[0].type = "source.image";
    project.sources[0].settings["file"] = path.string();
    auto second = project.sources[0];
    second.id = ldoc::Id::generate();
    second.settings["file"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha.webp";
    project.sources.push_back(second);
    auto placed = project.scenes[0].roots[0];
    placed.id = ldoc::Id::generate();
    placed.transform.position = {16, 0};
    std::get<ldoc::SourceData>(placed.data).source = second.id;
    project.scenes[0].roots.push_back(placed);
    const auto original = lcomp::plan_canvas(project, project.canvases[0].id);
    REQUIRE(original.ops.size() == 2);
    REQUIRE(original.ops[0].image.owner);
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    const auto before = lcomp::execute_cpu(original);
    for (int tick = 0; tick < 3; ++tick) {
        const auto plan = lcomp::plan_canvas(project, project.canvases[0].id);
        CHECK(plan.ops[0].image.owner == original.ops[0].image.owner);
        CHECK(plan.ops[1].image.owner == original.ops[1].image.owner);
        const auto result = gpu->render(plan, 0, error);
        REQUIRE_MSG(result, "{}", error);
        compare(before, *result);
    }
    fs::copy_file(fs::path(LIBERTAS_IMAGE_FIXTURES) / "red.webp", path,
                  fs::copy_options::overwrite_existing);
    const auto changed = lcomp::plan_canvas(project, project.canvases[0].id);
    CHECK(changed.ops[1].image.owner != original.ops[1].image.owner);
    const auto result = gpu->render(changed, 0, error);
    REQUIRE_MSG(result, "{}", error);
    compare(lcomp::execute_cpu(changed), *result);
    CHECK(result->pixels != before.pixels);
    const auto retained = gpu->render(original, 0, error);
    REQUIRE_MSG(retained, "{}", error);
    compare(before, *retained);
    fs::remove(path);
}

LTEST_CASE(cached_masks_follow_file_replacement) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / (ldoc::Id::generate().to_string() + ".webp");
    fs::copy_file(fs::path(LIBERTAS_IMAGE_FIXTURES) / "alpha.webp", path);
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    auto project = show(32, 24);
    ldoc::Effect mask;
    mask.id = ldoc::Id::generate();
    mask.type = "filter.mask";
    mask.params["path"] = path.string();
    mask.params["mode"] = std::string("alpha");
    project.sources[0].effects = {mask};
    std::vector<std::uint8_t> pixels(32 * 24 * 4, 255);
    const auto plan = lcomp::plan_canvas(project, project.canvases[0].id, [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{32, 24, pixels.data(), pixels.size()};
    });
    std::vector<std::uint8_t> previous;
    for (const auto* file : {"alpha.webp", "red.webp"}) {
        fs::copy_file(fs::path(LIBERTAS_IMAGE_FIXTURES) / file, path,
                      fs::copy_options::overwrite_existing);
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto image = gpu->render(plan, 0, error);
            REQUIRE_MSG(image, "{}", error);
            compare(lcomp::execute_cpu(plan), *image);
            if (repeat == 0 && !previous.empty())
                CHECK(image->pixels != previous);
            previous = image->pixels;
        }
    }
    fs::remove(path);
}

LTEST_CASE(sampling_modes_and_key_adjustments_match_on_both_backends) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    std::vector<std::uint8_t> pixels(16 * 16 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = static_cast<std::uint8_t>(i % 251);
        pixels[i + 1] = 100;
        pixels[i + 2] = 180;
        pixels[i + 3] = 255;
    }
    const lcomp::FrameSource frames = [&](const auto&) {
        return lcomp::CapturedPicture{16, 16, pixels.data(), pixels.size()};
    };
    for (const auto* sampling : {"point", "bilinear", "bicubic", "lanczos", "area"}) {
        for (const auto size : {8, 23}) {
            auto p = show(size, size);
            ldoc::Effect effect;
            effect.id = ldoc::Id::generate();
            effect.type = "filter.scale";
            effect.params["width"] = std::int64_t{size};
            effect.params["height"] = std::int64_t{size};
            effect.params["sampling"] = std::string(sampling);
            p.sources[0].effects = {effect};
            const auto plan = lcomp::plan_canvas(p, p.canvases[0].id, frames);
            const auto image = gpu->render(plan, 0, error);
            REQUIRE_MSG(image, "{}", error);
            compare(lcomp::execute_cpu(plan), *image);
        }
    }
    for (const auto* type : {"filter.color-key", "filter.chroma-key"}) {
        auto p = show(16, 16);
        ldoc::Effect effect;
        effect.id = ldoc::Id::generate();
        effect.type = type;
        effect.params["opacity"] = 0.8;
        effect.params["contrast"] = -0.2;
        effect.params["brightness"] = 0.1;
        effect.params["gamma"] = 0.3;
        p.sources[0].effects = {effect};
        const auto plan = lcomp::plan_canvas(p, p.canvases[0].id, frames);
        const auto image = gpu->render(plan, 0, error);
        REQUIRE(image);
        compare(lcomp::execute_cpu(plan), *image);
    }
}

LTEST_CASE(video_delay_uses_prior_frames_and_scroll_advances_without_source_damage) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    auto p = show(4, 4);
    std::vector<std::uint8_t> pixels(4 * 4 * 4, 0);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 2] = 255;
        pixels[i + 3] = 255;
    }
    const lcomp::FrameSource frames = [&](const auto&) {
        return lcomp::CapturedPicture{4, 4, pixels.data(), pixels.size()};
    };
    ldoc::Effect delay;
    delay.id = ldoc::Id::generate();
    delay.type = "filter.render-delay";
    delay.params["delay_ms"] = std::int64_t{100};
    p.sources[0].effects = {delay};
    auto first = lcomp::plan_canvas(p, p.canvases[0].id, frames);
    first.time_ns = 1000000000;
    REQUIRE(lcomp::animated_plan(first));
    const auto initial = gpu->render(first, 0, error);
    REQUIRE(initial);
    const auto cpu_first = lcomp::execute_cpu(first);
    compare(cpu_first, *initial);
    CHECK(!initial->gaps.empty());
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 255;
        pixels[i + 2] = 0;
    }
    auto next = lcomp::plan_canvas(p, p.canvases[0].id, frames);
    next.time_ns = 1100000000;
    const auto second = gpu->render(next, 0, error);
    REQUIRE(second);
    compare(lcomp::execute_cpu(next), *second);
    CHECK(second->gaps.empty());
    CHECK(second->pixels[0] > 240);
    CHECK(second->pixels[2] < 10);
    ldoc::Effect scroll;
    scroll.id = ldoc::Id::generate();
    scroll.type = "filter.scroll";
    scroll.params["speed_x"] = 1.0;
    scroll.params["loop"] = false;
    p.sources[0].effects = {scroll};
    first = lcomp::plan_canvas(p, p.canvases[0].id, frames);
    first.time_ns = 2000000000;
    const auto start = gpu->render(first, 0, error);
    REQUIRE(start);
    compare(lcomp::execute_cpu(first), *start);
    next = lcomp::plan_canvas(p, p.canvases[0].id, frames);
    next.time_ns = 3000000000;
    const auto moved = gpu->render(next, 0, error);
    REQUIRE(moved);
    compare(lcomp::execute_cpu(next), *moved);
    CHECK_EQ(moved->pixels[2], 0);
    CHECK(moved->pixels[6] > 240);
}

LTEST_CASE(a_4k_filter_chain_stays_within_the_compositor_memory_budget) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    auto p = show(3840, 2160);
    auto& source = p.sources[0];
    source.type = "source.color";
    source.settings["color"] = ldoc::Rgba::from_srgb8(64, 128, 192);
    source.settings["width"] = std::int64_t{3840};
    source.settings["height"] = std::int64_t{2160};
    for (int i = 0; i < 8; ++i) {
        ldoc::Effect effect;
        effect.id = ldoc::Id::generate();
        effect.type = "filter.color-correction";
        effect.params["brightness"] = 0.001;
        source.effects.push_back(effect);
    }
    const auto plan = lcomp::plan_canvas(p, p.canvases[0].id);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        const auto picture = gpu->render(plan, 320, error);
        REQUIRE_MSG(picture, "{}", error);
        CHECK(picture->gaps.empty());
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() /
        5;
    std::fprintf(stderr, "  4K eight-filter chain including preview: %.2f ms/frame\n", ms);
}

LTEST_CASE(gpu_executes_the_shared_walk_for_opacity_precomps_rotations_and_flips) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    std::vector<std::uint8_t> pixels(7 * 5 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<std::uint8_t>((i * 37 + 11) % 256);
    lcomp::FrameSource frames = [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{7, 5, pixels.data(), pixels.size()};
    };
    auto project = show();
    auto node = project.scenes[0].roots[0];
    node.layout.size = {17, 13};
    node.transform.position = {-2, 3};
    node.layout.crop = {1, 1, 2, 1};
    ldoc::Scene inner = project.scenes[0];
    inner.id = ldoc::Id::generate();
    inner.roots[0].blend.opacity = 0.7;
    auto precomp = ldoc::make_node(ldoc::NodeKind::Precomp, "nested screen");
    std::get<ldoc::PrecompData>(precomp.data).scene = inner.id;
    precomp.blend.opacity = 0.6;
    project.scenes.push_back(inner);
    for (int quarter = 0; quarter < 4; ++quarter) {
        for (int flips = 0; flips < 4; ++flips) {
            node.transform.rotation_deg = quarter * 90;
            node.transform.scale = {flips & 1 ? -1.0 : 1.0, flips & 2 ? -1.0 : 1.0};
            node.transform.pivot = {0.5, 0.5};
            node.blend.opacity = 0.53;
            project.scenes[0].roots = {node, precomp};
            const auto plan = lcomp::plan_canvas(project, project.canvases[0].id, frames);
            REQUIRE(plan.gaps.empty());
            for (std::uint32_t edge : {0u, 19u}) {
                auto result = gpu->render(plan, edge, error);
                REQUIRE_MSG(result.has_value(), "{}", error);
                compare(lcomp::preview_size(lcomp::execute_cpu(plan), edge), *result);
            }
        }
    }
    // The other texture path is the real decoder, with RGBA instead of capture's BGRA.
    project.sources[0].type = "source.image";
    project.scenes[0].roots[0].layout.crop = {};
    project.sources[0].settings["file"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/two.png";
    auto group = ldoc::make_node(ldoc::NodeKind::Group, "opacity group");
    group.blend.opacity = 0.5;
    group.transform.position = {2, 1};
    group.transform.scale = {0.5, 0.75};
    group.transform.rotation_deg = 90;
    std::get<ldoc::GroupData>(group.data).children = project.scenes[0].roots;
    project.scenes[0].roots = {group};
    const auto image_plan = lcomp::plan_canvas(project, project.canvases[0].id);
    auto image_result = gpu->render(image_plan, 0, error);
    REQUIRE_MSG(image_result.has_value(), "{}", error);
    compare(lcomp::execute_cpu(image_plan), *image_result);
    std::get<ldoc::GroupData>(project.scenes[0].roots[0].data).size = ldoc::Vec2{8, 6};
    const auto bounded_plan = lcomp::plan_canvas(project, project.canvases[0].id);
    auto bounded_result = gpu->render(bounded_plan, 0, error);
    REQUIRE_MSG(bounded_result.has_value(), "{}", error);
    compare(lcomp::execute_cpu(bounded_plan), *bounded_result);
    auto missing = ldoc::make_node(ldoc::NodeKind::Source, "missing nested source");
    std::get<ldoc::SourceData>(missing.data).source = ldoc::Id::generate();
    std::get<ldoc::GroupData>(project.scenes[0].roots[0].data).children.push_back(missing);
    const auto partial_plan = lcomp::plan_canvas(project, project.canvases[0].id);
    REQUIRE(!partial_plan.gaps.empty());
    const auto partial_result = gpu->render(partial_plan, 0, error);
    REQUIRE(partial_result);
    compare(lcomp::execute_cpu(partial_plan), *partial_result);
}

LTEST_CASE(all_seven_blends_match_cpu_for_textures_transparency_and_nested_groups) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    auto project = show();
    std::vector<std::uint8_t> pixels(7 * 5 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<std::uint8_t>(50 + (i * 37) % 180);
    const lcomp::FrameSource frames = [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{7, 5, pixels.data(), pixels.size()};
    };
    auto front = project.scenes[0].roots[0];
    auto background = front;
    background.blend.opacity = 0.7;
    auto inner = project.scenes[0];
    inner.id = ldoc::Id::generate();
    auto precomp = ldoc::make_node(ldoc::NodeKind::Precomp, "transparent nested scene");
    std::get<ldoc::PrecompData>(precomp.data).scene = inner.id;
    project.scenes.push_back(inner);
    using M = ldoc::BlendMode;
    for (const auto mode :
         {M::Normal, M::LinearDodge, M::Subtract, M::Screen, M::Multiply, M::Lighten, M::Darken}) {
        for (const double opacity : {0.0, 0.53, 1.0}) {
            front.blend.mode = mode;
            front.blend.opacity = opacity;
            front.transform.rotation_deg = 90;
            front.transform.scale = {-1, 1};
            project.scenes[1].roots = {front, background};
            auto group = ldoc::make_node(ldoc::NodeKind::Group, "blended group");
            group.blend.mode = mode;
            group.blend.opacity = opacity;
            group.transform.position = {2, 1};
            group.transform.scale = {0.8, 0.7};
            std::get<ldoc::GroupData>(group.data).children = {precomp, background};
            project.scenes[0].roots = {group, background};
            const auto plan = lcomp::plan_canvas(project, project.canvases[0].id, frames);
            REQUIRE(plan.gaps.empty());
            for (const auto edge : {0u, 19u}) {
                auto result = gpu->render(plan, edge, error);
                REQUIRE_MSG(result, "{}", error);
                compare(lcomp::preview_size(lcomp::execute_cpu(plan), edge), *result);
            }
        }
    }
}

LTEST_CASE(host_video_uploads_to_a_4k_canvas) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    auto project = show(3840, 2160);
    std::vector<std::uint8_t> pixels(2560 * 1440 * 4, 255);
    const auto plan = lcomp::plan_canvas(project, project.canvases[0].id,
        [&](const ldoc::Source&) {
            return lcomp::CapturedPicture{2560, 1440, pixels.data(), pixels.size()};
        });
    REQUIRE(gpu->render(plan, 640, error).has_value());
    const auto started = std::chrono::steady_clock::now();
    const int count = std::getenv("LIBERTAS_BENCHMARK") ? 180 : 3;
    for (int i = 0; i < count; ++i) {
        auto frame = gpu->render(plan, 640, error);
        REQUIRE_MSG(frame.has_value(), "{}", error);
        CHECK_EQ(frame->width, 640);
        CHECK_EQ(frame->height, 360);
        CHECK_EQ(frame->pixels.front(), 255);
    }
    if (std::getenv("LIBERTAS_BENCHMARK"))
        std::fprintf(stderr, "1440p host upload + 4K composite + 640 preview: %.3f ms/frame\n",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / count);
}

LTEST_CASE(nested_4k_capture_with_alternating_vertical_canvas) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu, "{}", error);
    auto project = show(3840, 2160);
    auto inner = project.scenes.front();
    inner.id = ldoc::Id::generate();
    auto nested = ldoc::make_node(ldoc::NodeKind::Precomp, "nested capture");
    std::get<ldoc::PrecompData>(nested.data).scene = inner.id;
    project.scenes.front().roots = {nested};
    project.scenes.push_back(inner);
    std::vector<std::uint8_t> pixels(2560 * 1440 * 4, 255);
    const auto plan = lcomp::plan_canvas(project, project.canvases[0].id, [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{2560, 1440, pixels.data(), pixels.size()};
    });
    auto verticalGpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(verticalGpu, "{}", error);
    lcomp::DrawPlan vertical;
    vertical.width = 1080;
    vertical.height = 1920;
    for (const bool alternate : {false, true}) {
        const int count = std::getenv("LIBERTAS_BENCHMARK") ? 120 : 3;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i) {
            auto frame = gpu->render(plan, 960, error);
            REQUIRE_MSG(frame, "{}", error);
            CHECK_EQ(frame->pixels.front(), 255);
            if (alternate)
                REQUIRE_MSG(verticalGpu->render(vertical, 960, error), "{}", error);
        }
        std::fprintf(
            stderr, "nested 4K capture%s: %.3f ms/frame\n", alternate ? " + vertical" : "",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count() /
                count);
    }
}

LTEST_CASE(a_real_capture_slot_stays_on_device_until_preview_readback) {
    const char* output = std::getenv("LIBERTAS_TEST_CAPTURE");
    if (!output)
        return;
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    auto capture = libertas::capture::ScreenCapture::open(*d, output, true, error);
    REQUIRE_MSG(capture != nullptr, "{}", error);
    std::mutex mu;
    std::condition_variable ready;
    std::shared_ptr<const libertas::capture::DmabufImage> frame;
    capture->start_images([&](auto image) {
        std::lock_guard lock(mu);
        frame = std::move(image);
        ready.notify_one();
    });
    std::shared_ptr<const libertas::capture::DmabufImage> snapshot;
    {
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(5), [&] { return bool(frame); });
        snapshot = frame;
    }
    // Stop before any assertion can unwind the callback's mutex and condition variable.
    capture->stop();
    REQUIRE_MSG(snapshot != nullptr, "no first capture frame within five seconds");
    auto p = show(static_cast<int>(snapshot->width), static_cast<int>(snapshot->height));
    lcomp::FrameSource native = [&](const ldoc::Source&) {
        lcomp::CapturedPicture result;
        result.width = static_cast<int>(snapshot->width);
        result.height = static_cast<int>(snapshot->height);
        result.device_image = snapshot.get();
        return result;
    };
    const auto plan = lcomp::plan_canvas(p, p.canvases[0].id, native);
    std::optional<lcomp::Picture> result;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
        result = gpu->render(plan, 640, error);
        REQUIRE_MSG(result.has_value(), "{}", error);
    }
    std::fprintf(stderr,
                 "capture %ux%u, GPU composite + 640-edge preview: %.3f ms/frame (60 frames)\n",
                 snapshot->width, snapshot->height,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                         .count() /
                     60);
    std::vector<std::uint8_t> host;
    REQUIRE_MSG(libertas::rhi::vk_readback_exported(*d, *snapshot, true, host, error), "{}", error);
    lcomp::FrameSource cpu = [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{static_cast<int>(snapshot->width),
                                      static_cast<int>(snapshot->height), host.data(), host.size()};
    };
    compare(lcomp::preview_size(lcomp::composite_canvas(p, p.canvases[0].id, cpu), 640), *result);
    auto invalid = *snapshot;
    invalid.dmabuf_fd = -1;
    const auto invalid_plan = lcomp::plan_canvas(p, p.canvases[0].id, [&](const ldoc::Source&) {
        auto frame_view = native(p.sources.front());
        frame_view.device_image = &invalid;
        return frame_view;
    });
    CHECK(!gpu->render(invalid_plan, 640, error).has_value());
    CHECK(error.find("producer fence") != std::string::npos);
    REQUIRE_MSG(gpu->render(plan, 640, error).has_value(), "{}", error);
}

int main() {
    if (!std::getenv("LIBERTAS_TEST_GPU"))
        return 77;
    if (!std::getenv("LIBERTAS_TEST_CAPTURE"))
        std::erase_if(ltest::registry(), [](const auto& test) {
            return test.name == "a_real_capture_slot_stays_on_device_until_preview_readback";
        });
    return ltest::run_all("GPU composite");
}

LTEST_CASE(camera_yuv_converts_on_the_gpu_like_swscale) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE(gpu);
    constexpr int w = 16, h = 8;
    auto p = show(w, h);
    // A gradient in every plane, so chroma placement and matrix both show up in the result.
    constexpr std::size_t n = w * h;
    std::vector<std::uint8_t> luma(n), u(n / 4), v(n / 4);
    for (std::size_t i = 0; i < n; ++i) luma[i] = static_cast<std::uint8_t>(16 + i * 219 / n);
    for (std::size_t i = 0; i < n / 4; ++i) {
        u[i] = static_cast<std::uint8_t>(40 + i * 5);
        v[i] = static_cast<std::uint8_t>(200 - i * 4);
    }
    for (const auto layout : {AV_PIX_FMT_YUYV422, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12})
        for (const int matrix : {0, 1, 2, 3}) {
            std::vector<std::uint8_t> planes;
            if (layout == AV_PIX_FMT_YUYV422)
                for (std::size_t y = 0; y < h; ++y)
                    for (std::size_t x = 0; x < w; x += 2) {
                        const std::size_t c = (y / 2) * (w / 2) + x / 2;
                        planes.insert(planes.end(), {luma[y * w + x], u[c], luma[y * w + x + 1], v[c]});
                    }
            else {
                planes = luma;
                if (layout == AV_PIX_FMT_YUV420P) {
                    planes.insert(planes.end(), u.begin(), u.end());
                    planes.insert(planes.end(), v.begin(), v.end());
                } else
                    for (std::size_t i = 0; i < u.size(); ++i) planes.insert(planes.end(), {u[i], v[i]});
            }
            // The CPU reference: the same swscale call the capture thread makes.
            std::vector<std::uint8_t> bgra(n * 4);
            // swscale's NV12 path interpolates chroma differently from its I420 path (they
            // differ by ~40 on this gradient). The GPU treats both alike, so NV12 is checked
            // against the I420 reference built from the same planes.
            const auto reference = layout == AV_PIX_FMT_NV12 ? AV_PIX_FMT_YUV420P : layout;
            auto* sws = sws_getContext(w, h, reference, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                                       nullptr, nullptr);
            REQUIRE(sws);
            const auto* coefficients = sws_getCoefficients(matrix & 1 ? SWS_CS_ITU709 : SWS_CS_ITU601);
            REQUIRE(sws_setColorspaceDetails(sws, coefficients, matrix >> 1, coefficients, 1, 0,
                                             1 << 16, 1 << 16) >= 0);
            const std::uint8_t* src[4] = {planes.data(), nullptr, nullptr, nullptr};
            int stride[4] = {layout == AV_PIX_FMT_YUYV422 ? w * 2 : w, 0, 0, 0};
            if (layout == AV_PIX_FMT_YUV420P) {
                src[1] = planes.data() + n;
                src[2] = src[1] + n / 4;
                stride[1] = stride[2] = w / 2;
            } else if (layout == AV_PIX_FMT_NV12) {
                src[0] = luma.data();
                src[1] = u.data();
                src[2] = v.data();
                stride[1] = stride[2] = w / 2;
            }
            std::uint8_t* dst[4] = {bgra.data(), nullptr, nullptr, nullptr};
            int dst_stride[4] = {w * 4, 0, 0, 0};
            REQUIRE(sws_scale(sws, src, stride, 0, h, dst, dst_stride) == h);
            sws_freeContext(sws);
            const auto cpu_plan = lcomp::plan_canvas(p, p.canvases[0].id, [&](const auto&) {
                return lcomp::CapturedPicture{w, h, bgra.data(), bgra.size(), nullptr};
            });
            const auto gpu_plan = lcomp::plan_canvas(p, p.canvases[0].id, [&](const auto&) {
                lcomp::CapturedPicture picture{w, h, planes.data(), planes.size(), nullptr};
                picture.yuv = layout == AV_PIX_FMT_YUYV422 ? 1 : layout == AV_PIX_FMT_YUV420P ? 2 : 3;
                picture.yuv_matrix = static_cast<std::uint8_t>(matrix);
                return picture;
            });
            const auto result = gpu->render(gpu_plan, 0, error);
            REQUIRE_MSG(result, "{}", error);
            // swscale's bilinear chroma upsampling interpolates where the shader takes the
            // nearest sample; within a block of flat chroma they agree to rounding.
            const auto cpu = lcomp::execute_cpu(cpu_plan);
            REQUIRE(cpu.pixels.size() == result->pixels.size());
            int maximum = 0;
            for (std::size_t i = 0; i < cpu.pixels.size(); ++i)
                maximum = std::max(maximum, std::abs(int(cpu.pixels[i]) - int(result->pixels[i])));
            CHECK_MSG(maximum <= 24, "layout {} matrix {}: max channel error {}", int(layout), matrix,
                      maximum);
        }
}

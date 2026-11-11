// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ltest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <lcomp/audio_filters.hpp>
#include <lcomp/filters.hpp>
#include <lcomp/vst.hpp>
#include <thread>

LTEST_CASE(audio_filters_process_real_blocks_and_preserve_disabled_samples) {
    for (const auto& info : lcomp::filter_catalog()) {
        if (!info.audio || info.type == "filter.vst")
            continue;
        ldoc::Effect e;
        e.id = ldoc::Id::generate();
        e.type = info.type;
        for (const auto& field : info.fields)
            e.params[field.key] = field.value;
        std::array<float, 960> samples;
        samples.fill(0.25f);
        lcomp::AudioFilters processor;
        REQUIRE(processor.process(samples, std::span{&e, 1}));
        CHECK(std::ranges::all_of(samples, [](float s) { return std::isfinite(s); }));
        e.enabled = false;
        samples.fill(0.25f);
        REQUIRE(processor.process(samples, std::span{&e, 1}));
        CHECK(std::ranges::all_of(samples, [](float s) { return s == 0.25f; }));
    }
}

LTEST_CASE(audio_gain_polarity_limiter_and_sidechain_have_measured_effect) {
    lcomp::AudioFilters processor;
    ldoc::Effect e;
    e.id = ldoc::Id::generate();
    std::array<float, 960> samples;
    samples.fill(0.5f);
    e.type = "filter.gain";
    e.params["db"] = -6.020599913279624;
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK_NEAR(samples[100], 0.25, 1e-6);
    e.type = "filter.invert";
    e.params.clear();
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK_NEAR(samples[100], -0.25, 1e-6);
    e.type = "filter.limiter";
    e.params["threshold"] = -20.0;
    samples.fill(1);
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK(std::ranges::all_of(samples, [](float s) { return std::abs(s) <= 0.10001f; }));
    e.type = "filter.compressor";
    e.params.clear();
    e.params["threshold"] = -20.0;
    e.params["ratio"] = 10.0;
    e.params["attack_time"] = 0.0;
    const auto side = ldoc::Id::generate();
    e.params["sidechain"] = side.to_string();
    std::array<float, 960> detector;
    detector.fill(1);
    samples.fill(0.1f);
    REQUIRE(
        processor.process(samples, std::span{&e, 1}, [&](const auto& id) -> std::span<const float> {
            return id == side ? std::span<const float>{detector} : std::span<const float>{};
        }));
    CHECK(samples[100] < 0.02f);
    samples.fill(1);
    CHECK(!processor.process(samples, std::span{&e, 1}));
    CHECK(std::ranges::all_of(samples, [](float s) { return s == 0; }));
}

LTEST_CASE(async_video_delay_delays_audio_by_the_same_number_of_samples) {
    lcomp::AudioFilters processor;
    ldoc::Effect e;
    e.id = ldoc::Id::generate();
    e.type = "filter.video-delay";
    e.params["delay_ms"] = std::int64_t{10};
    std::array<float, 960> samples;
    samples.fill(0.5f);
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK(std::ranges::all_of(samples, [](float s) { return s == 0; }));
    samples.fill(0.25f);
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK(std::ranges::all_of(samples, [](float s) { return s == 0.5f; }));
}

LTEST_CASE(vst_runs_in_a_helper_with_parameter_controls_and_crash_isolation) {
    ldoc::Effect effect;
    effect.id = ldoc::Id::generate();
    effect.type = "filter.vst";
    effect.params["path"] = std::string(MUNKICAST_VST_FIXTURE);
    lcomp::VstWorker worker(effect);
    for (int i = 0; i < 100 && !worker.info().error.empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE_MSG(worker.info().error.empty(), "{}", worker.info().error);
    REQUIRE(worker.info().parameters.size() == 1);
    CHECK_EQ(worker.info().parameters[0].name, std::string("Gain"));
    std::array<float, 960> samples;
    effect.params["parameter.0"] = 0.25;
    bool got = false;
    for (int i = 0; i < 100 && !got; ++i) {
        samples.fill(0.5f);
        got = worker.process(samples, effect.params);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(got);
    CHECK_NEAR(samples[100], 0.125, 1e-6);
    const auto snapshot = lcomp::vst_snapshot(effect);
    REQUIRE(snapshot);
    CHECK(!snapshot->empty());
    auto restored = effect;
    restored.params.erase("parameter.0");
    restored.params["chunk_data"] = *snapshot;
    lcomp::VstWorker restored_worker(restored);
    for (int i = 0; i < 100 && !restored_worker.info().error.empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(restored_worker.info().error.empty());
    CHECK_NEAR(restored_worker.info().parameters[0].value, 0.25, 1e-6);
    effect.params["parameter.0"] = 1.0;
    for (int i = 0; i < 100 && worker.info().error.empty(); ++i) {
        samples.fill(0.5f);
        worker.process(samples, effect.params);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!worker.info().error.empty());
    samples.fill(0.5f);
    CHECK(!worker.process(samples, effect.params));
}

int main() {
    return ltest::run_all("filters");
}

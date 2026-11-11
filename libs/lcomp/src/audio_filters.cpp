// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <lcomp/audio_filters.hpp>
#include <lcomp/filters.hpp>
#include <lcomp/vst.hpp>
#include <map>
#include <numbers>
#include <rnnoise.h>
#include <speex/speex_preprocess.h>

namespace lcomp {
struct AudioFilters::Impl {
    struct State {
        ldoc::ParamMap params;
        std::string type;
        double envelope = 0, gain = 1, hold = 0, rms = 0;
        bool open = false;
        std::array<std::array<double, 11>, 2> eq{};
        std::array<DenoiseState*, 2> rn{};
        std::array<SpeexPreprocessState*, 2> speex{};
        std::deque<float> delay;
        std::unique_ptr<VstWorker> vst;

        ~State() {
            for (auto* p : rn)
                if (p)
                    rnnoise_destroy(p);
            for (auto* p : speex)
                if (p)
                    speex_preprocess_state_destroy(p);
        }
    };

    std::map<ldoc::Id, std::unique_ptr<State>> states;
};

AudioFilters::AudioFilters() : impl_(std::make_unique<Impl>()) {}

AudioFilters::~AudioFilters() = default;

std::expected<void, std::string>
AudioFilters::process(std::span<float> samples, std::span<const ldoc::Effect> effects,
                      const std::function<std::span<const float>(const ldoc::Id&)>& sidechain) {
    const auto fail = [&](std::string error) -> std::expected<void, std::string> {
        std::ranges::fill(samples, 0);
        return std::unexpected(std::move(error));
    };
    if (samples.size() != 960)
        return fail("Audio filters require 480 stereo frames");
    for (float& sample : samples)
        if (!std::isfinite(sample))
            sample = 0;
    std::erase_if(impl_->states, [&](const auto& pair) {
        return std::ranges::none_of(effects,
                                    [&](const auto& e) { return e.enabled && e.id == pair.first; });
    });
    for (const auto& e : effects) {
        if (!e.enabled)
            continue;
        const auto* info = filter_info(e.type);
        if (!info)
            return fail("Unknown audio filter: " + e.type);
        if (!info->audio && e.type != "filter.video-delay")
            continue;
        if (auto valid = validate_filter(e); !valid)
            return fail(valid.error());
        auto& ptr = impl_->states[e.id];
        bool changed = ptr && ptr->params != e.params;
        if (changed && e.type == "filter.vst" && ptr->type == e.type) {
            ldoc::Effect previous = e;
            previous.params = ptr->params;
            changed = filter_text(previous, "path") != filter_text(e, "path") ||
                      filter_text(previous, "chunk_data") != filter_text(e, "chunk_data") ||
                      filter_number(previous, "program") != filter_number(e, "program");
        }
        if (!ptr || changed || ptr->type != e.type) {
            ptr = std::make_unique<Impl::State>();
            ptr->params = e.params;
            ptr->type = e.type;
        }
        auto& state = *ptr;
        const auto n = [&](const char* key) { return filter_number(e, key); };
        const auto amp = [](double db) { return std::pow(10.0, db / 20); };
        const auto coefficient = [](double ms) { return ms <= 0 ? 0 : std::exp(-1 / (ms * 48)); };
        if (e.type == "filter.vst") {
            if (filter_text(e, "path").empty())
                return fail("Choose a Linux VST2 plugin");
            if (!state.vst)
                state.vst = std::make_unique<VstWorker>(e);
            if (!state.vst->process(samples, e.params))
                return fail("VST plugin warming up, unavailable, or failed");
        } else if (e.type == "filter.video-delay") {
            const auto length = static_cast<std::size_t>(n("delay_ms")) * 96;
            if (state.delay.size() != length)
                state.delay.assign(length, 0);
            if (length)
                for (auto& sample : samples) {
                    state.delay.push_back(sample);
                    sample = state.delay.front();
                    state.delay.pop_front();
                }
        } else if (e.type == "filter.gain" || e.type == "filter.invert") {
            const float g = e.type == "filter.invert" ? -1 : static_cast<float>(amp(n("db")));
            for (auto& sample : samples)
                sample *= g;
        } else if (e.type == "filter.equalizer") {
            const double low = 2 * std::sin(std::numbers::pi * 800 / 48000),
                         high = 2 * std::sin(std::numbers::pi * 5000 / 48000);
            for (std::size_t i = 0; i < samples.size(); ++i) {
                auto& s = state.eq[i % 2];
                const double input = samples[i];
                s[0] += low * (input - s[0]) + 1e-20;
                s[4] += high * (input - s[4]) + 1e-20;
                for (std::size_t j = 1; j < 4; ++j) {
                    s[j] += low * (s[j - 1] - s[j]);
                    s[j + 4] += high * (s[j + 3] - s[j + 4]);
                }
                const double l = s[3], h = s[10] - s[7], m = s[10] - l - h;
                samples[i] =
                    static_cast<float>(l * amp(n("low")) + m * amp(n("mid")) + h * amp(n("high")));
                s[10] = s[9];
                s[9] = s[8];
                s[8] = input;
            }
        } else if (e.type == "filter.noise-suppression") {
            const bool rn = filter_text(e, "method") == "rnnoise";
            if (rn && rnnoise_get_frame_size() != 480)
                return fail("RNNoise frame size is unsupported");
            for (std::size_t ch = 0; ch < 2; ++ch) {
                if (rn) {
                    if (!state.rn[ch])
                        state.rn[ch] = rnnoise_create(nullptr);
                    if (!state.rn[ch])
                        return fail("Cannot allocate RNNoise state");
                    std::array<float, 480> input{}, output{};
                    for (std::size_t i = 0; i < 480; ++i)
                        input[i] = samples[i * 2 + ch] * 32768;
                    rnnoise_process_frame(state.rn[ch], output.data(), input.data());
                    for (std::size_t i = 0; i < 480; ++i)
                        samples[i * 2 + ch] = output[i] / 32768;
                } else {
                    if (!state.speex[ch])
                        state.speex[ch] = speex_preprocess_state_init(480, 48000);
                    if (!state.speex[ch])
                        return fail("Cannot allocate Speex state");
                    int level = static_cast<int>(n("suppress_level")), enabled = 1, disabled = 0;
                    speex_preprocess_ctl(state.speex[ch], SPEEX_PREPROCESS_SET_DENOISE, &enabled);
                    speex_preprocess_ctl(state.speex[ch], SPEEX_PREPROCESS_SET_AGC, &disabled);
                    speex_preprocess_ctl(state.speex[ch], SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
                                         &level);
                    std::array<spx_int16_t, 480> input{};
                    for (std::size_t i = 0; i < 480; ++i)
                        input[i] = static_cast<spx_int16_t>(
                            std::lround(std::clamp(samples[i * 2 + ch], -1.0f, 0.999969f) * 32768));
                    speex_preprocess_run(state.speex[ch], input.data());
                    for (std::size_t i = 0; i < 480; ++i)
                        samples[i * 2 + ch] = input[i] / 32768.0f;
                }
            }
        } else {
            std::span<const float> detector = samples;
            const auto side = filter_text(e, "sidechain");
            if (!side.empty() && side != "none") {
                const auto id = ldoc::Id::parse(side);
                if (!id || !sidechain)
                    return fail("Compressor sidechain is unavailable");
                detector = sidechain(*id);
                if (detector.size() != samples.size())
                    return fail("Compressor sidechain is unavailable");
            }
            const bool gate = e.type == "filter.noise-gate", limiter = e.type == "filter.limiter",
                       up = e.type == "filter.upward-compressor",
                       expander = e.type == "filter.expander";
            const double attack = coefficient(n("attack_time")),
                         release = coefficient(n("release_time"));
            for (std::size_t i = 0; i < samples.size(); i += 2) {
                const double peak = std::max(std::abs(detector[i]), std::abs(detector[i + 1]));
                double gain = 1;
                if (gate) {
                    if (peak >= amp(n("open_threshold"))) {
                        state.open = true;
                        state.hold = n("hold_time") * 48;
                    } else if (peak < amp(n("close_threshold"))) {
                        if (state.hold > 0)
                            --state.hold;
                        else
                            state.open = false;
                    }
                    const double target = state.open ? 1 : 0, co = state.open ? attack : release;
                    state.gain = target + co * (state.gain - target);
                    gain = state.gain;
                } else if (limiter) {
                    const double target = peak > 0 ? std::min(1.0, amp(n("threshold")) / peak) : 1;
                    state.gain =
                        target < state.gain ? target : target + release * (state.gain - target);
                    gain = state.gain;
                } else {
                    double detected = peak;
                    if ((up || expander) && filter_text(e, "detector") == "RMS") {
                        const double rmsco = std::exp2(-100.0 / 48000);
                        state.rms = rmsco * state.rms + (1 - rmsco) * peak * peak;
                        detected = std::sqrt(state.rms);
                    }
                    const double co = detected > state.envelope ? attack : release;
                    state.envelope = detected + co * (state.envelope - detected);
                    const double db = 20 * std::log10(std::max(state.envelope, 1e-12)),
                                 delta = db - n("threshold"), ratio = n("ratio");
                    double change = 0;
                    if (up) {
                        double diff = -delta;
                        const double knee = n("knee");
                        if (db <= (n("threshold") - 60) / 2)
                            diff = std::max(0.0, db + 60);
                        if (delta <= -knee / 2)
                            change = diff * (1 - ratio);
                        else if (knee > 0 && delta < knee / 2)
                            change = (1 - ratio) * std::pow(-delta + knee / 2, 2) / (2 * knee);
                    } else if (expander && delta < 0)
                        change = delta * (ratio - 1);
                    else if (!up && !expander && delta > 0)
                        change = -delta * (1 - 1 / ratio);
                    gain = amp(change + n("output_gain"));
                }
                samples[i] *= static_cast<float>(gain);
                samples[i + 1] *= static_cast<float>(gain);
            }
        }
    }
    return {};
}
} // namespace lcomp

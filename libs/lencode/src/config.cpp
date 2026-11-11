// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The join between what the document says and what the muxer needs. See runtime_config in
// encoder.hpp for why it lives here rather than in ldoc.

#include "lencode/encoder.hpp"

#include <utility>

namespace munkicast::encode {

RuntimeConfig runtime_config(const ldoc::Encode& encode, const ldoc::AudioEncode& audio,
                             const ldoc::Canvas& canvas, std::string path) {
    RuntimeConfig cfg;
    cfg.path = std::move(path);

    // A rescale says how big the picture is; the canvas always says how often it arrives.
    const double width = encode.rescale ? encode.rescale->x : canvas.width;
    const double height = encode.rescale ? encode.rescale->y : canvas.height;
    cfg.width = static_cast<std::uint32_t>(width > 0 ? width : canvas.width);
    cfg.height = static_cast<std::uint32_t>(height > 0 ? height : canvas.height);
    cfg.source_width = static_cast<std::uint32_t>(canvas.width);
    cfg.source_height = static_cast<std::uint32_t>(canvas.height);
    cfg.source_rgba = true;

    // Exact, never a double: 59.94 is 60000/1001 and a pipeline that rounds it drifts into
    // audio sync hours later.
    cfg.fps_num = canvas.fps.num > 0 ? canvas.fps.num : 60;
    cfg.fps_den = canvas.fps.den > 0 ? canvas.fps.den : 1;

    cfg.bitrate = static_cast<std::int64_t>(encode.encoder.bitrate_kbps) * 1000;

    const auto& encoder = encode.encoder;
    const auto extra = [&](const std::string& key, std::string fallback) {
        const auto it = encoder.extra.find(key);
        if (it == encoder.extra.end())
            return fallback;
        if (const auto* value = std::get_if<std::string>(&it->second))
            return *value;
        if (const auto* value = std::get_if<std::int64_t>(&it->second))
            return std::to_string(*value);
        if (const auto* value = std::get_if<bool>(&it->second))
            return std::string(*value ? "1" : "0");
        return fallback;
    };
    // An explicit selection must either open that codec or report why it cannot. Substituting
    // H.264 for a selected HEVC/AV1 encode would make the graph describe a different file.
    if (encoder.implementation == "x264" && encoder.codec == "h264")
        cfg.force_tier = "libx264";
    else if (encoder.implementation == "svt-av1" && encoder.codec == "av1")
        cfg.force_tier = "libsvtav1";
    else
        cfg.force_tier = encoder.codec + "_" + encoder.implementation;

    cfg.keyframe_interval_ms = encoder.keyframe_interval_ms;
    cfg.rescale_filter = extra("rescale_filter", "bicubic");
    auto& options = cfg.video_options;
    const auto quality = extra("quality", "23");
    if (!encoder.preset.empty())
        options["preset"] = encoder.preset;
    if (encoder.implementation == "nvenc") {
        if (encoder.preset.empty())
            options["preset"] = "p5";
        options["tune"] = extra("nvenc.tuning", "hq");
        options["rc"] = encoder.rate_control == "cqp" ? "constqp" : encoder.rate_control;
        if (encoder.rate_control == "cqp") {
            options["qp"] = quality;
            options["b"] = "0";
        }
        const auto multipass = extra("nvenc.multipass", "quarter");
        options["multipass"] = multipass == "quarter" ? "1" : multipass == "full" ? "2" : "0";
        options["profile"] = extra("nvenc.profile", encoder.codec == "h264" ? "high" : "main");
        options["rc-lookahead"] = extra("nvenc.lookahead", "0") == "1" ? "32" : "0";
        options["spatial-aq"] = extra("nvenc.adaptive_quantization", "1");
        options["bf"] = extra("nvenc.bframes", "2");
        options["gpu"] = extra("gpu", "-1");
        cfg.custom_options = extra("nvenc.options", "");
    } else if (encoder.implementation == "x264") {
        if (encoder.preset.empty())
            options["preset"] = "medium";
        const auto tune = extra("x264.tune", "");
        const auto profile = extra("x264.profile", "");
        if (!tune.empty())
            options["tune"] = tune;
        if (!profile.empty())
            options["profile"] = profile;
        if (encoder.rate_control == "crf") {
            options["crf"] = quality;
            options["b"] = "0";
        } else if (encoder.rate_control == "cbr") {
            options["minrate"] = std::to_string(cfg.bitrate);
            options["maxrate"] = std::to_string(cfg.bitrate);
            options["bufsize"] = std::to_string(cfg.bitrate * 2);
            options["nal-hrd"] = "cbr";
        }
        if (extra("x264.custom_buffer", "0") == "1") {
            // This field is stored in kilobits; FFmpeg's AVOptions use bits.
            const auto it = encoder.extra.find("x264.buffer_kbps");
            const auto* value =
                it == encoder.extra.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
            if (value && *value >= 0 && *value <= 1000000) {
                options["bufsize"] = std::to_string(*value * 1000);
                if (*value > 0)
                    options["maxrate"] = std::to_string(cfg.bitrate);
            }
        }
        const auto custom = extra("x264.options", "");
        if (!custom.empty())
            options["x264-params"] = custom;
    } else if (encoder.rate_control == "cqp") {
        options["global_quality"] = quality;
    }

    cfg.audio_bitrate = static_cast<std::int64_t>(audio.bitrate_kbps) * 1000;
    cfg.audio_codec = audio.codec;
    return cfg;
}

} // namespace munkicast::encode

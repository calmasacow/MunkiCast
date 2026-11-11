// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image.hpp"
#include "lut.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <lcomp/composite.hpp>
#include <lcomp/filters.hpp>
#include <map>
#include <numbers>
#include <sstream>

namespace lcomp {
namespace {
FilterField number(std::string key, std::string label, double value, double lo, double hi) {
    return {std::move(key), std::move(label), "real", value, lo, hi};
}

FilterField integer(std::string key, std::string label, int value, int lo, int hi) {
    return {std::move(key),      std::move(label),        "int",
            std::int64_t{value}, static_cast<double>(lo), static_cast<double>(hi)};
}

const ldoc::ParamValue* value(const ldoc::Effect& effect, std::string_view key) {
    if (const auto it = effect.params.find(std::string(key)); it != effect.params.end())
        return &it->second;
    if (const auto* info = filter_info(effect.type))
        for (const auto& field : info->fields)
            if (field.key == key)
                return &field.value;
    return nullptr;
}

float clamp(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

std::shared_ptr<Picture> mask_image(const std::string& path) {
    struct Entry {
        FileStamp stamp;
        std::shared_ptr<Picture> image;
    };

    static thread_local std::map<std::string, Entry> cache;
    const auto stamp = file_stamp(path);
    if (!stamp)
        return {};
    if (auto it = cache.find(path); it != cache.end() && it->second.stamp == *stamp)
        return it->second.image;
    auto decoded = decode_image(path);
    if (!decoded)
        return {};
    auto image = std::make_shared<Picture>();
    image->width = decoded->width;
    image->height = decoded->height;
    image->stride = static_cast<unsigned>(image->width) * 4;
    image->pixels = std::move(decoded->pixels);
    // ponytail: retain eight decoded masks; raise cache budget only after measuring mask-heavy
    // shows.
    if (cache.size() >= 8)
        cache.clear();
    cache[path] = {*stamp, image};
    return image;
}
} // namespace

const std::vector<FilterInfo>& filter_catalog() {
    static const std::vector<FilterInfo> catalog{
        {"filter.color-correction",
         "Color Correction",
         "color_filter",
         false,
         {number("gamma", "Gamma", 0, -3, 3), number("contrast", "Contrast", 0, -4, 4),
          number("brightness", "Brightness", 0, -1, 1),
          number("saturation", "Saturation", 0, -1, 5),
          number("hue_shift", "Hue shift", 0, -180, 180), number("opacity", "Opacity", 1, 0, 1),
          integer("color_multiply", "Multiply color (RGB)", 0xffffff, 0, 0xffffff),
          integer("color_add", "Add color (RGB)", 0, 0, 0xffffff)}},
        {"filter.crop",
         "Crop / Pad",
         "crop_filter",
         false,
         {{"relative", "Relative", "bool", true},
          integer("left", "Left", 0, -8192, 8192),
          integer("top", "Top", 0, -8192, 8192),
          integer("right", "Right", 0, -8192, 8192),
          integer("bottom", "Bottom", 0, -8192, 8192),
          integer("cx", "Width", 1920, 1, 8192),
          integer("cy", "Height", 1080, 1, 8192)}},
        {"filter.scale",
         "Scaling / Aspect Ratio",
         "scale_filter",
         false,
         {integer("width", "Width", 1920, 1, 8192),
          integer("height", "Height", 1080, 1, 8192),
          {"aspect_only", "Use width/height as aspect ratio", "bool", false},
          {"sampling",
           "Sampling",
           "choice",
           std::string{"point"},
           0,
           0,
           {"point", "bilinear", "bicubic", "lanczos", "area"}}}},
        {"filter.color-key",
         "Color Key",
         "color_key_filter",
         false,
         {integer("key_color", "Key color (RGB)", 0x00ff00, 0, 0xffffff),
          number("similarity", "Similarity", 0.08, 0, 1),
          number("smoothness", "Smoothness", 0.05, 0.001, 1), number("opacity", "Opacity", 1, 0, 1),
          number("contrast", "Contrast", 0, -4, 4), number("brightness", "Brightness", 0, -1, 1),
          number("gamma", "Gamma", 0, -3, 3)}},
        {"filter.chroma-key",
         "Chroma Key",
         "chroma_key_filter",
         false,
         {integer("key_color", "Key color (RGB)", 0x00ff00, 0, 0xffffff),
          number("similarity", "Similarity", 0.4, 0, 1),
          number("smoothness", "Smoothness", 0.08, 0.001, 1),
          number("spill", "Spill reduction", 0.1, 0.001, 1), number("opacity", "Opacity", 1, 0, 1),
          number("contrast", "Contrast", 0, -4, 4), number("brightness", "Brightness", 0, -1, 1),
          number("gamma", "Gamma", 0, -3, 3)}},
        {"filter.luma-key",
         "Luma Key",
         "luma_key_filter",
         false,
         {number("luma_min", "Minimum", 0, 0, 1), number("luma_max", "Maximum", 1, 0, 1),
          number("luma_min_smooth", "Minimum smoothness", 0, 0, 1),
          number("luma_max_smooth", "Maximum smoothness", 0, 0, 1)}},
        {"filter.sharpen",
         "Sharpen",
         "sharpness_filter",
         false,
         {number("sharpness", "Sharpness", 0.08, 0, 1)}},
        {"filter.mask",
         "Image Mask / Blend",
         "mask_filter",
         false,
         {{"path", "Image file", "text", std::string{}},
          {"mode",
           "Mode",
           "choice",
           std::string{"alpha-color"},
           0,
           0,
           {"alpha-color", "alpha", "multiply", "add", "subtract"}},
          number("opacity", "Opacity", 1, 0, 1),
          integer("color", "Tint (RGB)", 0xffffff, 0, 0xffffff),
          {"stretch", "Stretch mask", "bool", true}}},
        {"filter.gain", "Gain", "gain_filter", true, {number("db", "Gain (dB)", 0, -60, 60)}},
        {"filter.invert", "Invert Polarity", "invert_polarity_filter", true, {}},
        {"filter.equalizer",
         "3-Band Equalizer",
         "basic_eq_filter",
         true,
         {number("low", "Low (dB)", 0, -20, 20), number("mid", "Mid (dB)", 0, -20, 20),
          number("high", "High (dB)", 0, -20, 20)}},
        {"filter.noise-gate",
         "Noise Gate",
         "noise_gate_filter",
         true,
         {number("open_threshold", "Open threshold (dB)", -26, -96, 0),
          number("close_threshold", "Close threshold (dB)", -32, -96, 0),
          number("attack_time", "Attack (ms)", 25, 0, 10000),
          number("hold_time", "Hold (ms)", 200, 0, 10000),
          number("release_time", "Release (ms)", 150, 0, 10000)}},
        {"filter.compressor",
         "Compressor",
         "compressor_filter",
         true,
         {number("ratio", "Ratio", 10, 1, 100),
          number("threshold", "Threshold (dB)", -18, -96, 0),
          number("attack_time", "Attack (ms)", 6, 0, 10000),
          number("release_time", "Release (ms)", 60, 0, 10000),
          number("output_gain", "Output gain (dB)", 0, -60, 60),
          {"sidechain", "Sidechain source ID (or none)", "text", std::string{"none"}}}},
        {"filter.limiter",
         "Limiter",
         "limiter_filter",
         true,
         {number("threshold", "Threshold (dB)", -6, -96, 0),
          number("release_time", "Release (ms)", 60, 0, 10000)}},
        {"filter.expander",
         "Expander",
         "expander_filter",
         true,
         {number("ratio", "Ratio", 2, 1, 20),
          number("threshold", "Threshold (dB)", -40, -96, 0),
          number("attack_time", "Attack (ms)", 10, 0, 10000),
          number("release_time", "Release (ms)", 50, 0, 10000),
          number("output_gain", "Output gain (dB)", 0, -60, 60),
          {"detector", "Detection", "choice", std::string{"RMS"}, 0, 0, {"RMS", "peak"}}}},
        {"filter.upward-compressor",
         "Upward Compressor",
         "upward_compressor_filter",
         true,
         {number("ratio", "Ratio", 0.5, 0.01, 1),
          number("threshold", "Threshold (dB)", -20, -96, 0),
          number("attack_time", "Attack (ms)", 10, 0, 10000),
          number("release_time", "Release (ms)", 50, 0, 10000),
          number("output_gain", "Output gain (dB)", 0, -60, 60),
          {"detector", "Detection", "choice", std::string{"RMS"}, 0, 0, {"RMS", "peak"}},
          number("knee", "Knee (dB)", 10, 0, 20)}},
        {"filter.noise-suppression",
         "Noise Suppression",
         "noise_suppress_filter",
         true,
         {{"method", "Method", "choice", std::string{"rnnoise"}, 0, 0, {"rnnoise", "speex"}},
          number("suppress_level", "Suppression (dB)", -30, -60, 0)}},
        {"filter.lut",
         "Apply LUT",
         "clut_filter",
         false,
         {{"path", "LUT file (.cube / PNG)", "text", std::string{}},
          number("amount", "Amount", 1, 0, 1)}},
        {"filter.scroll",
         "Scroll",
         "scroll_filter",
         false,
         {number("speed_x", "Horizontal speed (px/s)", 0, -5000, 5000),
          number("speed_y", "Vertical speed (px/s)", 0, -5000, 5000),
          {"loop", "Loop", "bool", true},
          {"limit_cx", "Limit width", "bool", false},
          {"limit_cy", "Limit height", "bool", false},
          integer("cx", "Width", 100, 1, 8192),
          integer("cy", "Height", 100, 1, 8192)}},
        {"filter.hdr-tonemap",
         "HDR Tone Mapping",
         "hdr_tonemap_filter",
         false,
         {{"transfer",
           "Input transfer",
           "choice",
           std::string{"pq"},
           0,
           0,
           {"pq", "hlg", "linear"}},
          number("input_maximum", "Input peak (nits)", 1000, 80, 10000),
          number("output_white", "Output white (nits)", 300, 80, 1000)}},
        {"filter.sdr-white",
         "SDR White Level (HDR)",
         "sdr_on_hdr_filter",
         false,
         {number("sdr_white_level", "SDR white (nits)", 300, 80, 1000),
          number("gamma", "Gamma", 2.4, 1, 4)}},
        {"filter.render-delay",
         "Render Delay",
         "gpu_delay",
         false,
         {integer("delay_ms", "Delay (ms)", 0, 0, 500)}},
        {"filter.video-delay",
         "Video Delay (Async)",
         "async_delay_filter",
         false,
         {integer("delay_ms", "Delay (ms)", 0, 0, 20000)}},
        {"filter.vst",
         "VST 2.x Plugin",
         "vst_filter",
         true,
         {{"path", "Linux plugin (.so)", "text", std::string{}},
          integer("program", "Program", 0, 0, 99999),
          {"chunk_data", "Saved plugin state (Base64)", "text", std::string{}}}}};
    return catalog;
}

const FilterInfo* filter_info(std::string_view type) {
    for (const auto& info : filter_catalog())
        if (info.type == type)
            return &info;
    return nullptr;
}

double filter_number(const ldoc::Effect& effect, std::string_view key) {
    const auto* v = value(effect, key);
    if (v) {
        if (const auto* d = std::get_if<double>(v))
            return *d;
        if (const auto* i = std::get_if<std::int64_t>(v))
            return static_cast<double>(*i);
    }
    return 0;
}

std::string filter_text(const ldoc::Effect& effect, std::string_view key) {
    const auto* v = value(effect, key);
    const auto* s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string{};
}

bool filter_flag(const ldoc::Effect& effect, std::string_view key) {
    const auto* v = value(effect, key);
    const auto* b = v ? std::get_if<bool>(v) : nullptr;
    return b && *b;
}

std::expected<void, std::string> validate_filter(const ldoc::Effect& effect) {
    const auto* info = filter_info(effect.type);
    if (!info)
        return std::unexpected("Unknown filter type: " + effect.type);
    if (effect.mask || effect.blend)
        return std::unexpected("Filter mask/blend override is unsupported");
    if (effect.type == "filter.vst") {
        const auto path = filter_text(effect, "path"), chunk = filter_text(effect, "chunk_data");
        if (path.size() > 4096 || path.find('\0') != std::string::npos ||
            chunk.size() > 1024 * 1024)
            return std::unexpected("Plugin path or saved state exceeds supported size");
        if (std::ranges::any_of(chunk, [](unsigned char c) {
                return !(std::isalnum(c) || c == '+' || c == '/' || c == '=');
            }))
            return std::unexpected("Plugin state must be Base64");
        for (const auto& [key, v] : effect.params)
            if (key.starts_with("parameter.")) {
                unsigned index = 0;
                const auto parsed =
                    std::from_chars(key.data() + 10, key.data() + key.size(), index);
                const auto* n = std::get_if<double>(&v);
                if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() ||
                    index >= 512 || !n || !std::isfinite(*n) || *n < 0 || *n > 1)
                    return std::unexpected(
                        "Plugin parameter must be normalized 0-1 with index 0-511");
            }
    }
    for (const auto& field : info->fields) {
        const auto* v = value(effect, field.key);
        if (!v)
            return std::unexpected("Missing filter setting: " + field.key);
        if (field.kind == "real" || field.kind == "int") {
            if (!std::holds_alternative<double>(*v) && !std::holds_alternative<std::int64_t>(*v))
                return std::unexpected("Filter setting must be numeric: " + field.key);
            const auto n = filter_number(effect, field.key);
            if (!std::isfinite(n) || n < field.minimum || n > field.maximum ||
                (field.kind == "int" && n != std::floor(n)))
                return std::unexpected("Filter setting out of range: " + field.key);
        } else if (field.kind == "bool" && !std::holds_alternative<bool>(*v))
            return std::unexpected("Filter setting must be boolean: " + field.key);
        else if (field.kind != "bool" && !std::holds_alternative<std::string>(*v))
            return std::unexpected("Filter setting must be text: " + field.key);
        if (field.kind == "choice" &&
            std::ranges::find(field.choices, filter_text(effect, field.key)) == field.choices.end())
            return std::unexpected("Unknown filter choice: " + field.key);
    }
    return {};
}

bool video_filters_supported(std::span<const ldoc::Effect> effects) {
    return std::ranges::all_of(
        effects, [](const auto& e) { return !e.enabled || validate_filter(e).has_value(); });
}

std::expected<ldoc::Vec2, std::string> filter_size(std::span<const ldoc::Effect> effects, double w,
                                                   double h) {
    for (const auto& e : effects) {
        if (!e.enabled)
            continue;
        if (const auto* info = filter_info(e.type); info && info->audio)
            continue;
        if (auto valid = validate_filter(e); !valid)
            return std::unexpected(valid.error());
        if (e.type == "filter.mask" && !mask_image(filter_text(e, "path")))
            return std::unexpected("Mask image is missing or unreadable");
        if (e.type == "filter.lut" && !load_lut(filter_text(e, "path")))
            return std::unexpected(
                "LUT is unreadable or unsupported (2-64 entries per axis, finite 0-1 output)");
        if (e.type == "filter.crop") {
            w = filter_flag(e, "relative")
                    ? w - filter_number(e, "left") - filter_number(e, "right")
                    : filter_number(e, "cx");
            h = filter_flag(e, "relative")
                    ? h - filter_number(e, "top") - filter_number(e, "bottom")
                    : filter_number(e, "cy");
        } else if (e.type == "filter.scale") {
            const auto width = filter_number(e, "width"), height = filter_number(e, "height");
            if (filter_flag(e, "aspect_only")) {
                if (width / height > w / h)
                    w = std::floor(h * width / height);
                else
                    h = std::floor(w * height / width);
            } else {
                w = width;
                h = height;
            }
        } else if (e.type == "filter.scroll") {
            if (filter_flag(e, "limit_cx"))
                w = filter_number(e, "cx");
            if (filter_flag(e, "limit_cy"))
                h = filter_number(e, "cy");
        }
        if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 8192 || h > 8192 ||
            w * h > 33554432)
            return std::unexpected("Filtered image dimensions exceed supported range");
    }
    return ldoc::Vec2{w, h};
}

VideoFilter resolve_filter(const ldoc::Effect& e, int w, int h, std::uint64_t time_ns) {
    VideoFilter out;
    out.id = e.id;
    out.time_ns = time_ns;
    const auto size = filter_size(std::span{&e, 1}, w, h);
    if (!size)
        return out;
    out.width = static_cast<int>(size->x);
    out.height = static_cast<int>(size->y);
    auto& p = out.values;
    const auto n = [&](const char* key) { return static_cast<float>(filter_number(e, key)); };
    if (e.type == "filter.color-correction") {
        out.kind = 1;
        p[0] = n("gamma") < 0 ? 1 - n("gamma") : 1 / (1 + n("gamma"));
        p[1] = n("contrast") < 0 ? 1 / (1 - n("contrast")) : 1 + n("contrast");
        p[2] = n("brightness");
        p[3] = 1 + n("saturation");
        p[4] = n("hue_shift") * std::numbers::pi_v<float> / 180;
        p[5] = n("opacity");
        for (std::size_t k = 0; k < 3; ++k) {
            const auto shift = static_cast<unsigned>((2 - k) * 8);
            p[6 + k] =
                static_cast<float>((static_cast<unsigned>(n("color_multiply")) >> shift) & 255) /
                255;
            p[9 + k] =
                static_cast<float>((static_cast<unsigned>(n("color_add")) >> shift) & 255) / 255;
        }
        if (p[0] == 1 && p[1] == 1 && p[2] == 0 && p[3] == 1 && p[4] == 0 && p[5] == 1 &&
            p[6] == 1 && p[7] == 1 && p[8] == 1 && p[9] == 0 && p[10] == 0 && p[11] == 0)
            out.kind = 0;
    } else if (e.type == "filter.crop") {
        out.kind = 2;
        p[0] = n("left");
        p[1] = n("top");
    } else if (e.type == "filter.scale") {
        out.kind = 3;
        const auto sampling = filter_text(e, "sampling");
        p[0] = sampling == "bilinear"  ? 1
               : sampling == "bicubic" ? 2
               : sampling == "lanczos" ? 3
               : sampling == "area"    ? 4
                                       : 0;
    } else if (e.type == "filter.color-key" || e.type == "filter.chroma-key") {
        out.kind = e.type == "filter.color-key" ? 4 : 5;
        const auto c = static_cast<unsigned>(filter_number(e, "key_color"));
        p[0] = static_cast<float>((c >> 16) & 255) / 255;
        p[1] = static_cast<float>((c >> 8) & 255) / 255;
        p[2] = static_cast<float>(c & 255) / 255;
        p[3] = n("similarity");
        p[4] = n("smoothness");
        p[5] = n("spill");
        p[6] = n("opacity");
        const auto contrast = n("contrast"), gamma = n("gamma");
        p[7] = contrast < 0 ? 1 / (1 - contrast) : 1 + contrast;
        p[8] = n("brightness");
        p[9] = gamma < 0 ? 1 - gamma : 1 / (1 + gamma);
    } else if (e.type == "filter.luma-key") {
        out.kind = 6;
        p[0] = n("luma_min");
        p[1] = n("luma_max");
        p[2] = n("luma_min_smooth");
        p[3] = n("luma_max_smooth");
    } else if (e.type == "filter.sharpen") {
        out.kind = 7;
        p[0] = n("sharpness");
    } else if (e.type == "filter.mask") {
        out.kind = 8;
        out.auxiliary = mask_image(filter_text(e, "path"));
        p[0] = n("opacity");
        p[2] = filter_flag(e, "stretch") ? 1 : 0;
        const auto tint = static_cast<unsigned>(n("color"));
        p[3] = static_cast<float>((tint >> 16) & 255) / 255;
        p[4] = static_cast<float>((tint >> 8) & 255) / 255;
        p[5] = static_cast<float>(tint & 255) / 255;
        const auto mode = filter_text(e, "mode");
        p[1] = mode == "alpha-color" ? 0
               : mode == "alpha"     ? 1
               : mode == "multiply"  ? 2
               : mode == "add"       ? 3
                                     : 4;
    }
    if (e.type == "filter.lut") {
        out.kind = 9;
        const auto lut = load_lut(filter_text(e, "path"));
        if (lut) {
            out.auxiliary = lut->image;
            p[0] = static_cast<float>(lut->size);
            p[1] = n("amount");
            for (std::size_t k = 0; k < 3; ++k) {
                p[2 + k] = lut->minimum[k];
                p[5 + k] = lut->maximum[k];
            }
        }
    } else if (e.type == "filter.scroll") {
        struct Clock {
            ldoc::ParamMap params;
            std::uint64_t start = 0, last = 0;
        };

        static thread_local std::map<ldoc::Id, Clock> clocks;
        std::erase_if(clocks, [&](const auto& item) {
            return time_ns > item.second.last && time_ns - item.second.last > 60000000000ULL;
        });
        auto& clock = clocks[e.id];
        if (clock.params != e.params || time_ns < clock.start || !clock.last) {
            clock.params = e.params;
            clock.start = time_ns;
        }
        clock.last = time_ns;
        const double seconds = static_cast<double>(time_ns - clock.start) / 1e9;
        out.kind = 10;
        const bool loop = filter_flag(e, "loop");
        p[0] = static_cast<float>(
            loop ? std::fmod(seconds * filter_number(e, "speed_x"), w)
                 : std::clamp(seconds * filter_number(e, "speed_x"), -double(w), double(w)));
        p[1] = static_cast<float>(
            loop ? std::fmod(seconds * filter_number(e, "speed_y"), h)
                 : std::clamp(seconds * filter_number(e, "speed_y"), -double(h), double(h)));
        p[2] = filter_flag(e, "loop") ? 1 : 0;
    } else if (e.type == "filter.hdr-tonemap") {
        out.kind = 11;
        const auto transfer = filter_text(e, "transfer");
        p[0] = transfer == "pq" ? 0 : transfer == "hlg" ? 1 : 2;
        p[1] = n("input_maximum");
        p[2] = n("output_white");
    } else if (e.type == "filter.sdr-white") {
        out.kind = 12;
        p[0] = n("sdr_white_level") / 300;
        p[1] = n("gamma");
    } else if (e.type == "filter.render-delay" || e.type == "filter.video-delay") {
        out.kind = 13;
        p[0] = n("delay_ms");
    }
    return out;
}

Picture filter_picture(const Picture& input, const VideoFilter& filter) {
    if (filter.kind == 13 && filter.values[0] > 0) {
        struct History {
            int w = 0, h = 0;
            float delay = 0;
            std::uint64_t last = 0;
            std::deque<std::pair<std::uint64_t, Picture>> frames;
        };

        static thread_local std::map<ldoc::Id, History> histories;
        std::erase_if(histories, [&](const auto& item) {
            return filter.time_ns > item.second.last &&
                   filter.time_ns - item.second.last > 21000000000ULL;
        });
        auto& history = histories[filter.id];
        if (history.w != input.width || history.h != input.height ||
            history.delay != filter.values[0] || filter.time_ns < history.last)
            history = {input.width, input.height, filter.values[0], 0, {}};
        const auto delay = static_cast<std::uint64_t>(filter.values[0] * 1000000);
        const auto due = filter.time_ns > delay ? filter.time_ns - delay : 0;
        while (history.frames.size() > 1 && history.frames[1].first <= due)
            history.frames.pop_front();
        std::size_t bytes = 0;
        for (const auto& [id, h] : histories)
            for (const auto& [time, frame] : h.frames)
                bytes += frame.pixels.size();
        bool full = false;
        if (history.last != filter.time_ns || history.frames.empty()) {
            if (bytes + input.pixels.size() > 256ULL * 1024 * 1024 || history.frames.size() >= 1200)
                full = true;
            else
                history.frames.push_back({filter.time_ns, input});
            history.last = filter.time_ns;
        }
        if (!full && !history.frames.empty() && history.frames.front().first <= due)
            return history.frames.front().second;
        Picture empty = input;
        std::ranges::fill(empty.pixels, 0);
        empty.gaps.push_back(
            {filter.id, "Video delay", full ? GapReason::UnsupportedEffect : GapReason::NoFrame});
        empty.status = PictureStatus::Partial;
        return empty;
    }
    Picture out;
    out.width = filter.width;
    out.height = filter.height;
    out.stride = static_cast<unsigned>(out.width) * 4;
    out.status = input.status;
    out.gaps = input.gaps;
    out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height));
    const auto& p = filter.values;
    const auto read = [&](int x, int y) {
        std::array<float, 4> c{};
        x = std::clamp(x, 0, input.width - 1);
        y = std::clamp(y, 0, input.height - 1);
        const auto* v = input.read_from() + static_cast<std::size_t>(y) * input.stride +
                        static_cast<std::size_t>(x) * 4;
        for (std::size_t k = 0; k < 4; ++k)
            c[k] = v[k] / 255.0f;
        return c;
    };
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            auto c = read(x, y);
            if (filter.kind == 2) {
                const int sx = x + static_cast<int>(p[0]), sy = y + static_cast<int>(p[1]);
                c = sx < 0 || sy < 0 || sx >= input.width || sy >= input.height
                        ? std::array<float, 4>{}
                        : read(sx, sy);
            } else if (filter.kind == 3) {
                const float rx = static_cast<float>(input.width) / static_cast<float>(out.width);
                const float ry = static_cast<float>(input.height) / static_cast<float>(out.height);
                if (p[0] == 0)
                    c = read(static_cast<int>(static_cast<float>(x) * rx),
                             static_cast<int>(static_cast<float>(y) * ry));
                else {
                    const float sx = (static_cast<float>(x) + 0.5f) * rx - 0.5f,
                                sy = (static_cast<float>(y) + 0.5f) * ry - 0.5f;
                    const auto weight = [&](float d, float ratio) {
                        d = std::abs(d);
                        if (p[0] == 4)
                            return std::max(0.0f, std::min(d + 0.5f, ratio * 0.5f) -
                                                      std::max(d - 0.5f, -ratio * 0.5f));
                        if (p[0] == 1)
                            return std::max(0.0f, 1 - d);
                        if (p[0] == 2)
                            return d < 1   ? (1.5f * d - 2.5f) * d * d + 1
                                   : d < 2 ? ((-0.5f * d + 2.5f) * d - 4) * d + 2
                                           : 0;
                        if (d < 1e-6f)
                            return 1.0f;
                        const float v = std::numbers::pi_v<float> * d;
                        return d < 3 ? std::sin(v) * std::sin(v / 3) / (v * v / 3) : 0;
                    };
                    const float ax = p[0] == 4   ? rx * 0.5f + 0.5f
                                     : p[0] == 3 ? 3
                                     : p[0] == 2 ? 2
                                                 : 1;
                    const float ay = p[0] == 4 ? ry * 0.5f + 0.5f : ax;
                    c = {};
                    float total = 0;
                    for (int iy = static_cast<int>(std::ceil(sy - ay));
                         iy <= static_cast<int>(std::floor(sy + ay)); ++iy)
                        for (int ix = static_cast<int>(std::ceil(sx - ax));
                             ix <= static_cast<int>(std::floor(sx + ax)); ++ix) {
                            const float w = weight(static_cast<float>(ix) - sx, rx) *
                                            weight(static_cast<float>(iy) - sy, ry);
                            const auto sample = read(ix, iy);
                            for (std::size_t k = 0; k < 3; ++k)
                                c[k] += sample[k] * sample[3] * w;
                            c[3] += sample[3] * w;
                            total += w;
                        }
                    for (std::size_t k = 0; k < 3; ++k)
                        c[k] = c[3] > 1e-6f ? c[k] / c[3] : 0;
                    c[3] /= total;
                }
            } else if (filter.kind == 1) {
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = (std::pow(std::max(c[k], 0.0f), p[0]) - 0.5f) * p[1] + 0.5f + p[2];
                const float l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = l + (c[k] - l) * p[3];
                const float co = std::cos(p[4]), si = std::sin(p[4]),
                            sum = (c[0] + c[1] + c[2]) * (1 - co) / 3;
                const auto old = c;
                c[0] = old[0] * co + (old[2] - old[1]) * si / std::sqrt(3.0f) + sum;
                c[1] = old[1] * co + (old[0] - old[2]) * si / std::sqrt(3.0f) + sum;
                c[2] = old[2] * co + (old[1] - old[0]) * si / std::sqrt(3.0f) + sum;
                c[3] *= p[5];
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = c[k] * p[6 + k] + p[9 + k];
            } else if (filter.kind == 4 || filter.kind == 5) {
                float distance = 0;
                if (filter.kind == 4)
                    for (std::size_t k = 0; k < 3; ++k)
                        distance += (c[k] - p[k]) * (c[k] - p[k]);
                else {
                    const float u = -0.168736f * (c[0] - p[0]) - 0.331264f * (c[1] - p[1]) +
                                    0.5f * (c[2] - p[2]);
                    const float v = 0.5f * (c[0] - p[0]) - 0.418688f * (c[1] - p[1]) -
                                    0.081312f * (c[2] - p[2]);
                    distance = u * u + v * v;
                }
                const float base = std::sqrt(distance) - p[3];
                c[3] *= std::pow(clamp(base / p[4]), 1.5f);
                if (filter.kind == 5) {
                    const float spill = std::pow(clamp(base / p[5]), 1.5f),
                                l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
                    for (std::size_t k = 0; k < 3; ++k)
                        c[k] = l + (c[k] - l) * spill;
                }
                c[3] *= p[6];
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = std::pow(std::max(0.0f, (c[k] - 0.5f) * p[7] + 0.5f + p[8]), p[9]);
            } else if (filter.kind == 6) {
                const float l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
                c[3] *= (p[2] > 0 ? clamp((l - p[0]) / p[2]) : float(l >= p[0])) *
                        (p[3] > 0 ? clamp((p[1] - l) / p[3]) : float(l <= p[1]));
            } else if (filter.kind == 7) {
                const auto a = read(x - 1, y), b = read(x + 1, y), d = read(x, y - 1),
                           e = read(x, y + 1);
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] += p[0] * (4 * c[k] - a[k] - b[k] - d[k] - e[k]);
            } else if (filter.kind == 8 && filter.auxiliary) {
                const auto& m = *filter.auxiliary;
                float ux = (static_cast<float>(x) + 0.5f) / static_cast<float>(out.width),
                      uy = (static_cast<float>(y) + 0.5f) / static_cast<float>(out.height);
                if (p[2] == 0) {
                    const float scale =
                        std::min(static_cast<float>(out.width) / static_cast<float>(m.width),
                                 static_cast<float>(out.height) / static_cast<float>(m.height));
                    ux = (ux - 0.5f) * static_cast<float>(out.width) /
                             (static_cast<float>(m.width) * scale) +
                         0.5f;
                    uy = (uy - 0.5f) * static_cast<float>(out.height) /
                             (static_cast<float>(m.height) * scale) +
                         0.5f;
                }
                const int mx = std::clamp(static_cast<int>(ux * static_cast<float>(m.width)), 0,
                                          m.width - 1),
                          my = std::clamp(static_cast<int>(uy * static_cast<float>(m.height)), 0,
                                          m.height - 1);
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] *= p[3 + k];
                const auto* v = m.read_from() + static_cast<std::size_t>(my) * m.stride +
                                static_cast<std::size_t>(mx) * 4;
                if (p[1] < 2)
                    c[3] *= (p[1] == 0 ? static_cast<float>(v[0] + v[1] + v[2]) / 765.0f
                                       : v[3] / 255.0f) *
                            p[0];
                else
                    for (std::size_t k = 0; k < 3; ++k) {
                        const float mask = v[k] / 255.0f;
                        const float changed = p[1] == 2   ? c[k] * mask
                                              : p[1] == 3 ? c[k] + mask
                                                          : c[k] - mask;
                        c[k] += (changed - c[k]) * p[0];
                    }
            }
            if (filter.kind == 9 && filter.auxiliary) {
                const int n = static_cast<int>(p[0]);
                const auto& lut = *filter.auxiliary;
                std::array<int, 3> lo{}, hi{};
                std::array<float, 3> f{}, result{};
                for (std::size_t k = 0; k < 3; ++k) {
                    const float v = clamp((c[k] - p[2 + k]) / (p[5 + k] - p[2 + k])) *
                                    static_cast<float>(n - 1);
                    lo[k] = static_cast<int>(v);
                    hi[k] = std::min(n - 1, lo[k] + 1);
                    f[k] = v - static_cast<float>(lo[k]);
                }
                for (int corner = 0; corner < 8; ++corner) {
                    const int r = corner & 1 ? hi[0] : lo[0], g = corner & 2 ? hi[1] : lo[1],
                              b = corner & 4 ? hi[2] : lo[2];
                    const float weight = (corner & 1 ? f[0] : 1 - f[0]) *
                                         (corner & 2 ? f[1] : 1 - f[1]) *
                                         (corner & 4 ? f[2] : 1 - f[2]);
                    const auto* pixel = lut.read_from() + static_cast<std::size_t>(g) * lut.stride +
                                        static_cast<std::size_t>(b * n + r) * 4;
                    for (std::size_t k = 0; k < 3; ++k)
                        result[k] += pixel[k] / 255.0f * weight;
                }
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] += (result[k] - c[k]) * p[1];
            } else if (filter.kind == 10) {
                int sx = x - static_cast<int>(p[0]), sy = y - static_cast<int>(p[1]);
                if (p[2] > 0) {
                    sx = (sx % input.width + input.width) % input.width;
                    sy = (sy % input.height + input.height) % input.height;
                }
                c = sx < 0 || sy < 0 || sx >= input.width || sy >= input.height
                        ? std::array<float, 4>{}
                        : read(sx, sy);
            } else if (filter.kind == 11 || filter.kind == 12) {
                for (std::size_t k = 0; k < 3; ++k) {
                    float linear = 0;
                    if (filter.kind == 12)
                        linear = std::pow(std::max(c[k], 0.0f), p[1]) * p[0];
                    else {
                        float nits = 0;
                        if (p[0] == 0) {
                            const float v = std::pow(std::max(c[k], 0.0f), 1 / 78.84375f);
                            nits = 10000 * std::pow(std::max(v - 0.8359375f, 0.0f) /
                                                        std::max(18.8515625f - 18.6875f * v, 1e-6f),
                                                    1 / 0.1593017578125f);
                        } else if (p[0] == 1)
                            nits = (c[k] <= 0.5f ? c[k] * c[k] / 3
                                                 : (std::exp((c[k] - 0.55991073f) / 0.17883277f) +
                                                    0.28466892f) /
                                                       12) *
                                   p[1];
                        else
                            nits = c[k] * p[1];
                        const float v = nits / p[2], white = p[1] / p[2];
                        linear = v * (1 + v / (white * white)) / (1 + v);
                    }
                    c[k] = linear <= 0.0031308f ? 12.92f * linear
                                                : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
                }
            }
            auto* dst = out.pixels.data() + static_cast<std::size_t>(y) * out.stride +
                        static_cast<std::size_t>(x) * 4;
            for (std::size_t k = 0; k < 4; ++k)
                dst[k] = static_cast<std::uint8_t>(std::lround(clamp(c[k]) * 255));
        }
    return out;
}
} // namespace lcomp

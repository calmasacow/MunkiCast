// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <expected>
#include <ldoc/node.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lcomp {
struct FilterField {
    std::string key, label, kind;
    ldoc::ParamValue value;
    double minimum = 0, maximum = 0;
    std::vector<std::string> choices{};
};

struct FilterInfo {
    std::string type, name, obs;
    bool audio = false;
    std::vector<FilterField> fields;
};

[[nodiscard]] const std::vector<FilterInfo>& filter_catalog();
[[nodiscard]] const FilterInfo* filter_info(std::string_view type);
[[nodiscard]] std::expected<void, std::string> validate_filter(const ldoc::Effect& effect);
[[nodiscard]] double filter_number(const ldoc::Effect& effect, std::string_view key);
[[nodiscard]] std::string filter_text(const ldoc::Effect& effect, std::string_view key);
[[nodiscard]] bool filter_flag(const ldoc::Effect& effect, std::string_view key);
[[nodiscard]] bool video_filters_supported(std::span<const ldoc::Effect> effects);
[[nodiscard]] std::expected<ldoc::Vec2, std::string>
filter_size(std::span<const ldoc::Effect> effects, double width, double height);

// Same packed operation on CPU and Vulkan. Values are validated before packing.
struct Picture;

struct VideoFilter {
    ldoc::Id id;
    std::uint64_t time_ns = 0;
    int kind = 0;
    int width = 0, height = 0;
    std::array<float, 16> values{};
    std::shared_ptr<Picture> auxiliary;
};

[[nodiscard]] VideoFilter resolve_filter(const ldoc::Effect& effect, int width, int height,
                                         std::uint64_t time_ns = 0);
[[nodiscard]] Picture filter_picture(const Picture& input, const VideoFilter& filter);
} // namespace lcomp

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ldoc {
namespace {

double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

std::uint8_t to_u8(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

template <typename T, std::size_t N>
std::optional<T> lookup(const std::array<std::pair<T, std::string_view>, N>& table,
                        std::string_view s) {
    for (const auto& [value, name] : table)
        if (name == s)
            return value;
    return std::nullopt;
}

template <typename T, std::size_t N>
std::string_view name_of(const std::array<std::pair<T, std::string_view>, N>& table, T v) {
    for (const auto& [value, name] : table)
        if (value == v)
            return name;
    return "unknown";
}

constexpr std::array<std::pair<ColorSpace, std::string_view>, 5> kColorSpaces = {{
    {ColorSpace::Srgb, "srgb"},
    {ColorSpace::Rec709, "rec709"},
    {ColorSpace::Rec2020, "rec2020"},
    {ColorSpace::Rec2100Pq, "rec2100-pq"},
    {ColorSpace::Rec2100Hlg, "rec2100-hlg"},
}};

constexpr std::array<std::pair<FitMode, std::string_view>, 6> kFitModes = {{
    {FitMode::Natural, "natural"},
    {FitMode::Stretch, "stretch"},
    {FitMode::Contain, "contain"},
    {FitMode::Cover, "cover"},
    {FitMode::FillWidth, "fill-width"},
    {FitMode::FillHeight, "fill-height"},
}};

constexpr std::array<std::pair<BlendMode, std::string_view>, 25> kBlendModes = {{
    {BlendMode::Normal, "normal"},
    {BlendMode::Dissolve, "dissolve"},
    {BlendMode::Darken, "darken"},
    {BlendMode::Multiply, "multiply"},
    {BlendMode::ColorBurn, "color-burn"},
    {BlendMode::LinearBurn, "linear-burn"},
    {BlendMode::Lighten, "lighten"},
    {BlendMode::Screen, "screen"},
    {BlendMode::ColorDodge, "color-dodge"},
    {BlendMode::LinearDodge, "linear-dodge"},
    {BlendMode::Overlay, "overlay"},
    {BlendMode::SoftLight, "soft-light"},
    {BlendMode::HardLight, "hard-light"},
    {BlendMode::VividLight, "vivid-light"},
    {BlendMode::LinearLight, "linear-light"},
    {BlendMode::PinLight, "pin-light"},
    {BlendMode::HardMix, "hard-mix"},
    {BlendMode::Difference, "difference"},
    {BlendMode::Exclusion, "exclusion"},
    {BlendMode::Subtract, "subtract"},
    {BlendMode::Divide, "divide"},
    {BlendMode::Hue, "hue"},
    {BlendMode::Saturation, "saturation"},
    {BlendMode::Color, "color"},
    {BlendMode::Luminosity, "luminosity"},
}};

} // namespace

Rgba Rgba::from_srgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return Rgba{srgb_to_linear(r / 255.0), srgb_to_linear(g / 255.0), srgb_to_linear(b / 255.0),
                a / 255.0};
}

std::array<std::uint8_t, 4> Rgba::to_srgb8() const {
    return {to_u8(linear_to_srgb(std::clamp(r, 0.0, 1.0))),
            to_u8(linear_to_srgb(std::clamp(g, 0.0, 1.0))),
            to_u8(linear_to_srgb(std::clamp(b, 0.0, 1.0))), to_u8(a)};
}

double Fps::hz() const {
    return den > 0 ? static_cast<double>(num) / static_cast<double>(den) : 0.0;
}

std::string_view to_string(ColorSpace cs) { return name_of(kColorSpaces, cs); }
std::optional<ColorSpace> color_space_from_string(std::string_view s) {
    return lookup(kColorSpaces, s);
}

std::string_view to_string(FitMode m) { return name_of(kFitModes, m); }
std::optional<FitMode> fit_mode_from_string(std::string_view s) { return lookup(kFitModes, s); }

std::string_view to_string(BlendMode m) { return name_of(kBlendModes, m); }
std::optional<BlendMode> blend_mode_from_string(std::string_view s) {
    return lookup(kBlendModes, s);
}

bool needs_backdrop(BlendMode mode) {
    switch (mode) {
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
        return true;
    default:
        return false;
    }
}

} // namespace ldoc

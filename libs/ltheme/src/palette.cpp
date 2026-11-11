// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ltheme/palette.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace ltheme {
namespace {

std::optional<int> hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return std::nullopt;
}

std::uint8_t clamp_u8(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
}

constexpr std::array<std::string_view, role_count()> kRoleNames = {
    "window",     "panel-background", "panel-header", "panel-header-active",
    "border",     "divider",          "divider-hover", "text",
    "text-muted", "accent",           "drop-fill",    "drop-border",
    "good",       "warning",          "error",
};

} // namespace

std::string Color::argb_hex() const {
    return std::format("#{:02x}{:02x}{:02x}{:02x}", a, r, g, b);
}

std::string Color::css_hex() const {
    return std::format("#{:02x}{:02x}{:02x}{:02x}", r, g, b, a);
}

Color Color::with_alpha(double alpha) const {
    return Color{r, g, b, clamp_u8(alpha * 255.0)};
}

Color Color::mix(const Color& other, double t) const {
    const double u = std::clamp(t, 0.0, 1.0);
    const auto lerp = [u](std::uint8_t x, std::uint8_t y) {
        return clamp_u8(static_cast<double>(x) * (1.0 - u) + static_cast<double>(y) * u);
    };
    return Color{lerp(r, other.r), lerp(g, other.g), lerp(b, other.b), lerp(a, other.a)};
}

std::optional<Color> parse_color(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);

    if (s.starts_with("#"))
        s.remove_prefix(1);
    else if (s.starts_with("0x") || s.starts_with("0X"))
        s.remove_prefix(2);
    else
        return std::nullopt;

    std::array<int, 8> d{};
    if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8)
        return std::nullopt;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto v = hex_digit(s[i]);
        if (!v)
            return std::nullopt;
        d[i] = *v;
    }

    const auto byte = [](int hi, int lo) { return static_cast<std::uint8_t>(hi * 16 + lo); };
    switch (s.size()) {
    case 3:
        return Color{byte(d[0], d[0]), byte(d[1], d[1]), byte(d[2], d[2]), 255};
    case 4:
        return Color{byte(d[0], d[0]), byte(d[1], d[1]), byte(d[2], d[2]), byte(d[3], d[3])};
    case 6:
        return Color{byte(d[0], d[1]), byte(d[2], d[3]), byte(d[4], d[5]), 255};
    default:
        return Color{byte(d[0], d[1]), byte(d[2], d[3]), byte(d[4], d[5]), byte(d[6], d[7])};
    }
}

std::string_view role_name(Role r) {
    const auto i = static_cast<std::size_t>(r);
    return i < kRoleNames.size() ? kRoleNames[i] : "unknown";
}

std::optional<Role> role_from_name(std::string_view name) {
    for (std::size_t i = 0; i < kRoleNames.size(); ++i)
        if (kRoleNames[i] == name)
            return static_cast<Role>(i);
    return std::nullopt;
}

Palette Palette::fallback() {
    Palette p;
    p.set_dark(true);
    p.set_source("built-in");
    const Color accent{0x5c, 0x9c, 0xf5, 0xff};
    p.set(Role::Window, Color{0x0d, 0x0e, 0x12, 0xff});
    p.set(Role::PanelBackground, Color{0x16, 0x18, 0x1e, 0xff});
    p.set(Role::PanelHeader, Color{0x1e, 0x21, 0x29, 0xff});
    p.set(Role::PanelHeaderActive, Color{0x2a, 0x2e, 0x39, 0xff});
    p.set(Role::Border, Color{0x33, 0x38, 0x45, 0xff});
    p.set(Role::Divider, Color{0x0d, 0x0e, 0x12, 0xff});
    p.set(Role::DividerHover, accent);
    p.set(Role::Text, Color{0xd2, 0xd8, 0xe4, 0xff});
    p.set(Role::TextMuted, Color{0x7c, 0x85, 0x99, 0xff});
    p.set(Role::Accent, accent);
    p.set(Role::DropFill, accent.with_alpha(0.22));
    p.set(Role::DropBorder, accent);
    p.set(Role::Good, Color{0x7a, 0xc0, 0x6a, 0xff});
    p.set(Role::Warning, Color{0xd9, 0xa5, 0x5c, 0xff});
    p.set(Role::Error, Color{0xe0, 0x66, 0x72, 0xff});
    return p;
}

} // namespace ltheme

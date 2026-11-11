// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// The theme token model.
///
/// Deliberately toolkit-neutral: a palette is a set of named colours and nothing else. The UI
/// converts them for whatever it draws with, and the theme can therefore be parsed and tested
/// without a window system, a compositor, or Qt.
namespace ltheme {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    /// "#AARRGGBB" -- the form Qt and QML parse natively.
    [[nodiscard]] std::string argb_hex() const;
    /// "#rrggbbaa" -- the form CSS and most config files use.
    [[nodiscard]] std::string css_hex() const;

    [[nodiscard]] Color with_alpha(double alpha) const;
    /// Mix towards `other`. t = 0 is this colour, t = 1 is `other`.
    [[nodiscard]] Color mix(const Color& other, double t) const;

    friend bool operator==(const Color&, const Color&) = default;
};

/// Parses "#rgb", "#rrggbb", "#rrggbbaa" and the "0x"-prefixed forms. Returns nullopt rather than
/// a default colour, so a typo in a theme file is distinguishable from black.
[[nodiscard]] std::optional<Color> parse_color(std::string_view s);

/// The roles the MunkiCast UI actually draws with. Adding one here is a deliberate act: every role
/// has to be derivable from any theme we might follow, including a sparse or hostile one.
enum class Role : std::uint8_t {
    Window,            ///< behind everything, visible only in gaps
    PanelBackground,   ///< a panel's content area
    PanelHeader,       ///< the tab bar of an unfocused group
    PanelHeaderActive, ///< the tab bar of the focused group
    Border,            ///< the hairline around a panel
    Divider,           ///< the draggable gap between two panels
    DividerHover,
    Text,
    TextMuted,
    Accent,
    DropFill,   ///< translucent wash over the zone under the cursor (ADR-0016)
    DropBorder, ///< its outline, which must read against the canvas as well as a list
    Good,
    Warning,
    Error,
    Count_,
};

[[nodiscard]] constexpr std::size_t role_count() {
    return static_cast<std::size_t>(Role::Count_);
}

[[nodiscard]] std::string_view role_name(Role r);
[[nodiscard]] std::optional<Role> role_from_name(std::string_view name);

class Palette {
public:
    /// The shipped palette, used when no theme provider has anything to say. Dark, neutral, and
    /// deliberately not a copy of any particular Omarchy theme.
    [[nodiscard]] static Palette fallback();

    [[nodiscard]] Color get(Role r) const { return colors_[static_cast<std::size_t>(r)]; }
    void set(Role r, Color c) { colors_[static_cast<std::size_t>(r)] = c; }

    [[nodiscard]] bool dark() const { return dark_; }
    void set_dark(bool d) { dark_ = d; }

    /// Where this palette came from, for the UI to show and for bug reports to carry.
    [[nodiscard]] const std::string& source() const { return source_; }
    void set_source(std::string s) { source_ = std::move(s); }

    friend bool operator==(const Palette&, const Palette&) = default;

private:
    std::array<Color, role_count()> colors_{};
    bool dark_ = true;
    std::string source_ = "built-in";
};

} // namespace ltheme

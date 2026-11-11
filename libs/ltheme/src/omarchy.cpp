// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ltheme/omarchy.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>

#include <toml++/toml.hpp>

namespace ltheme {
namespace {

std::filesystem::path home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::filesystem::path(h);
    return std::filesystem::path(".");
}

std::filesystem::path state_home() {
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x)
        return std::filesystem::path(x);
    return home_dir() / ".local" / "state";
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' ||
                          s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t'))
        s.remove_suffix(1);
    return std::string(s);
}

using TokenMap = std::map<std::string, Color, std::less<>>;

/// Look up the first token that exists, in preference order. Records a miss under the first name,
/// which is the one worth reporting.
struct Resolver {
    const TokenMap& tokens;
    std::vector<std::string>* missing;

    std::optional<Color> operator()(std::initializer_list<const char*> names) const {
        for (const char* n : names) {
            const auto it = tokens.find(n);
            if (it != tokens.end())
                return it->second;
        }
        if (missing && names.size() > 0)
            missing->emplace_back(*names.begin());
        return std::nullopt;
    }
};

} // namespace

std::filesystem::path omarchy_theme_dir() {
    return state_home() / "omarchy" / "current" / "theme";
}

std::vector<std::filesystem::path> watch_paths(const std::filesystem::path& dir) {
    // The theme directory is replaced wholesale on a theme switch, so watching only the file
    // inside it loses the watch at the moment it matters. Watch the file, the directory that
    // holds it, and the directory above -- some of which may not exist yet, which is fine and is
    // why they are all returned.
    std::vector<std::filesystem::path> out;
    out.push_back(dir / "colors.toml");
    out.push_back(dir);
    if (dir.has_parent_path()) {
        out.push_back(dir.parent_path() / "theme.name");
        out.push_back(dir.parent_path());
    }
    return out;
}

std::vector<std::filesystem::path> watch_paths() {
    return watch_paths(omarchy_theme_dir());
}

bool omarchy_present() {
    std::error_code ec;
    return std::filesystem::exists(omarchy_theme_dir() / "colors.toml", ec);
}

Palette palette_from_tokens(const std::vector<std::pair<std::string, Color>>& tokens, bool dark,
                            std::vector<std::string>* missing) {
    TokenMap map;
    for (const auto& [k, v] : tokens)
        map.insert_or_assign(k, v);

    Palette p = Palette::fallback();
    p.set_dark(dark);
    const Resolver tok{map, missing};

    // The mapping is written in terms of *relative depth*, not absolute lightness, so it holds
    // for a light theme as well: Omarchy's `dark_background` is a step away from `background` in
    // whichever direction the theme runs, and that is all these roles need.
    if (const auto c = tok({"darker_background", "dark_background", "background"}))
        p.set(Role::Window, *c);
    if (const auto c = tok({"background"}))
        p.set(Role::PanelBackground, *c);
    if (const auto c = tok({"dark_background", "background"}))
        p.set(Role::PanelHeader, *c);
    if (const auto c = tok({"lighter_background", "selection", "background"}))
        p.set(Role::PanelHeaderActive, *c);
    if (const auto c = tok({"muted", "dark_foreground"}))
        p.set(Role::Border, *c);
    if (const auto c = tok({"darker_background", "dark_background"}))
        p.set(Role::Divider, *c);
    if (const auto c = tok({"foreground"}))
        p.set(Role::Text, *c);
    if (const auto c = tok({"dark_foreground", "muted"}))
        p.set(Role::TextMuted, *c);
    if (const auto c = tok({"red", "bright_red"}))
        p.set(Role::Error, *c);
    if (const auto c = tok({"yellow", "orange", "bright_yellow"}))
        p.set(Role::Warning, *c);
    if (const auto c = tok({"green", "bright_green"}))
        p.set(Role::Good, *c);

    // The accent drives the drag overlay, so it is derived last and the three stay consistent:
    // a theme that changes its accent changes the drop highlight with it.
    if (const auto c = tok({"accent", "blue", "bright_blue"})) {
        p.set(Role::Accent, *c);
        p.set(Role::DividerHover, *c);
        p.set(Role::DropBorder, *c);
        // ADR-0016: translucent fill plus a clear border, because the overlay is drawn over live
        // panel content -- the program canvas included, which can be any colour at all.
        p.set(Role::DropFill, c->with_alpha(0.28));
    }
    return p;
}

std::expected<OmarchyTheme, std::string> read_omarchy(const std::filesystem::path& theme_dir) {
    const std::filesystem::path colors = theme_dir / "colors.toml";
    std::error_code ec;
    if (!std::filesystem::exists(colors, ec))
        return std::unexpected(std::format("{} does not exist", colors.string()));

    toml::table tbl;
    try {
        tbl = toml::parse_file(colors.string());
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("{}: line {}: {}", colors.string(),
                                           e.source().begin.line,
                                           std::string_view{e.description()}));
    }

    std::vector<std::pair<std::string, Color>> tokens;
    std::vector<std::string> unparsable;
    for (const auto& [key, value] : tbl) {
        const auto s = value.value<std::string>();
        if (!s)
            continue;
        const std::string name{key.str()};
        if (name == "mode")
            continue;
        if (const auto c = parse_color(*s))
            tokens.emplace_back(name, *c);
        else
            unparsable.push_back(name);
    }
    if (tokens.empty())
        return std::unexpected(
            std::format("{} contains no usable colours", colors.string()));

    // Mode comes from the file when it says, and from Omarchy's `light.mode` marker otherwise.
    bool dark = true;
    if (const auto mode = tbl["mode"].value<std::string>())
        dark = (*mode != "light");
    else if (std::filesystem::exists(theme_dir / "light.mode", ec))
        dark = false;

    OmarchyTheme out;
    out.missing_tokens = std::move(unparsable);
    out.palette = palette_from_tokens(tokens, dark, &out.missing_tokens);

    // The theme's name lives beside the directory, not inside it.
    std::string name;
    if (theme_dir.has_parent_path()) {
        try {
            const auto np = theme_dir.parent_path() / "theme.name";
            if (std::filesystem::exists(np, ec)) {
                std::ifstream in{np};
                std::string line;
                std::getline(in, line);
                name = trimmed(line);
            }
        } catch (...) {
            // A missing or unreadable name is cosmetic; the colours are what matter.
        }
    }
    if (name.empty())
        name = theme_dir.parent_path().filename().string();
    out.name = name;
    out.palette.set_source(std::format("omarchy:{}", name));
    return out;
}

std::expected<OmarchyTheme, std::string> read_omarchy() {
    return read_omarchy(omarchy_theme_dir());
}

} // namespace ltheme

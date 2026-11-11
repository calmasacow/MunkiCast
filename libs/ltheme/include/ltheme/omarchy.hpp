// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ltheme/palette.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

/// Omarchy theme following.
///
/// Omarchy keeps the active theme at `~/.local/state/omarchy/current/theme`, with a flat
/// `colors.toml` of semantic tokens and the theme's name in a sibling `theme.name`. Switching
/// themes rewrites those files in place, so following the theme is a matter of reading them and
/// watching them -- no IPC, no D-Bus, no vendor API.
///
/// This header is pure filesystem and parsing. Watching is the application's job, because the
/// right watcher depends on its event loop; `watch_paths()` says what to watch.
namespace ltheme {

struct OmarchyTheme {
    Palette palette;
    /// The theme's own name, as Omarchy records it ("tokyo-night", "catppuccin", ...).
    std::string name;
    /// Tokens `colors.toml` did not provide. The corresponding roles fall back to the built-in
    /// palette rather than to black, and this says which so the UI can be honest about it.
    std::vector<std::string> missing_tokens;
};

/// `$OMARCHY_STATE/current/theme`, honouring XDG_STATE_HOME, with no requirement that it exists.
[[nodiscard]] std::filesystem::path omarchy_theme_dir();

/// True when this machine looks like Omarchy: the theme directory and its colours are present.
[[nodiscard]] bool omarchy_present();

/// Everything whose change should trigger a re-read. Includes paths that do not exist yet, so
/// that a watcher set up before Omarchy writes them still picks the theme up.
[[nodiscard]] std::vector<std::filesystem::path> watch_paths();
[[nodiscard]] std::vector<std::filesystem::path> watch_paths(const std::filesystem::path& dir);

[[nodiscard]] std::expected<OmarchyTheme, std::string> read_omarchy();
[[nodiscard]] std::expected<OmarchyTheme, std::string>
read_omarchy(const std::filesystem::path& theme_dir);

/// Builds a palette from an already-loaded set of Omarchy tokens. Exposed so the mapping can be
/// tested against hand-written token sets, including sparse and hostile ones.
[[nodiscard]] Palette palette_from_tokens(const std::vector<std::pair<std::string, Color>>& tokens,
                                          bool dark, std::vector<std::string>* missing);

} // namespace ltheme

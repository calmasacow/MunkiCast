// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "llayout/layout.hpp"
#include "llayout/workspaces.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/// Layout persistence. TOML, because the project already has to read TOML (Omarchy's
/// `colors.toml` for theme following, and effect manifests per ADR-0004), and because a layout
/// file a user can read and hand-edit is a layout file a user can report a bug about.
namespace llayout {

/// Version 2 dropped the `inputs` panel. Its job -- listing the project's sources so one can be
/// put into a second scene -- moved into the Sources panel's add menu, because a source is a
/// source and having two panels for it meant two words for one thing. A version 1 file naming
/// `inputs` would otherwise render a "this build has nothing to draw there" placeholder, so
/// reading one drops that tab.
///
/// Version 3 renames `stats` to `info`. That panel counts what is in the project and lists what
/// is wrong with it; it was never statistics, and broadcast statistics are a different panel
/// with a different job. The rename shipped *without* this migration, which is the bug it
/// exists to fix: every layout written before it -- version 2 files included, which is why this
/// is not gated on version 1 -- names a panel this build does not have, and draws the
/// placeholder for ever because nothing ever removes it.
///
/// A rename, not a removal. A file that already has `info` keeps it and loses the stale `stats`
/// beside it, because the same panel twice in one group is not what anybody saved.
inline constexpr int kLayoutFormatVersion = 3;

[[nodiscard]] std::string to_toml(const Layout& l);

[[nodiscard]] std::expected<Layout, std::string> from_toml(std::string_view text);

/// Atomic, fsynced, versioned (ADR-0015 rule 5).
[[nodiscard]] std::expected<void, std::string> save(const Layout& l,
                                                    const std::filesystem::path& path);

[[nodiscard]] std::expected<Layout, std::string> load(const std::filesystem::path& path);

struct LoadOutcome {
    Layout layout;
    /// Empty when the file loaded. Otherwise what went wrong, for the UI to surface -- a layout
    /// that fails to parse falls back to the default *and says so*, rather than silently
    /// restoring something incoherent.
    std::string complaint;
    /// True when `layout` is the built-in default rather than what was on disk.
    bool used_default = false;
};

/// What the application calls at startup. Never fails.
[[nodiscard]] LoadOutcome load_or_default(const std::filesystem::path& path);

// ---- the whole set of saved workspaces ------------------------------------------------------

[[nodiscard]] std::string to_toml(const Workspaces& w);
[[nodiscard]] std::expected<Workspaces, std::string> workspaces_from_toml(std::string_view text);

[[nodiscard]] std::expected<void, std::string> save(const Workspaces& w,
                                                    const std::filesystem::path& path);

struct WorkspacesLoadOutcome {
    Workspaces workspaces;
    /// Empty when the file loaded; otherwise what was wrong with it.
    std::string complaint;
    bool used_default = false;
};

/// Never fails. A file that will not parse, or that parses into something structurally wrong,
/// is replaced by the defaults *and said so* -- ADR-0015 rule 5. A file in the older
/// single-layout format is adopted as the Build workspace rather than discarded.
[[nodiscard]] WorkspacesLoadOutcome load_workspaces_or_default(const std::filesystem::path& path);

} // namespace llayout

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "llayout/layout.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llayout {

/// A named, saved arrangement of the workspace.
///
/// Not a workspace in the compositor's sense. Nothing here is an OS window or a virtual desktop
/// (ADR-0015): these are saved *layouts*, one per job. Building a show and running one want
/// different panels on screen, and switching between them by hand every time is what makes
/// people leave the wrong one up.
struct Workspace {
    std::string name;
    /// "Build" and "Live". They can be rearranged and have panels hidden like any other, and
    /// they can be reset to how they shipped -- but they cannot be renamed or removed, so there
    /// is always a known arrangement to get back to.
    bool builtin = false;
    Layout layout;

    friend bool operator==(const Workspace&, const Workspace&) = default;
};

/// The set of saved arrangements, and which one is on screen.
class Workspaces {
public:
    /// Build and Live, in that order, with Build active.
    [[nodiscard]] static Workspaces defaults();

    [[nodiscard]] const std::vector<Workspace>& items() const { return items_; }
    [[nodiscard]] std::size_t size() const { return items_.size(); }
    [[nodiscard]] std::size_t active() const { return active_; }

    [[nodiscard]] Layout& active_layout();
    [[nodiscard]] const Layout& active_layout() const;

    [[nodiscard]] bool set_active(std::size_t index);

    /// Append one. An empty name, or one already taken, is refused -- two tabs reading "Live"
    /// is the kind of thing that only hurts while something else has your attention.
    std::expected<std::size_t, std::string> add(std::string name, Layout layout);
    std::expected<void, std::string> remove(std::size_t index);
    std::expected<void, std::string> rename(std::size_t index, std::string name);
    /// Back to how it shipped, for a built-in; back to the build arrangement otherwise.
    std::expected<void, std::string> reset(std::size_t index);

    /// Replace one's layout, which is what every drag and divider move does to the active one.
    std::expected<void, std::string> set_layout(std::size_t index, Layout layout);

    /// Mark one as built in. Only the loader uses this, and it decides from the *name* rather
    /// than from what the file claimed -- otherwise a hand-edited file could mark every
    /// workspace removable and leave no known arrangement to get back to.
    void make_builtin(std::size_t index);

    /// Structurally wrong: no entries, an out-of-range active index, a missing built-in, a
    /// duplicate or empty name, or a layout that does not validate.
    [[nodiscard]] std::optional<std::string> validate() const;

    /// The three that must always exist.
    static constexpr std::string_view kBuild = "Build";
    static constexpr std::string_view kLive = "Live";
    /// The node graph. Not a dock arrangement at all: the shell draws a fixed view for this
    /// one and never reads its layout. It is a workspace anyway so that it is a tab like the
    /// others, with one switching rule rather than two.
    static constexpr std::string_view kEncode = "Encode";

    /// Whether a name is one of the three. The loader decides `builtin` from the name rather
    /// than from the file, so this is the single place that list is written down.
    [[nodiscard]] static bool is_builtin_name(std::string_view name) {
        return name == kBuild || name == kLive || name == kEncode;
    }

    friend bool operator==(const Workspaces&, const Workspaces&) = default;

private:
    [[nodiscard]] bool name_taken(std::string_view name, std::size_t except) const;

    std::vector<Workspace> items_;
    std::size_t active_ = 0;
};

} // namespace llayout

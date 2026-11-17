// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llayout/workspaces.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace llayout {

Layout Workspaces::default_layout_for(std::string_view name) {
    if (name == kLive)
        return Layout::default_live();
    if (name == kMedia)
        return Layout::default_media();
    if (name == kEdit)
        return Layout::default_edit();
    if (name == kGraphics)
        return Layout::default_graphics();
    // Build, and anything not built in. Encode too: nothing ever draws its layout -- the shell
    // swaps in a fixed node-graph view for it -- but a Workspace has to hold one that
    // validate() accepts. If the node graph ever grows dockable panels, it gets its own tree
    // here like the rest.
    return Layout::default_build();
}

Workspaces Workspaces::defaults() {
    Workspaces w;
    for (const std::string_view name : {kBuild, kLive, kEncode, kMedia, kEdit, kGraphics})
        w.items_.push_back(Workspace{std::string{name}, true, default_layout_for(name)});
    w.active_ = 0;
    return w;
}

Layout& Workspaces::active_layout() {
    // Guaranteed in range: every mutator that could empty the list refuses to, and validate()
    // rejects a set with no entries on load.
    return items_[active_].layout;
}

const Layout& Workspaces::active_layout() const {
    return items_[active_].layout;
}

bool Workspaces::set_active(std::size_t index) {
    if (index >= items_.size())
        return false;
    active_ = index;
    return true;
}

bool Workspaces::name_taken(std::string_view name, std::size_t except) const {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i != except && items_[i].name == name)
            return true;
    }
    return false;
}

std::expected<std::size_t, std::string> Workspaces::add(std::string name, Layout layout) {
    if (name.empty())
        return std::unexpected(std::string{"a workspace needs a name"});
    if (name_taken(name, items_.size()))
        return std::unexpected(std::format("there is already a workspace called '{}'", name));
    if (const auto bad = layout.validate())
        return std::unexpected(*bad);
    items_.push_back(Workspace{std::move(name), false, std::move(layout)});
    return items_.size() - 1;
}

std::expected<void, std::string> Workspaces::remove(std::size_t index) {
    if (index >= items_.size())
        return std::unexpected(std::format("there is no workspace {}", index));
    if (items_[index].builtin)
        return std::unexpected(
            std::format("'{}' is built in and cannot be removed; it can be reset",
                        items_[index].name));
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    // The active one may have been the one removed, or may have shifted down by one.
    if (active_ >= items_.size())
        active_ = items_.empty() ? 0 : items_.size() - 1;
    else if (active_ > index)
        --active_;
    return {};
}

std::expected<void, std::string> Workspaces::rename(std::size_t index, std::string name) {
    if (index >= items_.size())
        return std::unexpected(std::format("there is no workspace {}", index));
    if (items_[index].builtin)
        return std::unexpected(
            std::format("'{}' is built in and cannot be renamed", items_[index].name));
    if (name.empty())
        return std::unexpected(std::string{"a workspace needs a name"});
    if (name_taken(name, index))
        return std::unexpected(std::format("there is already a workspace called '{}'", name));
    items_[index].name = std::move(name);
    return {};
}

std::expected<void, std::string> Workspaces::reset(std::size_t index) {
    if (index >= items_.size())
        return std::unexpected(std::format("there is no workspace {}", index));
    // A built-in goes back to the arrangement it shipped with, which is the point of it being
    // built in. Anything else goes back to the building arrangement.
    items_[index].layout = default_layout_for(items_[index].name);
    return {};
}

std::expected<void, std::string> Workspaces::set_layout(std::size_t index, Layout layout) {
    if (index >= items_.size())
        return std::unexpected(std::format("there is no workspace {}", index));
    if (const auto bad = layout.validate())
        return std::unexpected(*bad);
    items_[index].layout = std::move(layout);
    return {};
}

void Workspaces::make_builtin(std::size_t index) {
    if (index < items_.size())
        items_[index].builtin = true;
}

std::optional<std::string> Workspaces::validate() const {
    if (items_.empty())
        return "there are no workspaces";
    if (active_ >= items_.size())
        return std::format("the active workspace is {} of {}", active_, items_.size());

    std::set<std::string> seen;
    for (const Workspace& w : items_) {
        if (w.name.empty())
            return "a workspace has no name";
        if (!seen.insert(w.name).second)
            return std::format("two workspaces are both called '{}'", w.name);
        if (const auto bad = w.layout.validate())
            return std::format("'{}': {}", w.name, *bad);
    }
    // Losing one of these to a hand-edited file would leave no known arrangement to get back
    // to, so each is required to be present *and* marked built in.
    for (const std::string_view name : {kBuild, kLive, kEncode, kMedia, kEdit, kGraphics}) {
        const bool present = std::ranges::any_of(items_, [name](const Workspace& w) {
            return w.builtin && w.name == name;
        });
        if (!present)
            return std::format("the built-in '{}' workspace is missing", name);
    }
    return std::nullopt;
}

} // namespace llayout

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llayout/workspaces.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace llayout {

Workspaces Workspaces::defaults() {
    Workspaces w;
    w.items_.push_back(Workspace{std::string{kBuild}, true, Layout::default_build()});
    w.items_.push_back(Workspace{std::string{kLive}, true, Layout::default_live()});
    // ponytail: Encode carries Build's layout because a Workspace has to hold one that
    // validate() accepts, and nothing ever draws this one -- the shell swaps in a fixed view
    // for it. If the node graph ever grows dockable panels, this is where its own default
    // tree goes.
    w.items_.push_back(Workspace{std::string{kEncode}, true, Layout::default_build()});
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
    // built in. Anything else goes back to the building arrangement, there being no other
    // sensible origin for it.
    items_[index].layout = (items_[index].name == kLive) ? Layout::default_live()
                                                         : Layout::default_build();
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
    bool has_build = false;
    bool has_live = false;
    bool has_encode = false;
    for (const Workspace& w : items_) {
        if (w.name.empty())
            return "a workspace has no name";
        if (!seen.insert(w.name).second)
            return std::format("two workspaces are both called '{}'", w.name);
        if (const auto bad = w.layout.validate())
            return std::format("'{}': {}", w.name, *bad);
        has_build = has_build || (w.builtin && w.name == kBuild);
        has_live = has_live || (w.builtin && w.name == kLive);
        has_encode = has_encode || (w.builtin && w.name == kEncode);
    }
    // Losing one of these to a hand-edited file would leave no known arrangement to get back to.
    if (!has_build)
        return std::format("the built-in '{}' workspace is missing", kBuild);
    if (!has_live)
        return std::format("the built-in '{}' workspace is missing", kLive);
    if (!has_encode)
        return std::format("the built-in '{}' workspace is missing", kEncode);
    return std::nullopt;
}

} // namespace llayout

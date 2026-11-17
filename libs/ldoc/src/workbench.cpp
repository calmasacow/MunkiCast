// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/workbench.hpp"

#include <utility>

namespace ldoc {

TimelineHistory& Workbench::open_timeline(Timeline timeline) {
    const Id id = timeline.id;
    const auto it = timelines_.find(id);
    // Already open: hand back the same editor, undo stack and all. Replacing it here would
    // silently throw away the work of whoever opened it first.
    if (it != timelines_.end())
        return it->second;
    return timelines_.emplace(id, TimelineHistory{std::move(timeline)}).first->second;
}

TimelineHistory* Workbench::timeline(const Id& id) {
    const auto it = timelines_.find(id);
    return it == timelines_.end() ? nullptr : &it->second;
}

const TimelineHistory* Workbench::timeline(const Id& id) const {
    const auto it = timelines_.find(id);
    return it == timelines_.end() ? nullptr : &it->second;
}

void Workbench::close_timeline(const Id& id) {
    timelines_.erase(id);
}

std::vector<Id> Workbench::open_timelines() const {
    std::vector<Id> ids;
    ids.reserve(timelines_.size());
    for (const auto& [id, _] : timelines_)
        ids.push_back(id);
    return ids;
}

GraphicHistory& Workbench::open_graphic(Graphic graphic) {
    const Id id = graphic.id;
    const auto it = graphics_.find(id);
    if (it != graphics_.end())
        return it->second;
    return graphics_.emplace(id, GraphicHistory{std::move(graphic)}).first->second;
}

GraphicHistory* Workbench::graphic(const Id& id) {
    const auto it = graphics_.find(id);
    return it == graphics_.end() ? nullptr : &it->second;
}

const GraphicHistory* Workbench::graphic(const Id& id) const {
    const auto it = graphics_.find(id);
    return it == graphics_.end() ? nullptr : &it->second;
}

void Workbench::close_graphic(const Id& id) {
    graphics_.erase(id);
}

std::vector<Id> Workbench::open_graphics() const {
    std::vector<Id> ids;
    ids.reserve(graphics_.size());
    for (const auto& [id, _] : graphics_)
        ids.push_back(id);
    return ids;
}

std::vector<Id> Workbench::dirty_documents() const {
    std::vector<Id> ids;
    for (const auto& [id, history] : timelines_) {
        if (history.dirty())
            ids.push_back(id);
    }
    for (const auto& [id, history] : graphics_) {
        if (history.dirty())
            ids.push_back(id);
    }
    return ids;
}

void Workbench::clear() {
    timelines_.clear();
    graphics_.clear();
}

} // namespace ldoc

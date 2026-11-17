// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/subdocument.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace ldoc {

std::string_view to_string(TrackKind k) {
    switch (k) {
    case TrackKind::Video:
        return "video";
    case TrackKind::Audio:
        return "audio";
    }
    return "video";
}

std::optional<TrackKind> track_kind_from_string(std::string_view s) {
    if (s == "video")
        return TrackKind::Video;
    if (s == "audio")
        return TrackKind::Audio;
    return std::nullopt;
}

std::string_view to_string(LayerKind k) {
    switch (k) {
    case LayerKind::Rect:
        return "rect";
    case LayerKind::Text:
        return "text";
    case LayerKind::Image:
        return "image";
    }
    return "rect";
}

std::optional<LayerKind> layer_kind_from_string(std::string_view s) {
    if (s == "rect")
        return LayerKind::Rect;
    if (s == "text")
        return LayerKind::Text;
    if (s == "image")
        return LayerKind::Image;
    return std::nullopt;
}

// ---- Track -----------------------------------------------------------------------------------

const Clip* Track::clip(const Id& wanted) const {
    const auto it = std::ranges::find(clips, wanted, &Clip::id);
    return it == clips.end() ? nullptr : &*it;
}

Clip* Track::clip(const Id& wanted) {
    const auto it = std::ranges::find(clips, wanted, &Clip::id);
    return it == clips.end() ? nullptr : &*it;
}

const Clip* Track::clip_at(Frame at) const {
    // Clips are sorted and do not overlap, so the first one starting after `at` bounds the
    // search: the only candidate is the one before it.
    const auto it = std::ranges::upper_bound(clips, at, {}, &Clip::start);
    if (it == clips.begin())
        return nullptr;
    const Clip& candidate = *std::prev(it);
    return candidate.covers(at) ? &candidate : nullptr;
}

// ---- Timeline --------------------------------------------------------------------------------

const Track* Timeline::track(const Id& wanted) const {
    const auto it = std::ranges::find(tracks, wanted, &Track::id);
    return it == tracks.end() ? nullptr : &*it;
}

Track* Timeline::track(const Id& wanted) {
    const auto it = std::ranges::find(tracks, wanted, &Track::id);
    return it == tracks.end() ? nullptr : &*it;
}

Frame Timeline::duration() const {
    Frame longest = 0;
    for (const Track& t : tracks) {
        // Clips are sorted by start and do not overlap, so the last one ends last.
        if (!t.clips.empty())
            longest = std::max(longest, t.clips.back().end());
    }
    return longest;
}

std::optional<std::string> Timeline::validate() const {
    if (!fps.valid())
        return std::format("timeline '{}' has an invalid frame rate ({}/{})", name, fps.num,
                           fps.den);
    if (width <= 0 || height <= 0)
        return std::format("timeline '{}' has an invalid size ({}x{})", name, width, height);
    if (playhead < 0)
        return std::format("timeline '{}' has a negative playhead", name);
    if (in_point && out_point && *in_point >= *out_point)
        return std::format("timeline '{}' has an in point at or after its out point", name);

    std::set<Id> track_ids;
    std::set<Id> clip_ids;
    for (const Track& t : tracks) {
        if (!track_ids.insert(t.id).second)
            return std::format("timeline '{}' has two tracks with the same id", name);

        Frame previous_end = 0;
        bool first = true;
        for (const Clip& c : t.clips) {
            if (!clip_ids.insert(c.id).second)
                return std::format("timeline '{}' has two clips with the same id", name);
            if (c.duration <= 0)
                return std::format("clip '{}' on track '{}' has no length", c.name, t.name);
            if (c.start < 0)
                return std::format("clip '{}' on track '{}' starts before zero", c.name, t.name);
            if (c.source_in < 0)
                return std::format("clip '{}' on track '{}' starts before its media", c.name,
                                   t.name);
            // Order and overlap in one test: a clip starting before the previous one ended is
            // either out of order or on top of it, and neither has a defined picture.
            if (!first && c.start < previous_end)
                return std::format("clips '{}' and later overlap or are out of order on track "
                                   "'{}'",
                                   c.name, t.name);
            previous_end = c.end();
            first = false;
        }
    }

    std::set<Id> marker_ids;
    for (const Marker& m : markers) {
        if (!marker_ids.insert(m.id).second)
            return std::format("timeline '{}' has two markers with the same id", name);
        if (m.at < 0)
            return std::format("marker '{}' is before zero", m.name);
    }
    return std::nullopt;
}

// ---- Channel ---------------------------------------------------------------------------------

double Channel::value_at(Frame at) const {
    if (keys.empty())
        return constant;
    if (at <= keys.front().at)
        return keys.front().value;
    if (at >= keys.back().at)
        return keys.back().value;

    const auto next = std::ranges::upper_bound(keys, at, {}, &Keyframe::at);
    const Keyframe& b = *next;
    const Keyframe& a = *std::prev(next);
    const Frame span = b.at - a.at;
    if (span <= 0)
        return b.value;

    double t = static_cast<double>(at - a.at) / static_cast<double>(span);
    // Ease belongs to the segment it opens, so a designer sets it on the key they are leaving.
    if (a.ease)
        t = t * t * (3.0 - 2.0 * t); // smoothstep
    return a.value + (b.value - a.value) * t;
}

// ---- Graphic ---------------------------------------------------------------------------------

const Layer* Graphic::layer(const Id& wanted) const {
    const auto it = std::ranges::find(layers, wanted, &Layer::id);
    return it == layers.end() ? nullptr : &*it;
}

Layer* Graphic::layer(const Id& wanted) {
    const auto it = std::ranges::find(layers, wanted, &Layer::id);
    return it == layers.end() ? nullptr : &*it;
}

std::vector<std::string> Graphic::data_fields() const {
    std::set<std::string> unique;
    for (const Layer& l : layers) {
        if (!l.data_field.empty())
            unique.insert(l.data_field);
    }
    return {unique.begin(), unique.end()};
}

std::optional<std::string> Graphic::validate() const {
    if (!fps.valid())
        return std::format("graphic '{}' has an invalid frame rate ({}/{})", name, fps.num,
                           fps.den);
    if (width <= 0 || height <= 0)
        return std::format("graphic '{}' has an invalid size ({}x{})", name, width, height);
    if (duration <= 0)
        return std::format("graphic '{}' has no duration", name);
    if (playhead < 0)
        return std::format("graphic '{}' has a negative playhead", name);

    std::set<Id> ids;
    for (const Layer& l : layers) {
        if (!ids.insert(l.id).second)
            return std::format("graphic '{}' has two layers with the same id", name);
        if (l.kind == LayerKind::Image && !l.asset)
            return std::format("image layer '{}' names no picture", l.name);

        const std::pair<const Channel*, const char*> channels[] = {
            {&l.x, "x"},   {&l.y, "y"},
            {&l.width, "width"}, {&l.height, "height"},
            {&l.rotation, "rotation"}, {&l.opacity, "opacity"},
        };
        for (const auto& [channel, what] : channels) {
            Frame previous = 0;
            bool first = true;
            for (const Keyframe& k : channel->keys) {
                if (k.at < 0)
                    return std::format("a {} keyframe on layer '{}' is before zero", what,
                                       l.name);
                if (!first && k.at <= previous)
                    return std::format("the {} keyframes on layer '{}' are out of order or "
                                       "doubled",
                                       what, l.name);
                if (!std::isfinite(k.value))
                    return std::format("a {} keyframe on layer '{}' is not a number", what,
                                       l.name);
                previous = k.at;
                first = false;
            }
            if (!std::isfinite(channel->constant))
                return std::format("the {} of layer '{}' is not a number", what, l.name);
        }
    }
    return std::nullopt;
}

} // namespace ldoc

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/subcommand.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace ldoc {
namespace {

/// Insert a clip into a track's sorted, non-overlapping clip list.
///
/// Both halves of the invariant are checked here rather than by the caller, because every path
/// that puts a clip on a track -- add, move, split, and the inverse of a remove -- comes through
/// this one function. A rule enforced in one place is a rule that holds.
std::expected<void, std::string> insert_clip(Track& track, Clip clip) {
    if (clip.duration <= 0)
        return std::unexpected(std::format("a clip needs a length"));
    if (clip.start < 0)
        return std::unexpected("a clip cannot start before zero");
    if (track.locked)
        return std::unexpected(std::format("track '{}' is locked", track.name));
    if (track.clip(clip.id))
        return std::unexpected("that clip is already on this track");

    const auto at = std::ranges::upper_bound(track.clips, clip.start, {}, &Clip::start);
    // The neighbour on each side has to clear it. Refusing is deliberate: shortening a clip the
    // operator did not name, to make room for one they dropped, is a change they did not ask
    // for and would not see.
    if (at != track.clips.begin() && std::prev(at)->end() > clip.start)
        return std::unexpected(std::format("'{}' would overlap '{}'", clip.name,
                                           std::prev(at)->name));
    if (at != track.clips.end() && clip.end() > at->start)
        return std::unexpected(std::format("'{}' would overlap '{}'", clip.name, at->name));

    track.clips.insert(at, std::move(clip));
    return {};
}

std::expected<Clip, std::string> take_clip(Track& track, const Id& id) {
    if (track.locked)
        return std::unexpected(std::format("track '{}' is locked", track.name));
    const auto it = std::ranges::find(track.clips, id, &Clip::id);
    if (it == track.clips.end())
        return std::unexpected("there is no such clip on that track");
    Clip taken = std::move(*it);
    track.clips.erase(it);
    return taken;
}

std::expected<Track*, std::string> find_track(Timeline& timeline, const Id& id) {
    Track* track = timeline.track(id);
    if (!track)
        return std::unexpected("there is no such track");
    return track;
}

} // namespace

TimelineCommand make_timeline_batch(std::vector<TimelineCommand> commands,
                                    std::string description) {
    auto body = std::make_shared<TimelineBatchBody>();
    body->commands = std::move(commands);
    return TimelineBatch{std::move(body), std::move(description)};
}

GraphicCommand make_graphic_batch(std::vector<GraphicCommand> commands,
                                  std::string description) {
    auto body = std::make_shared<GraphicBatchBody>();
    body->commands = std::move(commands);
    return GraphicBatch{std::move(body), std::move(description)};
}

std::string_view to_string(ChannelId c) {
    switch (c) {
    case ChannelId::X:
        return "x";
    case ChannelId::Y:
        return "y";
    case ChannelId::Width:
        return "width";
    case ChannelId::Height:
        return "height";
    case ChannelId::Rotation:
        return "rotation";
    case ChannelId::Opacity:
        return "opacity";
    }
    return "x";
}

std::optional<ChannelId> channel_from_string(std::string_view s) {
    if (s == "x")
        return ChannelId::X;
    if (s == "y")
        return ChannelId::Y;
    if (s == "width")
        return ChannelId::Width;
    if (s == "height")
        return ChannelId::Height;
    if (s == "rotation")
        return ChannelId::Rotation;
    if (s == "opacity")
        return ChannelId::Opacity;
    return std::nullopt;
}

namespace {

Channel* channel_of(Layer& layer, ChannelId which) {
    switch (which) {
    case ChannelId::X:
        return &layer.x;
    case ChannelId::Y:
        return &layer.y;
    case ChannelId::Width:
        return &layer.width;
    case ChannelId::Height:
        return &layer.height;
    case ChannelId::Rotation:
        return &layer.rotation;
    case ChannelId::Opacity:
        return &layer.opacity;
    }
    return nullptr;
}

} // namespace

// ---- timeline --------------------------------------------------------------------------------

std::expected<AppliedTo<TimelineCommand>, std::string> apply(Timeline& timeline,
                                                             const TimelineCommand& command) {
    using Result = std::expected<AppliedTo<TimelineCommand>, std::string>;

    return std::visit(
        [&timeline](const auto& c) -> Result {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, SetTimelineName>) {
                if (c.name.empty())
                    return std::unexpected("a timeline needs a name");
                SetTimelineName inverse{timeline.name};
                const std::string description = std::format("Rename timeline to '{}'", c.name);
                timeline.name = c.name;
                return AppliedTo<TimelineCommand>{inverse, description};

            } else if constexpr (std::is_same_v<T, AddTrack>) {
                if (timeline.track(c.track.id))
                    return std::unexpected("that track is already here");
                const std::size_t at = std::min(c.index, timeline.tracks.size());
                timeline.tracks.insert(timeline.tracks.begin() +
                                           static_cast<std::ptrdiff_t>(at),
                                       c.track);
                return AppliedTo<TimelineCommand>{
                    RemoveTrack{c.track.id}, std::format("Add track '{}'", c.track.name)};

            } else if constexpr (std::is_same_v<T, RemoveTrack>) {
                const auto it = std::ranges::find(timeline.tracks, c.track, &Track::id);
                if (it == timeline.tracks.end())
                    return std::unexpected("there is no such track");
                // The whole track, clips and all, so undo brings the work back rather than an
                // empty lane where it used to be.
                const auto index =
                    static_cast<std::size_t>(std::distance(timeline.tracks.begin(), it));
                AddTrack inverse{*it, index};
                const std::string description = std::format("Remove track '{}'", it->name);
                timeline.tracks.erase(it);
                return AppliedTo<TimelineCommand>{std::move(inverse), description};

            } else if constexpr (std::is_same_v<T, SetTrack>) {
                auto track = find_track(timeline, c.track);
                if (!track)
                    return std::unexpected(track.error());
                if (c.name.empty())
                    return std::unexpected("a track needs a name");
                SetTrack inverse{c.track, (*track)->name, (*track)->enabled, (*track)->locked};
                const std::string description = std::format("Change track '{}'", c.name);
                (*track)->name = c.name;
                (*track)->enabled = c.enabled;
                (*track)->locked = c.locked;
                return AppliedTo<TimelineCommand>{std::move(inverse), description};

            } else if constexpr (std::is_same_v<T, AddClip>) {
                auto track = find_track(timeline, c.track);
                if (!track)
                    return std::unexpected(track.error());
                if (const auto r = insert_clip(**track, c.clip); !r)
                    return std::unexpected(r.error());
                return AppliedTo<TimelineCommand>{
                    RemoveClip{c.track, c.clip.id},
                    std::format("Add '{}'", c.clip.name)};

            } else if constexpr (std::is_same_v<T, RemoveClip>) {
                auto track = find_track(timeline, c.track);
                if (!track)
                    return std::unexpected(track.error());
                auto taken = take_clip(**track, c.clip);
                if (!taken)
                    return std::unexpected(taken.error());
                const std::string description = std::format("Remove '{}'", taken->name);
                return AppliedTo<TimelineCommand>{AddClip{c.track, std::move(*taken)},
                                                  description};

            } else if constexpr (std::is_same_v<T, MoveClip>) {
                auto from = find_track(timeline, c.track);
                if (!from)
                    return std::unexpected(from.error());
                const bool across = !c.to_track.is_nil() && c.to_track != c.track;
                Track* destination = *from;
                if (across) {
                    auto to = find_track(timeline, c.to_track);
                    if (!to)
                        return std::unexpected(to.error());
                    destination = *to;
                }

                auto taken = take_clip(**from, c.clip);
                if (!taken)
                    return std::unexpected(taken.error());

                const Frame was_start = taken->start;
                Clip moved = *taken;
                moved.start = c.start;
                if (const auto r = insert_clip(*destination, std::move(moved)); !r) {
                    // Put it back exactly where it was. A refused move must leave the timeline
                    // byte-identical, and the clip is already out of its track by this point.
                    (void)insert_clip(**from, std::move(*taken));
                    return std::unexpected(r.error());
                }
                return AppliedTo<TimelineCommand>{
                    MoveClip{across ? c.to_track : c.track, c.clip, was_start,
                             across ? c.track : Id{}},
                    std::format("Move '{}'", taken->name)};

            } else if constexpr (std::is_same_v<T, TrimClip>) {
                auto track = find_track(timeline, c.track);
                if (!track)
                    return std::unexpected(track.error());
                auto taken = take_clip(**track, c.clip);
                if (!taken)
                    return std::unexpected(taken.error());

                TrimClip inverse{c.track, c.clip, taken->start, taken->duration,
                                 taken->source_in};
                Clip trimmed = *taken;
                trimmed.start = c.start;
                trimmed.duration = c.duration;
                trimmed.source_in = c.source_in;
                if (trimmed.source_in < 0)
                    trimmed.source_in = 0;
                if (const auto r = insert_clip(**track, std::move(trimmed)); !r) {
                    (void)insert_clip(**track, std::move(*taken));
                    return std::unexpected(r.error());
                }
                return AppliedTo<TimelineCommand>{inverse,
                                                  std::format("Trim '{}'", taken->name)};

            } else if constexpr (std::is_same_v<T, SetClip>) {
                auto track = find_track(timeline, c.track);
                if (!track)
                    return std::unexpected(track.error());
                Clip* clip = (*track)->clip(c.clip);
                if (!clip)
                    return std::unexpected("there is no such clip on that track");
                SetClip inverse{c.track, c.clip, clip->name, clip->gain_db};
                const std::string description = std::format("Change '{}'", c.name);
                clip->name = c.name;
                clip->gain_db = c.gain_db;
                return AppliedTo<TimelineCommand>{std::move(inverse), description};

            } else if constexpr (std::is_same_v<T, SplitClip>) {
                auto track = find_track(timeline, c.track);
                if (!track)
                    return std::unexpected(track.error());
                Clip* clip = (*track)->clip(c.clip);
                if (!clip)
                    return std::unexpected("there is no such clip on that track");
                if (!clip->covers(c.at) || c.at == clip->start)
                    return std::unexpected("a cut has to fall inside the clip");
                if ((*track)->locked)
                    return std::unexpected(std::format("track '{}' is locked", (*track)->name));
                if (c.new_clip.is_nil() || (*track)->clip(c.new_clip))
                    return std::unexpected("the new piece needs an id of its own");

                // Everything the inverse needs, read before the insert below: that insert can
                // reallocate the clip vector, and `clip` would then dangle.
                const std::string name = clip->name;
                const Frame was_start = clip->start;
                const Frame was_duration = clip->duration;
                const Frame was_source_in = clip->source_in;

                // The right-hand piece carries the media window on from where the left one
                // stops, so the two together show exactly what the one clip showed.
                Clip right = *clip;
                right.id = c.new_clip;
                right.start = c.at;
                right.duration = clip->end() - c.at;
                right.source_in = clip->source_at(c.at);
                clip->duration = c.at - was_start;

                const auto at = std::ranges::upper_bound((*track)->clips, right.start, {},
                                                         &Clip::start);
                (*track)->clips.insert(at, std::move(right));

                // Undone by removing the new piece and only then restoring the original's
                // length: the other order would trim the left clip back over the piece still
                // sitting beside it, which `insert_clip` refuses as an overlap.
                const std::string description = std::format("Split '{}'", name);
                return AppliedTo<TimelineCommand>{
                    make_timeline_batch({RemoveClip{c.track, c.new_clip},
                                         TrimClip{c.track, c.clip, was_start, was_duration,
                                                  was_source_in}},
                                        description),
                    description};

            } else if constexpr (std::is_same_v<T, AddMarker>) {
                if (std::ranges::any_of(timeline.markers, [&c](const Marker& m) {
                        return m.id == c.marker.id;
                    }))
                    return std::unexpected("that marker is already here");
                if (c.marker.at < 0)
                    return std::unexpected("a marker cannot be before zero");
                const auto at =
                    std::ranges::upper_bound(timeline.markers, c.marker.at, {}, &Marker::at);
                timeline.markers.insert(at, c.marker);
                return AppliedTo<TimelineCommand>{
                    RemoveMarker{c.marker.id}, std::format("Add marker '{}'", c.marker.name)};

            } else if constexpr (std::is_same_v<T, RemoveMarker>) {
                const auto it = std::ranges::find(timeline.markers, c.marker, &Marker::id);
                if (it == timeline.markers.end())
                    return std::unexpected("there is no such marker");
                AddMarker inverse{*it};
                const std::string description = std::format("Remove marker '{}'", it->name);
                timeline.markers.erase(it);
                return AppliedTo<TimelineCommand>{std::move(inverse), description};

            } else if constexpr (std::is_same_v<T, SetTimelineRange>) {
                if (c.in_point && *c.in_point < 0)
                    return std::unexpected("the in point cannot be before zero");
                if (c.in_point && c.out_point && *c.in_point >= *c.out_point)
                    return std::unexpected("the in point has to come before the out point");
                SetTimelineRange inverse{timeline.in_point, timeline.out_point};
                timeline.in_point = c.in_point;
                timeline.out_point = c.out_point;
                return AppliedTo<TimelineCommand>{inverse, "Set range"};

            } else if constexpr (std::is_same_v<T, SetTimelineFormat>) {
                if (!c.fps.valid())
                    return std::unexpected("that is not a frame rate");
                if (c.width <= 0 || c.height <= 0)
                    return std::unexpected("that is not a frame size");
                SetTimelineFormat inverse{timeline.fps, timeline.width, timeline.height};
                const std::string description =
                    std::format("Set timeline to {}x{}", c.width, c.height);
                timeline.fps = c.fps;
                timeline.width = c.width;
                timeline.height = c.height;
                return AppliedTo<TimelineCommand>{inverse, description};

            } else if constexpr (std::is_same_v<T, TimelineBatch>) {
                if (!c.body)
                    return std::unexpected("an empty batch");
                // All or nothing: a batch that fails half way is a document in a state the
                // operator never asked for and cannot undo to.
                const Timeline before = timeline;
                std::vector<TimelineCommand> inverses;
                inverses.reserve(c.body->commands.size());
                for (const TimelineCommand& step : c.body->commands) {
                    auto applied = apply(timeline, step);
                    if (!applied) {
                        timeline = before;
                        return std::unexpected(applied.error());
                    }
                    inverses.push_back(std::move(applied->inverse));
                }
                // Undone backwards, because each inverse assumes the ones after it have
                // already been undone.
                std::ranges::reverse(inverses);
                return AppliedTo<TimelineCommand>{
                    make_timeline_batch(std::move(inverses), c.description), c.description};

            } else {
                static_assert(sizeof(T) == 0, "unhandled timeline command");
            }
        },
        command);
}

// ---- graphic ---------------------------------------------------------------------------------

std::expected<AppliedTo<GraphicCommand>, std::string> apply(Graphic& graphic,
                                                            const GraphicCommand& command) {
    using Result = std::expected<AppliedTo<GraphicCommand>, std::string>;

    return std::visit(
        [&graphic](const auto& c) -> Result {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, SetGraphicName>) {
                if (c.name.empty())
                    return std::unexpected("a graphic needs a name");
                SetGraphicName inverse{graphic.name};
                const std::string description = std::format("Rename graphic to '{}'", c.name);
                graphic.name = c.name;
                return AppliedTo<GraphicCommand>{inverse, description};

            } else if constexpr (std::is_same_v<T, AddLayer>) {
                if (graphic.layer(c.layer.id))
                    return std::unexpected("that layer is already here");
                if (c.layer.kind == LayerKind::Image && !c.layer.asset)
                    return std::unexpected("an image layer needs a picture");
                const std::size_t at = std::min(c.index, graphic.layers.size());
                graphic.layers.insert(graphic.layers.begin() + static_cast<std::ptrdiff_t>(at),
                                      c.layer);
                return AppliedTo<GraphicCommand>{RemoveLayer{c.layer.id},
                                                 std::format("Add '{}'", c.layer.name)};

            } else if constexpr (std::is_same_v<T, RemoveLayer>) {
                const auto it = std::ranges::find(graphic.layers, c.layer, &Layer::id);
                if (it == graphic.layers.end())
                    return std::unexpected("there is no such layer");
                if (it->locked)
                    return std::unexpected(std::format("layer '{}' is locked", it->name));
                const auto index =
                    static_cast<std::size_t>(std::distance(graphic.layers.begin(), it));
                AddLayer inverse{*it, index};
                const std::string description = std::format("Remove '{}'", it->name);
                graphic.layers.erase(it);
                return AppliedTo<GraphicCommand>{std::move(inverse), description};

            } else if constexpr (std::is_same_v<T, MoveLayer>) {
                const auto it = std::ranges::find(graphic.layers, c.layer, &Layer::id);
                if (it == graphic.layers.end())
                    return std::unexpected("there is no such layer");
                const auto from =
                    static_cast<std::size_t>(std::distance(graphic.layers.begin(), it));
                const std::size_t to = std::min(c.index, graphic.layers.size() - 1);
                if (from == to)
                    return std::unexpected("that layer is already there");
                Layer moved = std::move(*it);
                const std::string description = std::format("Reorder '{}'", moved.name);
                graphic.layers.erase(it);
                graphic.layers.insert(graphic.layers.begin() + static_cast<std::ptrdiff_t>(to),
                                      std::move(moved));
                return AppliedTo<GraphicCommand>{MoveLayer{c.layer, from}, description};

            } else if constexpr (std::is_same_v<T, SetLayer>) {
                Layer* layer = graphic.layer(c.layer);
                if (!layer)
                    return std::unexpected("there is no such layer");
                if (c.name.empty())
                    return std::unexpected("a layer needs a name");
                if (c.font_size <= 0.0)
                    return std::unexpected("a font size has to be positive");
                SetLayer inverse{c.layer,   layer->name,  layer->visible,    layer->locked,
                                 layer->color, layer->text, layer->data_field, layer->font_size};
                const std::string description = std::format("Change '{}'", c.name);
                layer->name = c.name;
                layer->visible = c.visible;
                layer->locked = c.locked;
                layer->color = c.color;
                layer->text = c.text;
                layer->data_field = c.data_field;
                layer->font_size = c.font_size;
                return AppliedTo<GraphicCommand>{std::move(inverse), description};

            } else if constexpr (std::is_same_v<T, SetChannel>) {
                Layer* layer = graphic.layer(c.layer);
                if (!layer)
                    return std::unexpected("there is no such layer");
                if (layer->locked)
                    return std::unexpected(std::format("layer '{}' is locked", layer->name));
                Channel* channel = channel_of(*layer, c.channel);
                if (!channel)
                    return std::unexpected("there is no such property");

                // Validated here rather than left to the document check, so a bad curve never
                // enters the document at all.
                Frame previous = 0;
                bool first = true;
                for (const Keyframe& k : c.value.keys) {
                    if (k.at < 0)
                        return std::unexpected("a keyframe cannot be before zero");
                    if (!first && k.at <= previous)
                        return std::unexpected("keyframes have to be in order, one per frame");
                    previous = k.at;
                    first = false;
                }
                SetChannel inverse{c.layer, c.channel, *channel};
                const std::string description =
                    std::format("Change {} of '{}'", to_string(c.channel), layer->name);
                *channel = c.value;
                return AppliedTo<GraphicCommand>{std::move(inverse), description};

            } else if constexpr (std::is_same_v<T, SetGraphicFormat>) {
                if (!c.fps.valid())
                    return std::unexpected("that is not a frame rate");
                if (c.width <= 0 || c.height <= 0)
                    return std::unexpected("that is not a frame size");
                if (c.duration <= 0)
                    return std::unexpected("a graphic needs a duration");
                SetGraphicFormat inverse{graphic.fps, graphic.width, graphic.height,
                                         graphic.duration};
                const std::string description =
                    std::format("Set graphic to {}x{}", c.width, c.height);
                graphic.fps = c.fps;
                graphic.width = c.width;
                graphic.height = c.height;
                graphic.duration = c.duration;
                return AppliedTo<GraphicCommand>{inverse, description};

            } else if constexpr (std::is_same_v<T, GraphicBatch>) {
                if (!c.body)
                    return std::unexpected("an empty batch");
                const Graphic before = graphic;
                std::vector<GraphicCommand> inverses;
                inverses.reserve(c.body->commands.size());
                for (const GraphicCommand& step : c.body->commands) {
                    auto applied = apply(graphic, step);
                    if (!applied) {
                        graphic = before;
                        return std::unexpected(applied.error());
                    }
                    inverses.push_back(std::move(applied->inverse));
                }
                std::ranges::reverse(inverses);
                return AppliedTo<GraphicCommand>{
                    make_graphic_batch(std::move(inverses), c.description), c.description};

            } else {
                static_assert(sizeof(T) == 0, "unhandled graphic command");
            }
        },
        command);
}

// ---- names -----------------------------------------------------------------------------------

std::string_view command_name(const TimelineCommand& command) {
    return std::visit(
        [](const auto& c) -> std::string_view {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SetTimelineName>)
                return "timeline.setName";
            else if constexpr (std::is_same_v<T, AddTrack>)
                return "timeline.addTrack";
            else if constexpr (std::is_same_v<T, RemoveTrack>)
                return "timeline.removeTrack";
            else if constexpr (std::is_same_v<T, SetTrack>)
                return "timeline.setTrack";
            else if constexpr (std::is_same_v<T, AddClip>)
                return "timeline.addClip";
            else if constexpr (std::is_same_v<T, RemoveClip>)
                return "timeline.removeClip";
            else if constexpr (std::is_same_v<T, MoveClip>)
                return "timeline.moveClip";
            else if constexpr (std::is_same_v<T, TrimClip>)
                return "timeline.trimClip";
            else if constexpr (std::is_same_v<T, SetClip>)
                return "timeline.setClip";
            else if constexpr (std::is_same_v<T, SplitClip>)
                return "timeline.splitClip";
            else if constexpr (std::is_same_v<T, AddMarker>)
                return "timeline.addMarker";
            else if constexpr (std::is_same_v<T, RemoveMarker>)
                return "timeline.removeMarker";
            else if constexpr (std::is_same_v<T, SetTimelineRange>)
                return "timeline.setRange";
            else if constexpr (std::is_same_v<T, SetTimelineFormat>)
                return "timeline.setFormat";
            else
                return "timeline.batch";
        },
        command);
}

std::string_view command_name(const GraphicCommand& command) {
    return std::visit(
        [](const auto& c) -> std::string_view {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SetGraphicName>)
                return "graphic.setName";
            else if constexpr (std::is_same_v<T, AddLayer>)
                return "graphic.addLayer";
            else if constexpr (std::is_same_v<T, RemoveLayer>)
                return "graphic.removeLayer";
            else if constexpr (std::is_same_v<T, MoveLayer>)
                return "graphic.moveLayer";
            else if constexpr (std::is_same_v<T, SetLayer>)
                return "graphic.setLayer";
            else if constexpr (std::is_same_v<T, SetChannel>)
                return "graphic.setChannel";
            else if constexpr (std::is_same_v<T, SetGraphicFormat>)
                return "graphic.setFormat";
            else
                return "graphic.batch";
        },
        command);
}

std::string describe(const TimelineCommand& command) {
    // The description a successful apply returns is the authoritative one -- it can name the
    // clip that was actually touched. This is for a command that has not been applied yet.
    return std::visit(
        [](const auto& c) -> std::string {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, TimelineBatch>)
                return c.description;
            else
                return std::string{command_name(TimelineCommand{c})};
        },
        command);
}

std::string describe(const GraphicCommand& command) {
    return std::visit(
        [](const auto& c) -> std::string {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, GraphicBatch>)
                return c.description;
            else
                return std::string{command_name(GraphicCommand{c})};
        },
        command);
}

} // namespace ldoc

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/subdocument.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// Edits to a timeline or a graphic.
///
/// A separate bus from `ldoc::Command`, and deliberately so. These commands cannot name a
/// scene, a source or an output, and a project command cannot name a clip or a layer; the two
/// alphabets do not overlap. That is what makes the undo isolation in `ldoc::Workbench` a
/// property of the types rather than a rule someone has to remember to follow -- a timeline edit
/// is not merely *routed* away from the project's undo stack, it cannot be applied to a project
/// at all.
///
/// The same two rules as the project bus hold here: applying a command returns its inverse, and
/// a command that cannot be applied changes nothing.
namespace ldoc {

// ---- timeline --------------------------------------------------------------------------------

struct SetTimelineName {
    std::string name;
    friend bool operator==(const SetTimelineName&, const SetTimelineName&) = default;
};

struct AddTrack {
    Track track;
    std::size_t index = 0;
    friend bool operator==(const AddTrack&, const AddTrack&) = default;
};

struct RemoveTrack {
    Id track;
    friend bool operator==(const RemoveTrack&, const RemoveTrack&) = default;
};

struct SetTrack {
    Id track;
    std::string name;
    bool enabled = true;
    bool locked = false;
    bool muted = false;
    bool solo = false;
    double gain_db = 0.0;
    double pan = 0.0;
    AutomationMode automation = AutomationMode::Read;
    bool patched = true;
    bool armed = false;
    friend bool operator==(const SetTrack&, const SetTrack&) = default;
};

/// Set the transition into a clip. A `Cut` of zero length removes one.
struct SetTransition {
    Id track;
    Id clip;
    Transition transition;
    friend bool operator==(const SetTransition&, const SetTransition&) = default;
};

struct SetMasterGain {
    double gain_db = 0.0;
    friend bool operator==(const SetMasterGain&, const SetMasterGain&) = default;
};

/// Place a clip on a track. Refused when it would overlap one already there -- the caller trims
/// or chooses another track, because silently shortening someone else's clip is not something a
/// drop should do.
struct AddClip {
    Id track;
    Clip clip;
    friend bool operator==(const AddClip&, const AddClip&) = default;
};

struct RemoveClip {
    Id track;
    Id clip;
    friend bool operator==(const RemoveClip&, const RemoveClip&) = default;
};

/// Move a clip in time, and optionally to another track. The destination must be free.
struct MoveClip {
    Id track;
    Id clip;
    Frame start = 0;
    /// Nil to stay on the track it is on.
    Id to_track;
    friend bool operator==(const MoveClip&, const MoveClip&) = default;
};

/// Trim a clip's edges. Both ends are given in timeline frames; the media window follows, so a
/// trim at the head keeps the picture under the cursor still rather than sliding it.
struct TrimClip {
    Id track;
    Id clip;
    Frame start = 0;
    Frame duration = 0;
    Frame source_in = 0;
    friend bool operator==(const TrimClip&, const TrimClip&) = default;
};

struct SetClip {
    Id track;
    Id clip;
    std::string name;
    double gain_db = 0.0;
    /// The clip's whole effect list. Carried entire, like every other field here, so that an
    /// inverse restores exactly what was there rather than a flag about it.
    std::vector<Effect> effects;
    friend bool operator==(const SetClip&, const SetClip&) = default;
};

/// Cut a clip in two at a frame, which is the single most-used edit there is.
struct SplitClip {
    Id track;
    Id clip;
    Frame at = 0;
    /// The id the right-hand piece takes. Given by the caller rather than minted inside, so
    /// that applying this command twice -- a redo, or a journal replay -- produces the same
    /// document both times.
    Id new_clip;
    friend bool operator==(const SplitClip&, const SplitClip&) = default;
};

struct AddMarker {
    Marker marker;
    friend bool operator==(const AddMarker&, const AddMarker&) = default;
};

struct RemoveMarker {
    Id marker;
    friend bool operator==(const RemoveMarker&, const RemoveMarker&) = default;
};

/// The review range. An absent end clears that side.
struct SetTimelineRange {
    std::optional<Frame> in_point;
    std::optional<Frame> out_point;
    friend bool operator==(const SetTimelineRange&, const SetTimelineRange&) = default;
};

struct SetTimelineFormat {
    Fps fps{};
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    friend bool operator==(const SetTimelineFormat&, const SetTimelineFormat&) = default;
};

struct TimelineBatchBody;

struct TimelineBatch {
    std::shared_ptr<const TimelineBatchBody> body;
    std::string description;
    friend bool operator==(const TimelineBatch&, const TimelineBatch&) = default;
};

/// An edit to a timeline. The playhead is deliberately absent: where the operator is looking is
/// not something they undo, and an undo stack full of cursor moves is one that cannot reach the
/// edit underneath them.
using TimelineCommand =
    std::variant<SetTimelineName, AddTrack, RemoveTrack, SetTrack, AddClip, RemoveClip, MoveClip,
                 TrimClip, SetClip, SplitClip, AddMarker, RemoveMarker, SetTimelineRange,
                 SetTimelineFormat, SetTransition, SetMasterGain, TimelineBatch>;

struct TimelineBatchBody {
    std::vector<TimelineCommand> commands;
    friend bool operator==(const TimelineBatchBody&, const TimelineBatchBody&) = default;
};

[[nodiscard]] TimelineCommand make_timeline_batch(std::vector<TimelineCommand> commands,
                                                  std::string description);

// ---- graphic ---------------------------------------------------------------------------------

struct SetGraphicName {
    std::string name;
    friend bool operator==(const SetGraphicName&, const SetGraphicName&) = default;
};

struct AddLayer {
    Layer layer;
    std::size_t index = 0;
    friend bool operator==(const AddLayer&, const AddLayer&) = default;
};

struct RemoveLayer {
    Id layer;
    friend bool operator==(const RemoveLayer&, const RemoveLayer&) = default;
};

struct MoveLayer {
    Id layer;
    std::size_t index = 0;
    friend bool operator==(const MoveLayer&, const MoveLayer&) = default;
};

/// Everything about a layer that is not a curve: its name, flags, colour, text, binding.
struct SetLayer {
    Id layer;
    std::string name;
    bool visible = true;
    bool locked = false;
    std::uint32_t color = 0xFFFFFF;
    std::string text;
    std::string data_field;
    double font_size = 48.0;
    friend bool operator==(const SetLayer&, const SetLayer&) = default;
};

/// Which property a channel command addresses. A name rather than a pointer, because a command
/// has to survive being written to a file and read back.
enum class ChannelId : std::uint8_t {
    X,
    Y,
    Width,
    Height,
    Rotation,
    Opacity,
};

[[nodiscard]] std::string_view to_string(ChannelId c);
[[nodiscard]] std::optional<ChannelId> channel_from_string(std::string_view s);

/// Replace one channel wholesale: its constant and all of its keys.
///
/// One command rather than add/remove/move keyframe commands. Dragging a key is a gesture that
/// produces a command per frame, and every one of those is "the curve now looks like this";
/// expressing it as a diff would cost an inverse that is harder to compute than the state it
/// replaces, for a curve that is a handful of numbers.
struct SetChannel {
    Id layer;
    ChannelId channel = ChannelId::X;
    Channel value;
    friend bool operator==(const SetChannel&, const SetChannel&) = default;
};

struct SetGraphicFormat {
    Fps fps{};
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Frame duration = 150;
    friend bool operator==(const SetGraphicFormat&, const SetGraphicFormat&) = default;
};

struct GraphicBatchBody;

struct GraphicBatch {
    std::shared_ptr<const GraphicBatchBody> body;
    std::string description;
    friend bool operator==(const GraphicBatch&, const GraphicBatch&) = default;
};

using GraphicCommand =
    std::variant<SetGraphicName, AddLayer, RemoveLayer, MoveLayer, SetLayer, SetChannel,
                 SetGraphicFormat, GraphicBatch>;

struct GraphicBatchBody {
    std::vector<GraphicCommand> commands;
    friend bool operator==(const GraphicBatchBody&, const GraphicBatchBody&) = default;
};

[[nodiscard]] GraphicCommand make_graphic_batch(std::vector<GraphicCommand> commands,
                                                std::string description);

// ---- applying --------------------------------------------------------------------------------

template <typename C>
struct AppliedTo {
    /// A command that, applied to the resulting document, restores the previous one.
    C inverse;
    std::string description;
};

[[nodiscard]] std::expected<AppliedTo<TimelineCommand>, std::string> apply(
    Timeline& timeline, const TimelineCommand& command);

[[nodiscard]] std::expected<AppliedTo<GraphicCommand>, std::string> apply(
    Graphic& graphic, const GraphicCommand& command);

[[nodiscard]] std::string_view command_name(const TimelineCommand& command);
[[nodiscard]] std::string_view command_name(const GraphicCommand& command);
[[nodiscard]] std::string describe(const TimelineCommand& command);
[[nodiscard]] std::string describe(const GraphicCommand& command);

} // namespace ldoc

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// Timelines and graphics: the project's *sub-documents*.
///
/// A scene belongs to the show. A timeline and a graphic are things an operator opens, works in
/// for a while, and closes -- more like files than like parts of the live document. That
/// difference is why they are modelled apart from `Project` rather than as more vectors inside
/// it, and it is what makes their undo isolation (see `ldoc::Workbench`) truthful rather than a
/// filter over one shared stack.
///
/// Each lives in its own file so that it can be copied into another project: `.mtime` for a
/// timeline, `.manim` for a graphic. The project file carries only a manifest of which ones
/// exist, exactly as it does for scenes, so a sub-document that will not parse costs that one
/// document and nothing else.
namespace ldoc {

// ---- timelines -------------------------------------------------------------------------------

/// A position on a timeline, in frames at the timeline's own rate.
///
/// Frames rather than seconds, because every edit an operator makes is to a frame boundary and
/// a timeline stored in floating-point seconds accumulates error until a cut that was flush
/// shows one black frame. Converting to seconds is `frames / rate`, done at the edges.
using Frame = std::int64_t;

enum class TrackKind : std::uint8_t {
    Video,
    Audio,
};

[[nodiscard]] std::string_view to_string(TrackKind k);
[[nodiscard]] std::optional<TrackKind> track_kind_from_string(std::string_view s);

/// One clip on a track: a window onto a media file, placed at a point in time.
///
/// `source_in` is where the window opens in the *media*, `start` where it lands on the
/// *timeline*, and `duration` is shared by both. Trimming changes `source_in` and `duration`
/// together; sliding a clip changes only `start`. Keeping the two spaces separate is what makes
/// a ripple trim expressible as arithmetic rather than as a special case.
struct Clip {
    Id id;
    /// The media this shows, by asset id. An asset the project no longer has leaves the clip in
    /// place and offline, rather than deleting the operator's edit for them.
    Id asset;
    /// Where this clip starts on the timeline.
    Frame start = 0;
    /// How long it runs. Always > 0 for a clip that exists; a zero-length clip is a removal.
    Frame duration = 0;
    /// The first frame of the media that is shown.
    Frame source_in = 0;
    /// Gain in dB for an audio clip, linear 0 for video. Separate from any effect, because
    /// level is the one thing adjusted constantly and it should never cost an effect slot.
    double gain_db = 0.0;
    /// Shown on the clip rather than derived from the asset, so a renamed asset does not
    /// silently relabel an edit that has been reviewed.
    std::string name;

    [[nodiscard]] Frame end() const { return start + duration; }
    /// The media frame shown at timeline frame `at`, which is only meaningful inside the clip.
    [[nodiscard]] Frame source_at(Frame at) const { return source_in + (at - start); }
    [[nodiscard]] bool covers(Frame at) const { return at >= start && at < end(); }

    friend bool operator==(const Clip&, const Clip&) = default;
};

/// A lane of clips, in start order and never overlapping.
///
/// The non-overlap rule is the track's whole job: two clips on one track at one time have no
/// defined answer for what is on screen, so the edit operations maintain the invariant instead
/// of the renderer guessing. Layering is what more tracks are for.
struct Track {
    Id id;
    std::string name;
    TrackKind kind = TrackKind::Video;
    /// Hidden tracks still exist and still edit; they contribute nothing to the picture.
    bool enabled = true;
    /// A locked track refuses every edit, which is what stops a finished layer being nudged.
    bool locked = false;
    /// Clips, sorted by `start`. `Timeline::validate` enforces both the order and the gaps.
    std::vector<Clip> clips;

    [[nodiscard]] const Clip* clip(const Id& wanted) const;
    [[nodiscard]] Clip* clip(const Id& wanted);
    /// The clip covering `at`, or nothing when the track is empty there.
    [[nodiscard]] const Clip* clip_at(Frame at) const;

    friend bool operator==(const Track&, const Track&) = default;
};

/// A named point on the timeline, for notes and for navigation.
struct Marker {
    Id id;
    Frame at = 0;
    std::string name;
    /// Packed 0xRRGGBB. Markers are scanned visually, so the colour is the index.
    std::uint32_t color = 0xFFCC00;

    friend bool operator==(const Marker&, const Marker&) = default;
};

/// An edit: tracks of clips over a shared time base.
struct Timeline {
    Id id;
    std::string name;
    /// The rate every `Frame` in this document is counted in. A timeline does not inherit the
    /// canvas rate: an edit of 24p footage keeps its own cadence whatever it is played out at.
    Fps fps{30, 1};
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    /// Video tracks render bottom-first, so the last one in this vector is the topmost layer --
    /// the same order the timeline panel draws them in, read upwards.
    std::vector<Track> tracks;
    std::vector<Marker> markers;
    /// Where the playhead was left. Saved because reopening an edit where you left it is the
    /// difference between resuming work and finding your place again.
    Frame playhead = 0;
    /// The review range, when one is set. Both ends are in timeline frames, `in` inclusive and
    /// `out` exclusive, which is the convention every edit operation here uses.
    std::optional<Frame> in_point;
    std::optional<Frame> out_point;

    [[nodiscard]] const Track* track(const Id& wanted) const;
    [[nodiscard]] Track* track(const Id& wanted);
    /// The last frame any clip reaches. Zero for an empty timeline.
    [[nodiscard]] Frame duration() const;

    /// Structurally wrong: a bad rate or size, clips out of order, clips that overlap on one
    /// track, a non-positive duration, a negative start, or duplicate ids.
    [[nodiscard]] std::optional<std::string> validate() const;

    friend bool operator==(const Timeline&, const Timeline&) = default;
};

// ---- graphics --------------------------------------------------------------------------------

/// What a graphic layer draws.
enum class LayerKind : std::uint8_t {
    /// A rectangle of colour.
    Rect,
    /// A line of text, which is what most broadcast templates are made of.
    Text,
    /// A still, by asset id.
    Image,
};

[[nodiscard]] std::string_view to_string(LayerKind k);
[[nodiscard]] std::optional<LayerKind> layer_kind_from_string(std::string_view s);

/// A property value at a point in time, in the graphic's own frames.
struct Keyframe {
    Frame at = 0;
    double value = 0.0;
    /// Cubic ease in/out when true, linear when false. One flag rather than full Bézier
    /// handles: it covers the overwhelming majority of broadcast motion, and the parameter a
    /// designer actually reaches for next is spring, not a second control point.
    ///
    /// ponytail: upgrade to Bézier handles when a designer asks for a curve this cannot make.
    bool ease = true;

    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

/// One animatable property: a constant, or a curve through keyframes.
struct Channel {
    /// Used when there are no keyframes, and as the value a new keyframe takes.
    double constant = 0.0;
    /// Sorted by `at`, never two at one frame.
    std::vector<Keyframe> keys;

    /// The value at `at`: the constant when there are no keys, the nearest end when outside
    /// the key range, and the interpolation between the surrounding pair otherwise.
    [[nodiscard]] double value_at(Frame at) const;
    [[nodiscard]] bool animated() const { return !keys.empty(); }

    friend bool operator==(const Channel&, const Channel&) = default;
};

/// An element of a graphic, animated over time.
struct Layer {
    Id id;
    std::string name;
    LayerKind kind = LayerKind::Rect;
    bool visible = true;
    bool locked = false;

    /// Geometry, each animatable. In graphic pixels, origin top-left.
    Channel x;
    Channel y;
    Channel width;
    Channel height;
    Channel rotation;
    /// 0..1. The channel every lower third animates.
    Channel opacity;

    /// Packed 0xRRGGBB fill, for Rect and Text.
    std::uint32_t color = 0xFFFFFF;
    /// Text content, and the field name that overrides it at play-out.
    std::string text;
    /// When set, the value is supplied at play-out from the show's data rather than from this
    /// file. The authored `text` is then the preview placeholder -- which is what makes a
    /// template a template rather than a one-off.
    std::string data_field;
    double font_size = 48.0;
    /// The still this layer draws, for `LayerKind::Image`.
    std::optional<Id> asset;

    friend bool operator==(const Layer&, const Layer&) = default;
};

/// A motion graphic: layers animated over a fixed duration, played out as an HTML5 overlay.
struct Graphic {
    Id id;
    std::string name;
    Fps fps{30, 1};
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    /// How long the whole graphic runs. A template's in and out animations live inside this.
    Frame duration = 150;
    /// Drawn in order, so the last layer is on top.
    std::vector<Layer> layers;
    Frame playhead = 0;

    [[nodiscard]] const Layer* layer(const Id& wanted) const;
    [[nodiscard]] Layer* layer(const Id& wanted);
    /// Every `data_field` set on a layer, deduplicated and sorted: the fields an operator fills
    /// in when they play this template.
    [[nodiscard]] std::vector<std::string> data_fields() const;

    [[nodiscard]] std::optional<std::string> validate() const;

    friend bool operator==(const Graphic&, const Graphic&) = default;
};

/// What the project file records about a sub-document it owns: enough to list it without
/// opening it, which is what the Media and Edit panels show.
struct SubdocRef {
    Id id;
    std::string name;
    /// The generation the project last saw it written at, used to notice a file that was
    /// changed behind this build's back, exactly as `SceneRef` does.
    std::uint64_t generation = 0;

    friend bool operator==(const SubdocRef&, const SubdocRef&) = default;
};

} // namespace ldoc

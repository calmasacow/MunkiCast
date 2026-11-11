// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/node.hpp"
#include "ldoc/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ldoc {

/// Bumped only by a change that older builds cannot read. Migrations are forward-only, with
/// golden fixtures; a project newer than this opens read-only rather than silently dropping the
/// fields this build does not know about.
inline constexpr int kSchemaVersion = 4;

/// An output surface with its own size, rate and colour space.
///
/// ADR-0020: a canvas is a pipeline, not a scale factor. Two canvases means two composites and
/// two independent frame deadlines, which is why they are first-class here rather than a
/// rescale applied late.
struct Canvas {
    Id id;
    std::string name;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Fps fps{};
    ColorSpace color_space = ColorSpace::Rec709;

    /// What this canvas broadcasts when the show cannot be supervised (ADR-0025).
    ///
    /// Per canvas rather than shared, because a per-canvas override can reposition a card but
    /// cannot swap the source binding -- and a vertical show wants a separately authored vertical
    /// clip, not a letterboxed landscape one. Absent means the canvas falls back to black and
    /// silence, which is safe but is not something anyone chose.
    std::optional<Id> emergency_scene;

    friend bool operator==(const Canvas&, const Canvas&) = default;
};

enum class BusKind : std::uint8_t {
    Input,
    Submix,
    Monitor,
    /// A numbered track in the recording container.
    Track,
    /// What goes to the live outputs.
    Program,
    /// The copyright-safe recording track.
    Vod,
};

[[nodiscard]] std::string_view to_string(BusKind k);
[[nodiscard]] std::optional<BusKind> bus_kind_from_string(std::string_view s);

/// An audio bus.
///
/// Note what is *not* here: a fixed six-track checkbox grid. "Eight tracks, thirty-two inputs"
/// is a count limit on this list, not a feature, and the copyright-safe VOD track is an ordinary
/// Track bus with the music send left off -- no special case, no separate code path that can
/// disagree with the normal one.
struct Bus {
    Id id;
    std::string name;
    BusKind kind = BusKind::Submix;
    std::int32_t channels = 2;
    std::vector<Effect> inserts;
    std::vector<Send> sends;
    double gain_db = 0.0;
    double pan = 0.0;
    bool mute = false;
    /// Heard locally, which never changes what goes out (ADR-0031). There is no solo: on a
    /// console solo is a monitoring aid, but this is document state the program mix is built
    /// from, so it would silence every other bus on air.
    bool monitor = false;
    /// Mixer view state that belongs to the show, not the workspace (ADR-0032). Hidden hides a
    /// strip and nothing else; locked stops the fader and nothing else.
    bool hidden = false;
    bool level_locked = false;
    double delay_ms = 0.0;

    friend bool operator==(const Bus&, const Bus&) = default;
};

/// Per-scene audio routing for one source node, as a set of deltas.
struct AudioOverride {
    std::optional<Id> bus;
    std::optional<double> gain_db;
    std::optional<bool> mute;
    std::optional<std::vector<Send>> sends;

    [[nodiscard]] bool empty() const { return !bus && !gain_db && !mute && !sends; }
    friend bool operator==(const AudioOverride&, const AudioOverride&) = default;
};

/// A scene is a tree, not a list, and it belongs to exactly one canvas (ADR-0027).
///
/// Scenes stay in one flat `Project::scenes` list with an owner field rather than nesting inside
/// `Canvas`, because ADR-0022 stores one scene per file in `scenes/<ulid>.json` with a manifest
/// in `project.json`. Nesting would make the in-memory and on-disk shapes disagree about what a
/// scene is; a flat list with an owner keeps both.
struct Scene {
    Id id;
    std::string name;
    /// The canvas this scene is authored for. Not optional: a scene with no canvas has no size,
    /// no frame rate and nowhere to go.
    Id canvas;
    /// `roots[0]` is the topmost node. See GroupData::children.
    std::vector<Node> roots;
    /// Keyed by the source node's id.
    std::map<Id, AudioOverride> audio_overrides;
    /// Marked by the operator as one they reach for. Part of the document rather than of the
    /// workspace, because it is a property of the show -- it should follow the project to
    /// another machine, and it is the same set whichever layout is on screen.
    bool favourite = false;
    std::vector<Effect> effects{};

    friend bool operator==(const Scene&, const Scene&) = default;
};

/// A directed cue from a scene on one canvas to a scene on another (ADR-0027).
///
/// Not a sharing relationship. Nothing about either scene's *contents* crosses: when `from`
/// becomes the program scene of its canvas's deck, the deck targeting `to`'s canvas takes `to`
/// to program using **that deck's own transition**. Two canvases are two pipelines (ADR-0020)
/// and a vertical show may well cut where the widescreen one dissolves.
///
/// Links live in `Project::scene_links` rather than on either scene. A relationship is not a
/// property of either end, deleting a scene has to prune every link that names it -- one list is
/// one place to prune -- and the linking panel is a view over this list rather than a walk of
/// every scene.
///
/// Firing is **one hop, never a chain**: if A cues B and B cues C, activating A takes B and
/// stops. That is a rule about what happens, not about what can be stored, so both links may
/// exist; anyone wanting C adds A -> C, which is visible in the list. A chain is a cue sequence
/// no panel can show you, and across three canvases it can close into a cycle.
struct SceneLink {
    Id id;
    /// A scene. Activating it on its canvas's deck fires the cue.
    Id from;
    /// A scene on a *different* canvas.
    Id to;

    friend bool operator==(const SceneLink&, const SceneLink&) = default;
};

/// A program/preview pair with its transition state, targeting one canvas. N canvases means N
/// decks and therefore N independent program paths.
struct Deck {
    Id id;
    std::string name;
    Id canvas;
    std::optional<Id> program_scene;
    std::optional<Id> preview_scene;
    bool studio_mode = false;
    std::string transition = "transition.cut";
    std::int32_t transition_ms = 300;

    friend bool operator==(const Deck&, const Deck&) = default;
};

/// An instance of something that produces pixels or audio.
///
/// Project-level rather than per-placement, so two source nodes showing the same camera open
/// one device. See SourceData in `node.hpp` for which of the two "source" types is which.
/// The `type` resolves against a manifest, like an effect does.
struct Source {
    Id id;
    std::string name;
    std::string type;
    ParamMap settings;
    /// For file-backed sources.
    std::optional<Id> asset;
    std::vector<Effect> effects{};

    friend bool operator==(const Source&, const Source&) = default;
};

/// A file the project depends on, content-addressed so a move can be detected and repaired.
struct Asset {
    Id id;
    /// Relative to the project directory when the file lives inside it.
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
    /// Other places this file has been seen, for relocating it after a move.
    std::vector<std::string> relocation_hints;

    friend bool operator==(const Asset&, const Asset&) = default;
};

struct EncoderConfig {
    /// "h264", "hevc", "av1".
    std::string codec = "h264";
    /// Which implementation: "nvenc", "qsv", "vaapi", "x264", "svt-av1".
    std::string implementation = "x264";
    std::int32_t bitrate_kbps = 6000;
    std::int32_t keyframe_interval_ms = 2000;
    std::string preset;
    std::string rate_control = "cbr";
    /// Escape hatch for implementation-specific knobs.
    ParamMap extra;

    friend bool operator==(const EncoderConfig&, const EncoderConfig&) = default;
};

enum class DestinationKind : std::uint8_t { Rtmp, Rtmps, Srt, Whip, File, Replay };

[[nodiscard]] std::string_view to_string(DestinationKind k);
[[nodiscard]] std::optional<DestinationKind> destination_kind_from_string(std::string_view s);

/// Where an output goes.
///
/// **There is deliberately no field here that can hold a stream key.** `credential_ref` names an
/// entry in a secret store kept outside the project directory; the project file holds the name,
/// never the value.
///
/// This is not caution for its own sake. The project format is a directory of JSON chosen
/// *because* it is git-diffable and easy to share when asking for help -- and a stream key in
/// that file means every one of those acts leaks it. The author of this project has already had
/// a live key read out of a config dump during development. A format that cannot carry the
/// secret cannot leak it, and no amount of redaction discipline is as reliable as not having the
/// field.
struct Destination {
    DestinationKind kind = DestinationKind::Rtmp;
    /// The ingest URL, or output directory for File. See `credential_ref` for stream keys.
    std::string url;
    /// The name of a credential, never the credential.
    std::string credential_ref;

    /// Catalog service or custom transport ID; empty in older projects.
    std::string service;
    std::uint32_t replay_seconds = 20;
    std::uint32_t replay_memory_mb = 512;

    friend bool operator==(const Destination&, const Destination&) = default;
};

/// One encode of one canvas: a bitstream, named, that any number of destinations can receive.
///
/// First-class rather than a property of an output (ADR-0029), because "send Twitch's 1080p
/// encode to Kick and Facebook as well" is the thing operators actually want and the GPU
/// actually cares about. As a field on Output it could only be expressed as one output borrowing
/// another's encoder, which raises an ownership question with no good answer: deleting the
/// Twitch output would take Kick and Facebook down with it.
///
/// What this makes impossible is worth as much as what it makes possible. An output names an
/// encode and an encode names a canvas, so there is no edge that can point back and a cycle
/// cannot be written down. And `encodes.size()` is the number to compare against the measured
/// NVENC ceiling -- a figure `validate()` can state without walking anything.
struct Encode {
    Id id;
    /// What the UI offers to reuse. **Name it for what it is -- "1080p60 8 Mbps" -- and never
    /// for a destination**: an encode called "Twitch 1080p" that keeps running after Twitch
    /// stops reads as a bug rather than as the feature it is.
    std::string name;
    /// What is being encoded. Nil means the graph input is disconnected.
    Id canvas;
    EncoderConfig encoder;
    /// Twitch's 1080p out of a 1440p canvas. Absent means the canvas's own size.
    std::optional<Vec2> rescale;
    /// Where this sits on the Encode workspace's graph. View state, but it belongs to the
    /// project rather than to the machine: an arrangement someone laid out by hand should
    /// survive being opened on the other desk, for the same reason `Scene::favourite` is here.
    Vec2 position;

    friend bool operator==(const Encode&, const Encode&) = default;
};

/// One encode of one bus: an audio bitstream, named, that any number of destinations receive.
///
/// First-class for exactly the reason `Encode` is (ADR-0029). This used to be four string keys
/// inside `EncoderConfig::extra` -- `audio.codec`, `audio.bitrate_kbps`, `audio.track`,
/// `audio.vod_track` -- which could say "this video encode carries AAC at 320" but could not say
/// that eight destinations are receiving *the same* AAC bitstream. One encoder, one stream, many
/// receivers is the whole shape of the thing, and the old form could not write it down.
///
/// Cycles remain impossible: an output names encodes, an encode names a bus or canvas, and
/// neither can point back.
struct AudioEncode {
    Id id;
    /// Named for what it is -- "AAC 320" -- never for a destination, same rule as `Encode`.
    std::string name;
    /// Which bus is encoded. Program for most shows; the Vod bus for the copyright-safe track.
    /// Nil means the graph input is disconnected.
    Id bus;
    /// "aac" or "opus".
    std::string codec = "aac";
    std::int32_t bitrate_kbps = 160;
    /// Escape hatch for implementation-specific knobs, as on EncoderConfig.
    ParamMap extra;
    /// Where this sits on the Encode workspace's graph. See Encode::position.
    Vec2 position;

    friend bool operator==(const AudioEncode&, const AudioEncode&) = default;
};

/// One destination receiving one encode.
///
/// N outputs across M canvases falls out of this being a list rather than a mode. Outputs
/// naming the same encode share one bitstream and therefore share everything about it --
/// bitrate, keyframe cadence, resolution, colour. There is no per-destination bitrate when
/// sharing, because there is one bitstream; an operator who wants Kick at a different bitrate
/// wants a second encode, and now has to say so.
struct Output {
    Id id;
    std::string name;
    /// Which video bitstream this destination receives. Nil means Video 0 is disconnected.
    Id encode;
    /// Which audio bitstream it receives. Absent means the destination carries no audio, which
    /// is a thing a file-only video capture wants and a live destination does not -- validate()
    /// warns rather than refuses, because silence is a choice an operator can mean.
    std::optional<Id> audio_encode;
    Destination destination;
    bool enabled = false;
    /// Where this sits on the Encode workspace's graph. See Encode::position.
    Vec2 position;

    friend bool operator==(const Output&, const Output&) = default;
};

struct Settings {
    /// Sample rate for the whole audio graph. One rate, resampled at the edges.
    std::int32_t sample_rate = 48000;
    /// How much the renderer may run ahead, in frames.
    std::int32_t render_queue_frames = 3;
    ParamMap extra;

    friend bool operator==(const Settings&, const Settings&) = default;
};

struct Project {
    int schema_version = kSchemaVersion;
    Id id;
    std::string name;
    /// Incremented on every committed save. The save protocol writes this last, so it can never
    /// name files that are not already on disk.
    std::uint64_t generation = 0;

    std::vector<Canvas> canvases;
    std::vector<Bus> buses;
    std::vector<Scene> scenes;
    /// Cues between scenes on different canvases. See SceneLink.
    std::vector<SceneLink> scene_links;
    std::vector<Deck> decks;
    std::vector<Source> sources;
    std::vector<Asset> assets;
    /// The bitstreams. An output names one; several may name the same one.
    std::vector<Encode> encodes;
    std::vector<AudioEncode> audio_encodes;
    std::vector<Output> outputs;
    Settings settings;

    [[nodiscard]] const Canvas* canvas(const Id& wanted) const;
    [[nodiscard]] const Scene* scene(const Id& wanted) const;
    [[nodiscard]] const SceneLink* scene_link(const Id& wanted) const;
    [[nodiscard]] Scene* scene(const Id& wanted);
    [[nodiscard]] const Bus* bus(const Id& wanted) const;
    [[nodiscard]] const Source* source(const Id& wanted) const;
    [[nodiscard]] const Encode* encode(const Id& wanted) const;
    [[nodiscard]] const AudioEncode* audio_encode(const Id& wanted) const;
    [[nodiscard]] const std::vector<Effect>* effect_stack(const Id& owner) const;
    [[nodiscard]] std::vector<Effect>* effect_stack(const Id& owner);

    friend bool operator==(const Project&, const Project&) = default;
};

/// Resolve a source clone to its shared input, preserving the input ID/settings and composing
/// source filters. Missing targets and cycles return no value; no capture is duplicated.
/// Audio taps also require Audio enabled at every clone; video bypass never skips audio filters.
[[nodiscard]] std::optional<Source> resolve_source(const Project& project, const Source& source,
                                                   bool audio = false);

/// A new project with one 1080p60 canvas, a program bus and a VOD track, one empty scene and a
/// deck pointed at it. Enough to open and look at; nothing that pretends to be configured.
[[nodiscard]] Project make_default_project(std::string name);

/// Duplicate a scene, minting fresh ids throughout (see `duplicate` in node.hpp for exactly which
/// references are rewritten and which are preserved).
[[nodiscard]] Scene duplicate_scene(const Scene& scene, std::string new_name);

// ---- integrity ------------------------------------------------------------------------------

enum class Severity : std::uint8_t { Error, Warning };

struct Problem {
    Severity severity = Severity::Error;
    std::string message;
    /// What the problem is about, so the UI can select it rather than just describe it.
    Id subject;
};

/// Every structural problem in the project, rather than the first one.
///
/// A document has many independent things wrong with it after a bad import or a hand-edit, and a
/// validator that stops at the first makes fixing them a game of whack-a-mole. Errors mean the
/// project cannot be rendered as written; warnings mean it can, but something in it is useless.
[[nodiscard]] std::vector<Problem> validate(const Project& project);

/// Convenience: true when `validate` returned no Error-severity problems.
[[nodiscard]] bool is_valid(const Project& project);

/// Whether a source of this type can be trusted to keep working when something else has already
/// gone wrong: no device to be unplugged, no network, no browser engine, nothing to wedge.
///
/// Unknown types answer **false**. A safety net built from something we cannot vouch for is not
/// one, and a conservative default is the only one that stays correct as source types are added.
///
/// A built-in table until source manifests exist (ADR-0004), where this belongs beside the rest
/// of a source type's declaration.
[[nodiscard]] bool is_self_contained(std::string_view source_type);

/// Whether a source of this type carries audio of its own, and so gets a fader in the mixer.
///
/// Unknown types answer **true** -- the opposite default to `is_self_contained`, because the
/// mistakes are not symmetric: a fader for something silent does nothing, while a missing fader
/// is audio on air that cannot be muted. Says nothing about whether a *particular* source is
/// making sound: a media file with no audio track is still a type that can have one.
[[nodiscard]] bool has_audio(std::string_view source_type);
[[nodiscard]] bool has_audio(const Source& source);

/// Why `scene` is unsuitable as an emergency scene, or empty if it is suitable.
///
/// Reported as warnings by `validate`, and used by the designation flow to refuse and to offer
/// the loop fix before the command is ever issued. The command itself permits whatever the model
/// permits -- see ADR-0023 on why a command must not refuse a state the model allows.
[[nodiscard]] std::vector<Problem> check_emergency_scene(const Project& project,
                                                         const Id& scene);

/// What an encode is called when nobody has named it: "1080p60 8 Mbps", from the height it
/// actually sends, the canvas's rate and the bitrate.
///
/// Never from a destination. An encode called "Twitch 1080p" that keeps feeding Kick after
/// Twitch stops reads as a bug rather than as the feature it is (ADR-0029), and sharing is
/// precisely the state this model makes reachable.
///
/// Shared by the migration, which names the encodes it invents, and by the shell, which uses it
/// to tell an auto-named encode from one the operator renamed: an encode whose name still
/// matches this for its old settings is one nobody has named, so it follows the settings.
[[nodiscard]] std::string default_encode_name(const Project& project, const Encode& encode);

} // namespace ldoc

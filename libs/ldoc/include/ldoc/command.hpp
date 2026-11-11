// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <expected>
#include <optional>
#include <string>
#include <memory>
#include <string_view>
#include <variant>

/// The command bus.
///
/// Every document mutation is one of these: validated, serialisable, journalable and undoable.
/// The UI, global hotkeys, a Stream Deck, a websocket API and scripting are then all the same
/// thing -- producers of commands -- rather than five code paths that each mutate the document
/// slightly differently and disagree about undo.
///
/// Two properties make the rest of the system fall out of this one:
///
/// **Applying a command returns its inverse, and the inverse is itself a command.** Undo is
/// therefore not a separate mechanism with its own bugs; it is the same apply() running a command
/// that was built from the state the first one replaced. It also means an undo step can be
/// written to the journal exactly like anything else.
///
/// **A command that cannot be applied changes nothing.** Every one validates before it mutates,
/// so a refused command leaves the project byte-identical rather than half-applied.
namespace ldoc {

// ---- project --------------------------------------------------------------------------------

struct SetProjectName {
    std::string name;
    friend bool operator==(const SetProjectName&, const SetProjectName&) = default;
};

// ---- scenes ---------------------------------------------------------------------------------

struct AddScene {
    Scene scene;
    std::size_t index = 0;
    friend bool operator==(const AddScene&, const AddScene&) = default;
};

struct RemoveScene {
    Id scene;
    friend bool operator==(const RemoveScene&, const RemoveScene&) = default;
};

/// Reorder a scene within the project's list, which is the order the scene list shows.
/// Mark a scene as one the operator reaches for, so it appears in the favourites panel.
struct SetSceneFavourite {
    Id scene;
    bool favourite = true;
    friend bool operator==(const SetSceneFavourite&, const SetSceneFavourite&) = default;
};

struct MoveScene {
    Id scene;
    std::size_t index = 0;
    friend bool operator==(const MoveScene&, const MoveScene&) = default;
};

struct RenameScene {
    Id scene;
    std::string name;
    friend bool operator==(const RenameScene&, const RenameScene&) = default;
};

// ---- nodes ----------------------------------------------------------------------------------

struct AddNode {
    Id scene;
    /// Absent means a root of the scene.
    std::optional<Id> parent;
    std::size_t index = 0;
    Node node;
    friend bool operator==(const AddNode&, const AddNode&) = default;
};

struct RemoveNode {
    Id scene;
    Id node;
    friend bool operator==(const RemoveNode&, const RemoveNode&) = default;
};

struct MoveNode {
    Id scene;
    Id node;
    std::optional<Id> parent;
    std::size_t index = 0;
    friend bool operator==(const MoveNode&, const MoveNode&) = default;
};

struct RenameNode {
    Id scene;
    Id node;
    std::string name;
    friend bool operator==(const RenameNode&, const RenameNode&) = default;
};

struct SetNodeVisible {
    Id scene;
    Id node;
    bool visible = true;
    friend bool operator==(const SetNodeVisible&, const SetNodeVisible&) = default;
};

struct SetNodeLocked {
    Id scene;
    Id node;
    bool locked = false;
    friend bool operator==(const SetNodeLocked&, const SetNodeLocked&) = default;
};

struct SetNodeTransform {
    Id scene;
    Id node;
    Transform transform;
    friend bool operator==(const SetNodeTransform&, const SetNodeTransform&) = default;
};

struct SetNodeLayout {
    Id scene;
    Id node;
    Layout layout;
    friend bool operator==(const SetNodeLayout&, const SetNodeLayout&) = default;
};

struct SetNodeBlend {
    Id scene;
    Id node;
    Blend blend;
    friend bool operator==(const SetNodeBlend&, const SetNodeBlend&) = default;
};

/// A source node's own audio routing: which bus it feeds, its sends, its fader and its mute.
///
/// Only a source node has anywhere to keep this, so it is refused on anything else: a group has no
/// audio field, which means there would be nothing for the inverse to restore.
struct SetNodeAudio {
    Id scene;
    Id node;
    AudioRouting audio;
    friend bool operator==(const SetNodeAudio&, const SetNodeAudio&) = default;
};

// ---- effects --------------------------------------------------------------------------------

struct SetEffects {
    Id owner;
    std::vector<Effect> effects;
    friend bool operator==(const SetEffects&, const SetEffects&) = default;
};

struct AddEffect {
    Id scene;
    Id node;
    std::size_t index = 0;
    Effect effect;
    friend bool operator==(const AddEffect&, const AddEffect&) = default;
};

struct RemoveEffect {
    Id scene;
    Id node;
    Id effect;
    friend bool operator==(const RemoveEffect&, const RemoveEffect&) = default;
};

struct SetEffectParams {
    Id scene;
    Id node;
    Id effect;
    ParamMap params;
    friend bool operator==(const SetEffectParams&, const SetEffectParams&) = default;
};

struct SetEffectEnabled {
    Id scene;
    Id node;
    Id effect;
    bool enabled = true;
    friend bool operator==(const SetEffectEnabled&, const SetEffectEnabled&) = default;
};

/// Bind a source node to a source, or to nothing.
///
/// Sources are project-level, so this is a reference rather than ownership: two source nodes
/// binding the same source show one camera, not two.
///
/// Its wire name is `node.set-source`. It was `layer.set-source` before the version 2 bump, and
/// the reader still accepts that spelling: the journal is read during crash recovery, which is
/// the worst possible place for a strict parser, and `command_name()` is documented as stable
/// on the websocket API. Only the new name is ever written.
struct SetNodeSource {
    Id scene;
    Id node;
    std::optional<Id> source;
    friend bool operator==(const SetNodeSource&, const SetNodeSource&) = default;
};

// ---- sources --------------------------------------------------------------------------------

struct AddSource {
    Source source;
    std::size_t index = 0;
    friend bool operator==(const AddSource&, const AddSource&) = default;
};

struct RemoveSource {
    Id source;
    friend bool operator==(const RemoveSource&, const RemoveSource&) = default;
};

/// Replace a source wholesale, identified by `value.id`.
///
/// One command rather than one per field. The inverse is then exactly the previous struct, which
/// cannot drift from what it is reversing, and a rename and a settings change are the same kind
/// of edit as far as undo is concerned. The cost is a coarser undo step, which is the right
/// trade for a properties panel that commits on editing-finished anyway.
struct SetSource {
    Source value;
    friend bool operator==(const SetSource&, const SetSource&) = default;
};

// ---- scene links ----------------------------------------------------------------------------

/// Add a cue from one canvas's scene to another's (ADR-0027).
///
/// Unlike most references in this file, both ends are **preconditions** rather than references:
/// a link is defined by the relationship between two scenes, so one naming a scene that does not
/// exist is not a link in a degraded state, it is nonsense. The three structural rules -- the two
/// ends on different canvases, at most one link from a scene to any one canvas, and never cueing
/// a canvas's emergency scene -- are enforced here too, because each describes a state the model
/// has no meaning for.
///
/// Deliberately **not** enforced: that `from` is on the first canvas. The operator's answer was
/// "one-way, main leads" and the linking panel only ever offers Main's scenes, but that is which
/// links are worth making rather than which can exist -- and firing is one hop, so a chain does
/// nothing surprising even if one is somehow stored. See the implementation note in ADR-0027.
struct AddSceneLink {
    SceneLink link;
    std::size_t index = 0;
    friend bool operator==(const AddSceneLink&, const AddSceneLink&) = default;
};

struct RemoveSceneLink {
    Id link;
    friend bool operator==(const RemoveSceneLink&, const RemoveSceneLink&) = default;
};

/// There is no SetSceneLink. A link is nothing but its two ends, so changing one is removing it
/// and adding another -- and the linking panel commits a whole tick-list as one Batch anyway.

// ---- canvases -------------------------------------------------------------------------------

struct AddCanvas {
    Canvas canvas;
    std::size_t index = 0;
    friend bool operator==(const AddCanvas&, const AddCanvas&) = default;
};

struct RemoveCanvas {
    Id canvas;
    friend bool operator==(const RemoveCanvas&, const RemoveCanvas&) = default;
};

struct SetCanvas {
    Canvas value;
    friend bool operator==(const SetCanvas&, const SetCanvas&) = default;
};

// ---- buses ----------------------------------------------------------------------------------

struct AddBus {
    Bus bus;
    std::size_t index = 0;
    friend bool operator==(const AddBus&, const AddBus&) = default;
};

struct RemoveBus {
    Id bus;
    friend bool operator==(const RemoveBus&, const RemoveBus&) = default;
};

struct SetBus {
    Bus value;
    friend bool operator==(const SetBus&, const SetBus&) = default;
};

// ---- encodes --------------------------------------------------------------------------------

struct AddEncode {
    Encode encode;
    std::size_t index = 0;
    friend bool operator==(const AddEncode&, const AddEncode&) = default;
};

/// Refused while an output still names it (ADR-0029).
///
/// Unlike removing a source -- which leaves its nodes drawing nothing, exactly as they would
/// unbound -- an output with no bitstream is not a degraded state the model has a meaning for.
/// It is the same line a deck's canvas sits on: validate() calls it an error, so the command
/// refuses it. Point the outputs elsewhere, or delete them, first.
struct RemoveEncode {
    Id encode;
    friend bool operator==(const RemoveEncode&, const RemoveEncode&) = default;
};

struct SetEncode {
    Encode value;
    friend bool operator==(const SetEncode&, const SetEncode&) = default;
};

// ---- audio encodes --------------------------------------------------------------------------

struct AddAudioEncode {
    AudioEncode encode;
    std::size_t index = 0;
    friend bool operator==(const AddAudioEncode&, const AddAudioEncode&) = default;
};

/// Refused while an output still names it, exactly as RemoveEncode is.
///
/// A destination with no audio bitstream is not a degraded state the model has a meaning for;
/// it is a destination that will go to air silent. Point the outputs elsewhere, or clear their
/// audio, first.
struct RemoveAudioEncode {
    Id encode;
    friend bool operator==(const RemoveAudioEncode&, const RemoveAudioEncode&) = default;
};

struct SetAudioEncode {
    AudioEncode value;
    friend bool operator==(const SetAudioEncode&, const SetAudioEncode&) = default;
};

// ---- outputs --------------------------------------------------------------------------------

struct AddOutput {
    Output output;
    std::size_t index = 0;
    friend bool operator==(const AddOutput&, const AddOutput&) = default;
};

struct RemoveOutput {
    Id output;
    friend bool operator==(const RemoveOutput&, const RemoveOutput&) = default;
};

struct SetOutput {
    Output value;
    friend bool operator==(const SetOutput&, const SetOutput&) = default;
};

// ---- project settings -------------------------------------------------------------------------

struct SetProjectSettings {
    Settings value;
    friend bool operator==(const SetProjectSettings&, const SetProjectSettings&) = default;
};

// ---- decks ----------------------------------------------------------------------------------

/// A deck's own configuration: its name, its canvas, studio mode, and which transition it uses.
///
/// A deck is one program path for one canvas. The scenes it
/// points at are references and are set by SetDeckProgram/SetDeckPreview; its canvas is a
/// precondition, because a deck with no canvas has nothing to put on screen and validate() calls
/// that an error.
struct SetDeck {
    Deck value;
    friend bool operator==(const SetDeck&, const SetDeck&) = default;
};

struct AddDeck {
    Deck value;
    std::size_t index = 0;
    friend bool operator==(const AddDeck&, const AddDeck&) = default;
};

struct RemoveDeck {
    Id deck;
    friend bool operator==(const RemoveDeck&, const RemoveDeck&) = default;
};

struct SetDeckProgram {
    Id deck;
    std::optional<Id> scene;
    friend bool operator==(const SetDeckProgram&, const SetDeckProgram&) = default;
};

struct SetDeckPreview {
    Id deck;
    std::optional<Id> scene;
    friend bool operator==(const SetDeckPreview&, const SetDeckPreview&) = default;
};

/// Absent clears it, so the canvas falls back to black and silence.
///
/// This permits any scene id, including one that does not exist or is unsuitable: see ADR-0023 on
/// why a command may only refuse states the document model itself refuses. Checking suitability,
/// explaining it and offering to fix the clip's loop setting is the designation flow's job, which
/// runs `check_emergency_scene` before issuing this at all.
struct SetCanvasEmergencyScene {
    Id canvas;
    std::optional<Id> scene;
    friend bool operator==(const SetCanvasEmergencyScene&,
                           const SetCanvasEmergencyScene&) = default;
};

/// Several commands that are one thing the operator did, and therefore one undo step.
///
/// "Add a camera" is three commands -- make the source, make the source node, bind them --
/// and nobody pressing Ctrl+Z after that wants the node back without its source. So the group
/// applies and reverses as a unit.
///
/// `apply()` is all-or-nothing: if any member is refused, the ones already applied are reversed
/// before the error comes back, so a half-applied batch cannot exist. The inverse is the members'
/// inverses *in reverse order*, which is the only order that can work -- the bind has to come
/// undone before the node it bound does.
///
/// `description` is the purpose, not a list. The undo menu should read "Add Camera", not
/// "Add source; Add node; Bind node".
///
/// The body is held indirectly, and that is a language constraint rather than a choice: a Batch
/// is one of `Command`'s alternatives, so it cannot contain a `std::vector<Command>` directly --
/// the variant would need Batch to be complete while Batch needed the variant to be. A
/// `shared_ptr` to an incomplete type is fine, and the batch is immutable once built, so sharing
/// it costs nothing.
struct BatchBody;

struct Batch {
    std::shared_ptr<const BatchBody> body;
    std::string description;

    /// Compares what the batches *contain*, not which object they point at. Two batches built
    /// separately from equal commands are equal -- which the round-trip and undo/redo property
    /// tests rely on.
    friend bool operator==(const Batch& a, const Batch& b);
};

using Command = std::variant<
    SetProjectName, AddScene, RemoveScene, RenameScene, MoveScene, SetSceneFavourite, AddNode,
    RemoveNode, MoveNode, RenameNode, SetNodeVisible, SetNodeLocked, SetNodeTransform,
    SetNodeLayout, SetNodeBlend, SetNodeAudio, SetEffects, AddEffect, RemoveEffect, SetEffectParams,
    SetEffectEnabled, SetDeck, AddDeck, RemoveDeck, SetDeckProgram, SetDeckPreview, AddSceneLink,
    RemoveSceneLink, SetCanvasEmergencyScene, SetNodeSource, AddSource, RemoveSource, SetSource,
    AddCanvas, RemoveCanvas, SetCanvas, AddBus, RemoveBus, SetBus, AddOutput, RemoveOutput,
    SetOutput, AddEncode, RemoveEncode, SetEncode, AddAudioEncode, RemoveAudioEncode,
    SetAudioEncode, SetProjectSettings, Batch>;

struct BatchBody {
    std::vector<Command> commands;
    friend bool operator==(const BatchBody&, const BatchBody&) = default;
};

/// Build a batch. Prefer this to assembling a `Batch` by hand; it is the only place the body
/// gets allocated.
[[nodiscard]] Command make_batch(std::vector<Command> commands, std::string description);

struct Applied {
    /// A command that, applied to the resulting project, restores the previous one.
    Command inverse;
    /// What to put in the undo menu: "Hide Camera", "Rename scene to 'Intro'".
    std::string description;
};

/// Apply `command` to `project`.
///
/// On failure the project is untouched and the error says what was wrong, in terms of the
/// document rather than of the code.
[[nodiscard]] std::expected<Applied, std::string> apply(Project& project,
                                                        const Command& command);

/// The stable wire name, as it appears in the journal and on the websocket API.
[[nodiscard]] std::string_view command_name(const Command& command);
[[nodiscard]] std::string describe(const Command& command);

[[nodiscard]] std::string command_to_json(const Command& command);
[[nodiscard]] std::expected<Command, std::string> command_from_json(std::string_view text);

} // namespace ldoc

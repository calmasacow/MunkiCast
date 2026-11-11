// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

/// The on-disk representation: text in, text out, no filesystem. Keeping it that way is what
/// makes the format fuzzable and the round-trip property testable without touching a disk.
///
/// Two rules shape everything here.
///
/// **Every default is written explicitly.** OBS's sparse settings blobs are precisely why its own
/// importer needs a per-source-id table of what an absent key used to mean, and why a plugin
/// update can silently change the behaviour of a project nobody edited. A file that states every
/// value cannot do that.
///
/// **Reading is lenient, writing is strict.** An absent key on read takes the code's default, so
/// a project written by an older build still opens. A key that is present but the wrong type is
/// an error, because that is corruption or a hand-edit, not an older version.
namespace ldoc {

/// Deterministic: the same project always produces the same bytes, so a project directory diffs
/// cleanly and a golden fixture means something.
[[nodiscard]] std::string project_to_json(const Project& project);
[[nodiscard]] std::string assets_to_json(const std::vector<Asset>& assets);
/// `generation` is stamped into the file so that opening a project can tell whether this scene
/// was written by the same save that wrote `project.json` -- which is how a torn save is detected
/// rather than guessed at.
[[nodiscard]] std::string scene_to_json(const Scene& scene, std::uint64_t generation = 0);

/// One entry of `project.json`'s scene manifest.
///
/// The manifest is a property of the *file*, not of the model: in memory a Project simply holds
/// its scenes. It exists on disk so that opening a project knows which scene files to look for,
/// and -- through `generation` -- whether each one was written by the same save that wrote
/// `project.json`. That is how a torn save is detected rather than inferred.
struct SceneRef {
    Id id;
    std::string name;
    std::uint64_t generation = 0;

    friend bool operator==(const SceneRef&, const SceneRef&) = default;
};

/// What a version 1 output carried before ADR-0029 made an encode a document object.
///
/// Filled only when the file is version 1. The migration turns each of these into an `Encode`
/// and points its output at it, sharing one encode between outputs that asked for the same
/// thing. Keeping it out of `Output` is deliberate: the model should not grow a field whose only
/// purpose is to remember a shape it no longer has.
struct LegacyOutputEncode {
    Id output;
    Id canvas;
    EncoderConfig encoder;
};

struct ProjectFile {
    /// `scenes` is empty; they live in their own files and are loaded by `project_io`.
    Project project;
    std::vector<SceneRef> scenes;
    /// Empty for a version 2 file. See LegacyOutputEncode.
    std::vector<LegacyOutputEncode> legacy_outputs;
};

[[nodiscard]] std::expected<ProjectFile, std::string> project_from_json(std::string_view text);
[[nodiscard]] std::expected<std::vector<Asset>, std::string> assets_from_json(
    std::string_view text);
struct SceneFile {
    Scene scene;
    std::uint64_t generation = 0;
    /// The schema the file was written in, so the caller can tell a version 1 scene -- which has
    /// no canvas of its own and may carry `canvas_overrides` -- from one that is already current.
    int schema_version = kSchemaVersion;
    /// How many nodes carried `canvas_overrides`, which version 2 deleted (ADR-0027). Reported
    /// on open rather than dropped in silence: discarding authored work quietly is how a format
    /// bump earns a reputation.
    std::size_t dropped_canvas_overrides = 0;
};

[[nodiscard]] std::expected<SceneFile, std::string> scene_from_json(std::string_view text);

} // namespace ldoc

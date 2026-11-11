// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/command.hpp"

#include <deque>
#include <expected>
#include <string>
#include <string_view>

namespace ldoc {

/// A project plus its undo stack.
///
/// Undo is not a second mechanism here. Applying a command returns the command that reverses it,
/// so undoing is `apply()` again with that one -- and undoing *that* returns the original effect,
/// which is what goes on the redo stack. There is one code path, so there is one set of bugs,
/// and anything that can be undone can also be journaled and replayed.
///
/// Compare `llayout::History`, which snapshots: a workspace layout is a few dozen nodes, while a
/// project is scenes full of them, and two hundred snapshots of that is a different proposition.
/// Commands also carry *what the operator did*, which is what the undo menu needs to say.
class DocumentHistory {
public:
    explicit DocumentHistory(Project project, std::size_t limit = 200);

    [[nodiscard]] const Project& project() const { return project_; }

    /// Apply a command and record it. On failure nothing changed and nothing was recorded.
    /// Returns the description, for the undo menu and the status line.
    std::expected<std::string, std::string> run(const Command& command);

    /// Apply a command as part of a continuous gesture, so that a whole drag is one undo step.
    ///
    /// A fader dragged across a panel produces a command per frame. Recorded one per step, a
    /// single drag buries every other edit the operator made under four hundred entries; and
    /// "undo" then means "move the fader back one pixel", which is not what they asked for.
    ///
    /// `gesture` names the thing being dragged -- "bus.gain:<id>" -- and must be specific enough
    /// that two different controls never share it. When this command's gesture matches the one
    /// at the top of the stack, the two collapse: the *newer* effect is kept, and the *older*
    /// inverse, which is the state before the gesture began. One step, one undo, back to where
    /// the drag started.
    ///
    /// `end_gesture()` closes it. Two separate drags of the same fader are two undo steps, and
    /// what separates them is the operator letting go -- not a timeout, which would merge or
    /// split a gesture depending on how fast they moved.
    ///
    /// The journal is not coalesced. Every command is still recorded as it was applied, because
    /// replaying them in order reaches the same project either way, and a journal that dropped
    /// the intermediate steps would be a journal that disagreed with what was fsynced.
    std::expected<std::string, std::string> run(const Command& command,
                                                std::string_view gesture);

    /// Close the current gesture, so the next command starts a new undo step even if it names
    /// the same one. Harmless when no gesture is open.
    void end_gesture() { gesture_.clear(); }
    [[nodiscard]] std::string_view open_gesture() const { return gesture_; }

    [[nodiscard]] bool can_undo() const { return !past_.empty(); }
    [[nodiscard]] bool can_redo() const { return !future_.empty(); }
    /// "Hide Camera", for an Undo menu item that says what it will undo.
    [[nodiscard]] std::string undo_description() const;
    [[nodiscard]] std::string redo_description() const;

    /// Both return the command that was *actually applied* to the document, which for an undo
    /// is the inverse. A listener -- or, once the document lives in another process, a replica of
    /// it -- can then update precisely instead of re-reading everything.
    std::expected<Command, std::string> undo();
    std::expected<Command, std::string> redo();

    [[nodiscard]] std::size_t undo_depth() const { return past_.size(); }
    [[nodiscard]] std::size_t redo_depth() const { return future_.size(); }

    /// Replace the project and forget the history, for loading a different one.
    void reset(Project project);

    /// Record that the document was written as this generation.
    ///
    /// Not `reset`: saving must not throw away the undo stack. The generation is file metadata
    /// rather than document content -- it is not something the operator did and not something
    /// they can undo -- so it is set here without touching the history.
    void note_saved(std::uint64_t generation);

    /// Apply a command without recording it, for replaying a journal into a freshly loaded
    /// project: those commands already happened, and they are not undo steps for this session.
    std::expected<void, std::string> replay(const Command& command);

private:
    struct Step {
        Command undo;
        std::string description;
        /// Which continuous gesture produced this step, empty for an ordinary command. Kept on
        /// the step rather than only in `gesture_` so that an undo of a coalesced step cannot
        /// be merged into by whatever comes next.
        std::string gesture;
    };

    Project project_;
    std::deque<Step> past_;
    std::deque<Step> future_;
    std::size_t limit_;
    /// The gesture currently open, if any. Cleared by `end_gesture()`, and by anything that
    /// makes merging into the top of the stack wrong: an ordinary command, an undo, a redo, a
    /// replay or a reset.
    std::string gesture_;
};

} // namespace ldoc

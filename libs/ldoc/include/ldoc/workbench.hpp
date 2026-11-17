// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/subcommand.hpp"
#include "ldoc/subdocument.hpp"

#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ldoc {

/// An open sub-document and its own undo stack.
///
/// The same design as `DocumentHistory`: applying a command returns the command that reverses
/// it, so undo is `apply()` again rather than a second mechanism with its own bugs.
template <typename Doc, typename Cmd>
class SubHistory {
public:
    SubHistory() = default;
    explicit SubHistory(Doc doc, std::size_t limit = 200)
        : doc_(std::move(doc)), limit_(limit) {}

    [[nodiscard]] const Doc& document() const { return doc_; }

    std::expected<std::string, std::string> run(const Cmd& command) {
        return run(command, std::string_view{});
    }

    /// Apply a command as part of a continuous gesture, so a whole drag is one undo step.
    /// `gesture` names the thing being dragged -- "clip.trim:<id>" -- specifically enough that
    /// two controls never share it.
    std::expected<std::string, std::string> run(const Cmd& command, std::string_view gesture) {
        auto applied = apply(doc_, command);
        if (!applied)
            return std::unexpected(applied.error());

        future_.clear();
        // A command in the same open gesture replaces the newer effect but keeps the *older*
        // inverse, which is the state the drag started from. One drag, one undo step.
        if (!gesture.empty() && !past_.empty() && past_.back().gesture == gesture) {
            past_.back().description = applied->description;
        } else {
            past_.push_back(Step{std::move(applied->inverse), applied->description,
                                 std::string{gesture}});
            if (past_.size() > limit_)
                past_.pop_front();
        }
        gesture_ = std::string{gesture};
        dirty_ = true;
        return applied->description;
    }

    void end_gesture() { gesture_.clear(); }

    [[nodiscard]] bool can_undo() const { return !past_.empty(); }
    [[nodiscard]] bool can_redo() const { return !future_.empty(); }
    [[nodiscard]] std::string undo_description() const {
        return past_.empty() ? std::string{} : past_.back().description;
    }
    [[nodiscard]] std::string redo_description() const {
        return future_.empty() ? std::string{} : future_.back().description;
    }
    [[nodiscard]] std::size_t undo_depth() const { return past_.size(); }
    [[nodiscard]] std::size_t redo_depth() const { return future_.size(); }

    std::expected<Cmd, std::string> undo() {
        if (past_.empty())
            return std::unexpected("there is nothing to undo");
        Step step = std::move(past_.back());
        past_.pop_back();
        auto applied = apply(doc_, step.undo);
        if (!applied) {
            past_.push_back(std::move(step));
            return std::unexpected(applied.error());
        }
        future_.push_back(Step{std::move(applied->inverse), step.description, std::string{}});
        gesture_.clear();
        // An undo is as much a change to the file as the edit it reverses: a document undone
        // back to its saved state still has to be written for the file to match.
        dirty_ = true;
        return step.undo;
    }

    std::expected<Cmd, std::string> redo() {
        if (future_.empty())
            return std::unexpected("there is nothing to redo");
        Step step = std::move(future_.back());
        future_.pop_back();
        auto applied = apply(doc_, step.undo);
        if (!applied) {
            future_.push_back(std::move(step));
            return std::unexpected(applied.error());
        }
        past_.push_back(Step{std::move(applied->inverse), step.description, std::string{}});
        gesture_.clear();
        dirty_ = true;
        return step.undo;
    }

    /// True once anything has been applied that the file on disk does not hold.
    [[nodiscard]] bool dirty() const { return dirty_; }
    void note_saved() { dirty_ = false; }

private:
    struct Step {
        Cmd undo;
        std::string description;
        std::string gesture;
    };

    Doc doc_{};
    std::deque<Step> past_;
    std::deque<Step> future_;
    std::size_t limit_ = 200;
    std::string gesture_;
    bool dirty_ = false;
};

using TimelineHistory = SubHistory<Timeline, TimelineCommand>;
using GraphicHistory = SubHistory<Graphic, GraphicCommand>;

/// The open sub-documents, each with an undo stack of its own.
///
/// This is where the isolation the operator asked for is kept, and it is worth stating the rule
/// plainly because it is the thing they have to be able to trust:
///
/// **Undo acts on the document that has focus, and on nothing else.** With a timeline focused,
/// undo reverses the last edit to *that timeline*, however many edits were made to the show or
/// to another timeline in between. Switching away and back moves no stack and loses no position:
/// each document keeps its own past and future exactly as it was left.
///
/// The types enforce it rather than a convention: a `TimelineCommand` cannot be applied to a
/// `Project` and a project `Command` cannot be applied to a `Timeline`, so a timeline edit
/// cannot reach the project's undo stack even by mistake.
///
/// The one case that genuinely crosses the line is a project command that *destroys* a
/// sub-document -- deleting a timeline. There is no correct way to undo an edit to a document
/// that no longer exists, so closing one drops its stack rather than leaving entries that would
/// fail on the way back. Undoing the deletion itself reopens the document at its saved state,
/// which is honest about what was and was not recoverable.
class Workbench {
public:
    // ---- timelines ---------------------------------------------------------------------------

    /// Open a timeline for editing, or return the one already open. Opening twice is a no-op
    /// rather than a reset: the second caller must not discard the first one's undo stack.
    TimelineHistory& open_timeline(Timeline timeline);
    [[nodiscard]] TimelineHistory* timeline(const Id& id);
    [[nodiscard]] const TimelineHistory* timeline(const Id& id) const;
    /// Close it, dropping its undo stack. Unsaved work is the caller's to deal with first.
    void close_timeline(const Id& id);
    [[nodiscard]] std::vector<Id> open_timelines() const;

    // ---- graphics ----------------------------------------------------------------------------

    GraphicHistory& open_graphic(Graphic graphic);
    [[nodiscard]] GraphicHistory* graphic(const Id& id);
    [[nodiscard]] const GraphicHistory* graphic(const Id& id) const;
    void close_graphic(const Id& id);
    [[nodiscard]] std::vector<Id> open_graphics() const;

    /// Every open document with unsaved changes, so closing a project can say which.
    [[nodiscard]] std::vector<Id> dirty_documents() const;

    void clear();

private:
    // Keyed by id and never reordered, so a reference handed out by `open_*` stays valid while
    // other documents are opened and closed around it.
    std::map<Id, TimelineHistory> timelines_;
    std::map<Id, GraphicHistory> graphics_;
};

} // namespace ldoc

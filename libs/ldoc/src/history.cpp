// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/history.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace ldoc {

DocumentHistory::DocumentHistory(Project project, std::size_t limit)
    : project_(std::move(project)), limit_(std::max<std::size_t>(limit, 1)) {}

std::expected<std::string, std::string> DocumentHistory::run(const Command& command) {
    // An ordinary command ends whatever gesture was open: letting a drag merge with the edit
    // that followed it would put two unrelated things on one undo step.
    gesture_.clear();
    return run(command, std::string_view{});
}

std::expected<std::string, std::string> DocumentHistory::run(const Command& command,
                                                             std::string_view gesture) {
    auto applied = apply(project_, command);
    if (!applied)
        return std::unexpected(applied.error());

    // Collapse into the step already on top when this continues the same gesture. The inverse
    // kept is the older one -- the state before the drag started -- and the description is the
    // newer one, since it describes where the fader has actually ended up.
    const bool continues = !gesture.empty() && gesture_ == gesture && !past_.empty() &&
                           past_.back().gesture == gesture;
    if (continues) {
        past_.back().description = applied->description;
    } else {
        past_.push_back(Step{std::move(applied->inverse), applied->description,
                             std::string{gesture}});
        gesture_ = gesture;
    }
    // A new action makes the redo tail unreachable: the operator has chosen a different branch.
    future_.clear();
    while (past_.size() > limit_)
        past_.pop_front();
    return past_.back().description;
}

std::expected<void, std::string> DocumentHistory::replay(const Command& command) {
    gesture_.clear();
    auto applied = apply(project_, command);
    if (!applied)
        return std::unexpected(applied.error());
    return {};
}

std::string DocumentHistory::undo_description() const {
    return past_.empty() ? std::string{} : past_.back().description;
}

std::string DocumentHistory::redo_description() const {
    return future_.empty() ? std::string{} : future_.front().description;
}

std::expected<Command, std::string> DocumentHistory::undo() {
    if (past_.empty())
        return std::unexpected("there is nothing to undo");

    const Step step = past_.back();
    auto applied = apply(project_, step.undo);
    if (!applied) {
        // The stack and the document have disagreed, which should be impossible: every inverse
        // was built from the state it reverses. Say so rather than silently dropping the step
        // and leaving the stack pointing at a document it no longer describes.
        return std::unexpected(
            std::format("'{}' could not be undone: {}", step.description, applied.error()));
    }
    past_.pop_back();
    // Undoing closes any open gesture: the step it would have merged into is gone.
    gesture_.clear();
    future_.push_front(Step{std::move(applied->inverse), step.description, step.gesture});
    while (future_.size() > limit_)
        future_.pop_back();
    return step.undo;
}

std::expected<Command, std::string> DocumentHistory::redo() {
    if (future_.empty())
        return std::unexpected("there is nothing to redo");

    const Step step = future_.front();
    auto applied = apply(project_, step.undo);
    if (!applied)
        return std::unexpected(
            std::format("'{}' could not be redone: {}", step.description, applied.error()));
    future_.pop_front();
    gesture_.clear();
    past_.push_back(Step{std::move(applied->inverse), step.description, step.gesture});
    while (past_.size() > limit_)
        past_.pop_front();
    return step.undo;
}

void DocumentHistory::note_saved(std::uint64_t generation) {
    project_.generation = generation;
}

void DocumentHistory::reset(Project project) {
    project_ = std::move(project);
    past_.clear();
    future_.clear();
    gesture_.clear();
}

} // namespace ldoc

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "llayout/layout.hpp"

#include <cstddef>
#include <deque>

namespace llayout {

/// Undo for workspace layout (ADR-0015 rule 6: "a bad drag is never destructive").
///
/// Snapshots, not commands. A layout tree is a few dozen nodes, so a snapshot costs less than the
/// bookkeeping an inverse-command scheme would need, and it cannot drift out of step with the
/// thing it describes -- which is the failure mode that makes undo stacks untrustworthy.
///
/// This is a **separate** stack from document history, and that is a deliberate refinement of
/// ADR-0015 rule 6 rather than a reading of it. Adobe does not put workspace changes in the
/// history panel, and the reason is sound: if rearranging a panel and deleting a scene share one
/// stack, Ctrl+Z stops being a thing the operator can press without looking. Mid-show, that is
/// the difference between a safe keystroke and a gamble.
class History {
public:
    explicit History(Layout initial, std::size_t limit = 64);

    [[nodiscard]] const Layout& current() const { return current_; }

    /// Record `next` as a new step, keeping the state it replaces for undo. Call this only after
    /// a mutation actually applied: committing a no-op would spend an undo step on nothing, so
    /// an identical layout is ignored and reported as such.
    bool commit(Layout next);

    [[nodiscard]] bool can_undo() const { return !past_.empty(); }
    [[nodiscard]] bool can_redo() const { return !future_.empty(); }
    bool undo();
    bool redo();

    [[nodiscard]] std::size_t undo_depth() const { return past_.size(); }
    [[nodiscard]] std::size_t redo_depth() const { return future_.size(); }

    /// Replace the layout and forget the history. For loading a file or resetting to the default,
    /// where there is nothing meaningful to go back to.
    void reset(Layout l);

private:
    Layout current_;
    std::deque<Layout> past_;
    std::deque<Layout> future_;
    std::size_t limit_;
};

} // namespace llayout

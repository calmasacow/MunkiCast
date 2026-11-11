// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llayout/history.hpp"

#include <algorithm>
#include <utility>

namespace llayout {

History::History(Layout initial, std::size_t limit)
    : current_(std::move(initial)), limit_(std::max<std::size_t>(limit, 1)) {}

bool History::commit(Layout next) {
    if (next == current_)
        return false;

    past_.push_back(std::move(current_));
    current_ = std::move(next);
    // A new step makes the redo tail unreachable, which is the usual and expected behaviour: the
    // operator has chosen a different branch.
    future_.clear();
    while (past_.size() > limit_)
        past_.pop_front();
    return true;
}

bool History::undo() {
    if (past_.empty())
        return false;
    future_.push_front(std::move(current_));
    current_ = std::move(past_.back());
    past_.pop_back();
    while (future_.size() > limit_)
        future_.pop_back();
    return true;
}

bool History::redo() {
    if (future_.empty())
        return false;
    past_.push_back(std::move(current_));
    current_ = std::move(future_.front());
    future_.pop_front();
    while (past_.size() > limit_)
        past_.pop_front();
    return true;
}

void History::reset(Layout l) {
    current_ = std::move(l);
    past_.clear();
    future_.clear();
}

} // namespace llayout

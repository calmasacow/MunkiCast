// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <functional>
#include <lcomp/preview.hpp>
#include <memory>
#include <thread>

// ponytail: one process per retained source; pool by decoding policy if cold startup is next.
class SourceWorker {
  public:
    using Callback = std::function<void(std::shared_ptr<const lcomp::PreviewFrame>)>;
    SourceWorker(const ldoc::Source& source, Callback callback,
                 std::function<void(std::span<const float>)> audio = {},
                 bool initial_active = true);
    ~SourceWorker();

    void set_active(bool active) { active_.store(active); }

    void restart() { restart_.fetch_add(1); }

    void stop() { thread_.request_stop(); }

  private:
    std::atomic<bool> active_{true};
    bool priority_ = false;
    std::atomic<std::int64_t> restart_{0};
    std::jthread thread_;
};

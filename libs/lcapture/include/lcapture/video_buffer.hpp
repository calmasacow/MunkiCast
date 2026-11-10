// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcapture/video.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace libertas::capture {

/// Bounded input jitter buffer. Caller supplies locking; sampling never waits or copies pixels.
/// Monotonic driver timestamps supply frame intervals, independently of USB delivery jitter.
/// Missing/invalid timestamps fall back to sequence and the negotiated period.
class VideoBuffer {
  public:
    using Clock = std::chrono::steady_clock;
    struct Sample {
        std::shared_ptr<const VideoFrame> frame;
        std::uint64_t sequence = 0;
        Clock::time_point time{};
    };

    explicit VideoBuffer(std::chrono::nanoseconds period) : period_(period) {
        if (period <= period.zero() || period > std::chrono::seconds(10))
            throw std::invalid_argument("Invalid buffered video period");
    }
    [[nodiscard]] auto delay() const { return period_ * 2; }

    void clear() {
        queue_.clear();
        current_ = {};
        captured_at_ = {};
        sampled_at_ = {};
        anchored_ = false;
    }

    void push(std::shared_ptr<const VideoFrame> frame, std::uint64_t sequence,
              Clock::time_point arrival) {
        if (!frame) { clear(); return; }
        const auto step = frame->sequence - driver_;
        if (anchored_ && step == 0) return;
        auto captured = frame->captured_at != Clock::time_point{} &&
            frame->captured_at <= arrival && arrival - frame->captured_at <= std::chrono::seconds(2)
                ? frame->captured_at : Clock::time_point{};
        if (anchored_ && step <= 256 && captured <= captured_at_) captured = {};
        // Reset after a driver restart, large discontinuity or stalled/reclocked input.
        if (!anchored_ || step > 256) {
            clear();
            time_ = arrival;
        } else {
            // A nominal "60 fps" device can actually run at 61.3 Hz. Advancing by the
            // advertised period would repeatedly flush/refill this queue as its clocks drift.
            // Use driver intervals while retaining the initial arrival phase and fixed delay.
            time_ += captured_at_ != Clock::time_point{} && captured > captured_at_
                ? captured - captured_at_ : period_ * step;
            if (time_ > arrival + delay() || arrival - time_ > period_ * 6) {
                clear();
                time_ = arrival;
            }
        }
        anchored_ = true;
        captured_at_ = captured;
        driver_ = frame->sequence;
        queue_.push_back({std::move(frame), sequence, time_});
        // ponytail: two-frame delay, four queued leases; longer stalls drop old input.
        while (queue_.size() > 4) queue_.pop_front();
    }

    [[nodiscard]] bool ready(Clock::time_point at) const {
        for (const auto& entry : queue_)
            if (entry.time <= at - delay() && entry.sequence != current_.sequence) return true;
        return false;
    }

    Sample sample(Clock::time_point at) {
        if (current_.frame && at <= sampled_at_) return current_;
        const auto elapsed = at - sampled_at_;
        sampled_at_ = at;
        while (current_.frame && !queue_.empty() && queue_.front().sequence <= current_.sequence)
            queue_.pop_front();
        // Integer-rate sampling must keep its phase: 120 -> 60 should choose every
        // second input, rather than alternate one/three when driver timestamps jitter.
        // Matching rates are the one-step case. Real drift still crosses this bounded
        // quarter-input-period band and resamples; genuine gaps/stalls use timestamps.
        // ponytail: strides 1..4 fit the lease bound; fractional ratios use timestamps.
        const auto target = at - delay();
        const auto tolerance = period_ / 4;
        const auto steps = (elapsed + period_ / 2) / period_;
        if (current_.frame && steps >= 1 && steps <= 4 &&
            elapsed >= period_ * steps - tolerance && elapsed <= period_ * steps + tolerance) {
            const auto driver = current_.frame->sequence + static_cast<std::uint32_t>(steps);
            const auto next = std::ranges::find_if(queue_, [&](const auto& entry) {
                return entry.frame->sequence == driver;
            });
            if (next != queue_.end() && next->time >= target - period_ - tolerance &&
                next->time <= target + tolerance) {
                current_ = std::move(*next);
                queue_.erase(queue_.begin(), std::next(next));
                return current_;
            }
        }
        while (!queue_.empty() && queue_.front().time <= at - delay()) {
            current_ = std::move(queue_.front());
            queue_.pop_front();
        }
        // Show the first frame during initial prefill; subsequent sampling follows the clock.
        if (!current_.frame && !queue_.empty())
            current_ = queue_.front();
        return current_;
    }

  private:
    std::chrono::nanoseconds period_;
    std::deque<Sample> queue_;
    Sample current_;
    Clock::time_point time_{};
    Clock::time_point captured_at_{};
    Clock::time_point sampled_at_{};
    std::uint32_t driver_ = 0;
    bool anchored_ = false;
};
} // namespace libertas::capture

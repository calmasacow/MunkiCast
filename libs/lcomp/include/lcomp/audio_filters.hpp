// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <expected>
#include <functional>
#include <ldoc/node.hpp>
#include <memory>
#include <span>
#include <string>

namespace lcomp {
// One processing instance per signal path. Fixed stereo 48 kHz, 480-frame blocks.
class AudioFilters {
  public:
    AudioFilters();
    ~AudioFilters();
    AudioFilters(const AudioFilters&) = delete;
    AudioFilters& operator=(const AudioFilters&) = delete;
    std::expected<void, std::string>
    process(std::span<float> samples, std::span<const ldoc::Effect> effects,
            const std::function<std::span<const float>(const ldoc::Id&)>& sidechain = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lcomp

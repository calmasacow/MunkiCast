// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <ldoc/document.hpp>
#include <lrhi/composite.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// Renderer-owned Wayland windows. Only controls/status cross the UI socket; display pixels
/// stay on the compositing GPU. A slow/disconnected display must never stall the encoder.
class Projectors {
  public:
    Projectors();
    ~Projectors();
    std::string control(const std::string& json, const ldoc::Project& project);
    std::string state() const;
    std::optional<std::chrono::nanoseconds> pump(const ldoc::Project& project);
    bool wants(const ldoc::Id& canvas) const;
    bool dirty() const;
    std::vector<libertas::rhi::PresentationTarget> acquire(const ldoc::Id& canvas,
                                                           libertas::rhi::VkCtx* device);
    void present(const ldoc::Id& canvas, bool success);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

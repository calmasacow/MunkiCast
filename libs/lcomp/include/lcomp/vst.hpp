// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <expected>
#include <ldoc/node.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lcomp {
struct VstParameter {
    std::string name, unit;
    float value = 0;
};

struct VstInfo {
    std::string error = "Loading plugin";
    int programs = 0, latency = 0;
    std::vector<VstParameter> parameters;
    bool snapshot_ready = false;
    std::string snapshot{};
};

class VstWorker {
  public:
    explicit VstWorker(const ldoc::Effect& effect);
    ~VstWorker();
    VstWorker(const VstWorker&) = delete;
    VstWorker& operator=(const VstWorker&) = delete;
    bool process(std::span<float> samples, const ldoc::ParamMap& parameters);
    VstInfo info() const;
    bool request_state(const ldoc::ParamMap& parameters);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

VstInfo vst_probe(const ldoc::Effect& effect);
std::expected<std::string, std::string> vst_snapshot(const ldoc::Effect& effect);
} // namespace lcomp

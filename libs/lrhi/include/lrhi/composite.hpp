// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcomp/composite.hpp>
#include <lrhi/vulkan.hpp>

namespace libertas::rhi {

/// Executes the shared plan on one device. The device and all borrowed frames must outlive it.
class GpuCompositor {
  public:
    static std::unique_ptr<GpuCompositor> create(VkCtx& device, std::string& error);
    ~GpuCompositor();
    [[nodiscard]] std::optional<lcomp::Picture> render(const lcomp::DrawPlan& plan,
                                                       std::uint32_t max_edge, std::string& error,
                                                       lcomp::Picture* full_output = nullptr);

  private:
    GpuCompositor();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace libertas::rhi

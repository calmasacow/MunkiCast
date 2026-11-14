// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcomp/composite.hpp>
#include <lrhi/vulkan.hpp>
#include <span>

namespace munkicast::rhi {

/// A compositor-owned display slot. A foreign slot has been released by Wayland before
/// being reused; the GPU must acquire ownership before overwriting it. Never write a slot
/// still on screen. Display backpressure drops an update instead of blocking the encoder.
struct PresentationTarget {
    const capture::DmabufImage* image = nullptr;
    bool foreign = false;
};

/// Executes the shared plan on one device. With `nv12_output`, `full_output` comes back as a
/// GPU-resident NV12 frame (`Picture::nv12`) when possible, and as RGBA pixels otherwise.
/// With `wide_output`, those RGBA pixels are `Rgba16` -- 16 bits a component, which is what the
/// compositor already holds internally, so a 10-bit encode is fed 10-bit data rather than
/// upconverted 8-bit. The preview-sized picture this returns stays `Rgba8` either way: it goes to
/// a UI on an SDR desktop, and sending it wide would double the IPC for pixels nothing can show.
/// `nv12_output` wins when both are asked, since NV12 is 8-bit by construction.
/// The device and all borrowed frames must outlive it.
class GpuCompositor {
  public:
    static std::unique_ptr<GpuCompositor> create(VkCtx& device, std::string& error);
    ~GpuCompositor();
    [[nodiscard]] std::optional<lcomp::Picture>
    render(const lcomp::DrawPlan& plan, std::uint32_t max_edge, std::string& error,
           lcomp::Picture* full_output = nullptr,
           std::span<const PresentationTarget> displays = {}, bool nv12_output = false,
           bool wide_output = false);

  private:
    GpuCompositor();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace munkicast::rhi

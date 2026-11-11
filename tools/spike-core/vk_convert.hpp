// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lrhi/vulkan.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace munkicast::spike {

// The convert pass builds on lrhi's device (ADR-0019 measured it there, as vk_import).
using munkicast::rhi::VkCtx;
using munkicast::rhi::VkExportedImage;

/// How to interpret and encode colour. ADR-0014: these are *configuration*, never detection -
/// the capture device will happily claim a range it does not deliver, so the only honest source
/// of truth is what the user tells us.
struct ColorCfg {
    bool full_range = false;  ///< limited (16-235) by default, which is what broadcast expects
    bool bt601 = false;       ///< BT.709 by default; 601 is for SD-era sources
};

/// The host-side description of one converted frame. `data` is tightly packed NV12:
/// `height` rows of `y_stride`, then `height/2` rows of `uv_stride` starting at `uv_offset`.
struct Nv12Frame {
    std::vector<std::uint8_t> data;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t y_stride = 0;
    std::uint32_t uv_stride = 0;
    std::uint32_t uv_offset = 0;
};

/// Where the time inside one convert went. ADR-0019 recorded a single 61 ms convert against a
/// p99 of 1.386 ms and could not say which part stalled, which is the whole reason this exists:
/// a stall that cannot be attributed cannot be fixed.
struct ConvertTiming {
    double record_ms = 0.0;  ///< building the command buffer (descriptors, barriers, dispatch)
    double submit_ms = 0.0;  ///< vkQueueSubmit alone, which is where a driver stall shows up
    double wait_ms = 0.0;    ///< blocked on the fence, i.e. waiting for the GPU
    double copy_ms = 0.0;    ///< memcpy out of the mapped staging buffer
};

/// Converts an imported/exported BGRA image to NV12 on the GPU and reads back the result.
///
/// This replaces "read back BGRA, then sws_scale on the CPU". It moves 1.5 bytes/pixel instead of
/// 4 and spends no CPU on the conversion at all (ADR-0019).
///
/// `written_by_foreign` must be true when a compositor produced the contents, so the barrier
/// acquires queue-family ownership - the same rule as the readback path.
bool vk_convert_nv12(VkCtx& ctx, const VkExportedImage& img, bool written_by_foreign,
                     const ColorCfg& color, Nv12Frame& out, std::string& error,
                     ConvertTiming* timing = nullptr);

/// A short description of the convert pipeline's state, for logging.
std::string vk_convert_note(VkCtx& ctx);

} // namespace munkicast::spike

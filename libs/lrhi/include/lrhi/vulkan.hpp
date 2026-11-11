// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcapture/dmabuf.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace munkicast::rhi {

using munkicast::capture::CapturedFrame;
using munkicast::capture::DmabufImage;

struct VkCtx;

void vk_destroy(VkCtx* ctx);

/// Deleter declared here so unique_ptr works with the incomplete VkCtx type.
struct VkCtxDeleter {
    void operator()(VkCtx* p) const { vk_destroy(p); }
};
using VkCtxPtr = std::unique_ptr<VkCtx, VkCtxDeleter>;

/// Creates an instance + device with the external-memory/dmabuf extensions.
/// `prefer_drm_node` selects the device whose DRM primary/render node matches, when non-empty;
/// otherwise the first discrete GPU wins. Device choice is never by enumeration index (ADR-0008).
VkCtxPtr vk_init(const std::string& prefer_drm_node, std::string& error);

/// Reports what the device actually supports, so PipeWire negotiation can advertise the truth
/// rather than a guess. Returns modifiers valid for sampling/transfer of `drm_format`.
std::vector<std::uint64_t> vk_supported_modifiers(VkCtx& ctx, std::uint32_t drm_format);

/// A short description of the selected device, for logging.
std::string vk_device_name(VkCtx& ctx);
/// Matches CUDA's cuDeviceGetUuid, so an encoder can open the same physical GPU.
std::array<std::uint8_t, 16> vk_device_uuid(VkCtx& ctx);
bool vk_matches_drm_device(VkCtx& ctx, std::uint64_t device);

/// Imports one dmabuf frame as a VkImage and copies it back to host memory, proving the import
/// produced real pixels. `out` receives width*height*4 bytes in the image's byte order.
/// A VkImage whose memory is exported as a dmabuf, for a compositor to render into.
///
/// This is the inverse of the import path and it is the better arrangement: when *we* allocate,
/// we choose the modifier from the intersection up front, so the accept-then-fail trap of
/// ADR-0010 cannot occur - there is nothing to accept.
///
/// The description itself now lives in `libs/lcapture` (ADR-0041), because the capture backend
/// moved there and a library cannot depend on a spike. This alias keeps the spike's own name: the
/// allocator is still here, and renaming it across fourteen call sites would put unrelated churn
/// in the commit that moved the backend.
using VkExportedImage = munkicast::capture::DmabufImage;

/// Creates an image the compositor can render into, picking from `allowed` (most preferred first).
///
/// `host_memory` places the allocation in system RAM rather than VRAM. That is required for a
/// buffer a *different* GPU must also read: a dmabuf backed by one card's VRAM cannot generally be
/// addressed by another, and the failure surfaces far from the cause - on this machine as
/// `intel: the execbuf ioctl keeps returning ENOMEM` at submit time, after the import succeeded.
bool vk_create_exportable(VkCtx& ctx, std::uint32_t width, std::uint32_t height,
                          std::uint32_t drm_format, const std::vector<std::uint64_t>& allowed,
                          bool host_memory, VkExportedImage& out, std::string& error);

/// Copies an exported image back to host memory as BGRA. `written_by_foreign` must be true when a
/// compositor (not us) produced the contents, so the barrier acquires queue ownership.
bool vk_readback_exported(VkCtx& ctx, const VkExportedImage& img, bool written_by_foreign,
                          std::vector<std::uint8_t>& out, std::string& error);

void vk_destroy_exportable(VkCtx& ctx, VkExportedImage& img);

/// Describes the memory the readback staged through. Reported rather than assumed: picking an
/// uncached host-visible type instead of the cached one cost 46 ms a frame and looked like a PCIe
/// limit (ADR-0018), so the choice is printed where a human will see it.
std::string vk_readback_note(VkCtx& ctx);

/// Blits `src` into `dst`, scaling if their sizes differ, and releases `dst` to the foreign queue
/// family so another device may acquire it.
///
/// This is ADR-0007's cross-GPU transfer step. Scaling happens here, on the compositing GPU,
/// deliberately: moving a 1440p buffer across the bus to downscale it on the far side wastes the
/// bandwidth that is the scarce resource in a dual-encode setup.
bool vk_blit_exported(VkCtx& ctx, const VkExportedImage& src, bool src_foreign,
                      const VkExportedImage& dst, std::string& error);

/// Describes an exported image as a CapturedFrame, so the import path can consume it.
CapturedFrame vk_exported_as_frame(const VkExportedImage& img, std::uint32_t drm_format);

bool vk_import_and_readback(VkCtx& ctx, const CapturedFrame& frame, std::vector<std::uint8_t>& out,
                            std::string& error);

} // namespace munkicast::rhi

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Vulkan context, shared between the spike-core translation units that need its guts.
//
// This is an *internal* header: it is not in vk_import.hpp because callers outside spike-core
// are deliberately given only an opaque VkCtx. Everything here is implementation detail that
// vulkan.cpp and anything building a pipeline on this context both have to agree on.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace munkicast::rhi {

/// Owned by whoever built a pipeline on top of this context; opaque here.
struct ConvertState;
struct VkCtx;

/// Staging resources for host readback, kept alive across frames. See `readback_image` for why
/// per-frame allocation and an uncached memory type were both wrong.
struct ReadbackStaging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    std::uint32_t type_index = UINT32_MAX;
    bool cached = false;
    bool coherent = false;
};

struct VkCtx {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::string device_name;
    std::array<std::uint8_t, 16> uuid{}; ///< VkPhysicalDeviceIDProperties::deviceUUID
    bool have_foreign_queue = false;
    bool have_memory_budget = false;

    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_modifier_properties = nullptr;
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;

    ReadbackStaging readback;

    /// Compute pipeline + buffers for BGRA->NV12, built on first use. Owned by whoever built it
    /// -- the encode spike, today -- and torn down through `on_destroy` below.
    ConvertState* convert = nullptr;
    /// Called by `vk_destroy` before the device goes, so a pipeline built on this context can be
    /// released without this library having to know what it is.
    void (*on_destroy)(VkCtx&) = nullptr;
};

} // namespace munkicast::rhi

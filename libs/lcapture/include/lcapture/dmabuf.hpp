// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace munkicast::capture {

/// A dmabuf-backed image, described by the fields a Wayland `zwp_linux_buffer_params_v1` needs.
///
/// Deliberately a plain description and not a handle: `image` and `memory` are `void*` because
/// nothing in this library creates or destroys them. The capture backend's job is to wrap an
/// already-allocated buffer as a `wl_buffer` and ask the compositor to render into it, so the
/// allocator -- today a spike's Vulkan context, tomorrow `lrhi` -- stays on the other side of this
/// boundary and can change without this header changing.
///
/// Why the client allocates at all: see ADR-0011. When *we* allocate, the modifier is chosen from
/// the intersection of what the compositor accepts and what the device can use, so the
/// accept-then-fail trap of ADR-0010 cannot occur -- there is nothing to accept.
struct DmabufImage {
    void* image = nullptr;  ///< VkImage, or whatever the allocator's image handle is
    void* memory = nullptr; ///< VkDeviceMemory, likewise
    int dmabuf_fd = -1;     ///< owned by the caller; close() or hand to a wl_buffer
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t modifier = 0; ///< the modifier the driver actually chose
    std::uint32_t n_planes = 0;
    std::uint32_t offsets[4] = {};
    std::uint32_t strides[4] = {};
    std::uint32_t drm_format = 0;
};

/// One plane of a dmabuf frame. Fds here are borrowed unless the owner says otherwise.
struct DmabufPlane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    std::int64_t size = 0;
};

/// A frame as a capture backend delivered it, before anything has imported it.
///
/// Shared by both backends on purpose: the PipeWire/portal path and the compositor path describe
/// a frame the same way, and the import side should not be able to tell which one produced it.
struct CapturedFrame {
    std::uint64_t sequence = 0;
    std::int64_t pts = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t drm_format = 0; ///< DRM_FORMAT_* fourcc
    std::uint64_t modifier = 0;   ///< DRM format modifier, or kModifierInvalid
    bool is_dmabuf = false;       ///< false => shm/MemPtr fallback path
    std::vector<DmabufPlane> planes;
};

/// "No modifier was negotiated", as DRM spells it.
inline constexpr std::uint64_t kModifierInvalid = 0x00ffffffffffffffULL;

/// A DRM fourcc as its four characters ("AR24"), or "(none)" for zero.
///
/// Unprintable bytes become '?' rather than going to a terminal raw: this is used in error
/// messages, and a format code that is wrong is exactly when the bytes are not characters.
std::string fourcc_str(std::uint32_t fourcc);

} // namespace munkicast::capture

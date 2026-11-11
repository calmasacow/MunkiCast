// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vulkan dmabuf import for captured frames.
//
// The interesting parts, all of which are easy to get subtly wrong:
//   * the image must be created with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and an *explicit*
//     modifier + per-plane layout taken from what PipeWire handed us;
//   * vkGetMemoryFdPropertiesKHR, not the image's memoryTypeBits alone, decides which memory types
//     may back an imported fd;
//   * VkImportMemoryFdInfoKHR takes ownership of the fd, so PipeWire's borrowed fd must be dup'd;
//   * the first barrier is a queue-family *acquire* from VK_QUEUE_FAMILY_FOREIGN_EXT, which is what
//     preserves the external writer's contents.

#include "vk_ctx_internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <drm_fourcc.h>
#include <format>
#include <poll.h>
#include <lcore/log.hpp>
#include <lrhi/vulkan.hpp>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace munkicast::rhi {

// VkCtx and ReadbackStaging live in vk_ctx_internal.hpp so vk_convert.cpp can see them too.

namespace {

void destroy_staging(VkCtx& ctx, ReadbackStaging& st) {
    if (st.mapped != nullptr)
        vkUnmapMemory(ctx.device, st.memory);
    if (st.fence != VK_NULL_HANDLE)
        vkDestroyFence(ctx.device, st.fence, nullptr);
    if (st.cmd != VK_NULL_HANDLE)
        vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &st.cmd);
    if (st.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(ctx.device, st.buffer, nullptr);
    if (st.memory != VK_NULL_HANDLE)
        vkFreeMemory(ctx.device, st.memory, nullptr);
    st = ReadbackStaging{};
}


/// DRM fourcc -> Vulkan format. DRM names channels in little-endian word order, Vulkan in byte
/// order, so DRM_FORMAT_ARGB8888 ("AR24") is B8G8R8A8 in Vulkan terms.
VkFormat drm_to_vk(std::uint32_t drm) {
    switch (drm) {
    case DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

bool device_has_ext(VkPhysicalDevice d, const char* name) {
    std::uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, exts.data());
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

} // namespace

VkCtxPtr vk_init(const std::string& prefer_drm_node, std::string& error) {
    VkCtxPtr ctx(new VkCtx());

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "munkicast-capture-spike";
    app.apiVersion = VK_API_VERSION_1_3;

    static constexpr std::array<const char*, 2> kInstExts{
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(kInstExts.size());
    ici.ppEnabledExtensionNames = kInstExts.data();

    if (VkResult r = vkCreateInstance(&ici, nullptr, &ctx->instance); r != VK_SUCCESS) {
        error = std::format("vkCreateInstance failed ({})", static_cast<int>(r));
        return nullptr;
    }

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(ctx->instance, &count, devices.data());
    if (count == 0) {
        error = "no Vulkan devices";
        return nullptr;
    }

    // Required for importing dmabufs with an explicit modifier.
    static constexpr std::array<const char*, 5> kNeeded{
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devices) {
        bool ok = true;
        for (const char* e : kNeeded)
            if (std::strcmp(e, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) != 0 &&
                !device_has_ext(d, e))
                ok = false;
        if (!ok)
            continue;

        VkPhysicalDeviceDrmPropertiesEXT drm{};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drm;
        vkGetPhysicalDeviceProperties2(d, &p2);

        // Match by DRM node when asked, never by enumeration index (ADR-0008).
        if (!prefer_drm_node.empty()) {
            const std::string want = std::format("{}:{}", drm.renderMajor, drm.renderMinor);
            const std::string want_primary = std::format("{}:{}", drm.primaryMajor,
                                                          drm.primaryMinor);
            if (prefer_drm_node != want && prefer_drm_node != want_primary)
                continue;
            chosen = d;
            break;
        }
        if (chosen == VK_NULL_HANDLE ||
            p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            chosen = d;
    }

    if (chosen == VK_NULL_HANDLE) {
        error = "no device with the required external-memory/dmabuf extensions";
        return nullptr;
    }
    ctx->phys = chosen;
    ctx->have_foreign_queue =
        device_has_ext(chosen, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

    VkPhysicalDeviceIDProperties ids{};
    ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 named{};
    named.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    named.pNext = &ids;
    vkGetPhysicalDeviceProperties2(chosen, &named);
    ctx->device_name = named.properties.deviceName;
    std::copy(std::begin(ids.deviceUUID), std::end(ids.deviceUUID), ctx->uuid.begin());

    std::uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qn, qfp.data());
    bool found_q = false;
    for (std::uint32_t i = 0; i < qn; ++i) {
        constexpr auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((qfp[i].queueFlags & required) == required) {
            ctx->queue_family = i;
            found_q = true;
            break;
        }
    }
    if (!found_q) {
        error = "no graphics queue family";
        return nullptr;
    }

    std::vector<const char*> dev_exts{
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
    if (ctx->have_foreign_queue)
        dev_exts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    ctx->have_memory_budget = device_has_ext(ctx->phys, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (ctx->have_memory_budget)
        dev_exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = ctx->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(chosen, &features);
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = features.shaderInt64;
    enabled.shaderStorageImageExtendedFormats = features.shaderStorageImageExtendedFormats;
    dci.pEnabledFeatures = &enabled;

    if (VkResult r = vkCreateDevice(chosen, &dci, nullptr, &ctx->device); r != VK_SUCCESS) {
        error = std::format("vkCreateDevice failed ({})", static_cast<int>(r));
        return nullptr;
    }
    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);

    ctx->get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdPropertiesKHR"));
    ctx->get_image_modifier_properties =
        reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            vkGetDeviceProcAddr(ctx->device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    ctx->get_memory_fd =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdKHR"));
    if (ctx->get_memory_fd_properties == nullptr) {
        error = "vkGetMemoryFdPropertiesKHR unavailable";
        return nullptr;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx->queue_family;
    if (vkCreateCommandPool(ctx->device, &pci, nullptr, &ctx->pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return nullptr;
    }
    return ctx;
}

bool vk_matches_drm_device(VkCtx& ctx, std::uint64_t device) {
    VkPhysicalDeviceDrmPropertiesEXT drm{};
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &drm;
    vkGetPhysicalDeviceProperties2(ctx.phys, &props);
    return (drm.hasRender && device == static_cast<std::uint64_t>(
                                           makedev(static_cast<unsigned>(drm.renderMajor),
                                                   static_cast<unsigned>(drm.renderMinor)))) ||
           (drm.hasPrimary &&
            device == static_cast<std::uint64_t>(makedev(static_cast<unsigned>(drm.primaryMajor),
                                                         static_cast<unsigned>(drm.primaryMinor))));
}

void vk_destroy(VkCtx* ctx) {
    // Also frees the struct: this is the unique_ptr deleter target.
    if (ctx == nullptr)
        return;
    if (ctx->device != VK_NULL_HANDLE) {
        // Whoever built a pipeline on this context tears it down first. The NV12 convert pass
        // lives in the encode spike (ADR-0019) and is not this library's business; inverting the
        // call rather than linking to it is what lets `lrhi` be a dependency of that spike
        // instead of the other way round.
        if (ctx->on_destroy)
            ctx->on_destroy(*ctx);
        destroy_staging(*ctx, ctx->readback);
    }
    if (ctx->pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(ctx->device, ctx->pool, nullptr);
    if (ctx->device != VK_NULL_HANDLE)
        vkDestroyDevice(ctx->device, nullptr);
    if (ctx->instance != VK_NULL_HANDLE)
        vkDestroyInstance(ctx->instance, nullptr);
    delete ctx;
}

std::string vk_device_name(VkCtx& ctx) { return ctx.device_name; }
std::array<std::uint8_t, 16> vk_device_uuid(VkCtx& ctx) { return ctx.uuid; }

std::vector<std::uint64_t> vk_supported_modifiers(VkCtx& ctx, std::uint32_t drm_format) {
    std::vector<std::uint64_t> out;
    const VkFormat vkf = drm_to_vk(drm_format);
    if (vkf == VK_FORMAT_UNDEFINED)
        return out;

    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp{};
    fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp.pNext = &list;

    vkGetPhysicalDeviceFormatProperties2(ctx.phys, vkf, &fp);
    if (list.drmFormatModifierCount == 0)
        return out;

    std::vector<VkDrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = props.data();
    vkGetPhysicalDeviceFormatProperties2(ctx.phys, vkf, &fp);

    for (const auto& p : props) {
        // We need to sample from, or copy out of, the imported image.
        constexpr VkFormatFeatureFlags kWant =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((p.drmFormatModifierTilingFeatures & kWant) == kWant)
            out.push_back(p.drmFormatModifier);
    }
    return out;
}

namespace {

/// Copies an image to host memory as tightly-packed BGRA. Shared by the import and export paths so
/// the two cannot drift: a readback that differs between them would make their results
/// incomparable, which is the whole point of having both.
///
/// `written_by_foreign` selects a queue-family ownership *acquire* as the first barrier. That
/// acquire is what makes an external writer's contents visible; omitting it is undefined behaviour
/// that happens to work on some drivers, which is the worst kind of bug to ship.
///
/// Two things here were wrong in the first version and cost 46 ms a frame (ADR-0018):
///
/// 1. The memory type was chosen as the *first* `HOST_VISIBLE | HOST_COHERENT` match. On this
///    NVIDIA device that is type 2, which is host-visible and uncoherent-write-combined but
///    **not** `HOST_CACHED`; type 3 is the cached one. A `memcpy` that *reads* from
///    write-combined memory bypasses the cache and runs at a few hundred MB/s, so a 14 MiB frame
///    took tens of milliseconds. WC memory is fast to write and ruinous to read, and a readback
///    destination is read. `HOST_CACHED` is therefore a requirement, not a preference.
/// 2. Buffer, allocation, command buffer and fence were created and destroyed per call. A 14 MiB
///    `vkAllocateMemory` plus the first-touch page faults on every frame is pure overhead.
///
/// So the staging resources live in the context and the mapping is permanent.
bool readback_image(VkCtx& ctx, VkImage image, std::uint32_t width, std::uint32_t height,
                    bool written_by_foreign, std::vector<std::uint8_t>& out, std::string& error) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    ReadbackStaging& st = ctx.readback;

    if (st.capacity < bytes) {
        destroy_staging(ctx, st);

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(ctx.device, &bci, nullptr, &st.buffer) != VK_SUCCESS) {
            error = "vkCreateBuffer (staging) failed";
            return false;
        }

        VkMemoryRequirements bmr{};
        vkGetBufferMemoryRequirements(ctx.device, st.buffer, &bmr);

        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(ctx.phys, &mp);

        // Preference order. Cached first, because reading uncached host memory is the trap above.
        // Device-local is excluded from the first two passes: on a discrete card a type that is
        // both device-local and host-visible is resizable-BAR VRAM, and reading that back over
        // PCIe is the same mistake in a different costume (cf. ADR-0012, which had to avoid it
        // for the opposite reason).
        struct Pass {
            VkMemoryPropertyFlags want;
            VkMemoryPropertyFlags reject;
        };
        const Pass passes[] = {
            {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
            {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
            {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0},
        };

        st.type_index = UINT32_MAX;
        for (const Pass& p : passes) {
            for (std::uint32_t i = 0; i < mp.memoryTypeCount && st.type_index == UINT32_MAX; ++i) {
                const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
                if ((bmr.memoryTypeBits & (1u << i)) == 0u)
                    continue;
                if ((f & p.want) != p.want || (f & p.reject) != 0u)
                    continue;
                st.type_index = i;
                st.cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0u;
                st.coherent = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
            }
            if (st.type_index != UINT32_MAX)
                break;
        }
        if (st.type_index == UINT32_MAX) {
            error = "no host-visible memory type for readback";
            destroy_staging(ctx, st);
            return false;
        }

        VkMemoryAllocateInfo bai{};
        bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize = bmr.size;
        bai.memoryTypeIndex = st.type_index;
        if (vkAllocateMemory(ctx.device, &bai, nullptr, &st.memory) != VK_SUCCESS) {
            error = "vkAllocateMemory (staging) failed";
            destroy_staging(ctx, st);
            return false;
        }
        vkBindBufferMemory(ctx.device, st.buffer, st.memory, 0);

        // Mapped once and left mapped. vkMapMemory is not free and the pointer stays valid.
        if (vkMapMemory(ctx.device, st.memory, 0, VK_WHOLE_SIZE, 0, &st.mapped) != VK_SUCCESS) {
            error = "vkMapMemory failed";
            destroy_staging(ctx, st);
            return false;
        }

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = ctx.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(ctx.device, &cbai, &st.cmd) != VK_SUCCESS) {
            error = "vkAllocateCommandBuffers failed";
            destroy_staging(ctx, st);
            return false;
        }

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(ctx.device, &fci, nullptr, &st.fence) != VK_SUCCESS) {
            error = "vkCreateFence failed";
            destroy_staging(ctx, st);
            return false;
        }
        st.capacity = bmr.size;
    }

    vkResetCommandBuffer(st.cmd, 0);
    vkResetFences(ctx.device, 1, &st.fence);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(st.cmd, &cbbi);

    VkImageMemoryBarrier acquire{};
    acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acquire.oldLayout = written_by_foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (written_by_foreign) {
        acquire.srcQueueFamilyIndex =
            ctx.have_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = ctx.queue_family;
    } else {
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    acquire.image = image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(st.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &acquire);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = width;
    copy.bufferImageHeight = height;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(st.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buffer, 1, &copy);

    if (written_by_foreign) {
        VkImageMemoryBarrier release = acquire;
        release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = ctx.queue_family;
        release.dstQueueFamilyIndex = acquire.srcQueueFamilyIndex;
        vkCmdPipelineBarrier(st.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &release);
    }

    vkEndCommandBuffer(st.cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &st.cmd;

    if (vkQueueSubmit(ctx.queue, 1, &si, st.fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed";
        return false;
    }
    if (vkWaitForFences(ctx.device, 1, &st.fence, VK_TRUE, 2'000'000'000ULL) != VK_SUCCESS) {
        error = "fence wait timed out";
        return false;
    }

    // A cached, non-coherent mapping needs the host caches invalidated before the GPU's writes are
    // visible. Skipping this is another "works on the driver you tested" bug.
    if (!st.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = st.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(ctx.device, 1, &range);
    }

    out.resize(bytes);
    std::memcpy(out.data(), st.mapped, bytes);
    return true;
}

} // namespace

bool vk_import_and_readback(VkCtx& ctx, const CapturedFrame& frame, std::vector<std::uint8_t>& out,
                            std::string& error) {
    if (!frame.is_dmabuf || frame.planes.empty() || frame.planes.front().fd < 0) {
        error = "frame is not a dmabuf";
        return false;
    }
    const VkFormat vkf = drm_to_vk(frame.drm_format);
    if (vkf == VK_FORMAT_UNDEFINED) {
        error = std::format("unmapped DRM format {}", capture::fourcc_str(frame.drm_format));
        return false;
    }

    // ---- create the image with an explicit modifier + the plane layouts PipeWire gave us
    std::vector<VkSubresourceLayout> layouts(frame.planes.size());
    for (std::size_t i = 0; i < frame.planes.size(); ++i) {
        layouts[i] = {};
        layouts[i].offset = frame.planes[i].offset;
        layouts[i].rowPitch = frame.planes[i].stride;
        // size/arrayPitch/depthPitch must be 0 here; the implementation derives them.
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info{};
    mod_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    mod_info.drmFormatModifier = frame.modifier;
    mod_info.drmFormatModifierPlaneCount = static_cast<std::uint32_t>(layouts.size());
    mod_info.pPlaneLayouts = layouts.data();

    VkExternalMemoryImageCreateInfo ext_img{};
    ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    ext_img.pNext = &mod_info;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext_img;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkf;
    ici.extent = {frame.width, frame.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (VkResult r = vkCreateImage(ctx.device, &ici, nullptr, &image); r != VK_SUCCESS) {
        error = std::format("vkCreateImage (modifier 0x{:x}) failed ({})", frame.modifier,
                            static_cast<int>(r));
        return false;
    }

    auto cleanup_image = [&] { vkDestroyImage(ctx.device, image, nullptr); };

    // ---- which memory types may back this fd?
    VkMemoryFdPropertiesKHR fd_props{};
    fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (ctx.get_memory_fd_properties(ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                     frame.planes.front().fd, &fd_props) != VK_SUCCESS) {
        error = "vkGetMemoryFdPropertiesKHR failed";
        cleanup_image();
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(ctx.device, image, &mr);
    const std::uint32_t usable = mr.memoryTypeBits & fd_props.memoryTypeBits;
    if (usable == 0) {
        error = "no memory type satisfies both the image and the imported fd";
        cleanup_image();
        return false;
    }
    std::uint32_t mem_type = 0;
    for (std::uint32_t i = 0; i < 32; ++i)
        if ((usable & (1u << i)) != 0u) {
            mem_type = i;
            break;
        }

    // VkImportMemoryFdInfoKHR takes ownership of the fd, and PipeWire's is borrowed.
    const int dup_fd = ::dup(frame.planes.front().fd);
    if (dup_fd < 0) {
        error = std::format("dup(dmabuf fd) failed: {}", std::strerror(errno));
        cleanup_image();
        return false;
    }

    VkImportMemoryFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = dup_fd;

    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &import_info;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (VkResult r = vkAllocateMemory(ctx.device, &mai, nullptr, &memory); r != VK_SUCCESS) {
        error = std::format("vkAllocateMemory (import) failed ({})", static_cast<int>(r));
        ::close(dup_fd); // not consumed on failure
        cleanup_image();
        return false;
    }
    if (vkBindImageMemory(ctx.device, image, memory, 0) != VK_SUCCESS) {
        error = "vkBindImageMemory failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        cleanup_image();
        return false;
    }

    const bool ok = readback_image(ctx, image, frame.width, frame.height, true, out, error);
    vkFreeMemory(ctx.device, memory, nullptr);
    cleanup_image();
    return ok;
}


// ---------------------------------------------------------------- export: we allocate, they draw

namespace {

/// Picks a memory type: device-local for normal use, or host-visible system memory for a buffer
/// that a second GPU must also reach.
std::uint32_t pick_memory_type(VkPhysicalDevice phys, std::uint32_t type_bits, bool host_memory) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    if (host_memory) {
        // Prefer host-visible and *not* device-local: that combination is system RAM, which is the
        // only place both cards can address. A host-visible but device-local type on a discrete
        // card is resizable-BAR VRAM, which is exactly what must be avoided here.
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            const auto flags = mp.memoryTypes[i].propertyFlags;
            if ((type_bits & (1u << i)) != 0u &&
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u &&
                (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0u)
                return i;
        }
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((type_bits & (1u << i)) != 0u &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u)
                return i;
        return UINT32_MAX;
    }
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) != 0u &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
            return i;
    return UINT32_MAX;
}

/// How many memory planes a given modifier uses for this format. Guessing 1 works for linear
/// single-plane RGBA and then silently corrupts anything else, so ask the driver.
std::uint32_t modifier_plane_count(VkPhysicalDevice phys, VkFormat fmt, std::uint64_t modifier) {
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp{};
    fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);
    std::vector<VkDrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = props.data();
    vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);
    for (const auto& p : props)
        if (p.drmFormatModifier == modifier)
            return p.drmFormatModifierPlaneCount;
    return 0;
}

} // namespace

bool vk_create_exportable(VkCtx& ctx, std::uint32_t width, std::uint32_t height,
                          std::uint32_t drm_format, const std::vector<std::uint64_t>& allowed,
                          bool host_memory, VkExportedImage& out, std::string& error) {
    const VkFormat fmt = drm_to_vk(drm_format);
    if (fmt == VK_FORMAT_UNDEFINED) {
        error = std::format("unsupported drm format 0x{:08x}", drm_format);
        return false;
    }
    if (allowed.empty()) {
        error = "no candidate modifiers";
        return false;
    }
    if (ctx.get_memory_fd == nullptr || ctx.get_image_modifier_properties == nullptr) {
        error = "vkGetMemoryFdKHR / vkGetImageDrmFormatModifierPropertiesEXT unavailable";
        return false;
    }

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // Hand the driver the whole candidate list and let it choose; we read back which one it took.
    // Passing a single modifier instead would turn a perfectly good second choice into a failure.
    VkImageDrmFormatModifierListCreateInfoEXT modlist{};
    modlist.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    modlist.drmFormatModifierCount = static_cast<std::uint32_t>(allowed.size());
    modlist.pDrmFormatModifiers = allowed.data();
    modlist.pNext = &ext;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &modlist;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    // TRANSFER_DST and COLOR_ATTACHMENT matter: the compositor blits or renders into this image,
    // and a usage set that omits them can make the driver pick a layout it cannot write to.
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (VkResult r = vkCreateImage(ctx.device, &ici, nullptr, &image); r != VK_SUCCESS) {
        error = std::format("vkCreateImage (exportable) failed ({})", static_cast<int>(r));
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(ctx.device, image, &mr);
    const std::uint32_t type = pick_memory_type(ctx.phys, mr.memoryTypeBits, host_memory);
    if (type == UINT32_MAX) {
        error = host_memory ? "no host-visible memory type for a cross-GPU transfer image"
                            : "no device-local memory type for the exportable image";
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    VkExportMemoryAllocateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // A dedicated allocation is required for an exported image on most drivers, and is what makes
    // the exported fd describe exactly this image rather than a suballocation of a heap.
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = image;
    ded.pNext = &exp;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &ded;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (VkResult r = vkAllocateMemory(ctx.device, &mai, nullptr, &memory); r != VK_SUCCESS) {
        error = std::format("vkAllocateMemory (exportable) failed ({})", static_cast<int>(r));
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }
    if (vkBindImageMemory(ctx.device, image, memory, 0) != VK_SUCCESS) {
        error = "vkBindImageMemory (exportable) failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    VkImageDrmFormatModifierPropertiesEXT chosen{};
    chosen.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (ctx.get_image_modifier_properties(ctx.device, image, &chosen) != VK_SUCCESS) {
        error = "vkGetImageDrmFormatModifierPropertiesEXT failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    std::uint32_t planes = modifier_plane_count(ctx.phys, fmt, chosen.drmFormatModifier);
    if (planes == 0 || planes > 4)
        planes = 1;

    static constexpr VkImageAspectFlagBits kPlaneAspect[4]{
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    for (std::uint32_t i = 0; i < planes; ++i) {
        VkImageSubresource sub{};
        sub.aspectMask = kPlaneAspect[i];
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(ctx.device, image, &sub, &layout);
        out.offsets[i] = static_cast<std::uint32_t>(layout.offset);
        out.strides[i] = static_cast<std::uint32_t>(layout.rowPitch);
    }

    VkMemoryGetFdInfoKHR gfi{};
    gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfi.memory = memory;
    gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (ctx.get_memory_fd(ctx.device, &gfi, &fd) != VK_SUCCESS || fd < 0) {
        error = "vkGetMemoryFdKHR failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    out.image = image;
    out.memory = memory;
    out.dmabuf_fd = fd;
    out.width = width;
    out.height = height;
    out.modifier = chosen.drmFormatModifier;
    out.n_planes = planes;
    out.drm_format = drm_format;
    return true;
}

void vk_destroy_exportable(VkCtx& ctx, VkExportedImage& img) {
    if (img.dmabuf_fd >= 0) {
        ::close(img.dmabuf_fd);
        img.dmabuf_fd = -1;
    }
    if (img.image != nullptr) {
        vkDestroyImage(ctx.device, static_cast<VkImage>(img.image), nullptr);
        img.image = nullptr;
    }
    if (img.memory != nullptr) {
        vkFreeMemory(ctx.device, static_cast<VkDeviceMemory>(img.memory), nullptr);
        img.memory = nullptr;
    }
}

std::string vk_readback_note(VkCtx& ctx) {
    const ReadbackStaging& st = ctx.readback;
    if (st.type_index == UINT32_MAX)
        return "readback staging: not allocated";
    return std::format("readback staging: memory type {} ({}, {}), {:.2f} MiB resident",
                       st.type_index, st.cached ? "HOST_CACHED" : "UNCACHED - reads will be slow",
                       st.coherent ? "coherent" : "non-coherent",
                       static_cast<double>(st.capacity) / (1024.0 * 1024.0));
}

bool vk_readback_exported(VkCtx& ctx, const VkExportedImage& img, bool written_by_foreign,
                          std::vector<std::uint8_t>& out, std::string& error) {
    if (written_by_foreign) {
        pollfd ready{img.dmabuf_fd, POLLIN, 0};
        if (::poll(&ready, 1, 2000) <= 0 || (ready.revents & POLLIN) == 0) {
            error = "capture producer fence did not signal";
            return false;
        }
    }
    if (!readback_image(ctx, static_cast<VkImage>(img.image), img.width, img.height,
                        written_by_foreign, out, error))
        return false;
    const bool swap = img.drm_format == DRM_FORMAT_ABGR8888 || img.drm_format == DRM_FORMAT_XBGR8888;
    const bool opaque = img.drm_format == DRM_FORMAT_XRGB8888 || img.drm_format == DRM_FORMAT_XBGR8888;
    if (swap || opaque) {
        for (std::size_t i = 0; i < out.size(); i += 4) {
            if (swap) std::swap(out[i], out[i + 2]);
            if (opaque) out[i + 3] = 255;
        }
    }
    return true;
}


bool vk_blit_exported(VkCtx& ctx, const VkExportedImage& src, bool src_foreign,
                      const VkExportedImage& dst, std::string& error) {
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = ctx.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx.device, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    const std::uint32_t foreign =
        ctx.have_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;

    std::array<VkImageMemoryBarrier, 2> pre{};
    pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[0].srcAccessMask = 0;
    pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[0].srcQueueFamilyIndex = src_foreign ? foreign : VK_QUEUE_FAMILY_IGNORED;
    pre[0].dstQueueFamilyIndex = src_foreign ? ctx.queue_family : VK_QUEUE_FAMILY_IGNORED;
    pre[0].image = static_cast<VkImage>(src.image);
    pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[1].srcAccessMask = 0;
    pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[1].image = static_cast<VkImage>(dst.image);
    pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, static_cast<std::uint32_t>(pre.size()), pre.data());

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(src.width),
                          static_cast<std::int32_t>(src.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<std::int32_t>(dst.width),
                          static_cast<std::int32_t>(dst.height), 1};
    vkCmdBlitImage(cmd, static_cast<VkImage>(src.image), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   static_cast<VkImage>(dst.image), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    // Release ownership so the *other* device's acquire barrier is well defined. Skipping this is
    // undefined behaviour that appears to work on some drivers - the worst kind to ship.
    VkImageMemoryBarrier release{};
    release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    release.dstAccessMask = 0;
    release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release.srcQueueFamilyIndex = ctx.queue_family;
    release.dstQueueFamilyIndex = foreign;
    release.image = static_cast<VkImage>(dst.image);
    release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &release);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(ctx.device, &fci, nullptr, &fence);

    bool ok = true;
    if (vkQueueSubmit(ctx.queue, 1, &si, fence) != VK_SUCCESS) {
        error = "vkQueueSubmit (blit) failed";
        ok = false;
    } else if (vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, 2'000'000'000ULL) != VK_SUCCESS) {
        error = "blit fence wait timed out";
        ok = false;
    }

    vkDestroyFence(ctx.device, fence, nullptr);
    vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &cmd);
    return ok;
}

CapturedFrame vk_exported_as_frame(const VkExportedImage& img, std::uint32_t drm_format) {
    CapturedFrame f;
    f.width = img.width;
    f.height = img.height;
    f.drm_format = drm_format;
    f.modifier = img.modifier;
    f.is_dmabuf = true;
    for (std::uint32_t i = 0; i < img.n_planes; ++i) {
        capture::DmabufPlane p;
        // Borrowed: the import path dups the fd, and the exported image still owns it.
        p.fd = img.dmabuf_fd;
        p.offset = img.offsets[i];
        p.stride = img.strides[i];
        f.planes.push_back(p);
    }
    return f;
}

} // namespace munkicast::rhi

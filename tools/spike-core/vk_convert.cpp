// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// BGRA -> NV12 on the GPU, then one readback of the converted planes.
//
// ADR-0019 measured why this exists: reading back BGRA and converting with swscale cost 4.351 ms
// of CPU per frame and moved 14.06 MiB where 5.27 MiB would do. OBS converts in a shader and
// stages the converted planes even on its slowest path; we were doing both halves backwards.
//
// The destination is a DEVICE_LOCAL storage buffer that the compute shader writes, copied in one
// vkCmdCopyBuffer into the host-cached staging buffer. Writing straight to host memory from the
// shader would also work and would be slower: scattered 32-bit writes across PCIe are not what
// that bus is good at, whereas one linear copy is exactly what it is good at.

#include "vk_convert.hpp"

#include <vk_ctx_internal.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <format>

#include <vulkan/vulkan.h>

#include "shaders/bgra_to_nv12_spv.h"

namespace munkicast::spike {

using munkicast::rhi::VkCtx;

namespace {

struct PushConstants {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t y_stride;
    std::uint32_t uv_stride;
    std::uint32_t uv_offset;
    std::uint32_t full_range;
    std::uint32_t matrix_id;
};

std::uint32_t find_memory_type(VkPhysicalDevice phys, std::uint32_t bits,
                               VkMemoryPropertyFlags want, VkMemoryPropertyFlags reject) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) != 0u && (f & want) == want && (f & reject) == 0u)
            return i;
    }
    return UINT32_MAX;
}

} // namespace

/// Everything the convert path owns. Built once, on first use, and torn down with the context.
} // namespace munkicast::spike

// Defined in rhi because that is the namespace VkCtx::convert points into. The contents are the
// encode spike's business (ADR-0019); lrhi only forward-declares it and never looks inside.
namespace munkicast::rhi {

struct ConvertState {
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // Device-local destination the shader writes, and the host-cached buffer it is copied to.
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceMemory dst_mem = VK_NULL_HANDLE;
    VkBuffer host = VK_NULL_HANDLE;
    VkDeviceMemory host_mem = VK_NULL_HANDLE;
    void* host_mapped = nullptr;
    VkDeviceSize capacity = 0;
    bool host_coherent = false;
    bool host_cached = false;

    // The view is per-image, so it is recreated whenever the source image changes.
    VkImageView view = VK_NULL_HANDLE;
    VkImage view_of = VK_NULL_HANDLE;
};

} // namespace munkicast::rhi

namespace munkicast::spike {

using munkicast::rhi::ConvertState;

void vk_convert_destroy(VkCtx& ctx) {
    ConvertState* cs = ctx.convert;
    if (cs == nullptr)
        return;
    if (cs->view != VK_NULL_HANDLE)
        vkDestroyImageView(ctx.device, cs->view, nullptr);
    if (cs->host_mapped != nullptr)
        vkUnmapMemory(ctx.device, cs->host_mem);
    if (cs->host != VK_NULL_HANDLE)
        vkDestroyBuffer(ctx.device, cs->host, nullptr);
    if (cs->host_mem != VK_NULL_HANDLE)
        vkFreeMemory(ctx.device, cs->host_mem, nullptr);
    if (cs->dst != VK_NULL_HANDLE)
        vkDestroyBuffer(ctx.device, cs->dst, nullptr);
    if (cs->dst_mem != VK_NULL_HANDLE)
        vkFreeMemory(ctx.device, cs->dst_mem, nullptr);
    if (cs->fence != VK_NULL_HANDLE)
        vkDestroyFence(ctx.device, cs->fence, nullptr);
    if (cs->cmd != VK_NULL_HANDLE)
        vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &cs->cmd);
    if (cs->pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(ctx.device, cs->pool, nullptr);
    if (cs->pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(ctx.device, cs->pipeline, nullptr);
    if (cs->pipe_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(ctx.device, cs->pipe_layout, nullptr);
    if (cs->set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(ctx.device, cs->set_layout, nullptr);
    if (cs->sampler != VK_NULL_HANDLE)
        vkDestroySampler(ctx.device, cs->sampler, nullptr);
    if (cs->shader != VK_NULL_HANDLE)
        vkDestroyShaderModule(ctx.device, cs->shader, nullptr);
    delete cs;
    ctx.convert = nullptr;
}

namespace {

bool build_pipeline(VkCtx& ctx, std::string& error) {
    auto* cs = new ConvertState{};
    ctx.convert = cs;

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof(kBgraToNv12Spv);
    smci.pCode = kBgraToNv12Spv;
    if (vkCreateShaderModule(ctx.device, &smci, nullptr, &cs->shader) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed";
        return false;
    }

    // Nearest, clamp-to-edge: the shader uses texelFetch, so filtering never runs, but a
    // combined image sampler still needs a sampler object and an unfiltered one documents intent.
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(ctx.device, &sci, nullptr, &cs->sampler) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return false;
    }

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = static_cast<std::uint32_t>(bindings.size());
    dslci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(ctx.device, &dslci, nullptr, &cs->set_layout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &cs->set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(ctx.device, &plci, nullptr, &cs->pipe_layout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return false;
    }

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = cs->shader;
    cpci.stage.pName = "main";
    cpci.layout = cs->pipe_layout;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &cs->pipeline) !=
        VK_SUCCESS) {
        error = "vkCreateComputePipelines failed";
        return false;
    }

    const std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    dpci.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &cs->pool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = cs->pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &cs->set_layout;
    if (vkAllocateDescriptorSets(ctx.device, &dsai, &cs->set) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed";
        return false;
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = ctx.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx.device, &cbai, &cs->cmd) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(ctx.device, &fci, nullptr, &cs->fence) != VK_SUCCESS) {
        error = "vkCreateFence failed";
        return false;
    }
    return true;
}

bool ensure_buffers(VkCtx& ctx, VkDeviceSize bytes, std::string& error) {
    ConvertState* cs = ctx.convert;
    if (cs->capacity >= bytes)
        return true;

    if (cs->host_mapped != nullptr) {
        vkUnmapMemory(ctx.device, cs->host_mem);
        cs->host_mapped = nullptr;
    }
    if (cs->host != VK_NULL_HANDLE) vkDestroyBuffer(ctx.device, cs->host, nullptr);
    if (cs->host_mem != VK_NULL_HANDLE) vkFreeMemory(ctx.device, cs->host_mem, nullptr);
    if (cs->dst != VK_NULL_HANDLE) vkDestroyBuffer(ctx.device, cs->dst, nullptr);
    if (cs->dst_mem != VK_NULL_HANDLE) vkFreeMemory(ctx.device, cs->dst_mem, nullptr);
    cs->host = cs->dst = VK_NULL_HANDLE;
    cs->host_mem = cs->dst_mem = VK_NULL_HANDLE;
    cs->capacity = 0;

    auto make = [&](VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                    VkMemoryPropertyFlags reject, VkBuffer& buf, VkDeviceMemory& mem,
                    std::uint32_t* chosen) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(ctx.device, &bci, nullptr, &buf) != VK_SUCCESS) {
            error = "vkCreateBuffer failed";
            return false;
        }
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(ctx.device, buf, &mr);
        std::uint32_t type = find_memory_type(ctx.phys, mr.memoryTypeBits, want, reject);
        if (type == UINT32_MAX && reject != 0u)
            type = find_memory_type(ctx.phys, mr.memoryTypeBits, want, 0);
        if (type == UINT32_MAX) {
            error = "no suitable memory type for convert buffer";
            return false;
        }
        if (chosen != nullptr)
            *chosen = type;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = type;
        if (vkAllocateMemory(ctx.device, &mai, nullptr, &mem) != VK_SUCCESS) {
            error = "vkAllocateMemory failed";
            return false;
        }
        vkBindBufferMemory(ctx.device, buf, mem, 0);
        return true;
    };

    if (!make(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, cs->dst, cs->dst_mem, nullptr))
        return false;

    // Host side: cached, and not device-local. ADR-0018 - an uncached mapping here is a 25x
    // slowdown that nothing reports, and a device-local host-visible type is ReBAR VRAM.
    std::uint32_t host_type = UINT32_MAX;
    if (!make(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cs->host, cs->host_mem, &host_type))
        return false;

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(ctx.phys, &mp);
    const VkMemoryPropertyFlags hf = mp.memoryTypes[host_type].propertyFlags;
    cs->host_cached = (hf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0u;
    cs->host_coherent = (hf & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;

    if (vkMapMemory(ctx.device, cs->host_mem, 0, VK_WHOLE_SIZE, 0, &cs->host_mapped) !=
        VK_SUCCESS) {
        error = "vkMapMemory failed";
        return false;
    }
    cs->capacity = bytes;
    return true;
}

} // namespace

bool vk_convert_nv12(VkCtx& ctx, const VkExportedImage& img, bool written_by_foreign,
                     const ColorCfg& color, Nv12Frame& out, std::string& error,
                     ConvertTiming* timing) {
    const auto t_record = std::chrono::steady_clock::now();
    if (img.width % 4u != 0u || img.height % 2u != 0u) {
        error = std::format("convert needs width%4==0 and height%2==0, got {}x{}", img.width,
                            img.height);
        return false;
    }
    if (ctx.convert == nullptr && !build_pipeline(ctx, error)) {
        vk_convert_destroy(ctx);
        return false;
    }
    ConvertState* cs = ctx.convert;

    const std::uint32_t y_stride = img.width;
    const std::uint32_t uv_stride = img.width;
    const std::uint32_t uv_offset = y_stride * img.height;
    const VkDeviceSize bytes = uv_offset + static_cast<VkDeviceSize>(uv_stride) * (img.height / 2u);

    if (!ensure_buffers(ctx, bytes, error))
        return false;

    auto src_image = static_cast<VkImage>(img.image);
    if (cs->view == VK_NULL_HANDLE || cs->view_of != src_image) {
        if (cs->view != VK_NULL_HANDLE)
            vkDestroyImageView(ctx.device, cs->view, nullptr);
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = src_image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        // UNORM, never _SRGB: the matrix below is applied to encoded R'G'B', so sampling must
        // not linearise. Picking _SRGB here would silently double-apply the transfer function.
        ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(ctx.device, &ivci, nullptr, &cs->view) != VK_SUCCESS) {
            error = "vkCreateImageView failed";
            return false;
        }
        cs->view_of = src_image;
    }

    VkDescriptorImageInfo dii{};
    dii.sampler = cs->sampler;
    dii.imageView = cs->view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo dbi{};
    dbi.buffer = cs->dst;
    dbi.offset = 0;
    dbi.range = bytes;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = cs->set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &dii;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = cs->set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &dbi;
    vkUpdateDescriptorSets(ctx.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);

    vkResetCommandBuffer(cs->cmd, 0);
    vkResetFences(ctx.device, 1, &cs->fence);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cs->cmd, &cbbi);

    // Acquire from the foreign queue family, exactly as the readback path does - this is what
    // makes an external writer's pixels visible, and it is undefined behaviour to skip it.
    VkImageMemoryBarrier acquire{};
    acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    acquire.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (written_by_foreign) {
        acquire.srcQueueFamilyIndex =
            ctx.have_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = ctx.queue_family;
    } else {
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    acquire.image = src_image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cs->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &acquire);

    vkCmdBindPipeline(cs->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs->pipeline);
    vkCmdBindDescriptorSets(cs->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs->pipe_layout, 0, 1,
                            &cs->set, 0, nullptr);

    PushConstants pc{img.width,  img.height,
                     y_stride,   uv_stride,
                     uv_offset,  color.full_range ? 1u : 0u,
                     color.bt601 ? 1u : 0u};
    vkCmdPushConstants(cs->cmd, cs->pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    // One invocation per 4x2 luma block, 8x8 invocations per group.
    const std::uint32_t bx = (img.width / 4u + 7u) / 8u;
    const std::uint32_t by = (img.height / 2u + 7u) / 8u;
    vkCmdDispatch(cs->cmd, bx, by, 1);

    VkBufferMemoryBarrier bmb{};
    bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = cs->dst;
    bmb.offset = 0;
    bmb.size = bytes;
    vkCmdPipelineBarrier(cs->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bmb, 0, nullptr);

    VkBufferCopy copy{};
    copy.size = bytes;
    vkCmdCopyBuffer(cs->cmd, cs->dst, cs->host, 1, &copy);

    vkEndCommandBuffer(cs->cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cs->cmd;
    const auto t_submit = std::chrono::steady_clock::now();
    if (vkQueueSubmit(ctx.queue, 1, &si, cs->fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed";
        return false;
    }
    const auto t_wait = std::chrono::steady_clock::now();
    if (vkWaitForFences(ctx.device, 1, &cs->fence, VK_TRUE, 2'000'000'000ULL) != VK_SUCCESS) {
        error = "fence wait timed out";
        return false;
    }
    const auto t_copy = std::chrono::steady_clock::now();
    if (!cs->host_coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = cs->host_mem;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(ctx.device, 1, &range);
    }

    out.width = img.width;
    out.height = img.height;
    out.y_stride = y_stride;
    out.uv_stride = uv_stride;
    out.uv_offset = uv_offset;
    out.data.resize(bytes);
    std::memcpy(out.data.data(), cs->host_mapped, bytes);

    if (timing != nullptr) {
        using ms = std::chrono::duration<double, std::milli>;
        timing->record_ms = ms(t_submit - t_record).count();
        timing->submit_ms = ms(t_wait - t_submit).count();
        timing->wait_ms = ms(t_copy - t_wait).count();
        timing->copy_ms = ms(std::chrono::steady_clock::now() - t_copy).count();
    }
    return true;
}

std::string vk_convert_note(VkCtx& ctx) {
    const ConvertState* cs = ctx.convert;
    if (cs == nullptr)
        return "convert: not built";
    return std::format("convert staging: {} {}, {:.2f} MiB NV12 per frame",
                       cs->host_cached ? "HOST_CACHED" : "UNCACHED - reads will be slow",
                       cs->host_coherent ? "coherent" : "non-coherent",
                       static_cast<double>(cs->capacity) / (1024.0 * 1024.0));
}

} // namespace munkicast::spike

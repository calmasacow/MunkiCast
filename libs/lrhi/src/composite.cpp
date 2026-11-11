// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders/composite_spv.h"
#include "shaders/filter_spv.h"
#include "shaders/nv12_spv.h"
#include "shaders/preview_spv.h"
#include "shaders/yuv_spv.h"
#include "vk_ctx_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <drm_fourcc.h>
#include <format>
#include <lcomp/filters.hpp>
#include <lrhi/composite.hpp>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace libertas::rhi {
namespace {
void checked(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::format("{} ({})", action, static_cast<int>(result)));
}

std::uint32_t memory_type(VkCtx& c, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("no suitable GPU memory type");
}

// GPU memory the compositor suballocates from. vkAllocateMemory took 2-30 ms per call on the
// render thread whenever a new image size first appeared mid-show (media becoming ready).
// Blocks are reserved up front and kept; images and buffers take ranges out of them.
struct Arena {
    struct Range {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, size = 0;
        void* mapped = nullptr;
    };
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
        std::map<VkDeviceSize, VkDeviceSize> free; // offset -> size
    };
    VkCtx& c;
    std::uint32_t type = 0;
    bool host = false;
    VkDeviceSize block_size = 0;
    std::vector<std::unique_ptr<Block>> blocks;

    Arena(VkCtx& ctx, std::uint32_t memory_type, bool host_visible, VkDeviceSize block)
        : c(ctx), type(memory_type), host(host_visible), block_size(block) {}
    ~Arena() {
        for (auto& b : blocks) {
            if (b->mapped)
                vkUnmapMemory(c.device, b->memory);
            vkFreeMemory(c.device, b->memory, nullptr);
        }
    }
    Block& grow(VkDeviceSize at_least) {
        auto b = std::make_unique<Block>();
        b->size = std::max(block_size, at_least);
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = b->size;
        mi.memoryTypeIndex = type;
        checked(vkAllocateMemory(c.device, &mi, nullptr, &b->memory), "allocate compositor block");
        if (host)
            checked(vkMapMemory(c.device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped),
                    "map compositor block");
        b->free[0] = b->size;
        blocks.push_back(std::move(b));
        return *blocks.back();
    }
    Range take(const VkMemoryRequirements& req) {
        const auto align = std::max<VkDeviceSize>(req.alignment, 256);
        const auto fit = [&](Block& b) -> std::optional<Range> {
            for (auto it = b.free.begin(); it != b.free.end(); ++it) {
                const auto start = (it->first + align - 1) / align * align;
                const auto end = it->first + it->second;
                if (start + req.size > end)
                    continue;
                const auto head = it->first, tail = start + req.size;
                b.free.erase(it);
                if (start > head)
                    b.free[head] = start - head;
                if (end > tail)
                    b.free[tail] = end - tail;
                return Range{b.memory, start, req.size,
                             b.mapped ? static_cast<char*>(b.mapped) + start : nullptr};
            }
            return std::nullopt;
        };
        for (auto& b : blocks)
            if (auto r = fit(*b))
                return *r;
        auto r = fit(grow(req.size + align));
        if (!r)
            throw std::runtime_error("compositor block cannot fit allocation");
        return *r;
    }
    void give(const Range& r) {
        for (auto& b : blocks) {
            if (b->memory != r.memory)
                continue;
            auto [it, inserted] = b->free.emplace(r.offset, r.size);
            if (const auto next = std::next(it);
                next != b->free.end() && it->first + it->second == next->first) {
                it->second += next->second;
                b->free.erase(next);
            }
            if (it != b->free.begin())
                if (const auto prev = std::prev(it); prev->first + prev->second == it->first) {
                    prev->second += it->second;
                    b->free.erase(it);
                }
            return;
        }
    }
};

struct Image {
    VkCtx& c;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Arena* arena = nullptr;
    Arena::Range range;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0, height = 0;
    bool owns = true;

    ~Image() {
        if (view)
            vkDestroyImageView(c.device, view, nullptr);
        if (owns && image)
            vkDestroyImage(c.device, image, nullptr);
        if (arena)
            arena->give(range);
        else if (memory)
            vkFreeMemory(c.device, memory, nullptr);
    }
};

struct Buffer {
    VkCtx& c;
    VkBuffer buffer = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    Arena* arena = nullptr;
    Arena::Range range;

    ~Buffer() {
        if (buffer)
            vkDestroyBuffer(c.device, buffer, nullptr);
        if (arena)
            arena->give(range);
    }
};

// An NV12 frame NVENC reads in place through CUDA. Exported once as an opaque fd; reused while
// nobody outside holds `frame`, which the encoder keeps until NVENC has finished reading it.
struct Nv12Slot {
    VkCtx& c;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::shared_ptr<lcomp::DeviceNv12> frame;
    int width = 0, height = 0;

    ~Nv12Slot() {
        if (frame && frame->fd >= 0)
            ::close(frame->fd);
        if (buffer)
            vkDestroyBuffer(c.device, buffer, nullptr);
        if (memory)
            vkFreeMemory(c.device, memory, nullptr);
    }
};

struct Push {
    std::array<int, 4> rect{};
    std::array<int, 4> flags{};
    std::array<float, 4> colour{};
    std::array<float, 4> opacity{};
    std::array<int, 4> crop{};
};

static_assert(sizeof(Push) == 80);
// Keep composite.comp's mode constants aligned with the shared plan.
static_assert(static_cast<int>(ldoc::BlendMode::Normal) == 0 &&
              static_cast<int>(ldoc::BlendMode::Darken) == 2 &&
              static_cast<int>(ldoc::BlendMode::Multiply) == 3 &&
              static_cast<int>(ldoc::BlendMode::Lighten) == 6 &&
              static_cast<int>(ldoc::BlendMode::Screen) == 7 &&
              static_cast<int>(ldoc::BlendMode::LinearDodge) == 9 &&
              static_cast<int>(ldoc::BlendMode::Subtract) == 19);
} // namespace

struct GpuCompositor::Impl {
    VkCtx* c = nullptr;
    // One arena per memory type: device blocks 256 MiB, host staging 64 MiB. Declared first so
    // it is destroyed last, after every image and buffer has returned its range.
    std::map<std::uint32_t, std::unique_ptr<Arena>> arenas;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // LIBERTAS_BENCHMARK only: GPU timestamps splitting the fence wait into upload, compute
    // and readback time. Uploads are interleaved with draws, so they are summed per copy.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    std::uint32_t stamp_at = 0;
    double stamp_ns = 0;
    std::uint64_t upload_bytes = 0;
    unsigned dispatches = 0;
    // Slots 0-2: frame start, canvas composited, readback copied. Then begin/end pairs per upload.
    void stamp(VkPipelineStageFlagBits stage, std::uint32_t slot) {
        vkCmdWriteTimestamp(cmd, stage, timestamps, slot);
    }
    static constexpr std::uint32_t kStamps = 256;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline composite = VK_NULL_HANDLE, preview = VK_NULL_HANDLE, filter = VK_NULL_HANDLE,
               yuv = VK_NULL_HANDLE, nv12 = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Nv12Slot>> nv12_slots;
    bool nv12_broken = false;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Image>> images;
    std::vector<std::unique_ptr<Buffer>> uploads;
    std::map<VkImage, std::unique_ptr<Image>> foreign;
    std::vector<Image*> acquired;
    std::unique_ptr<Buffer> readback, full_readback;
    std::size_t image_at = 0, upload_at = 0;
    // Images the compositor may hold. Half of the device-local heap's budget at startup, which
    // leaves room for the encoder, Qt and other programs; 512 MiB where the driver cannot say.
    // LIBERTAS_GPU_BUDGET_MIB, set from Preferences, replaces it.
    std::uint64_t budget = 512ull << 20;
    std::uint64_t allocated = 0;

    struct Auxiliary {
        std::shared_ptr<lcomp::Picture> pixels;
        std::unique_ptr<Image> image;
    };

    struct SourceImage {
        std::shared_ptr<const void> pixels;
        std::unique_ptr<Image> image;
    };

    std::vector<SourceImage> source_images;
    std::uint64_t source_bytes = 0;

    std::vector<Auxiliary> auxiliaries;
    std::uint64_t auxiliary_bytes = 0;

    struct History {
        int width = 0, height = 0;
        float delay = 0;
        std::uint64_t last = 0;
        std::deque<std::pair<std::uint64_t, std::unique_ptr<Image>>> frames;
    };

    std::map<ldoc::Id, History> histories;
    std::vector<std::unique_ptr<Image>> retired, recycled;
    std::vector<lcomp::Gap> runtime_gaps;

    struct MediaImage {
        ldoc::Id source;
        std::vector<ldoc::Effect> filters;
        std::shared_ptr<const void> frame;
        std::unique_ptr<Image> image;
    };

    std::vector<MediaImage> media_images;
    std::uint64_t media_bytes = 0;

    Image& delayed(Image& input, const lcomp::VideoFilter& op) {
        auto& h = histories[op.id];
        if (h.width != input.width || h.height != input.height || h.delay != op.values[0] ||
            op.time_ns < h.last) {
            for (auto& frame : h.frames)
                retired.push_back(std::move(frame.second));
            h = {input.width, input.height, op.values[0], 0, {}};
        }
        const auto delay = static_cast<std::uint64_t>(op.values[0] * 1000000);
        const auto due = op.time_ns > delay ? op.time_ns - delay : 0;
        while (h.frames.size() > 1 && h.frames[1].first <= due) {
            retired.push_back(std::move(h.frames.front().second));
            h.frames.pop_front();
        }
        std::uint64_t bytes = 0;
        for (const auto& [id, history] : histories)
            bytes += static_cast<std::uint64_t>(history.width) *
                     static_cast<std::uint64_t>(history.height) * 8 * history.frames.size();
        bool full = false;
        if (h.last != op.time_ns || h.frames.empty()) {
            const auto need = static_cast<std::uint64_t>(input.width) *
                              static_cast<std::uint64_t>(input.height) * 8;
            if (bytes + need > 256ULL * 1024 * 1024 || h.frames.size() >= 1200)
                full = true;
            else {
                std::unique_ptr<Image> saved;
                const auto reuse = std::ranges::find_if(recycled, [&](const auto& image) {
                    return image && image->width == input.width && image->height == input.height;
                });
                if (reuse != recycled.end()) {
                    saved = std::move(*reuse);
                    recycled.erase(reuse);
                } else {
                    target(input.width, input.height, VK_FORMAT_R16G16B16A16_SFLOAT);
                    saved = std::move(images[image_at - 1]);
                }
                barrier(*saved, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = copy.srcSubresource;
                copy.extent = {static_cast<unsigned>(input.width),
                               static_cast<unsigned>(input.height), 1};
                vkCmdCopyImage(cmd, input.image, VK_IMAGE_LAYOUT_GENERAL, saved->image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                barrier(*saved, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                h.frames.emplace_back(op.time_ns, std::move(saved));
            }
            h.last = op.time_ns;
        }
        if (!full && !h.frames.empty() && h.frames.front().first <= due)
            return *h.frames.front().second;
        auto& empty = target(input.width, input.height, VK_FORMAT_R16G16B16A16_SFLOAT);
        barrier(empty, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue clear{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, empty.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        barrier(empty, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        runtime_gaps.push_back(
            {op.id, "Video delay",
             full ? lcomp::GapReason::UnsupportedEffect : lcomp::GapReason::NoFrame});
        return empty;
    }

    ~Impl() {
        if (!c)
            return;
        foreign.clear();
        source_images.clear();
        auxiliaries.clear();
        media_images.clear();
        images.clear();
        uploads.clear();
        readback.reset();
        full_readback.reset();
        nv12_slots.clear();
        histories.clear();
        retired.clear();
        recycled.clear();
        arenas.clear();
        if (sampler)
            vkDestroySampler(c->device, sampler, nullptr);
        if (descriptors)
            vkDestroyDescriptorPool(c->device, descriptors, nullptr);
        if (preview)
            vkDestroyPipeline(c->device, preview, nullptr);
        if (yuv)
            vkDestroyPipeline(c->device, yuv, nullptr);
        if (nv12)
            vkDestroyPipeline(c->device, nv12, nullptr);
        if (filter)
            vkDestroyPipeline(c->device, filter, nullptr);
        if (composite)
            vkDestroyPipeline(c->device, composite, nullptr);
        if (layout)
            vkDestroyPipelineLayout(c->device, layout, nullptr);
        if (set_layout)
            vkDestroyDescriptorSetLayout(c->device, set_layout, nullptr);
        if (fence)
            vkDestroyFence(c->device, fence, nullptr);
        if (timestamps)
            vkDestroyQueryPool(c->device, timestamps, nullptr);
        if (pool)
            vkDestroyCommandPool(c->device, pool, nullptr);
    }

    VkPipeline pipeline(const std::uint32_t* code, std::size_t size) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        checked(vkCreateShaderModule(c->device, &info, nullptr, &module), "create shader");
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        VkPipeline result = VK_NULL_HANDLE;
        const auto status =
            vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, nullptr, &result);
        vkDestroyShaderModule(c->device, module, nullptr);
        checked(status, "create composite pipeline");
        return result;
    }

    void init() {
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(c->phys, &features);
        if (!features.shaderInt64 || !features.shaderStorageImageExtendedFormats)
            throw std::runtime_error("device lacks composite shader features");
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R16G16B16A16_SFLOAT, &props);
        constexpr auto required =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.optimalTilingFeatures & required) != required)
            throw std::runtime_error("device lacks RGBA16F storage/sampling");
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = c->queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        checked(vkCreateCommandPool(c->device, &pi, nullptr, &pool), "create composite pool");
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        checked(vkAllocateCommandBuffers(c->device, &ai, &cmd),
                "allocate composite command buffer");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        checked(vkCreateFence(c->device, &fi, nullptr, &fence), "create composite fence");
        VkPhysicalDeviceProperties device_props{};
        vkGetPhysicalDeviceProperties(c->phys, &device_props);
        if (c->have_memory_budget) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT heaps{};
            heaps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
            VkPhysicalDeviceMemoryProperties2 memory{};
            memory.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            memory.pNext = &heaps;
            vkGetPhysicalDeviceMemoryProperties2(c->phys, &memory);
            std::uint64_t largest = 0;
            for (std::uint32_t i = 0; i < memory.memoryProperties.memoryHeapCount; ++i)
                if (memory.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    largest = std::max<std::uint64_t>(largest, heaps.heapBudget[i]);
            if (largest)
                budget = std::max(budget, largest / 2);
        }
        // Edit > Preferences: an explicit budget overrides the default (0 or unset keeps it).
        if (const char* mib = std::getenv("LIBERTAS_GPU_BUDGET_MIB"))
            if (const auto value = std::strtoull(mib, nullptr, 10); value >= 256)
                budget = value << 20;
        // Reserve the first device block now, at startup, not on the first frame that needs it.
        {
            VkImageCreateInfo probe{};
            probe.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            probe.imageType = VK_IMAGE_TYPE_2D;
            probe.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            probe.extent = {16, 16, 1};
            probe.mipLevels = probe.arrayLayers = 1;
            probe.samples = VK_SAMPLE_COUNT_1_BIT;
            probe.tiling = VK_IMAGE_TILING_OPTIMAL;
            probe.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkImage image = VK_NULL_HANDLE;
            checked(vkCreateImage(c->device, &probe, nullptr, &image), "probe image memory");
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(c->device, image, &req);
            vkDestroyImage(c->device, image, nullptr);
            arena(memory_type(*c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), false)
                .grow(0);
        }
        if (std::getenv("LIBERTAS_BENCHMARK") && device_props.limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo qi{};
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kStamps;
            checked(vkCreateQueryPool(c->device, &qi, nullptr, &timestamps),
                    "create composite timestamps");
            stamp_ns = device_props.limits.timestampPeriod;
        }
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        si.bindingCount = 4;
        si.pBindings = bindings;
        checked(vkCreateDescriptorSetLayout(c->device, &si, nullptr, &set_layout),
                "create descriptor layout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount = 1;
        li.pSetLayouts = &set_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(c->device, &li, nullptr, &layout), "create pipeline layout");
        composite = pipeline(kCompositeSpv, sizeof(kCompositeSpv));
        preview = pipeline(kPreviewSpv, sizeof(kPreviewSpv));
        filter = pipeline(kFilterSpv, sizeof(kFilterSpv));
        yuv = pipeline(kYuvSpv, sizeof(kYuvSpv));
        nv12 = pipeline(kNv12Spv, sizeof(kNv12Spv));
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}};
        VkDescriptorPoolCreateInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        di.maxSets = 4096;
        di.poolSizeCount = 3;
        di.pPoolSizes = sizes;
        checked(vkCreateDescriptorPool(c->device, &di, nullptr, &descriptors),
                "create descriptor pool");
        VkSamplerCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sm.magFilter = sm.minFilter = VK_FILTER_NEAREST;
        sm.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checked(vkCreateSampler(c->device, &sm, nullptr, &sampler), "create sampler");
    }

    void view(Image& image, bool opaque = false) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = image.format;
        vi.components.a = opaque ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checked(vkCreateImageView(c->device, &vi, nullptr, &image.view), "create image view");
    }

    Image& target(int width, int height, VkFormat format) {
        if (width <= 0 || height <= 0 || width > lcomp::kMaxPreviewEdge ||
            height > lcomp::kMaxPreviewEdge)
            throw std::runtime_error("invalid composite dimensions");
        const auto bytes =
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 8;
        allocated += bytes;
        if (allocated > budget)
            throw std::runtime_error(std::format("composite exceeds {} MiB GPU budget", budget >> 20));
        if (image_at == images.size())
            images.emplace_back();
        // Cached video frames change which scratch images are needed per tick.
        // Match unused resources by shape rather than rebuilding positional slots.
        const auto match = std::find_if(images.begin() + static_cast<std::ptrdiff_t>(image_at),
                                        images.end(), [&](const auto& image) {
                                            return image && image->width == width &&
                                                   image->height == height &&
                                                   image->format == format;
                                        });
        if (match != images.end())
            std::iter_swap(images.begin() + static_cast<std::ptrdiff_t>(image_at), match);
        // No spare of this shape: add a slot rather than overwrite a spare of another shape,
        // which the next frame would have to allocate again.
        if (match == images.end() && images[image_at])
            images.insert(images.begin() + static_cast<std::ptrdiff_t>(image_at), nullptr);
        auto& entry = images[image_at++];
        if (entry && entry->width == width && entry->height == height && entry->format == format)
            return *entry;
        entry = std::make_unique<Image>(*c);
        entry->width = width;
        entry->height = height;
        entry->format = format;
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        checked(vkCreateImage(c->device, &ci, nullptr, &entry->image), "create composite image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(c->device, entry->image, &req);
        auto& memory = arena(memory_type(*c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                             false);
        entry->range = memory.take(req);
        entry->arena = &memory;
        checked(vkBindImageMemory(c->device, entry->image, entry->range.memory, entry->range.offset),
                "bind composite image");
        view(*entry);
        return *entry;
    }

    // Readback wants HOST_CACHED: the CPU reads it. Upload staging must not be: the GPU then
    // reads it over PCIe with snooping. Write-combined memory is what the CPU memcpy writes
    // fastest and the GPU reads fastest.
    void buffer(std::unique_ptr<Buffer>& b, VkDeviceSize size, bool cached = true) {
        if (b && b->size >= size)
            return;
        b = std::make_unique<Buffer>(*c);
        b->size = size;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        checked(vkCreateBuffer(c->device, &bi, nullptr, &b->buffer), "create staging buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(c->device, b->buffer, &req);
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = req.size;
        try {
            if (!cached)
                throw std::runtime_error("write-combined staging");
            mi.memoryTypeIndex = memory_type(*c, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        } catch (const std::runtime_error&) {
            mi.memoryTypeIndex = memory_type(*c, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        auto& memory = arena(mi.memoryTypeIndex, true);
        b->range = memory.take(req);
        b->arena = &memory;
        b->mapped = b->range.mapped;
        checked(vkBindBufferMemory(c->device, b->buffer, b->range.memory, b->range.offset),
                "bind staging buffer");
    }

    Arena& arena(std::uint32_t type, bool host) {
        auto& a = arenas[type];
        if (!a)
            a = std::make_unique<Arena>(*c, type, host, host ? 64ull << 20 : 256ull << 20);
        return *a;
    }

    void barrier(Image& image, VkImageLayout old_layout, VkImageLayout next, bool acquire = false,
                 bool release = false) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = old_layout;
        b.newLayout = next;
        b.image = image.image;
        b.srcAccessMask = acquire || old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                              ? 0u
                              : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask = release ? 0u : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        const auto external =
            c->have_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        b.srcQueueFamilyIndex = acquire   ? external
                                : release ? c->queue_family
                                          : VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = acquire   ? c->queue_family
                                : release ? external
                                          : VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    Image& source(const lcomp::DrawOp& op) {
        if (op.device_image) {
            const auto& dmabuf = *static_cast<const capture::DmabufImage*>(op.device_image);
            const auto handle = static_cast<VkImage>(dmabuf.image);
            auto& entry = foreign[handle];
            if (!entry) {
                entry = std::make_unique<Image>(*c);
                entry->image = handle;
                entry->owns = false;
                entry->width = static_cast<int>(dmabuf.width);
                entry->height = static_cast<int>(dmabuf.height);
                const auto format = dmabuf.drm_format;
                if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888 &&
                    format != DRM_FORMAT_ABGR8888 && format != DRM_FORMAT_XBGR8888)
                    throw std::runtime_error("unsupported captured GPU format");
                entry->format = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888
                                    ? VK_FORMAT_B8G8R8A8_UNORM
                                    : VK_FORMAT_R8G8B8A8_UNORM;
                view(*entry, format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888);
            }
            if (std::ranges::find(acquired, entry.get()) == acquired.end()) {
                // Wayland delivers the slot; dma-buf's writer fence orders its GPU writes.
                pollfd ready{dmabuf.dmabuf_fd, POLLIN, 0};
                if (ready.fd < 0 || ::poll(&ready, 1, 2000) <= 0 || (ready.revents & POLLIN) == 0)
                    throw std::runtime_error("capture producer fence did not signal");
                barrier(*entry, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, true);
                acquired.push_back(entry.get());
            }
            return *entry;
        }
        const auto& pic = op.image;
        if (pic.owner) {
            const auto cached = std::ranges::find_if(source_images, [&](const auto& entry) {
                return entry.pixels == pic.owner && entry.image->width == pic.width &&
                       entry.image->height == pic.height &&
                       entry.image->format == (op.bgra ? VK_FORMAT_B8G8R8A8_UNORM
                                                       : VK_FORMAT_R8G8B8A8_UNORM);
            });
            if (cached != source_images.end())
                return *cached->image;
        }
        if (!pic.read_from())
            throw std::runtime_error("draw has no source pixels");
        if (op.yuv)
            return convert_yuv(op);
        auto& image = target(pic.width, pic.height,
                             op.bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM);
        if (upload_at == uploads.size())
            uploads.emplace_back();
        auto& stage = uploads[upload_at++];
        const auto bytes =
            static_cast<VkDeviceSize>(pic.width) * static_cast<VkDeviceSize>(pic.height) * 4;
        buffer(stage, bytes, false);
        std::memcpy(stage->mapped, pic.read_from(), bytes);
        upload_bytes += bytes;
        barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        const bool stamped = timestamps && stamp_at + 2 <= kStamps;
        if (stamped)
            stamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stamp_at++);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(pic.width),
                            static_cast<std::uint32_t>(pic.height), 1};
        vkCmdCopyBufferToImage(cmd, stage->buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        if (stamped)
            stamp(VK_PIPELINE_STAGE_TRANSFER_BIT, stamp_at++);
        barrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        if (pic.owner && source_images.size() < 16 && source_bytes + bytes <= 128ull * 1024 * 1024) {
            source_images.push_back({pic.owner, std::move(images[--image_at])});
            source_bytes += bytes;
        }
        return image;
    }

    // Camera planes go up as-is (2 or 1.5 bytes a pixel) and become RGBA8 on the GPU.
    Image& convert_yuv(const lcomp::DrawOp& op) {
        const auto& pic = op.image;
        const auto pixels = static_cast<VkDeviceSize>(pic.width) * static_cast<VkDeviceSize>(pic.height);
        const auto bytes = op.yuv == 1 ? pixels * 2 : pixels * 3 / 2;
        auto& image = target(pic.width, pic.height, VK_FORMAT_R8G8B8A8_UNORM);
        if (upload_at == uploads.size())
            uploads.emplace_back();
        auto& stage = uploads[upload_at++];
        buffer(stage, (bytes + 3) & ~VkDeviceSize{3}, false);
        std::memcpy(stage->mapped, pic.read_from(), bytes);
        upload_bytes += bytes;
        barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptors;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        checked(vkAllocateDescriptorSets(c->device, &ai, &set), "allocate composite descriptors");
        ++dispatches;
        const VkDescriptorImageInfo target_info{VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo planes{stage->buffer, 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (auto& w : writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &target_info;
        writes[1].dstBinding = 3;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &planes;
        vkUpdateDescriptorSets(c->device, 2, writes.data(), 0, nullptr);
        Push push;
        push.rect = {op.yuv, op.yuv_matrix, 0, 0};
        const bool stamped = timestamps && stamp_at + 2 <= kStamps;
        if (stamped)
            stamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stamp_at++);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, yuv);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (static_cast<std::uint32_t>(pic.width) + 15) / 16,
                      (static_cast<std::uint32_t>(pic.height) + 15) / 16, 1);
        if (stamped)
            stamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, stamp_at++);
        barrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        return image;
    }

    // A free slot of this size, or null when NVENC still holds all of them (or export failed);
    // the caller then reads back RGBA instead. Eight covers NVENC's surfaces plus one in hand.
    Nv12Slot* nv12_slot(int width, int height) {
        const auto idle = [](const auto& slot) { return slot->frame.use_count() == 1; };
        std::erase_if(nv12_slots, [&](const auto& slot) {
            return (slot->width != width || slot->height != height) && idle(slot);
        });
        for (auto& slot : nv12_slots)
            if (slot->width == width && slot->height == height && idle(slot))
                return slot.get();
        if (nv12_broken || nv12_slots.size() >= 8 || !c->get_memory_fd)
            return nullptr;
        try {
            auto slot = std::make_unique<Nv12Slot>(*c);
            slot->width = width;
            slot->height = height;
            // CUDA's own frame pool aligns rows to 256 bytes; NVENC is happy with that pitch.
            const auto pitch = (static_cast<std::uint32_t>(width) + 255u) & ~255u;
            VkExternalMemoryBufferCreateInfo external{};
            external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.pNext = &external;
            bi.size = VkDeviceSize{pitch} * static_cast<VkDeviceSize>(height) * 3 / 2;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            checked(vkCreateBuffer(c->device, &bi, nullptr, &slot->buffer), "create NV12 buffer");
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(c->device, slot->buffer, &req);
            VkMemoryDedicatedAllocateInfo dedicated{};
            dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicated.buffer = slot->buffer;
            VkExportMemoryAllocateInfo exported{};
            exported.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            exported.pNext = &dedicated;
            exported.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkMemoryAllocateInfo mi{};
            mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mi.pNext = &exported;
            mi.allocationSize = req.size;
            mi.memoryTypeIndex =
                memory_type(*c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            checked(vkAllocateMemory(c->device, &mi, nullptr, &slot->memory), "allocate NV12");
            checked(vkBindBufferMemory(c->device, slot->buffer, slot->memory, 0), "bind NV12");
            VkMemoryGetFdInfoKHR gi{};
            gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            gi.memory = slot->memory;
            gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            int fd = -1;
            checked(c->get_memory_fd(c->device, &gi, &fd), "export NV12");
            static std::atomic<std::uint64_t> next_id{0};
            slot->frame = std::make_shared<lcomp::DeviceNv12>(
                lcomp::DeviceNv12{fd, req.size, ++next_id, pitch});
            nv12_slots.push_back(std::move(slot));
            return nv12_slots.back().get();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "GPU NV12 output unavailable, reading back RGBA: %s\n", e.what());
            nv12_broken = true;
            return nullptr;
        }
    }

    // Straight RGBA8 canvas to NV12 in `slot`. The caller's fence wait completes the write before
    // the encoder sees the frame.
    // ponytail: host fence wait orders Vulkan before CUDA; an exported timeline semaphore
    // would let the next frame start without that wait.
    void convert_nv12(Image& src, Nv12Slot& slot) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptors;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        checked(vkAllocateDescriptorSets(c->device, &ai, &set), "allocate composite descriptors");
        ++dispatches;
        const VkDescriptorImageInfo source{sampler, src.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo target{slot.buffer, 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (auto& w : writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &source;
        writes[1].dstBinding = 3;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &target;
        vkUpdateDescriptorSets(c->device, 2, writes.data(), 0, nullptr);
        const auto pitch = static_cast<int>(slot.frame->pitch);
        Push push;
        push.rect = {slot.width, slot.height, pitch, pitch * slot.height};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, nv12);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (static_cast<std::uint32_t>(slot.width) + 31) / 32,
                      (static_cast<std::uint32_t>(slot.height) + 15) / 16, 1);
        VkBufferMemoryBarrier written{};
        written.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        written.srcQueueFamilyIndex = written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        written.buffer = slot.buffer;
        written.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &written, 0,
                             nullptr);
    }

    void dispatch(Image& dest, Image& src, VkPipeline pipe, const Push& push, int width, int height,
                  Image* auxiliary = nullptr) {
        if (width <= 0 || height <= 0)
            return;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptors;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        checked(vkAllocateDescriptorSets(c->device, &ai, &set), "allocate composite descriptors");
        ++dispatches;
        const VkDescriptorImageInfo infos[] = {
            {VK_NULL_HANDLE, dest.view, VK_IMAGE_LAYOUT_GENERAL},
            {sampler, src.view, VK_IMAGE_LAYOUT_GENERAL},
            {sampler, auxiliary ? auxiliary->view : src.view, VK_IMAGE_LAYOUT_GENERAL}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(c->device, 3, writes.data(), 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (static_cast<std::uint32_t>(width) + 15) / 16,
                      (static_cast<std::uint32_t>(height) + 15) / 16, 1);
        barrier(dest, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }

    Image& draw(const lcomp::DrawPlan& plan, bool transparent, unsigned depth = 0) {
        if (depth > 64)
            throw std::runtime_error("precomp nesting exceeds GPU limit");
        auto& dest = target(plan.width, plan.height, VK_FORMAT_R16G16B16A16_SFLOAT);
        barrier(dest, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue clear{};
        clear.float32[3] = transparent ? 0.0f : 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dest.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        barrier(dest, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        for (const auto& op : plan.ops) {
            const auto& r = op.rect;
            const int width = std::min(r.x1, plan.width) - std::max(r.x0, 0);
            const int height = std::min(r.y1, plan.height) - std::max(r.y0, 0);
            if (width <= 0 || height <= 0)
                continue;
            Push push;
            push.rect = {r.x0, r.y0, r.x1, r.y1};
            push.crop = op.crop;
            push.flags = {op.quarter, op.flip_x, op.flip_y,
                          !op.filters.empty() ? 2
                          : op.colour         ? 0
                          : op.nested         ? 2
                                              : 1};
            push.opacity[0] = static_cast<float>(std::clamp(op.opacity, 0.0, 1.0));
            push.opacity[1] = static_cast<float>(op.blend);
            Image* input = &dest;
            if (op.filters.size() == 1 && op.filters.front().enabled &&
                op.filters.front().type == "filter.scale" && !op.nested && !op.colour) {
                // Scale uploaded sRGB directly: avoid a linear intermediate and
                // converting every interpolation tap back to sRGB.
                const auto resolved = lcomp::resolve_filter(op.filters.front(), op.image.width,
                                                            op.image.height, plan.time_ns);
                auto cached = std::ranges::find_if(media_images, [&](const auto& entry) {
                    return entry.source == op.source_key && entry.filters == op.filters;
                });
                const auto bytes = static_cast<std::uint64_t>(resolved.width) *
                                   static_cast<std::uint64_t>(resolved.height) * 8;
                if (cached == media_images.end() && op.frame_owner && media_images.size() < 8 &&
                    media_bytes + bytes <= 128ull * 1024 * 1024) {
                    target(resolved.width, resolved.height, VK_FORMAT_R16G16B16A16_SFLOAT);
                    media_images.push_back(
                        {op.source_key, op.filters, {}, std::move(images[--image_at])});
                    media_bytes += bytes;
                    cached = std::prev(media_images.end());
                }
                const bool hit = cached != media_images.end() && cached->frame == op.frame_owner;
                input = cached != media_images.end() ? cached->image.get()
                                                     : &target(resolved.width, resolved.height,
                                                               VK_FORMAT_R16G16B16A16_SFLOAT);
                if (!hit) {
                    barrier(*input, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    Push packed;
                    packed.rect = {resolved.kind, 1, 0, 0};
                    std::memcpy(reinterpret_cast<char*>(&packed) + 16, resolved.values.data(),
                                sizeof(resolved.values));
                    dispatch(*input, source(op), filter, packed, resolved.width, resolved.height);
                    if (cached != media_images.end())
                        cached->frame = op.frame_owner;
                }
            } else if (!op.filters.empty()) {
                auto raw = op;
                raw.filters.clear();
                raw.opacity = 1;
                raw.blend = ldoc::BlendMode::Normal;
                raw.crop = {};
                raw.quarter = 0;
                raw.flip_x = raw.flip_y = false;
                const int w = op.nested ? op.nested->width : op.image.width;
                const int h = op.nested ? op.nested->height : op.image.height;
                raw.rect = {0, 0, w, h};
                lcomp::DrawPlan local;
                local.time_ns = plan.time_ns;
                local.width = w;
                local.height = h;
                local.ops.push_back(std::move(raw));
                local.filters = op.filters;
                input = &draw(local, true, depth + 1);
            } else if (op.colour) {
                const auto& c0 = *op.colour;
                push.colour = {static_cast<float>(c0.r), static_cast<float>(c0.g),
                               static_cast<float>(c0.b), static_cast<float>(c0.a)};
            } else {
                input = op.nested ? &draw(*op.nested, true, depth + 1) : &source(op);
            }
            dispatch(dest, *input, composite, push, width, height);
        }
        Image* result = &dest;
        std::vector<Image*> scratch{&dest};
        for (const auto& effect : plan.filters) {
            if (!effect.enabled ||
                (lcomp::filter_info(effect.type) && lcomp::filter_info(effect.type)->audio))
                continue;
            const auto resolved =
                lcomp::resolve_filter(effect, result->width, result->height, plan.time_ns);
            if (resolved.kind == 0)
                continue;
            if (resolved.kind == 13) {
                if (resolved.values[0] > 0)
                    result = &delayed(*result, resolved);
                continue;
            }
            const auto reuse = std::ranges::find_if(scratch, [&](const auto* image) {
                return image != result && image->width == resolved.width &&
                       image->height == resolved.height;
            });
            Image& output = reuse == scratch.end() ? target(resolved.width, resolved.height,
                                                            VK_FORMAT_R16G16B16A16_SFLOAT)
                                                   : **reuse;
            if (reuse == scratch.end())
                scratch.push_back(&output);
            barrier(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            Push packed;
            static_assert(sizeof(packed) == sizeof(std::array<int, 4>) + sizeof(resolved.values));
            packed.rect = {resolved.kind, 0, 0, 0};
            std::memcpy(reinterpret_cast<char*>(&packed) + 16, resolved.values.data(),
                        sizeof(resolved.values));
            Image* auxiliary = nullptr;
            if (resolved.auxiliary) {
                const auto cached = std::ranges::find_if(auxiliaries, [&](const auto& item) {
                    return item.pixels == resolved.auxiliary;
                });
                if (cached != auxiliaries.end()) {
                    auxiliary = cached->image.get();
                } else {
                    lcomp::DrawOp mask;
                    mask.image.width = resolved.auxiliary->width;
                    mask.image.height = resolved.auxiliary->height;
                    mask.image.stride = resolved.auxiliary->stride;
                    mask.image.borrowed = resolved.auxiliary->read_from();
                    auxiliary = &source(mask);
                    const auto bytes = static_cast<std::uint64_t>(mask.image.width) *
                                       static_cast<std::uint64_t>(mask.image.height) * 8;
                    // Decode caches replace immutable pictures when files change. Keep at
                    // most eight / 128 MiB of GPU copies, counted in the target budget.
                    if (auxiliaries.size() < 8 && auxiliary_bytes + bytes <= 128ull * 1024 * 1024) {
                        auxiliaries.push_back(
                            {resolved.auxiliary, std::move(images[image_at - 1])});
                        auxiliary_bytes += bytes;
                    }
                }
            }
            dispatch(output, *result, filter, packed, resolved.width, resolved.height, auxiliary);
            result = &output;
        }
        return *result;
    }

    lcomp::Picture render(const lcomp::DrawPlan& plan, std::uint32_t max_edge,
                          lcomp::Picture* full_output, std::span<const PresentationTarget> displays,
                          bool nv12_output) {
        const auto begin = std::chrono::steady_clock::now();
        lcomp::Picture out;
        out.width = plan.width;
        out.height = plan.height;
        out.status = plan.status;
        out.gaps = plan.gaps;
        if (plan.status == lcomp::PictureStatus::Unsupported) {
            if (full_output)
                *full_output = out;
            return out;
        }
        std::vector<const lcomp::DrawOp*> media;
        std::vector<std::shared_ptr<const void>> source_owners;
        const auto visit = [&](auto&& self, const lcomp::DrawPlan& current) -> void {
            for (const auto& op : current.ops) {
                if (op.frame_owner)
                    media.push_back(&op);
                if (op.image.owner)
                    source_owners.push_back(op.image.owner);
                if (op.nested)
                    self(self, *op.nested);
            }
        };
        visit(visit, plan);
        std::erase_if(media_images, [&](const auto& entry) {
            return std::ranges::none_of(media, [&](const auto* op) {
                return op->source_key == entry.source && op->filters == entry.filters;
            });
        });
        media_bytes = 0;
        for (const auto& entry : media_images)
            media_bytes += static_cast<std::uint64_t>(entry.image->width) *
                           static_cast<std::uint64_t>(entry.image->height) * 8;
        checked(vkResetCommandPool(c->device, pool, 0), "reset composite commands");
        checked(vkResetDescriptorPool(c->device, descriptors, 0), "reset composite descriptors");
        checked(vkResetFences(c->device, 1, &fence), "reset composite fence");
        image_at = upload_at = 0;
        std::erase_if(auxiliaries, [](const auto& item) { return item.pixels.use_count() == 1; });
        auxiliary_bytes = 0;
        for (const auto& item : auxiliaries)
            auxiliary_bytes += static_cast<std::uint64_t>(item.image->width) *
                               static_cast<std::uint64_t>(item.image->height) * 8;
        std::erase_if(source_images, [&](const auto& entry) {
            return std::ranges::find(source_owners, entry.pixels) == source_owners.end();
        });
        source_bytes = 0;
        for (const auto& entry : source_images)
            source_bytes += static_cast<std::uint64_t>(entry.image->width) *
                           static_cast<std::uint64_t>(entry.image->height) * 4;
        allocated = media_bytes + auxiliary_bytes + source_bytes;
        acquired.clear();
        recycled = std::move(retired);
        retired.clear();
        runtime_gaps.clear();
        std::erase_if(histories, [&](const auto& item) {
            return plan.time_ns > item.second.last &&
                   plan.time_ns - item.second.last > 21000000000ULL;
        });
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(cmd, &bi), "begin composite");
        upload_bytes = 0;
        dispatches = 0;
        stamp_at = 3;
        if (timestamps) {
            vkCmdResetQueryPool(cmd, timestamps, 0, kStamps);
            stamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        }
        Image& canvas = draw(plan, false);
        if (timestamps)
            stamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
        if (!runtime_gaps.empty()) {
            out.gaps.insert(out.gaps.end(), runtime_gaps.begin(), runtime_gaps.end());
            out.status = lcomp::PictureStatus::Partial;
        }
        // Export both sizes from this composition before releasing captured-frame leases.
        Image* full_rgba = nullptr;
        const auto export_picture = [&](lcomp::Picture& picture, std::uint32_t max_size,
                                        std::unique_ptr<Buffer>& destination, bool read = true) {
            const auto edge = static_cast<std::uint32_t>(std::max(plan.width, plan.height));
            if (max_size && max_size < edge) {
                picture.width = std::max(
                    1, static_cast<int>(static_cast<std::uint64_t>(plan.width) * max_size / edge));
                picture.height = std::max(
                    1, static_cast<int>(static_cast<std::uint64_t>(plan.height) * max_size / edge));
            }
            Image& reduced = target(picture.width, picture.height, VK_FORMAT_R8G8B8A8_UNORM);
            barrier(reduced, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            dispatch(reduced, canvas, preview, {}, picture.width, picture.height);
            if (picture.width == plan.width && picture.height == plan.height)
                full_rgba = &reduced;
            if (!read)
                return;
            picture.stride = static_cast<std::uint32_t>(picture.width) * 4;
            const auto size =
                static_cast<std::size_t>(picture.stride) * static_cast<std::size_t>(picture.height);
            buffer(destination, size);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {static_cast<std::uint32_t>(picture.width),
                                static_cast<std::uint32_t>(picture.height), 1};
            vkCmdCopyImageToBuffer(cmd, reduced.image, VK_IMAGE_LAYOUT_GENERAL, destination->buffer,
                                   1, &copy);
        };
        if (full_output) {
            auto pixels = std::move(full_output->pixels);
            *full_output = out;
            full_output->pixels = std::move(pixels);
            // Even sizes only: NV12 has one chroma sample per 2x2 block.
            auto* slot = nv12_output && plan.width % 2 == 0 && plan.height % 2 == 0
                             ? nv12_slot(plan.width, plan.height)
                             : nullptr;
            export_picture(*full_output, 0, full_readback, slot == nullptr);
            if (slot) {
                convert_nv12(*full_rgba, *slot);
                full_output->nv12 = slot->frame;
                full_output->stride = 0;
                full_output->pixels.clear();
            }
        }
        export_picture(out, max_edge, readback);
        // HDMI shares the very same full-resolution sRGB conversion as the encoder. No
        // projector pixels visit host memory, and all copies use this submission/fence.
        if (!displays.empty() && !full_rgba) {
            auto full = out;
            full.width = plan.width;
            full.height = plan.height;
            export_picture(full, 0, full_readback, false);
        }
        if (!displays.empty()) {
            barrier(*full_rgba, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            const auto external =
                c->have_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
            for (const auto& display : displays) {
                if (!display.image || !display.image->image)
                    continue;
                const auto& dst = *display.image;
                VkImageMemoryBarrier b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.image = static_cast<VkImage>(dst.image);
                b.oldLayout = display.foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcQueueFamilyIndex = display.foreign ? external : VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = display.foreign ? c->queue_family : VK_QUEUE_FAMILY_IGNORED;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &b);
                const double scale =
                    std::min(double(dst.width) / plan.width, double(dst.height) / plan.height);
                const int w = std::max(1, int(std::lround(plan.width * scale)));
                const int h = std::max(1, int(std::lround(plan.height * scale)));
                const int x = (int(dst.width) - w) / 2, y = (int(dst.height) - h) / 2;
                // A full-frame blit overwrites every pixel. Clearing first would double
                // display writes, especially expensive for a cross-GPU system-memory buffer.
                if (w != int(dst.width) || h != int(dst.height)) {
                    const VkClearColorValue black{{0, 0, 0, 1}};
                    vkCmdClearColorImage(cmd, b.image, b.newLayout, &black, 1, &b.subresourceRange);
                    b.oldLayout = b.newLayout;
                    b.srcAccessMask = b.dstAccessMask;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &b);
                }
                VkImageBlit blit{};
                blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {plan.width, plan.height, 1};
                blit.dstOffsets[0] = {x, y, 0};
                blit.dstOffsets[1] = {x + w, y + h, 1};
                vkCmdBlitImage(cmd, full_rgba->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.dstAccessMask = 0;
                b.srcQueueFamilyIndex = c->queue_family;
                b.dstQueueFamilyIndex = external;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &b);
            }
        }
        if (timestamps)
            stamp(VK_PIPELINE_STAGE_TRANSFER_BIT, 2);
        for (auto* image : acquired)
            barrier(*image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, false, true);
        VkMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &host, 0, nullptr, 0, nullptr);
        checked(vkEndCommandBuffer(cmd), "end composite");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        checked(vkQueueSubmit(c->queue, 1, &submit, fence), "submit composite");
        const auto submitted = std::chrono::steady_clock::now();
        checked(vkWaitForFences(c->device, 1, &fence, VK_TRUE, 2'000'000'000ULL),
                "GPU fence failed");
        if (timestamps) {
            // One line per frame: a miss is any frame over budget, not every 120th.
            std::uint64_t ticks[kStamps]{};
            if (vkGetQueryPoolResults(c->device, timestamps, 0, stamp_at, sizeof(ticks), ticks,
                                      sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) ==
                VK_SUCCESS) {
                const auto ms = [&](std::uint64_t t) { return static_cast<double>(t) * stamp_ns / 1e6; };
                std::uint64_t upload = 0;
                for (std::uint32_t i = 3; i + 1 < stamp_at; i += 2)
                    upload += ticks[i + 1] - ticks[i];
                std::fprintf(
                    stderr,
                    "GPU frame %u: prepare %.3f wait %.3f | gpu total %.3f upload %.3f "
                    "(%u copies, %.1f MB) composite %.3f readback %.3f | %u dispatches\n",
                    ++timing_frames_,
                    std::chrono::duration<double, std::milli>(submitted - begin).count(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              submitted)
                        .count(),
                    ms(ticks[2] - ticks[0]), ms(upload), (stamp_at - 3) / 2,
                    static_cast<double>(upload_bytes) / 1e6, ms(ticks[1] - ticks[0]),
                    ms(ticks[2] - ticks[1]), dispatches);
            }
        }
        const auto copy_picture = [](lcomp::Picture& picture, const Buffer& source) {
            const auto size =
                static_cast<std::size_t>(picture.stride) * static_cast<std::size_t>(picture.height);
            picture.pixels.resize(size);
            std::memcpy(picture.pixels.data(), source.mapped, size);
        };
        copy_picture(out, *readback);
        if (full_output && !full_output->nv12)
            copy_picture(*full_output, *full_readback);
        std::uint64_t spare_bytes = 0;
        for (std::size_t i = image_at; i < images.size(); ++i)
            if (images[i])
                spare_bytes += static_cast<std::uint64_t>(images[i]->width) *
                               static_cast<std::uint64_t>(images[i]->height) * 8;
        while (images.size() > image_at && allocated + spare_bytes > budget) {
            if (images.back())
                spare_bytes -= static_cast<std::uint64_t>(images.back()->width) *
                               static_cast<std::uint64_t>(images.back()->height) * 8;
            images.pop_back();
        }
        // Keep idle upload buffers for the next decoded frame; cache hits must
        // not free/reallocate mapped Vulkan memory on alternate ticks.
        VkDeviceSize staging_bytes = 0;
        for (const auto& stage : uploads)
            if (stage)
                staging_bytes += stage->size;
        while (uploads.size() > upload_at && staging_bytes > 128ull * 1024 * 1024) {
            staging_bytes -= uploads.back()->size;
            uploads.pop_back();
        }
        foreign.clear();
        acquired.clear();
        return out;
    }

    unsigned timing_frames_ = 0;
};

GpuCompositor::GpuCompositor() : impl_(std::make_unique<Impl>()) {}

GpuCompositor::~GpuCompositor() = default;

std::unique_ptr<GpuCompositor> GpuCompositor::create(VkCtx& device, std::string& error) {
    auto result = std::unique_ptr<GpuCompositor>(new GpuCompositor());
    result->impl_->c = &device;
    try {
        result->impl_->init();
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    return result;
}

std::optional<lcomp::Picture> GpuCompositor::render(const lcomp::DrawPlan& plan,
                                                    std::uint32_t max_edge, std::string& error,
                                                    lcomp::Picture* full_output,
                                                    std::span<const PresentationTarget> displays,
                                                    bool nv12_output) {
    try {
        return impl_->render(plan, max_edge, full_output, displays, nv12_output);
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}
} // namespace libertas::rhi

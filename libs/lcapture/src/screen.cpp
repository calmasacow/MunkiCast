// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One screen, captured into a ring of our own buffers. Consumers lease completed slots.
//
// The sequence here is `capture-spike`'s, with the measuring removed. Keeping it in one place
// matters because the order is not obvious and most of it is only discoverable by failing:
// the session has to exist before the compositor will say what it can deliver, the modifier
// intersection is the whole negotiation (we allocate, so there is no second round), and the
// buffers have to be registered as slots before any of them can be captured into.

#include <lcapture/screen.hpp>

#include <lrhi/vulkan.hpp>

#include <lcore/log.hpp>

#include <algorithm>
#include <atomic>
#include <thread>
#include <format>

namespace munkicast::capture {

namespace {

} // namespace

std::vector<WlOutputInfo> list_screens(std::string& error) {
    WlCapPtr cap = wlcap_open(error);
    if (!cap)
        return {};
    // Copied out before the connection closes: the list is owned by the connection, and the
    // caller wants an answer rather than a handle to keep alive.
    return wlcap_outputs(*cap);
}

struct ScreenCapture::Impl {
    rhi::VkCtx* device = nullptr;
    WlCapPtr cap;
    std::vector<std::shared_ptr<rhi::VkExportedImage>> ring;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t slot = 0;
    std::thread thread;
    std::atomic<bool> running{false};

    ~Impl() {
        cap.reset();
        ring.clear();
    }
};

ScreenCapture::ScreenCapture() : impl_(std::make_unique<Impl>()) {}
ScreenCapture::~ScreenCapture() {
    stop();
}

std::uint32_t ScreenCapture::width() const { return impl_->width; }
std::uint32_t ScreenCapture::height() const { return impl_->height; }

std::unique_ptr<ScreenCapture> ScreenCapture::open(rhi::VkCtx& device, const std::string& connector,
                                                   bool with_cursor, std::string& error, bool window) {
    auto self = std::unique_ptr<ScreenCapture>(new ScreenCapture());
    self->impl_->device = &device;

    self->impl_->cap = wlcap_open(error);
    if (!self->impl_->cap)
        return nullptr;

    WlCapCaps caps;
    if (!wlcap_begin_session(*self->impl_->cap, connector, with_cursor, caps, error, window))
        return nullptr;
    if (!rhi::vk_matches_drm_device(device, caps.dmabuf_device)) {
        error = "capture compositor uses another DRM device; cross-device capture is not validated";
        return nullptr;
    }
    self->impl_->width = caps.width;
    self->impl_->height = caps.height;

    // Four, for the reason the header gives: one buffer serialises the compositor against us.
    constexpr std::uint32_t kSlots = 4;

    for (const WlCapFormat& format : caps.formats) {
        const std::vector<std::uint64_t> ours =
            rhi::vk_supported_modifiers(device, format.drm_format);
        std::vector<std::uint64_t> common;
        for (std::uint64_t m : format.modifiers)
            if (std::find(ours.begin(), ours.end(), m) != ours.end())
                common.push_back(m);
        if (common.empty())
            continue;

        std::vector<rhi::VkExportedImage> ring(kSlots);
        bool ok = true;
        for (std::uint32_t i = 0; i < kSlots && ok; ++i) {
            // host_memory false: the compositor and we are on the same device here. A second GPU
            // would need the system-RAM round trip ADR-0019 measured, and that is an encode-path
            // decision rather than a preview one.
            if (!rhi::vk_create_exportable(device, caps.width, caps.height, format.drm_format,
                                           common, false, ring[i], error))
                ok = false;
            else if (!wlcap_add_slot(*self->impl_->cap, ring[i], format.drm_format, error))
                ok = false;
        }
        if (!ok) {
            for (rhi::VkExportedImage& img : ring)
                rhi::vk_destroy_exportable(device, img);
            continue;
        }
        for (auto& img : ring) {
            self->impl_->ring.emplace_back(new rhi::VkExportedImage(img),
                                           [&device](rhi::VkExportedImage* image) {
                                               rhi::vk_destroy_exportable(device, *image);
                                               delete image;
                                           });
        }
        lc::info("capture: {}x{} {} on {}", caps.width, caps.height,
                    fourcc_str(format.drm_format), connector.empty() ? "the first screen" : connector);
        return self;
    }

    // Every format the compositor offered was one the device cannot allocate with. Naming both
    // sides, because this is the failure that looks like "capture is broken" and is actually a
    // driver and compositor that have no modifier in common.
    error = std::format("no format the compositor offers ({}) can be allocated on this device",
                        caps.formats.size());
    return nullptr;
}

std::shared_ptr<const DmabufImage> ScreenCapture::capture_image(std::string& error) {
    for (std::size_t offset = 0; offset < impl_->ring.size(); ++offset) {
        const auto index = (impl_->slot + offset) % impl_->ring.size();
        if (impl_->ring[index].use_count() != 1)
            continue;
        if (!wlcap_capture_slot(*impl_->cap, index, impl_->width, impl_->height, nullptr, error, 0,
                                nullptr))
            return {};
        impl_->slot = static_cast<std::uint32_t>((index + 1) % impl_->ring.size());
        return impl_->ring[index];
    }
    error = "all capture slots are leased; consumer exceeded snapshot budget";
    return {};
}

bool ScreenCapture::read_frame(std::vector<std::uint8_t>& out, std::string& error) {
    const auto image = capture_image(error);
    return image && rhi::vk_readback_exported(*impl_->device, *image, true, out, error);
}

void ScreenCapture::start_images(std::function<void(std::shared_ptr<const DmabufImage>)> on_frame) {
    if (impl_->thread.joinable())
        return;
    impl_->running = true;
    impl_->thread = std::thread([this, on_frame = std::move(on_frame)] {
        std::string error;
        while (impl_->running) {
            auto image = capture_image(error);
            if (!image) {
                if (impl_->running)
                    lc::warn("capture stopped: {}", error);
                break;
            }
            on_frame(std::move(image));
        }
        on_frame({});
    });
}

void ScreenCapture::start(std::function<void(std::vector<std::uint8_t>&&)> on_frame) {
    if (impl_->thread.joinable())
        return;
    impl_->running = true;
    impl_->thread = std::thread([this, on_frame = std::move(on_frame)] {
        std::string error;
        while (impl_->running) {
            std::vector<std::uint8_t> pixels;
            if (!read_frame(pixels, error)) {
                if (!impl_->running)
                    break;
                // A failed capture is not a reason to spin: the session is usually gone (the
                // output unplugged, the compositor restarted) and retrying at speed would turn
                // one fault into a busy loop. Said once, then the thread stops; the caption
                // reports the source as not drawn, which is the honest outcome.
                lc::warn("capture stopped: {}", error);
                break;
            }
            on_frame(std::move(pixels));
        }
    });
}

void ScreenCapture::stop() {
    impl_->running = false;
    if (impl_->thread.joinable()) {
        // The thread is parked in wl_display_dispatch waiting for a frame that may never come on
        // a still screen, so asking it to stop is not enough to wake it. Closing the display's
        // descriptor is what ends that wait.
        wlcap_wake(*impl_->cap);
        impl_->thread.join();
    }
}

} // namespace munkicast::capture

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcapture/wlcap.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace munkicast::rhi {
struct VkCtx;
}

namespace munkicast::capture {

/// The monitors the compositor says are connected, right now.
///
/// Opens a connection, reads the output list, closes it. No GPU, no capture session, no device:
/// enumerating outputs is a registry read, which is why the UI can call this directly rather than
/// asking the render process for a list it would then have to keep fresh.
///
/// Empty with `error` set when the compositor has no capture protocol -- which is the honest
/// answer for "which screens can I capture", since on such a compositor the answer is none.
[[nodiscard]] std::vector<WlOutputInfo> list_screens(std::string& error);

/// Captures a named screen into a ring of client-allocated dmabufs (ADR-0011).
/// `start_images` publishes leased slots without readback; `start` is the CPU convenience path.
/// A slot is never reused while leased. The allocating device must outlive capture and leases.
class ScreenCapture {
public:
    /// Opens a capture session for `connector` ("DP-1", or empty for the first output --
    /// ADR-0041), allocating on `device`.
    ///
    /// Returns nothing with `error` set when the compositor has no capture protocol, the named
    /// output is not there, or no modifier is common to the compositor and the device. All three
    /// are things the caller reports rather than retries.
    static std::unique_ptr<ScreenCapture> open(rhi::VkCtx& device, const std::string& connector,
                                               bool with_cursor, std::string& error, bool window = false);

    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /// Width and height the compositor will deliver, which is the screen's own size.
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;

    /// Waits for the next frame and reads it back as BGRA8. Blocks until one arrives.
    ///
    /// Unbounded on purpose. ADR-0013 Consequence 4: a bounded wait must never be used to sample,
    /// because destroying a pending frame makes the compositor answer the next request at once
    /// with unchanged content -- so a caller that times out and re-asks measures its own timeout
    /// and calls it a frame rate. The only honest shapes are "wait" and "give up", and a preview
    /// that wants both needs a thread, not a timeout.
    ///
    /// Call this from `start()`'s thread rather than from anything holding a frame deadline.
    bool read_frame(std::vector<std::uint8_t>& out, std::string& error);

    /// Runs the capture in its own thread, calling `on_frame` with each one as it arrives.
    ///
    /// This is the shape the protocol wants. The compositor delivers when the screen changes
    /// (ADR-0013 again: capture is damage-driven), so the thread blocks on that and a still
    /// screen simply costs nothing until it moves. Whoever is drawing reads the latest frame
    /// whenever it happens to need one, and the two rates stop being the same number.
    ///
    /// `on_frame` runs on the capture thread, and is handed the buffer to take rather than to
    /// copy: it may move from it. The thread allocates a fresh one for the next frame, which
    /// costs an allocation instead of a 33 MB memcpy at 4K.
    ///
    /// The pixels are **BGRA**, as the compositor delivered them. Deliberately not converted
    /// here: a separate pass over the whole buffer measured 5.1 ms a frame at 4K, more than the
    /// composite it was feeding, and whoever samples the frame is already touching every pixel
    /// it will actually use. Converting on the way in means converting pixels that are about to
    /// be scaled away.
    void start(std::function<void(std::vector<std::uint8_t>&&)> on_frame);

    /// Publishes a lease on a completed slot; no readback or Vulkan submission on this thread.
    /// Keep at most one current frame and one renderer snapshot. Device must outlive all leases.
    void start_images(std::function<void(std::shared_ptr<const DmabufImage>)> on_frame);

    /// Stops the thread and waits for it. Called by the destructor; safe to call twice.
    void stop();

private:
    ScreenCapture();
    std::shared_ptr<const DmabufImage> capture_image(std::string& error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace munkicast::capture

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcapture/dmabuf.hpp>

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace munkicast::capture {

/// One output, as the compositor names it. `name` is the connector ("DP-1"), which is the same
/// identifier Hyprland and the portal's `mapping_id` use, so sources are comparable across
/// backends and stable across restarts - unlike a portal picker selection.
struct WlOutputInfo {
    std::string name;
    std::string description;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t refresh_mhz = 0;
};

struct WlWindowInfo {
    std::string id, title, app_id;
};
std::vector<WlWindowInfo> list_windows(std::string& error);
bool window_size(const std::string& id, int& width, int& height, std::string& error);

/// A DRM format plus the modifiers the compositor will accept for it.
struct WlCapFormat {
    std::uint32_t drm_format = 0;
    std::vector<std::uint64_t> modifiers;
};

/// What the compositor says it can deliver for a capture session. Note this comes from the
/// compositor itself, not from an intermediary - which is the whole reason for this backend.
struct WlCapCaps {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t dmabuf_device = 0; ///< dev_t of the device the buffers must be allocated on
    std::vector<WlCapFormat> formats;
    std::vector<std::uint32_t> shm_formats;
};

struct WlCap;
void wlcap_close(WlCap*);
struct WlCapDeleter {
    void operator()(WlCap* p) const { wlcap_close(p); }
};
using WlCapPtr = std::unique_ptr<WlCap, WlCapDeleter>;

/// Picks the output a screen source names, from the list the compositor advertised.
///
/// Split out of `wlcap_begin_session` and made pure so it can be tested without a compositor: the
/// suite runs headless over SSH, where no output list exists, and this is the half of "which
/// screen" that is ours rather than the protocol's (ADR-0041).
///
/// `name` is a connector as the compositor spells it ("DP-1"). Empty means the first output, which
/// is what a one-screen machine wants and what a source that has not been pointed anywhere yet
/// gets. A name that matches nothing is an error and not a fallback: silently capturing a
/// different screen than the one the project names is the failure an operator cannot see.
///
/// Returns the index into `outputs`, or nothing with `error` set.
std::optional<std::size_t> wlcap_select_output(const std::vector<WlOutputInfo>& outputs,
                                               const std::string& name, std::string& error);

/// Connects to the compositor and binds the capture globals.
/// Fails with a specific message when a required global is absent, so an unsupported compositor is
/// distinguishable from a broken one.
WlCapPtr wlcap_open(std::string& error);

const std::vector<WlOutputInfo>& wlcap_outputs(WlCap& cap);

/// Creates a capture session for `output_name` (empty = the first output) and waits for the
/// compositor to describe it. No picker, no dialog, no user interaction.
bool wlcap_begin_session(WlCap& cap, const std::string& output_name, bool with_cursor,
                         WlCapCaps& out, std::string& error, bool window = false);

/// Wraps an exported Vulkan image as a wl_buffer and captures exactly one frame into it.
/// On success the VkImage holds the screen contents. Blocks until `ready` or `failed`.
///
/// `slot` lets a caller rotate between several images. Capturing repeatedly into ONE image
/// serializes the compositor against us - it cannot draw the next frame while we hold its only
/// destination - so a single-buffer measurement understates the achievable rate and invents a
/// latency tail that the protocol does not have.
bool wlcap_capture_frame(WlCap& cap, const DmabufImage& img, std::uint32_t drm_format,
                         std::string& error);

/// Registers `img` as a reusable buffer slot. The wl_buffer is created once and kept, rather than
/// built and destroyed per frame, which is what the real capture worker must do.
bool wlcap_add_slot(WlCap& cap, const DmabufImage& img, std::uint32_t drm_format,
                    std::string& error);

/// Captures one frame into slot `index`, blocking until that frame is ready.
///
/// `out_presented_ns` receives the compositor's presentation timestamp for the frame, when it
/// sends one. That is the only way to tell "the capture path is slow" from "the screen did not
/// change": inter-arrival time measures the latter, and on an idle desktop it measures nothing
/// else at all.
///
/// `idle_timeout_ms` bounds the wait, 0 meaning for ever. Capture is damage-driven (ADR-0013), so
/// on a screen nobody is touching the compositor is *right* to send nothing, and an unbounded wait
/// there is a tool that hangs in silence - indistinguishable, over SSH, from one that has crashed.
/// On expiry this returns false with `*out_idle` set and an `error` that names that cause, so a
/// caller can tell a still screen from a real capture fault without parsing the string.
///
/// A timeout ends the wait; it is not a way to poll. Measured on argus under Hyprland: once the
/// pending frame is abandoned, the *next* request is answered at once with unchanged content, so a
/// caller that times out and asks again receives a frame per timeout that the screen never drew.
/// To keep waiting, pass 0 and watch from elsewhere - which is what encode-spike does.
bool wlcap_capture_slot(WlCap& cap, std::size_t index, std::uint32_t width, std::uint32_t height,
                        std::uint64_t* out_presented_ns, std::string& error,
                        std::uint32_t idle_timeout_ms = 0, bool* out_idle = nullptr);

/// Interrupts a blocking capture wait, so a thread parked on a still screen can be stopped.
///
/// Safe to call from another thread, and from any state: it writes one byte to a pipe the wait
/// polls alongside the display. The interrupted call returns false with an error saying capture
/// was stopped, which is a shutdown and not a fault.
void wlcap_wake(WlCap& cap);

/// The explanation for a wait that ran out with no frame. One wording for every tool, because the
/// point of it is that the cause - a still screen, not a fault - reads the same wherever it shows up.
std::string wlcap_idle_reason(std::uint32_t idle_timeout_ms);

/// major:minor of the dmabuf device, for matching against a Vulkan device's DRM properties.
std::string wlcap_device_str(std::uint64_t dev);

} // namespace munkicast::capture

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcapture/dmabuf.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace munkicast::spike {

// These moved to libs/lcapture beside DmabufImage: both capture backends describe a frame the
// same way, and the import side must not be able to tell which one produced it. Aliased rather
// than renamed so the PipeWire half of this spike stays as it was measured.
using DmabufPlane = munkicast::capture::DmabufPlane;

using CapturedFrame = munkicast::capture::CapturedFrame;

struct NegotiatedFormat {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t spa_format = 0;
    std::uint32_t drm_format = 0;
    std::uint64_t modifier = 0;
    bool have_modifier = false;
    std::int32_t fps_num = 0;
    std::int32_t fps_den = 1;
};

using munkicast::capture::kModifierInvalid;
inline constexpr std::uint64_t kModifierLinear = 0;

/// A long-lived PipeWire connection. One per process: pw_init/pw_deinit are not reliably
/// re-entrant, and the fd from OpenPipeWireRemote is effectively single-use, so a sweep must reuse
/// one connection and create/destroy only streams inside it. This is also the shape the real
/// capture worker needs, since it holds a persistent connection across reconfigurations.
struct PwSession;
void pw_session_close(PwSession*);
struct PwSessionDeleter {
    void operator()(PwSession* p) const { pw_session_close(p); }
};
using PwSessionPtr = std::unique_ptr<PwSession, PwSessionDeleter>;

/// Takes ownership of `pipewire_fd`.
PwSessionPtr pw_session_open(int pipewire_fd, std::string& error);

struct StreamConfig {
    std::uint32_t node_id = 0;
    /// Modifiers to advertise, most preferred first. Empty => shm only.
    std::vector<std::uint64_t> modifiers;
    /// Diagnostic: when non-zero, advertise a single fully-fixed format at this size instead of
    /// negotiable choices, to bisect which property a compositor rejects.
    std::uint32_t mirror_width = 0;
    std::uint32_t mirror_height = 0;
    std::uint32_t max_frames = 30;  ///< stop after this many
    /// Called per frame on the PipeWire thread.
    std::function<void(const CapturedFrame&)> on_frame;
};

/// Runs one negotiation attempt inside an existing session.
///
/// `timeout_ms` is a **no-progress watchdog**, re-armed on every frame - not a total time budget.
/// So a variant that cannot negotiate gives up quickly instead of hanging a sweep, while a slow
/// per-frame consumer (stage 3 imports and reads back on the GPU) is never cut off mid-run.
/// A timeout *after* frames have flowed is reported as success with a short frame count.
bool pw_try_variant(PwSession& session, const StreamConfig& cfg, NegotiatedFormat& out_format,
                    std::string& error, int timeout_ms = 4000);


/// Why a node is not usable. Keeping these apart is load-bearing, not cosmetic: a node that never
/// appears in the registry is a dead session (a phantom id from a stale restore token), whereas a
/// node that exists but offers no formats is a genuine source problem. Both surface downstream as
/// "no more input formats", which is why they were conflated for so long. See docs/adr/0009.
enum class NodeReadiness {
    Ready,       ///< node exists and advertises >= 1 SPA_PARAM_EnumFormat
    NodeMissing, ///< node id was never registered: the session is a phantom, discard it
    NoFormats,   ///< node exists but published no formats within the timeout
};

/// What a source node says it can produce, read structurally rather than printed.
///
/// `preferred` is the compositor's *current* layout: a modifier it published as a single fixed
/// value rather than as a choice. It matters because a compositor will advertise modifiers it
/// cannot actually allocate with, so its own preference is the candidate most likely to work -
/// far more meaningful than the order a Vulkan device happens to enumerate modifiers in.
struct NodeFormats {
    std::vector<std::uint64_t> offered;   ///< every modifier the node mentions, de-duplicated
    std::vector<std::uint64_t> preferred; ///< those given as a single fixed value, in param order
};

/// Polls `node_id` until it advertises at least one SPA_PARAM_EnumFormat, and reports *why* if it
/// never does.
///
/// portal ScreenCast.Start returns a node id before the node has populated its format params, and
/// for a restored session it may return a node id that is never registered at all. Every capture
/// consumer must validate here, before trusting the session.
NodeReadiness pw_probe_node(PwSession& session, std::uint32_t node_id,
                            NodeFormats* out_formats = nullptr, int timeout_ms = 5000);

/// `pw_probe_node(...) == NodeReadiness::Ready`.
bool pw_wait_node_ready(PwSession& session, std::uint32_t node_id, int timeout_ms = 5000);

/// Human-readable NodeReadiness, for logs.
const char* node_readiness_str(NodeReadiness r);

/// Prints the EnumFormat params a node offers, over an already-open session so it can be called at
/// the moment of a negotiation failure. This is how to learn which DRM format modifiers a
/// compositor will really hand us, rather than guessing. There is deliberately no variant that
/// opens its own connection: the portal fd is single-use, so one session must serve everything.
bool pw_dump_formats(PwSession& session, std::uint32_t node_id);

/// Human-readable fourcc, e.g. "AR24".
using munkicast::capture::fourcc_str;
/// Decodes the vendor nibble of a DRM format modifier, e.g. "NVIDIA(0x...)".
std::string modifier_str(std::uint64_t modifier);

} // namespace munkicast::spike

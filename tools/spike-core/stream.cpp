// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PipeWire consumer for a portal ScreenCast node, with explicit DRM format-modifier negotiation.
//
// The important difference from OBS's approach: we advertise the modifier list as a MANDATORY
// enumerated choice, so the server either agrees on one of our modifiers or renegotiates - rather
// than accepting a format and failing at import time. See docs/adr/0007 and the plan's section 3.

#include "stream.hpp"

#include <lcore/log.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/debug/format.h>
#include <spa/utils/result.h>

#include <drm_fourcc.h>

#include <ctime>

namespace munkicast::spike {



std::string modifier_str(std::uint64_t m) {
    if (m == kModifierInvalid)
        return "INVALID (implicit/driver-chosen)";
    if (m == kModifierLinear)
        return "LINEAR";
    const std::uint8_t vendor = static_cast<std::uint8_t>(m >> 56);
    const char* name = "unknown";
    switch (vendor) {
    case 0x01: name = "INTEL"; break;
    case 0x02: name = "AMD"; break;
    case 0x03: name = "NVIDIA"; break;
    case 0x04: name = "SAMSUNG"; break;
    case 0x08: name = "ARM"; break;
    case 0x0a: name = "AMLOGIC"; break;
    default: break;
    }
    return std::format("{}(0x{:x})", name, m);
}

namespace {

// SPA's SPA_RECTANGLE/SPA_FRACTION are C compound literals; C++ cannot take their address, so
// these are named constants instead.
const spa_rectangle kSizeDefault = {1920, 1080};
const spa_rectangle kSizeMin = {1, 1};
const spa_rectangle kSizeMax = {8192, 8192};
const spa_fraction kFpsDefault = {0, 1};  // 0/1 = "source-driven"; a nonzero default fails to
                                          // intersect a source whose framerate is fixed at 0/1.
const spa_fraction kFpsMin = {0, 1};
const spa_fraction kFpsMax = {360, 1};

/// SPA video format -> DRM fourcc. Only the formats a compositor realistically offers.
std::uint32_t spa_to_drm(std::uint32_t spa) {
    switch (spa) {
    case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_ARGB: return DRM_FORMAT_BGRA8888;
    case SPA_VIDEO_FORMAT_xRGB: return DRM_FORMAT_BGRX8888;
    case SPA_VIDEO_FORMAT_ABGR: return DRM_FORMAT_RGBA8888;
    case SPA_VIDEO_FORMAT_xBGR: return DRM_FORMAT_RGBX8888;
    default: return 0;
    }
}

const char* spa_format_name(std::uint32_t f) {
    switch (f) {
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    case SPA_VIDEO_FORMAT_ARGB: return "ARGB";
    case SPA_VIDEO_FORMAT_xRGB: return "xRGB";
    case SPA_VIDEO_FORMAT_ABGR: return "ABGR";
    case SPA_VIDEO_FORMAT_xBGR: return "xBGR";
    default: return "?";
    }
}

/// Per-attempt state. One of these exists while a single stream is negotiating/pumping.
struct Attempt {
    PwSession* session = nullptr;
    const StreamConfig* cfg = nullptr;
    NegotiatedFormat* fmt = nullptr;
    std::string error;

    pw_stream* stream = nullptr;
    spa_hook stream_listener{};

    std::uint32_t frames = 0;
    bool format_logged = false;
    bool fixated = false;
    bool timed_out = false;
    int watchdog_ms = 0; ///< the deadline is per-step, not per-run; see rearm_watchdog()
    std::array<std::uint8_t, 2048> fixate_buf{};

    void stop(); // defined after PwSession is complete
};

/// Builds one EnumFormat POD advertising our modifier list as a mandatory, unfixated choice.
spa_pod* build_format_with_modifiers(spa_pod_builder* b, std::uint32_t spa_format,
                                     const std::vector<std::uint64_t>& mods) {
    spa_pod_frame obj;
    spa_pod_builder_push_object(b, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);

    // MANDATORY (we require dmabuf) + DONT_FIXATE (server answers with the intersection, which we
    // then fixate in on_param_changed).
    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                         SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_frame choice;
    spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
    spa_pod_builder_long(b, static_cast<int64_t>(mods.front())); // default
    for (std::uint64_t m : mods)
        spa_pod_builder_long(b, static_cast<int64_t>(m));
    spa_pod_builder_pop(b, &choice);

    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(&kSizeDefault, &kSizeMin, &kSizeMax),
                        SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&kFpsDefault, &kFpsMin, &kFpsMax), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &obj));
}

/// Builds an EnumFormat POD with no modifier property: the shm/MemPtr fallback.
spa_pod* build_format_shm(spa_pod_builder* b) {
    return static_cast<spa_pod*>(spa_pod_builder_add_object(
        b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA,
                               SPA_VIDEO_FORMAT_RGBx),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&kSizeDefault, &kSizeMin, &kSizeMax),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&kFpsDefault, &kFpsMin, &kFpsMax)));
}

/// Builds an EnumFormat POD with the modifier pinned to one value. Required by the negotiation
/// protocol: the server answers our DONT_FIXATE choice with the intersection, and we must come back
/// with exactly one modifier before buffers can be allocated.
spa_pod* build_format_fixated(spa_pod_builder* b, std::uint32_t spa_format, std::uint64_t modifier,
                              spa_rectangle size, spa_fraction fps) {
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);
    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
    spa_pod_builder_long(b, static_cast<int64_t>(modifier));
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
                        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &f));
}

/// Mirrors a source's format exactly: all properties fixed, nothing negotiable. Diagnostic used to
/// bisect which negotiable property a compositor rejects.
spa_pod* build_format_mirror(spa_pod_builder* b, std::uint32_t spa_format, spa_rectangle size,
                             bool with_modifier, std::uint64_t modifier) {
    spa_pod_frame obj;
    spa_pod_builder_push_object(b, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);
    if (with_modifier) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(b, static_cast<int64_t>(modifier));
    }
    const spa_fraction fps{0, 1};
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
                        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &obj));
}

void on_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* err) {
    auto* r = static_cast<Attempt*>(data);
    lc::info("stream state {} -> {}{}{}", pw_stream_state_as_string(old),
             pw_stream_state_as_string(state), (err != nullptr) ? ": " : "",
             (err != nullptr) ? err : "");
    if (state == PW_STREAM_STATE_ERROR) {
        r->error = (err != nullptr) ? err : "stream error";
        r->stop();
    }
}

/// Every modifier the server left in a (possibly unfixated) modifier property.
///
/// This cannot be read with `spa_format_video_raw_parse`: when the property is still an unfixated
/// CHOICE_Enum - which is exactly the state the two-round dance leaves it in - that helper cannot
/// reduce the list to one value and silently leaves `modifier` at 0. Zero is a valid modifier
/// (DRM_FORMAT_MOD_LINEAR), so the failure is indistinguishable from the server having chosen
/// LINEAR, and the next step fixates a modifier the source never offered.
std::vector<std::uint64_t> extract_modifiers(const spa_pod_prop* prop) {
    std::vector<std::uint64_t> out;
    if (prop == nullptr)
        return out;
    std::uint32_t n_vals = 0;
    std::uint32_t choice = 0;
    const spa_pod* vals = spa_pod_get_values(&prop->value, &n_vals, &choice);
    if (vals == nullptr || vals->type != SPA_TYPE_Long)
        return out;
    const auto* body = static_cast<const std::uint64_t*>(SPA_POD_BODY_CONST(vals));
    for (std::uint32_t i = 0; i < n_vals; ++i) {
        // A CHOICE_Enum repeats its preferred value at index 0, so de-duplicate.
        if (std::find(out.begin(), out.end(), body[i]) == out.end())
            out.push_back(body[i]);
    }
    return out;
}

/// Picks the modifier to fixate: the first of *our* preferences that the source actually offers.
///
/// Our list is ordered by preference and contains only modifiers this device can import, so
/// honouring our order (rather than the server's) is what makes negotiation mean something.
///
/// `DRM_FORMAT_MOD_INVALID` is never chosen. It is not a modifier - it is the *absence* of one,
/// meaning "implicit, driver's choice" - so fixating it asks the compositor for something
/// meaningless. Measured: fixating INVALID on DP-1 reaches `streaming`, the node goes
/// `running -> idle` within a millisecond having allocated nothing, and the stream then errors
/// with `no more input formats`. A source that offers only INVALID is telling us it has no
/// explicit-modifier path, and the right answer is to drop the modifier property altogether
/// (the implicit/shm format we always advertise alongside), not to fixate the sentinel.
bool choose_modifier(const std::vector<std::uint64_t>& offered,
                     const std::vector<std::uint64_t>& prefs, std::uint64_t& out) {
    for (std::uint64_t p : prefs) {
        if (p == kModifierInvalid)
            continue;
        if (std::find(offered.begin(), offered.end(), p) != offered.end()) {
            out = p;
            return true;
        }
    }
    return false;
}

void on_param_changed(void* data, std::uint32_t id, const spa_pod* param) {
    auto* r = static_cast<Attempt*>(data);
    if (param == nullptr || id != SPA_PARAM_Format)
        return;

    spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_video || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) {
        r->error = "failed to parse negotiated video format";
        r->stop();
        return;
    }

    const spa_pod_prop* mod_prop =
        spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
    const bool have_mod = (mod_prop != nullptr);
    const bool dont_fixate =
        have_mod && (mod_prop->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) != 0;
    const std::vector<std::uint64_t> offered = extract_modifiers(mod_prop);

    r->fmt->width = info.info.raw.size.width;
    r->fmt->height = info.info.raw.size.height;
    r->fmt->spa_format = info.info.raw.format;
    r->fmt->drm_format = spa_to_drm(info.info.raw.format);
    // Read the modifier out of the property ourselves; see extract_modifiers().
    r->fmt->modifier = !offered.empty() ? offered.front()
                                        : (have_mod ? info.info.raw.modifier : kModifierInvalid);
    r->fmt->have_modifier = have_mod;
    r->fmt->fps_num = static_cast<std::int32_t>(info.info.raw.framerate.num);
    r->fmt->fps_den = static_cast<std::int32_t>(info.info.raw.framerate.denom);

    if (!r->format_logged) {
        r->format_logged = true;
        lc::info("server offers {}x{} {} (drm {}) fps {}/{} with {} modifier(s){}", r->fmt->width,
                 r->fmt->height, spa_format_name(r->fmt->spa_format),
                 fourcc_str(r->fmt->drm_format), r->fmt->fps_num, r->fmt->fps_den, offered.size(),
                 dont_fixate ? ", unfixated" : "");
        for (std::uint64_t m : offered)
            lc::info("  offered: {}", modifier_str(m));
    }

    // The dmabuf negotiation dance: if the server left the modifier unfixated, we must answer with
    // exactly one modifier before it can allocate buffers. Skipping this is what produces
    // "no more input formats".
    if (have_mod && !r->fixated && dont_fixate) {
        r->fixated = true;
        std::uint64_t chosen = 0;
        if (!choose_modifier(offered, r->cfg->modifiers, chosen)) {
            // A real, reportable mismatch rather than a mystery: the source and this device share
            // no modifier for this format. Say so, with both lists, instead of letting it surface
            // as "no more input formats" three callbacks later.
            std::string off;
            for (std::uint64_t m : offered)
                off += (off.empty() ? "" : " ") + modifier_str(m);
            std::string ours;
            for (std::uint64_t m : r->cfg->modifiers)
                ours += (ours.empty() ? "" : " ") + modifier_str(m);
            const bool only_invalid = offered.size() == 1 && offered.front() == kModifierInvalid;
            r->error = std::format(
                "no common explicit DRM format modifier for {}: source offers [{}], we can import "
                "[{}]{}",
                fourcc_str(r->fmt->drm_format), off, ours,
                only_invalid ? " - the source offers only DRM_FORMAT_MOD_INVALID, i.e. no "
                               "explicit-modifier path at all; retry without a modifier property"
                             : "");
            r->stop();
            return;
        }
        r->fmt->modifier = chosen;
        const spa_rectangle size = {r->fmt->width, r->fmt->height};
        const spa_fraction fps = {static_cast<std::uint32_t>(r->fmt->fps_num),
                                  static_cast<std::uint32_t>(r->fmt->fps_den == 0
                                                                 ? 1
                                                                 : r->fmt->fps_den)};
        spa_pod_builder fb = SPA_POD_BUILDER_INIT(
            r->fixate_buf.data(), static_cast<std::uint32_t>(r->fixate_buf.size()));
        const spa_pod* fixed[1];
        fixed[0] = build_format_fixated(&fb, r->fmt->spa_format, r->fmt->modifier, size, fps);
        lc::info("fixating modifier {} for {}", modifier_str(r->fmt->modifier),
                 spa_format_name(r->fmt->spa_format));
        if (pw_stream_update_params(r->stream, fixed, 1) < 0) {
            r->error = "pw_stream_update_params (fixate) failed";
            r->stop();
        }
        return;
    }

    // Tell the server what buffers we can take. Allowing DmaBuf is what gets us zero-copy;
    // MemFd/MemPtr remain permitted so a compositor that cannot do dmabuf still works.
    std::uint32_t data_types = (1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd);
    if (have_mod)
        data_types |= (1u << SPA_DATA_DmaBuf);

    std::array<std::uint8_t, 1024> buf{};
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf.data(), static_cast<std::uint32_t>(buf.size()));
    const spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_types)));

    if (pw_stream_update_params(r->stream, params, 1) < 0) {
        r->error = "pw_stream_update_params failed";
        r->stop();
    }
}

/// Pushes the attempt deadline out by `watchdog_ms`.
///
/// The deadline is a *no-progress* watchdog, not a total time budget. One number cannot serve both
/// jobs: a negotiation failure must be reported in seconds, while stage 3 does a full GPU import
/// and readback per frame and is legitimately far slower than the capture rate. Re-arming on every
/// frame gives "fail fast when nothing is happening, wait as long as it takes when it is".
void rearm_watchdog(Attempt& a);

void on_process(void* data) {
    auto* r = static_cast<Attempt*>(data);

    pw_buffer* b = pw_stream_dequeue_buffer(r->stream);
    if (b == nullptr)
        return;

    spa_buffer* sb = b->buffer;
    CapturedFrame frame;
    frame.sequence = r->frames;
    frame.width = r->fmt->width;
    frame.height = r->fmt->height;
    frame.drm_format = r->fmt->drm_format;
    frame.modifier = r->fmt->modifier;

    if (sb->n_datas > 0)
        frame.is_dmabuf = (sb->datas[0].type == SPA_DATA_DmaBuf);

    for (std::uint32_t i = 0; i < sb->n_datas; ++i) {
        const spa_data& d = sb->datas[i];
        DmabufPlane p;
        p.fd = (d.type == SPA_DATA_DmaBuf) ? static_cast<int>(d.fd) : -1;
        p.offset = d.chunk != nullptr ? d.chunk->offset : 0;
        p.stride = d.chunk != nullptr ? static_cast<std::uint32_t>(d.chunk->stride) : 0;
        p.size = static_cast<std::int64_t>(d.maxsize);
        frame.planes.push_back(p);
    }

    ++r->frames;
    rearm_watchdog(*r);
    if (r->cfg->on_frame)
        r->cfg->on_frame(frame);

    pw_stream_queue_buffer(r->stream, b);

    if (r->frames >= r->cfg->max_frames)
        r->stop();
}

constexpr pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = on_state_changed,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = on_param_changed,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = on_process,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr,
};

} // namespace

// ---------------------------------------------------------------------------- format dump

// Definition of the opaque session type declared in the header.
struct PwSession {
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    spa_source* timer = nullptr;
};

namespace {

struct Dumper {
    pw_main_loop* loop = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    spa_hook registry_listener{};
    spa_hook core_listener{};
    spa_hook node_listener{};
    pw_node* node = nullptr;
    std::uint32_t want_node = 0;
    int sync_seq = 0;
    std::uint32_t params_seen = 0;
    bool quiet = false;
    bool node_found = false;   ///< did the registry ever announce want_node?
    std::uint32_t nodes_seen = 0;
    NodeFormats formats;
};

void on_node_param(void* data, int, std::uint32_t id, std::uint32_t index, std::uint32_t,
                   const spa_pod* param) {
    auto* d = static_cast<Dumper*>(data);
    if (id != SPA_PARAM_EnumFormat || param == nullptr)
        return;
    ++d->params_seen;

    // Collect modifiers structurally. A property carrying one fixed value is the compositor's
    // current layout for this source; a choice is the set it is willing to consider.
    if (const spa_pod_prop* mp = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
        mp != nullptr) {
        const std::vector<std::uint64_t> mods = extract_modifiers(mp);
        const bool fixed = !spa_pod_is_choice(&mp->value) && mods.size() == 1;
        for (std::uint64_t m : mods) {
            if (std::find(d->formats.offered.begin(), d->formats.offered.end(), m) ==
                d->formats.offered.end())
                d->formats.offered.push_back(m);
        }
        if (fixed && std::find(d->formats.preferred.begin(), d->formats.preferred.end(),
                               mods.front()) == d->formats.preferred.end())
            d->formats.preferred.push_back(mods.front());
    }

    if (d->quiet)
        return;
    std::printf("  --- EnumFormat[%u] ---\n", index);
    std::fflush(stdout);
    spa_debug_format(4, nullptr, param);
}

constexpr pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = nullptr,
    .param = on_node_param,
};

void on_registry_global(void* data, std::uint32_t id, std::uint32_t, const char* type,
                        std::uint32_t version, const spa_dict*) {
    auto* d = static_cast<Dumper*>(data);
    if (type != nullptr && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
        ++d->nodes_seen;
    if (id != d->want_node || type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;
    d->node_found = true;

    d->node = static_cast<pw_node*>(
        pw_registry_bind(d->registry, id, PW_TYPE_INTERFACE_Node, version, 0));
    if (d->node == nullptr)
        return;
    pw_node_add_listener(d->node, &d->node_listener, &kNodeEvents, d);
    pw_node_enum_params(d->node, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
    d->sync_seq = pw_core_sync(d->core, PW_ID_CORE, 0);
}

constexpr pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = nullptr,
};

void on_core_done(void* data, std::uint32_t id, int seq) {
    auto* d = static_cast<Dumper*>(data);
    if (id == PW_ID_CORE && d->sync_seq != 0 && seq >= d->sync_seq)
        pw_main_loop_quit(d->loop);
}

constexpr pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = nullptr,
    .done = on_core_done,
    .ping = nullptr,
    .error = nullptr,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
    .bound_props = nullptr,
};

} // namespace

namespace {

/// One bind+enum_params+sync round against `node_id`. Returns how many EnumFormat params arrived.
struct ProbeResult {
    std::uint32_t params = 0;
    bool node_found = false;
    std::uint32_t nodes_seen = 0;
};

ProbeResult probe_node_params(PwSession& session, std::uint32_t node_id, bool verbose,
                              NodeFormats* out_formats = nullptr) {
    Dumper d;
    d.want_node = node_id;
    d.loop = session.loop;
    d.core = session.core;
    d.quiet = !verbose;

    pw_core_add_listener(d.core, &d.core_listener, &kCoreEvents, &d);
    d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(d.registry, &d.registry_listener, &kRegistryEvents, &d);
    d.sync_seq = pw_core_sync(d.core, PW_ID_CORE, 0);

    pw_main_loop_run(d.loop);

    spa_hook_remove(&d.registry_listener);
    spa_hook_remove(&d.core_listener);
    if (d.node != nullptr)
        spa_hook_remove(&d.node_listener);
    if (out_formats != nullptr)
        *out_formats = std::move(d.formats);
    return ProbeResult{d.params_seen, d.node_found, d.nodes_seen};
}

} // namespace

const char* node_readiness_str(NodeReadiness r) {
    switch (r) {
    case NodeReadiness::Ready: return "ready";
    case NodeReadiness::NodeMissing: return "node id does not exist";
    case NodeReadiness::NoFormats: return "node exists but offers no formats";
    }
    return "unknown";
}

NodeReadiness pw_probe_node(PwSession& session, std::uint32_t node_id, NodeFormats* out_formats,
                            int timeout_ms) {
    constexpr int kIntervalMs = 100;
    int waited = 0;
    ProbeResult last;
    for (;;) {
        last = probe_node_params(session, node_id, false, out_formats);
        if (last.params > 0) {
            if (waited > 0)
                lc::info("node {} became ready after {} ms", node_id, waited);
            return NodeReadiness::Ready;
        }
        if (waited >= timeout_ms) {
            // Distinguishing these two is the whole point: a node that never appears in the
            // registry is a dead session, not a slow one.
            if (!last.node_found) {
                lc::warn("node {} NEVER APPEARED in the registry after {} ms ({} other nodes "
                         "visible) - the portal handed back a node id that does not exist",
                         node_id, timeout_ms, last.nodes_seen);
                return NodeReadiness::NodeMissing;
            }
            lc::warn("node {} exists but advertises no EnumFormat params after {} ms", node_id,
                     timeout_ms);
            return NodeReadiness::NoFormats;
        }
        const timespec ts{0, static_cast<long>(kIntervalMs) * 1'000'000L};
        ::nanosleep(&ts, nullptr);
        waited += kIntervalMs;
    }
}

bool pw_wait_node_ready(PwSession& session, std::uint32_t node_id, int timeout_ms) {
    return pw_probe_node(session, node_id, nullptr, timeout_ms) == NodeReadiness::Ready;
}

bool pw_dump_formats(PwSession& session, std::uint32_t node_id) {
    Dumper d;
    d.want_node = node_id;
    d.loop = session.loop;
    d.core = session.core;

    pw_core_add_listener(d.core, &d.core_listener, &kCoreEvents, &d);
    d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(d.registry, &d.registry_listener, &kRegistryEvents, &d);
    d.sync_seq = pw_core_sync(d.core, PW_ID_CORE, 0);

    std::printf("\n=== formats offered by node %u (live session) ===\n", node_id);
    pw_main_loop_run(d.loop);
    std::printf("  (%u EnumFormat params)\n\n", d.params_seen);

    spa_hook_remove(&d.registry_listener);
    spa_hook_remove(&d.core_listener);
    if (d.node != nullptr)
        spa_hook_remove(&d.node_listener);
    return d.params_seen > 0;
}


namespace {

/// Builds the EnumFormat param list for one attempt into `b`.
void build_params(spa_pod_builder* b, const StreamConfig& cfg, std::vector<const spa_pod*>& params) {
    static constexpr std::array<std::uint32_t, 4> kFormats{
        SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA,
        SPA_VIDEO_FORMAT_RGBx};

    if (cfg.mirror_width > 0 && cfg.mirror_height > 0) {
        const spa_rectangle sz{cfg.mirror_width, cfg.mirror_height};
        if (!cfg.modifiers.empty())
            params.push_back(
                build_format_mirror(b, SPA_VIDEO_FORMAT_BGRA, sz, true, cfg.modifiers.front()));
        else
            params.push_back(build_format_mirror(b, SPA_VIDEO_FORMAT_BGRA, sz, false, 0));
        return;
    }
    if (!cfg.modifiers.empty())
        for (std::uint32_t f : kFormats)
            params.push_back(build_format_with_modifiers(b, f, cfg.modifiers));
    params.push_back(build_format_shm(b));
}

void on_attempt_timeout(void* data, std::uint64_t) {
    auto* a = static_cast<Attempt*>(data);
    a->timed_out = true;
    // Only a timeout with nothing to show for it is a failure. If frames were already flowing we
    // simply stop early with fewer than max_frames, which must not be reported as a broken stream
    // - stage 3 does per-frame GPU readback and can legitimately be slower than the budget.
    if (a->error.empty() && a->frames == 0)
        a->error = "timed out waiting for format negotiation";
    a->stop();
}

} // namespace

namespace {
void Attempt::stop() {
    if (session != nullptr && session->loop != nullptr)
        pw_main_loop_quit(session->loop);
}

void rearm_watchdog(Attempt& a) {
    if (a.session == nullptr || a.session->loop == nullptr || a.session->timer == nullptr ||
        a.watchdog_ms <= 0)
        return;
    timespec ts{};
    ts.tv_sec = a.watchdog_ms / 1000;
    ts.tv_nsec = static_cast<long>(a.watchdog_ms % 1000) * 1'000'000L;
    pw_loop_update_timer(pw_main_loop_get_loop(a.session->loop), a.session->timer, &ts, nullptr,
                         false);
}
} // namespace

PwSessionPtr pw_session_open(int pipewire_fd, std::string& error) {
    pw_init(nullptr, nullptr); // once per process: not reliably re-entrant

    PwSessionPtr s(new PwSession());
    s->loop = pw_main_loop_new(nullptr);
    if (s->loop == nullptr) {
        error = "pw_main_loop_new failed";
        return nullptr;
    }
    s->context = pw_context_new(pw_main_loop_get_loop(s->loop), nullptr, 0);
    if (s->context == nullptr) {
        error = "pw_context_new failed";
        return nullptr;
    }
    // Takes ownership of the portal's fd, which is effectively single-use.
    s->core = pw_context_connect_fd(s->context, pipewire_fd, nullptr, 0);
    if (s->core == nullptr) {
        error = std::format("pw_context_connect_fd failed: {}", std::strerror(errno));
        return nullptr;
    }
    return s;
}

void pw_session_close(PwSession* s) {
    if (s == nullptr)
        return;
    if (s->timer != nullptr && s->loop != nullptr)
        pw_loop_destroy_source(pw_main_loop_get_loop(s->loop), s->timer);
    if (s->core != nullptr)
        pw_core_disconnect(s->core);
    if (s->context != nullptr)
        pw_context_destroy(s->context);
    if (s->loop != nullptr)
        pw_main_loop_destroy(s->loop);
    delete s;
    pw_deinit();
}

bool pw_try_variant(PwSession& session, const StreamConfig& cfg, NegotiatedFormat& out_format,
                    std::string& error, int timeout_ms) {
    Attempt a;
    a.session = &session;
    a.cfg = &cfg;
    a.fmt = &out_format;
    out_format = NegotiatedFormat{};

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_APP_NAME, "munkicast-capture-spike",
                          nullptr);
    a.stream = pw_stream_new(session.core, "munkicast-capture", props);
    if (a.stream == nullptr) {
        error = "pw_stream_new failed";
        return false;
    }
    pw_stream_add_listener(a.stream, &a.stream_listener, &kStreamEvents, &a);

    std::array<std::uint8_t, 8192> pod_buf{};
    spa_pod_builder b =
        SPA_POD_BUILDER_INIT(pod_buf.data(), static_cast<std::uint32_t>(pod_buf.size()));
    std::vector<const spa_pod*> params;
    build_params(&b, cfg, params);

    const int res = pw_stream_connect(a.stream, SPA_DIRECTION_INPUT, cfg.node_id,
                                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT),
                                      params.data(), static_cast<std::uint32_t>(params.size()));
    if (res < 0) {
        error = std::format("pw_stream_connect failed: {}", spa_strerror(res));
        pw_stream_destroy(a.stream);
        return false;
    }

    // Bound the attempt so a variant that cannot negotiate does not hang a sweep. The timer is
    // recreated per attempt because its callback data points at this Attempt.
    pw_loop* loop = pw_main_loop_get_loop(session.loop);
    if (session.timer != nullptr)
        pw_loop_destroy_source(loop, session.timer);
    session.timer = pw_loop_add_timer(loop, on_attempt_timeout, &a);

    a.watchdog_ms = timeout_ms;
    rearm_watchdog(a);

    pw_main_loop_run(session.loop);

    // Destroy the timer before `a` dies: it holds a pointer to it.
    pw_loop_destroy_source(loop, session.timer);
    session.timer = nullptr;

    pw_stream_destroy(a.stream);

    if (!a.error.empty()) {
        error = std::format("{}{}", a.error,
                            a.format_logged ? "" : " [param_changed never fired: the server "
                                                   "rejected every advertised param]");
        return false;
    }
    if (a.frames == 0) {
        error = "negotiated but received no frames";
        return false;
    }
    return true;
}

} // namespace munkicast::spike

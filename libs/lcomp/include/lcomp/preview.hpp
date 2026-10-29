// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lcomp/composite.hpp"
#include "lipc/channel.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace lcomp {

/// The preview protocol. Version 11.
///
/// Whole messages on a `lipc::Channel`, bulk in sealed memfds. The bytes are little-endian and
/// carry no padding, so a reader does not depend on the writer's struct layout.
///
///   Hello    u8 kind, u16 version. Renderer to client, once, before anything else.
///   Project  u8 kind, u32 length, one memfd of `project_to_json`. Resets the assembly.
///   Scene    u8 kind, u32 length, one memfd of `scene_to_json`.
///   Commit   u8 kind, u32 maximum preview edge (0 = full size). Reuse the last snapshot
///            on refresh; Ack alone means no document, capture or size change.
///   Quit     u8 kind. The renderer exits.
///   Frame    u8 kind, 26-byte canvas id, u32 width, u32 height, u32 stride, u8 status,
///            u32 gap count, u64 cumulative render ns, u64 completed frames, u64 missed render
///            budgets, u32 canvas fps numerator, u32 denominator. The first frame for a canvas, and
///            any frame whose size changed, carries one memfd of tightly packed straight sRGB8.
///            Later frames of the same size reuse it: the client copies on receipt, and it does not
///            send another snapshot until it has copied, so the renderer is not writing what is
///            being read.
///   Ack      u8 kind. One per commit, after that commit's frames. The client may send again.
///   Fault    u8 kind, the rest UTF-8. Something about the snapshot could not be drawn at all.
///            An ack still follows, or the client would wait forever.
///   Audio    u8 kind, u32 count, then node/bus ULID, u32 left/right peaks in millionths,
///            u8 availability. Sent even when a video refresh only produces an Ack.
///   Pcm      u8 kind, source ULID, interleaved stereo float32 little-endian at 48 kHz.
///            At most 9600 samples; workers to renderer only, never to the UI.
///   Record   u8 kind, u8 start (0 = stop), 26-byte encode id, 26-byte output id, then the UTF-8
///   path.
///            Client to renderer. A stop carries the id and an empty path.
///   RecState u8 kind, u8 running, u64 encoded frames, u64 missed output slots, u64 encoding
///            lag, u8 flags (recording=1, streaming=2, control reply=4), then UTF-8 error text.
///            One per Record, counters with requested previews,
///            and one unprompted if an output stops on its own.
///
/// Active output runs on the renderer's clock using the last committed project. Preview
/// requests only control publication; partial document transfers never replace the live scene.
///
/// An empty payload cannot be sent (ADR-0026), and every message here starts with its kind, so
/// none of them are empty.
inline constexpr std::uint16_t kPreviewProtocol = 11;

/// Capture packet: u32 count, then source ULID, negotiated/received milli-FPS,
/// u16 error byte length and UTF-8 error. Rates describe arrivals, not unique images.
struct CaptureState {
    ldoc::Id source;
    std::uint32_t negotiated = 0, received = 0;
    std::string error;
};

struct AudioLevel {
    ldoc::Id node;
    float left = 0, right = 0;
    bool available = false;
};
struct AudioPacket { ldoc::Id source; std::vector<float> samples; };

/// Start or stop writing one encode to a file or RTMP destination.
///
/// The encode names the canvas, so this does not carry one: two recordings of the same canvas
/// at different bitrates are two encodes, and that is the only way to say it (ADR-0029).
enum class RecordOperation : std::uint8_t { Output, ReplayStart, ReplayStop, ReplaySave };

struct RecordRequest {
    bool start = false;
    ldoc::Id encode;
    /// Where to write. Empty for a stop; the container is chosen from the extension.
    std::string path;
    /// Explicit destination for streaming; nil preserves the recording route.
    ldoc::Id output = ldoc::Id::nil();
    RecordOperation operation = RecordOperation::Output;
};

/// What the renderer made of it. `error` empty means it did what was asked.
struct RecordState {
    bool running = false;
    std::string error;
    std::uint64_t frames = 0;
    std::uint64_t missed = 0;
    /// Missed slots attributable to the preceding synchronous encode/audio/mux operation.
    std::uint64_t encode_lag = 0;
    bool recording = false, streaming = false;
    bool control_reply = true;
    bool replay = false, replay_saving = false;
    std::uint64_t replay_bytes = 0, replay_duration_ms = 0;
    std::string clip_path = {};
};

/// One frame: owned pixels, or a retained read-only immutable mapping.
struct PreviewFrame {
    ldoc::Id canvas;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t stride = 0;
    PictureStatus status = PictureStatus::NoProgram;
    std::uint32_t gaps = 0;
    std::uint64_t render_ns = 0;
    std::uint64_t rendered_frames = 0;
    std::uint64_t render_lag_frames = 0;
    std::uint32_t fps_num = 0;
    std::uint32_t fps_den = 0;
    std::vector<std::uint8_t> pixels;
    std::span<const std::uint8_t> mapped{};
    std::shared_ptr<const void> storage{};

    [[nodiscard]] std::span<const std::uint8_t> bytes() const {
        return mapped.empty() ? std::span<const std::uint8_t>(pixels) : mapped;
    }
};

/// What `poll` produced. `fault` is set for Fault; `frame` is set for Frame.
struct PreviewEvent {
    enum class Kind : std::uint8_t { Hello, Frame, Ack, Fault, PeerGone, Audio, Pcm, Record, Capture };

    Kind kind = Kind::PeerGone;
    PreviewFrame frame;
    std::string fault;
    std::vector<AudioLevel> audio;
    AudioPacket pcm;
    RecordState record;
    std::vector<CaptureState> capture;
};

/// The UI's end of the preview, and the test's. It does not composite.
///
/// Send the latest document, then read frames. Only one snapshot is in flight at a time: call
/// `send_project` again after an Ack, not before. That is what keeps a drag from filling the
/// socket with projects the renderer will throw away.
class PreviewLink {
  public:
    explicit PreviewLink(lipc::Channel channel, bool borrow_immutable = false);
    ~PreviewLink();

    PreviewLink(const PreviewLink&) = delete;
    PreviewLink& operator=(const PreviewLink&) = delete;

    [[nodiscard]] bool valid() const { return channel_.valid(); }

    [[nodiscard]] int poll_fd() const { return channel_.poll_fd(); }

    /// Block until the renderer's hello arrives, or `timeout_ms` passes. A negative timeout
    /// waits forever. The version has to be `kPreviewProtocol`.
    [[nodiscard]] std::expected<void, std::string> take_hello(int timeout_ms);

    /// Project file, then one scene each, then commit. Fails without committing if a send
    /// does not fit, so the renderer never composites a half-snapshot from this call; a later
    /// call starts with Project, which resets whatever did arrive.
    [[nodiscard]] std::expected<void, std::string> send_project(const ldoc::Project& project,
                                                                std::uint32_t max_edge = 0);
    [[nodiscard]] std::expected<void, std::string> refresh(std::uint32_t max_edge = 0);

    /// Ask the renderer to start or stop writing an encode to a file. The answer comes back
    /// as a Record event, not as this call's return: the encoder opens on the renderer's
    /// thread and finding out whether it opened is worth one round trip.
    [[nodiscard]] std::expected<void, std::string> send_record(const RecordRequest& request);

    /// Ask the renderer to exit. A peer that has already gone is not an error here.
    void quit();

    /// One message, if one is waiting. `WouldBlock` is an empty optional, not an error: the
    /// peer is still there.
    [[nodiscard]] std::expected<std::optional<PreviewEvent>, std::string> poll();

    /// `poll` after waiting. `timeout_ms` as in `take_hello`.
    [[nodiscard]] std::expected<std::optional<PreviewEvent>, std::string> receive(int timeout_ms);

    void close() { channel_.close(); }

  private:
    /// A mapping of one canvas's frame buffer. Move-only: the view is unmapped exactly once.
    struct Mapping {
        lipc::Fd fd;
        void* view = nullptr;
        std::size_t bytes = 0;

        Mapping() = default;
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;

        Mapping(Mapping&& other) noexcept
            : fd(std::move(other.fd)), view(std::exchange(other.view, nullptr)),
              bytes(std::exchange(other.bytes, 0)) {}

        Mapping& operator=(Mapping&& other) noexcept {
            if (this != &other) {
                reset();
                fd = std::move(other.fd);
                view = std::exchange(other.view, nullptr);
                bytes = std::exchange(other.bytes, 0);
            }
            return *this;
        }

        ~Mapping() { reset(); }

        void reset() noexcept {
            if (view != nullptr && view != MAP_FAILED)
                ::munmap(view, bytes);
            view = nullptr;
            bytes = 0;
            fd.close();
        }
    };

    lipc::Channel channel_;
    bool borrow_immutable_ = false;
    /// Keyed by the canvas id's text. A handful of canvases; the string is what the message
    /// carries, so nothing is re-encoded to look up.
    std::vector<std::pair<std::string, Mapping>> mappings_;

    [[nodiscard]] std::expected<PreviewEvent, std::string> decode(lipc::Message message);
};

/// The renderer's end. Sends hello, then composites every committed snapshot until Quit or
/// the peer is gone. Returns rather than throws: this is the whole process, and an exception
/// leaving it would be a crash report with no picture attached.
///
/// `frames` is where captured screens come from, and is empty for a caller with no device -- a
/// test, or a build of this process that has not opened one. This library never opens a device
/// itself: ADR-0005 puts the GPU in `libertas-render`, and taking the frame source as a parameter
/// is what keeps `lcomp` free of Vulkan while the process that links it is not.
/// Called when a composite has finished with every frame it borrowed, so the supplier can let
/// its capture threads move on. Empty when there is nothing to release.
using FramesDone = std::function<void()>;

/// Whether any capture has a frame that no composite has drawn yet. Empty means "always draw",
/// which is what a caller with no capture wants.
using FramesFresh = std::function<bool()>;

/// When supplied, full_output receives the full canvas from the same composition as the preview.
using CanvasRenderer = std::function<Picture(const ldoc::Project&, const ldoc::Id&, std::uint32_t,
                                             Picture* full_output)>;
using AudioMeters = std::function<std::vector<AudioLevel>(const ldoc::Project&)>;
using AudioPcm = std::function<AudioPacket()>;

/// The recording side, supplied by whoever owns an encoder. Left empty by a client that does
/// not record -- the self-check and the tests -- so lcomp never has to know what FFmpeg is.
struct RecordHooks {
    /// Act on a Record message. Returns the state to report back.
    std::function<RecordState(const RecordRequest&, const ldoc::Project&)> control;
    /// Whether this canvas is being recorded. Asked once per canvas per commit, because the
    /// answer decides whether it has to be composited at full size as well as preview size.
    std::function<bool(const ldoc::Id&)> wants;
    /// One full-size frame and its scheduled output time, before variable render cost.
    /// May report counters while running, or a stop/failure.
    std::function<std::optional<RecordState>(const ldoc::Id&, const Picture&,
                                             std::chrono::steady_clock::time_point)>
        frame;
};

[[nodiscard]] std::expected<void, std::string>
serve_preview(lipc::Channel channel, FrameSource frames = {}, FramesDone done = {},
              FramesFresh fresh = {}, CanvasRenderer render = {}, AudioMeters audio = {},
              AudioPcm pcm = {}, RecordHooks record = {},
              std::function<std::vector<CaptureState>()> capture = {});

/// Start `libertas-render` with one end of a channel.
///
/// `child_end` is handed to the child and closed here. The child inherits that descriptor and
/// no other — `posix_spawn` with `closefrom`, not `fork`, because the UI is multithreaded and
/// a fork there can only call async-signal-safe functions. The child's descriptor is 3, named
/// by `LIBERTAS_CHANNEL_FD`, so it does not depend on which number this process happened to
/// hold.
[[nodiscard]] std::expected<pid_t, std::string> spawn_renderer(const std::filesystem::path& exe,
                                                               lipc::Fd child_end);

/// True when `send` failed because the socket buffer is full rather than because the peer died.
/// The text is what `lipc` reports; the two are different failures and the caller has to treat
/// them differently.
[[nodiscard]] bool send_would_block(const std::string& error);

} // namespace lcomp

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Encode and mux for the Phase-0 gate.
//
// Two things here are deliberate and load-bearing, and both come from earlier ADRs:
//
//  1. Colour conversion is configured, never inferred (ADR-0014). The capture side proved a device
//     will declare a range it does not deliver, so the matrix and range used to turn RGB into YUV
//     are stated explicitly and written into the bitstream, rather than left to libswscale's
//     defaults - which vary with resolution and would silently change the picture.
//
//  2. Audio carries the clock. Its presentation timestamps come from its own sample counter, which
//     is exact by construction, and video is placed against that same origin. Wall-clock arrival
//     time is used for neither.

#include "lencode/encoder.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <format>
#include <future>
#include <lcore/log.hpp>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// FFmpeg's dynamic CUDA loader: the driver is opened at runtime, so no CUDA toolkit is needed to
// build and machines without NVIDIA still start. Included before hwcontext_cuda.h, whose cuda.h
// include it satisfies. A system FFmpeg without nv-codec-headers builds without GPU input.
#if MUNKICAST_HAVE_FFNVCODEC
#include <ffnvcodec/dynlink_loader.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/hwcontext.h>
#if MUNKICAST_HAVE_FFNVCODEC
#include <libavutil/hwcontext_cuda.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace munkicast::encode {

const char* tier_name(VideoTier t) {
    switch (t) {
    case VideoTier::Vulkan:
        return "h264_vulkan";
    case VideoTier::Nvenc:
        return "h264_nvenc";
    case VideoTier::X264:
        return "libx264";
    default:
        return "none";
    }
}

struct ReplayPacket {
    std::shared_ptr<AVPacket> packet;
    AVRational time_base;
    std::int64_t time_ns = 0;
    bool video = false;
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
using OutputPacket = std::unique_ptr<AVPacket, PacketDeleter>;

struct Encoder {
    RuntimeConfig cfg;
    EncoderStats stats;

    AVFormatContext* fmt = nullptr;
    int file_fd = -1;
    bool network = false;
    std::chrono::steady_clock::time_point deadline;

    AVCodecContext* vctx = nullptr;
    AVStream* vst = nullptr;
    AVFrame* vframe = nullptr;
    SwsContext* sws = nullptr;

    // Device input (NVENC reading Vulkan memory through CUDA). All null on the CPU path.
    AVBufferRef* cuda_device = nullptr;
    AVBufferRef* cuda_frames = nullptr;
#if MUNKICAST_HAVE_FFNVCODEC
    CudaFunctions* cu = nullptr;
    struct Imported {
        std::uint64_t id = 0;
        CUexternalMemory memory = nullptr;
        CUdeviceptr pointer = 0;
    };
    std::vector<Imported> imported;
#endif

    AVCodecContext* actx = nullptr;
    AVStream* ast = nullptr;
    AVFrame* aframe = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    std::int64_t audio_samples = 0; ///< the master clock, in samples

    AVPacket* pkt = nullptr;

    /// The shared time origin, set by whichever stream submits first. Without one, the two streams
    /// start at different zeroes and the file plays with a fixed offset that looks like drift.
    std::int64_t t0_ns = AV_NOPTS_VALUE;
    std::atomic<bool> header_written = false;
    std::vector<EncoderPtr> outputs;
    mutable std::mutex output_mutex;
    std::string output_error;
    std::condition_variable output_ready;
    std::deque<OutputPacket> output_packets;
    std::size_t output_bytes = 0;
    bool output_finishing = false;
    std::jthread output_writer;
    std::int64_t output_origin_ns = 0;
    bool rebase_output = false;
    bool force_keyframe = false;
    std::deque<ReplayPacket> replay;
    std::uint32_t replay_seconds = 0;
    std::uint64_t replay_limit = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> replay_bytes =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::future<ReplayResult> replay_save;
    std::int64_t replay_latest_ns = 0;
};

namespace {

bool network_path(const std::string& path) {
    return path.starts_with("rtmp://") || path.starts_with("rtmps://");
}

// FFmpeg protocol diagnostics may contain the full publish URL, including the key.
thread_local bool private_network_log = false;

struct NetworkOperation {
    bool previous = private_network_log;

    explicit NetworkOperation(Encoder& enc) {
        static std::once_flag logging;
        std::call_once(logging, [] {
            av_log_set_callback([](void* ctx, int level, const char* fmt, va_list args) {
                if (!private_network_log)
                    av_log_default_callback(ctx, level, fmt, args);
            });
        });
        private_network_log = previous || enc.network;
        enc.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    ~NetworkOperation() { private_network_log = previous; }
};

int network_interrupt(void* opaque) {
    return std::chrono::steady_clock::now() >= static_cast<Encoder*>(opaque)->deadline;
}

int file_write(void* opaque, const std::uint8_t* data, int size) {
    const int fd = static_cast<Encoder*>(opaque)->file_fd;
    int written = 0;
    while (written < size) {
        const auto count = ::write(fd, data + written, static_cast<std::size_t>(size - written));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return AVERROR(count == 0 ? EIO : errno);
        written += static_cast<int>(count);
    }
    return written;
}

std::int64_t file_seek(void* opaque, std::int64_t offset, int whence) {
    const int fd = static_cast<Encoder*>(opaque)->file_fd;
    if (whence == AVSEEK_SIZE) {
        struct stat info{};
        return ::fstat(fd, &info) == 0 ? info.st_size : AVERROR(errno);
    }
    const auto result = ::lseek(fd, offset, whence & ~AVSEEK_FORCE);
    return result < 0 ? AVERROR(errno) : result;
}

std::string av_err(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, b, sizeof b);
    return b;
}

// Only the writer touches a running muxer. Closing is allowed to wait for storage;
// rendering and the other destinations must never wait for a file/socket write.
bool close_muxer(Encoder& output, std::string& error) {
    if (!output.header_written)
        return true;
    NetworkOperation operation(output);
    int result = av_write_trailer(output.fmt);
    if (output.fmt->pb) {
        avio_flush(output.fmt->pb);
        if (result >= 0 && output.fmt->pb->error < 0)
            result = output.fmt->pb->error;
    }
    if (result >= 0 && output.file_fd >= 0 && ::fsync(output.file_fd) < 0)
        result = AVERROR(errno);
    if (output.network) {
        const int closed = avio_closep(&output.fmt->pb);
        if (result >= 0)
            result = closed;
    } else if (output.fmt->pb) {
        av_freep(&output.fmt->pb->buffer);
        avio_context_free(&output.fmt->pb);
    }
    if (output.file_fd >= 0) {
        if (::close(output.file_fd) < 0 && result >= 0)
            result = AVERROR(errno);
        output.file_fd = -1;
    }
    output.header_written = false;
    if (result < 0) {
        error = std::format("Output did not close cleanly: {}", av_err(result));
        return false;
    }
    return true;
}

void write_output(Encoder& output) {
    for (;;) {
        OutputPacket packet;
        {
            std::unique_lock lock(output.output_mutex);
            output.output_ready.wait(lock, [&] {
                return output.output_finishing || !output.output_error.empty() ||
                       !output.output_packets.empty();
            });
            if (!output.output_error.empty() || output.output_packets.empty()) {
                output.output_packets.clear();
                output.output_bytes = 0;
                break;
            }
            packet = std::move(output.output_packets.front());
            output.output_packets.pop_front();
        }
        // av_interleaved_write_frame consumes the packet reference, including its size.
        const auto bytes = static_cast<std::size_t>(packet->size);
        NetworkOperation operation(output);
        int result = av_interleaved_write_frame(output.fmt, packet.get());
        if (result >= 0 && output.fmt->pb && output.fmt->pb->error < 0)
            result = output.fmt->pb->error;
        std::lock_guard lock(output.output_mutex);
        output.output_bytes -= bytes;
        if (result < 0 && output.output_error.empty())
            output.output_error = std::format("Output write failed: {}", av_err(result));
    }
    std::string error;
    close_muxer(output, error);
    std::lock_guard lock(output.output_mutex);
    if (output.output_error.empty())
        output.output_error = std::move(error);
}

void enqueue_output(Encoder& output, AVPacket* packet) {
    OutputPacket owned(packet);
    std::lock_guard lock(output.output_mutex);
    if (!output.output_error.empty() || output.output_finishing)
        return;
    // ponytail: 1,024 compressed packets / 64 MiB per destination, including the
    // in-flight bytes. A sustained slow sink stops explicitly; never silently drop
    // recording packets or grow memory without bound. Tune only with soak evidence.
    if (!packet || output.output_packets.size() >= 1024 ||
        static_cast<std::size_t>(packet->size) > 64 * 1048576 - output.output_bytes) {
        output.output_error = packet ? "Output stopped: write queue filled; destination too slow."
                                     : "Cannot allocate output packet.";
    } else {
        try {
            output.output_packets.push_back(std::move(owned));
            output.output_bytes += static_cast<std::size_t>(packet->size);
            if (!output.output_writer.joinable())
                output.output_writer = std::jthread([&output] { write_output(output); });
        } catch (const std::exception&) {
            output.output_error = "Cannot queue output or start its writer.";
        }
    }
    output.output_ready.notify_one();
}

bool open_muxer(Encoder* enc, std::string& error) {
    const auto& cfg = enc->cfg;
    int r = 0;
    AVDictionary* opts = nullptr;
    if (enc->network) {
        enc->fmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        enc->fmt->max_interleave_delta = 1'000'000;
        enc->fmt->interrupt_callback = {network_interrupt, enc};
        av_dict_set(&opts, "rw_timeout", "5000000", 0);
        av_dict_set(&opts, "rtmp_live", "live", 0);
        av_dict_set(&opts, "tls_verify", "1", 0);
        if ((r = avio_open2(&enc->fmt->pb, cfg.path.c_str(), AVIO_FLAG_WRITE,
                            &enc->fmt->interrupt_callback, &opts)) < 0) {
            av_dict_free(&opts);
            error = std::format("Could not connect to RTMP server: {}", av_err(r));
            return false;
        }
        av_dict_free(&opts);
        av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
    } else {
        // Create exclusively: neither an existing take nor a symlink may be overwritten.
        enc->file_fd = ::open(cfg.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (enc->file_fd < 0) {
            error = std::format("Cannot create recording '{}': {}", cfg.path, std::strerror(errno));
            return false;
        }
        constexpr int buffer_size = 32768;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(buffer_size));
        if (!buffer) {
            error = "cannot allocate the recording write buffer";
            return false;
        }
        enc->fmt->pb =
            avio_alloc_context(buffer, buffer_size, 1, enc, nullptr, file_write, file_seek);
        if (!enc->fmt->pb) {
            av_free(buffer);
            error = "cannot allocate the recording file writer";
            return false;
        }
        enc->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        // Fragmented containers remain playable after an interrupted recording.
        av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    }
    if ((r = avformat_write_header(enc->fmt, &opts)) < 0) {
        av_dict_free(&opts);
        error = std::format("avformat_write_header: {}", av_err(r));
        return false;
    }
    av_dict_free(&opts);
    enc->header_written = true;
    return true;
}

// Keep the keyframe preceding the time cutoff. Memory pressure drops whole GOPs; a GOP
// larger than the cap is discarded until another keyframe arrives. Audio cannot lead its video.
void buffer_packet(Encoder& enc, AVCodecContext* ctx) {
    if (!enc.replay_seconds)
        return;
    const bool video = ctx == enc.vctx;
    const bool key = video && (enc.pkt->flags & AV_PKT_FLAG_KEY);
    const auto time = av_rescale_q(enc.pkt->dts, ctx->time_base, AVRational{1, 1000000000});
    if (video)
        enc.replay_latest_ns = time;
    const auto drop_gop = [&] {
        if (enc.replay.empty())
            return;
        enc.replay.pop_front();
        while (!enc.replay.empty() &&
               !(enc.replay.front().video && (enc.replay.front().packet->flags & AV_PKT_FLAG_KEY)))
            enc.replay.pop_front();
    };
    for (;;) {
        const auto next = std::find_if(
            enc.replay.begin() + (enc.replay.empty() ? 0 : 1), enc.replay.end(),
            [](const auto& p) { return p.video && (p.packet->flags & AV_PKT_FLAG_KEY); });
        if (next == enc.replay.end() ||
            next->time_ns >
                enc.replay_latest_ns - static_cast<std::int64_t>(enc.replay_seconds) * 1000000000LL)
            break;
        drop_gop();
    }
    std::uint64_t charge = (enc.pkt->buf ? static_cast<std::uint64_t>(enc.pkt->buf->size)
                                         : static_cast<std::uint64_t>(enc.pkt->size)) +
                           sizeof(AVPacket) + sizeof(ReplayPacket) + 128;
    for (int i = 0; i < enc.pkt->side_data_elems; ++i)
        charge += enc.pkt->side_data[i].size;
    while (!enc.replay.empty() && enc.replay_bytes->load() + charge > enc.replay_limit)
        drop_gop();
    if ((enc.replay.empty() && !key) || charge > enc.replay_limit ||
        enc.replay_bytes->load() > enc.replay_limit - charge)
        return;
    auto* copy = av_packet_clone(enc.pkt);
    if (!copy)
        return;
    enc.replay_bytes->fetch_add(charge);
    enc.replay.push_back(
        {std::shared_ptr<AVPacket>(copy,
                                   [bytes = enc.replay_bytes, charge](AVPacket* p) {
                                       av_packet_free(&p);
                                       bytes->fetch_sub(charge);
                                   }),
         ctx->time_base, time, video});
}

/// Drains packets from an encoder into the muxer.
bool drain(Encoder& enc, AVCodecContext* ctx, AVStream*, std::uint64_t& bytes, std::string& error) {
    for (;;) {
        const int r = avcodec_receive_packet(ctx, enc.pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return true;
        if (r < 0) {
            error = std::format("avcodec_receive_packet: {}", av_err(r));
            return false;
        }
        bytes += static_cast<std::uint64_t>(enc.pkt->size);
        buffer_packet(enc, ctx);
        const bool video = ctx == enc.vctx;
        const auto write = [&](Encoder& output) {
            if (!encoder_output_active(output, 0))
                return;
            if (output.output_origin_ns == AV_NOPTS_VALUE) {
                if (!video || !(enc.pkt->flags & AV_PKT_FLAG_KEY))
                    return;
                output.output_origin_ns =
                    av_rescale_q(enc.pkt->dts, ctx->time_base, AVRational{1, 1000000000});
            }
            const auto origin =
                av_rescale_q(output.output_origin_ns, AVRational{1, 1000000000}, ctx->time_base);
            if (output.rebase_output && enc.pkt->dts < origin)
                return;
            AVPacket* packet = av_packet_clone(enc.pkt);
            if (packet) {
                auto* stream = video ? output.vst : output.ast;
                packet->pts -= origin;
                packet->dts -= origin;
                packet->stream_index = stream->index;
                av_packet_rescale_ts(packet, ctx->time_base, stream->time_base);
            }
            enqueue_output(output, packet);
        };
        write(enc);
        for (auto& output : enc.outputs)
            if (output)
                write(*output);
        av_packet_unref(enc.pkt);
        if (!enc.replay_seconds && !encoder_output_active(enc, 0) &&
            std::ranges::none_of(enc.outputs, [](const auto& output) {
                return output && encoder_output_active(*output, 0);
            })) {
            error = encoder_output_error(enc, 0);
            if (error.empty())
                error = "No output destinations remain";
            return false;
        }
    }
}

/// Tries one video encoder by name. Returns nullptr when the encoder is absent or refuses to open,
/// which is the ladder's whole mechanism - "not available" and "present but unusable here" must
/// both fall through to the next rung rather than abort the run.
AVCodecContext* try_video(const char* name, const RuntimeConfig& cfg, AVFormatContext* fmt,
                          std::string& why, AVBufferRef* hw_frames = nullptr) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (codec == nullptr) {
        why = "not built into this libavcodec";
        return nullptr;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (c == nullptr) {
        why = "avcodec_alloc_context3 failed";
        return nullptr;
    }
    c->width = static_cast<int>(cfg.width);
    c->height = static_cast<int>(cfg.height);
    c->time_base = AVRational{cfg.fps_den, cfg.fps_num};
    c->framerate = AVRational{cfg.fps_num, cfg.fps_den};
    // NV12 rather than I420: it is what NVENC wants natively, what the GPU convert shader
    // produces, and what a Vulkan video encode surface will be (G8_B8R8_2PLANE_420_UNORM). A
    // codec that cannot take it falls back to I420, and the GPU convert path is then refused
    // rather than silently feeding it the wrong layout.
    // `main10` is HEVC's and `high10` is H.264's name for the same thing, so the depth follows
    // the profile naming ten bits rather than one spelling of it. The UI offers only main10
    // today, but an operator can reach high10 through custom options, and a 10-bit profile
    // opened at 8-bit depth is exactly the dishonest case ADR-0071 is about.
    const auto profile = cfg.video_options.contains("profile")
                             ? cfg.video_options.at("profile")
                             : std::string{};
    c->pix_fmt = profile == "main10" || profile == "high10" ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    // AVCodec::pix_fmts was removed in FFmpeg 7; avcodec_get_supported_config is the current
    // way to ask, and it can answer "no list", which means the codec accepts anything.
    {
        const AVPixelFormat* fmts = nullptr;
        int n = 0;
        if (avcodec_get_supported_config(c, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                         reinterpret_cast<const void**>(&fmts), &n) >= 0 &&
            fmts != nullptr) {
            bool format_ok = false;
            for (int k = 0; k < n; ++k)
                if (fmts[k] == c->pix_fmt)
                    format_ok = true;
            if (!format_ok)
                c->pix_fmt =
                    c->pix_fmt == AV_PIX_FMT_P010LE ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        }
    }
    if (hw_frames) {
        c->hw_frames_ctx = av_buffer_ref(hw_frames);
        c->pix_fmt = AV_PIX_FMT_CUDA;
        c->sw_pix_fmt = AV_PIX_FMT_NV12;
    }
    c->bit_rate = cfg.bitrate;
    c->gop_size =
        cfg.keyframe_interval_ms == 0
            ? -1
            : std::max(1, static_cast<int>(
                              (static_cast<std::int64_t>(cfg.fps_num) * cfg.keyframe_interval_ms +
                               static_cast<std::int64_t>(cfg.fps_den) * 500) /
                              (static_cast<std::int64_t>(cfg.fps_den) * 1000)));
    c->max_b_frames = 0; // live: no reordering delay
    // State the colour properties rather than leaving them unset. An unset matrix in an H.264
    // bitstream means "assume BT.601", which is wrong for everything we produce.
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->color_range = AVCOL_RANGE_MPEG;
    if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (cfg.video_options.empty() && std::strcmp(name, "libx264") == 0) {
        av_opt_set(c->priv_data, "preset", "veryfast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    } else if (cfg.video_options.empty() && std::strcmp(name, "h264_nvenc") == 0) {
        av_opt_set(c->priv_data, "preset", "p4", 0);
        av_opt_set(c->priv_data, "tune", "ll", 0);
        av_opt_set(c->priv_data, "rc", "cbr", 0);
    }

    AVDictionary* options = nullptr;
    for (const auto& [key, value] : cfg.video_options) {
        if (key == "x264-params") {
            AVDictionary* parameters = nullptr;
            char* serialised = nullptr;
            const int parsed = av_dict_parse_string(&parameters, value.c_str(), "=", " :", 0);
            if (parsed >= 0)
                av_dict_get_string(parameters, &serialised, '=', ':');
            av_dict_free(&parameters);
            if (parsed < 0 || !serialised) {
                why = "x264 options must be key=value pairs separated by spaces or colons";
                av_dict_free(&options);
                avcodec_free_context(&c);
                return nullptr;
            }
            av_dict_set(&options, key.c_str(), serialised, 0);
            av_free(serialised);
        } else {
            av_dict_set(&options, key.c_str(), value.c_str(), 0);
        }
    }
    if (!cfg.custom_options.empty() &&
        av_dict_parse_string(&options, cfg.custom_options.c_str(), "=", " :", 0) < 0) {
        why = "encoder options must be key=value pairs separated by spaces or colons";
        av_dict_free(&options);
        avcodec_free_context(&c);
        return nullptr;
    }
    const int r = avcodec_open2(c, codec, &options);
    const std::string unused =
        av_dict_count(options) ? av_dict_get(options, "", nullptr, AV_DICT_IGNORE_SUFFIX)->key : "";
    av_dict_free(&options);
    if (r < 0 || !unused.empty()) {
        why = r < 0 ? av_err(r) : "unrecognised encoder option: " + unused;
        avcodec_free_context(&c);
        return nullptr;
    }
    return c;
}

#if MUNKICAST_HAVE_FFNVCODEC
// The CUDA device whose UUID is the Vulkan compositor's, so NVENC reads memory on that GPU.
// Null when there is none: no NVIDIA driver, or the compositor runs on another vendor's GPU.
AVBufferRef* cuda_device_for(const std::array<std::uint8_t, 16>& uuid, CudaFunctions* cu) {
    int count = 0;
    if (!cu->cuDeviceGetUuid || cu->cuInit(0) != CUDA_SUCCESS ||
        cu->cuDeviceGetCount(&count) != CUDA_SUCCESS)
        return nullptr;
    for (int i = 0; i < count; ++i) {
        CUdevice device = 0;
        CUuuid id{};
        if (cu->cuDeviceGet(&device, i) != CUDA_SUCCESS ||
            cu->cuDeviceGetUuid(&id, device) != CUDA_SUCCESS ||
            std::memcmp(id.bytes, uuid.data(), uuid.size()) != 0)
            continue;
        AVBufferRef* result = nullptr;
        if (av_hwdevice_ctx_create(&result, AV_HWDEVICE_TYPE_CUDA, std::to_string(i).c_str(),
                                   nullptr, 0) < 0)
            return nullptr;
        return result;
    }
    return nullptr;
}

// Reopens an NV12 NVENC context to take CUDA frames. Any failure leaves the CPU-input encoder.
void open_device_input(Encoder& enc, const char* name, AVFormatContext* fmt) {
    if (std::strcmp(name, "h264_nvenc") != 0 || enc.vctx->pix_fmt != AV_PIX_FMT_NV12 ||
        std::ranges::all_of(enc.cfg.gpu_uuid, [](auto b) { return b == 0; }))
        return;
    const auto give_up = [&](const char* why) {
        lc::warn("NVENC GPU input unavailable ({}); using system-memory frames", why);
        av_buffer_unref(&enc.cuda_frames);
        av_buffer_unref(&enc.cuda_device);
        cuda_free_functions(&enc.cu);
    };
    if (cuda_load_functions(&enc.cu, nullptr) < 0 || !enc.cu->cuImportExternalMemory ||
        !enc.cu->cuExternalMemoryGetMappedBuffer || !enc.cu->cuDestroyExternalMemory)
        return give_up("CUDA driver lacks external memory");
    enc.cuda_device = cuda_device_for(enc.cfg.gpu_uuid, enc.cu);
    if (!enc.cuda_device)
        return give_up("compositor GPU is not a CUDA device");
    enc.cuda_frames = av_hwframe_ctx_alloc(enc.cuda_device);
    if (!enc.cuda_frames)
        return give_up("frames context");
    auto* frames = reinterpret_cast<AVHWFramesContext*>(enc.cuda_frames->data);
    frames->format = AV_PIX_FMT_CUDA;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = enc.vctx->width;
    frames->height = enc.vctx->height;
    frames->initial_pool_size = 0; // Frames point at imported memory; the pool is never drawn.
    if (av_hwframe_ctx_init(enc.cuda_frames) < 0)
        return give_up("frames context init");
    auto cfg = enc.cfg;
    std::string why;
    // try_video sets NV12; the CUDA context replaces it with hardware frames of that layout.
    AVCodecContext* hardware = try_video(name, cfg, fmt, why, enc.cuda_frames);
    if (!hardware)
        return give_up(why.c_str());
    avcodec_free_context(&enc.vctx);
    enc.vctx = hardware;
    lc::info("NVENC reads frames from GPU memory (zero-copy)");
}
#else
void open_device_input(Encoder&, const char*, AVFormatContext*) {}
#endif

} // namespace

EncoderPtr encoder_open(const RuntimeConfig& cfg, std::string& error) {
    auto enc = EncoderPtr(new Encoder());
    enc->cfg = cfg;
    enc->network = network_path(cfg.path);
    NetworkOperation operation(*enc);
    if (cfg.path.find('\0') != std::string::npos) {
        error = "the recording path contains a null character";
        return nullptr;
    }
    if (enc->cfg.source_width == 0)
        enc->cfg.source_width = cfg.width;
    if (enc->cfg.source_height == 0)
        enc->cfg.source_height = cfg.height;

    int r = avformat_alloc_output_context2(&enc->fmt, nullptr,
                                           enc->network       ? "flv"
                                           : cfg.path.empty() ? "matroska"
                                                              : nullptr,
                                           cfg.path.empty() ? nullptr : cfg.path.c_str());
    if (r < 0 || enc->fmt == nullptr) {
        error = std::format("avformat_alloc_output_context2: {}", av_err(r));
        return nullptr;
    }

    // Walk the ladder. Pinning a tier is for bisecting a problem, not for normal use.
    struct Rung {
        VideoTier tier;
        const char* name;
    };

    std::vector<Rung> ladder = {
        {VideoTier::Nvenc, "h264_nvenc"},
        {VideoTier::X264, "libx264"},
    };
    if (!cfg.force_tier.empty()) {
        const auto tier = cfg.force_tier.ends_with("_nvenc") ? VideoTier::Nvenc
                          : cfg.force_tier == "libx264"      ? VideoTier::X264
                                                             : VideoTier::None;
        ladder = {{tier, cfg.force_tier.c_str()}};
    }
    for (const Rung& rung : ladder) {
        std::string why;
        enc->vctx = try_video(rung.name, cfg, enc->fmt, why);
        if (enc->vctx != nullptr) {
            enc->stats.tier = rung.tier;
            enc->stats.video_encoder = rung.name;
            open_device_input(*enc, rung.name, enc->fmt);
            break;
        }
        error = std::format("{}: {}", rung.name, why);
        lc::warn("encoder tier {} unavailable: {}", rung.name, why);
    }
    if (enc->vctx == nullptr) {
        if (error.empty())
            error = "no usable video encoder";
        return nullptr;
    }
    lc::info("video encoder: {} ({}x{} @ {}/{}, {} kbps)", enc->stats.video_encoder, cfg.width,
             cfg.height, cfg.fps_num, cfg.fps_den, cfg.bitrate / 1000);

    enc->vst = avformat_new_stream(enc->fmt, nullptr);
    if (enc->vst == nullptr) {
        error = "avformat_new_stream (video) failed";
        return nullptr;
    }
    enc->vst->time_base = enc->vctx->time_base;
    enc->vst->avg_frame_rate = enc->vctx->framerate;
    enc->vst->r_frame_rate = enc->vctx->framerate;
    avcodec_parameters_from_context(enc->vst->codecpar, enc->vctx);

    enc->vframe = av_frame_alloc();
    enc->vframe->format =
        enc->vctx->pix_fmt == AV_PIX_FMT_CUDA ? AV_PIX_FMT_NV12 : enc->vctx->pix_fmt;
    enc->vframe->width = enc->vctx->width;
    enc->vframe->height = enc->vctx->height;
    enc->vframe->colorspace = enc->vctx->colorspace;
    enc->vframe->color_range = enc->vctx->color_range;
    if ((r = av_frame_get_buffer(enc->vframe, 0)) < 0) {
        error = std::format("av_frame_get_buffer (video): {}", av_err(r));
        return nullptr;
    }

    // The frame API selects optimized conversions and divides work between CPU threads.
    // The legacy sws_scale path processes RGBA -> NV12 on one thread even for hardware encode.
    enc->sws = sws_alloc_context();
    if (enc->sws == nullptr) {
        error = "sws_alloc_context failed";
        return nullptr;
    }
    const int scale_flags = cfg.rescale_filter == "lanczos"   ? SWS_LANCZOS
                            : cfg.rescale_filter == "bicubic" ? SWS_BICUBIC
                                                              : SWS_BILINEAR;
    if (av_opt_set_int(enc->sws, "threads", 0, 0) < 0 ||
        av_opt_set_int(enc->sws, "sws_flags", scale_flags, 0) < 0) {
        error = "cannot configure video color conversion";
        return nullptr;
    }
    enc->vframe->color_primaries = enc->vctx->color_primaries;
    enc->vframe->color_trc = enc->vctx->color_trc;

    // --- audio ---
    if (cfg.audio_channels > 0) {
        const AVCodec* acodec = cfg.audio_codec == "aac"    ? avcodec_find_encoder(AV_CODEC_ID_AAC)
                                : cfg.audio_codec == "opus" ? avcodec_find_encoder(AV_CODEC_ID_OPUS)
                                                            : nullptr;
        if (acodec == nullptr) {
            error = "no encoder available for audio codec: " + cfg.audio_codec;
            return nullptr;
        }
        enc->actx = avcodec_alloc_context3(acodec);
        enc->actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        const AVSampleFormat* formats = nullptr;
        int format_count = 0;
        if (avcodec_get_supported_config(enc->actx, acodec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                         reinterpret_cast<const void**>(&formats),
                                         &format_count) >= 0 &&
            formats && format_count > 0 &&
            std::find(formats, formats + format_count, AV_SAMPLE_FMT_FLTP) ==
                formats + format_count)
            enc->actx->sample_fmt = formats[0];
        enc->actx->sample_rate = static_cast<int>(cfg.audio_rate);
        enc->actx->bit_rate = cfg.audio_bitrate;
        av_channel_layout_default(&enc->actx->ch_layout, static_cast<int>(cfg.audio_channels));
        enc->actx->time_base = AVRational{1, static_cast<int>(cfg.audio_rate)};
        if ((enc->fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            enc->actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((r = avcodec_open2(enc->actx, acodec, nullptr)) < 0) {
            error = std::format("avcodec_open2 (audio): {}", av_err(r));
            return nullptr;
        }
        enc->stats.audio_encoder = acodec->name;

        enc->ast = avformat_new_stream(enc->fmt, nullptr);
        enc->ast->time_base = enc->actx->time_base;
        avcodec_parameters_from_context(enc->ast->codecpar, enc->actx);

        AVChannelLayout in_layout;
        av_channel_layout_default(&in_layout, static_cast<int>(cfg.audio_channels));
        r = swr_alloc_set_opts2(&enc->swr, &enc->actx->ch_layout, enc->actx->sample_fmt,
                                static_cast<int>(cfg.audio_rate), &in_layout, AV_SAMPLE_FMT_FLT,
                                static_cast<int>(cfg.audio_rate), 0, nullptr);
        if (r < 0 || swr_init(enc->swr) < 0) {
            error = "swr_init failed";
            return nullptr;
        }
        enc->fifo = av_audio_fifo_alloc(enc->actx->sample_fmt, static_cast<int>(cfg.audio_channels),
                                        enc->actx->frame_size * 8);

        enc->aframe = av_frame_alloc();
        enc->aframe->format = enc->actx->sample_fmt;
        enc->aframe->nb_samples = enc->actx->frame_size;
        av_channel_layout_copy(&enc->aframe->ch_layout, &enc->actx->ch_layout);
        if ((r = av_frame_get_buffer(enc->aframe, 0)) < 0) {
            error = std::format("av_frame_get_buffer (audio): {}", av_err(r));
            return nullptr;
        }
        lc::info("audio encoder: {} ({} Hz, {} ch, {} kbps, frame {})", enc->stats.audio_encoder,
                 cfg.audio_rate, cfg.audio_channels, cfg.audio_bitrate / 1000,
                 enc->actx->frame_size);
    }

    if (!cfg.path.empty() && !open_muxer(enc.get(), error))
        return nullptr;
    enc->pkt = av_packet_alloc();
    return enc;
}

namespace {
EncoderPtr copy_output(Encoder& enc, const std::string& path, std::string& error) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        error = "Invalid output path";
        return nullptr;
    }
    auto output = EncoderPtr(new Encoder());
    output->cfg = enc.cfg;
    output->cfg.path = path;
    output->network = network_path(path);
    output->output_origin_ns = AV_NOPTS_VALUE;
    output->rebase_output = true;
    NetworkOperation operation(*output);
    int result = avformat_alloc_output_context2(&output->fmt, nullptr,
                                                output->network ? "flv" : nullptr, path.c_str());
    if (result < 0 || !output->fmt) {
        error = "Cannot create output container";
        return nullptr;
    }
    const auto stream = [&](AVStream* source) -> AVStream* {
        auto* copy = avformat_new_stream(output->fmt, nullptr);
        if (!copy || avcodec_parameters_copy(copy->codecpar, source->codecpar) < 0)
            return nullptr;
        copy->codecpar->codec_tag = 0;
        copy->time_base = source->time_base;
        copy->avg_frame_rate = source->avg_frame_rate;
        copy->r_frame_rate = source->r_frame_rate;
        return copy;
    };
    output->vst = stream(enc.vst);
    if (enc.ast)
        output->ast = stream(enc.ast);
    if (!output->vst || (enc.ast && !output->ast)) {
        error = "Cannot copy output stream parameters";
        return nullptr;
    }
    if (!open_muxer(output.get(), error))
        return nullptr;
    return output;
}
} // namespace

std::optional<std::size_t> encoder_add_output(Encoder& enc, const std::string& path,
                                              std::string& error) {
    auto output = copy_output(enc, path, error);
    if (!output)
        return std::nullopt;
    enc.outputs.push_back(std::move(output));
    enc.force_keyframe = true;
    return enc.outputs.size();
}

bool encoder_set_replay(Encoder& enc, std::uint32_t seconds, std::uint64_t bytes,
                        std::string& error) {
    if (seconds > 86400 || (seconds && (bytes < 1048576 || bytes > 1099511627776ULL))) {
        error = "Replay requires 1–86400 seconds and 1–1048576 MB.";
        return false;
    }
    enc.replay.clear();
    enc.replay_seconds = seconds;
    enc.replay_limit = bytes;
    enc.force_keyframe = true;
    return true;
}

ReplayStats encoder_replay_stats(const Encoder& enc) {
    const auto duration =
        enc.replay.empty() ? 0
                           : std::max<std::int64_t>(
                                 0, (enc.replay_latest_ns - enc.replay.front().time_ns) / 1000000);
    return {enc.replay_bytes->load(), static_cast<std::uint64_t>(duration), enc.replay_seconds != 0,
            enc.replay_save.valid()};
}

std::optional<ReplayResult> encoder_poll_replay(Encoder& enc) {
    if (!enc.replay_save.valid() ||
        enc.replay_save.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return std::nullopt;
    return enc.replay_save.get();
}

bool encoder_save_replay(Encoder& enc, const std::string& path, std::string& error) {
    if (enc.replay.empty() || !enc.replay_seconds) {
        error = "Replay buffer is not ready yet.";
        return false;
    }
    if (enc.replay_save.valid()) {
        error = "A replay clip is already being saved.";
        return false;
    }
    if (path.empty() || network_path(path) || !path.ends_with(".mkv")) {
        error = "A replay clip requires a local MKV path.";
        return false;
    }
    auto output = copy_output(enc, path, error);
    if (!output)
        return false;
    auto snapshot = enc.replay;
    try {
        enc.replay_save = std::async(std::launch::async, [output = std::move(output),
                                                          snapshot = std::move(snapshot),
                                                          path]() mutable {
            ReplayResult result{path, {}};
            const auto origin = snapshot.front().time_ns;
            for (const auto& stored : snapshot) {
                if (stored.time_ns < origin)
                    continue;
                auto* packet = av_packet_clone(stored.packet.get());
                if (!packet) {
                    result.error = "Cannot allocate replay packet.";
                    break;
                }
                auto* stream = stored.video ? output->vst : output->ast;
                const auto base = av_rescale_q(origin, AVRational{1, 1000000000}, stored.time_base);
                packet->pts -= base;
                packet->dts -= base;
                packet->stream_index = stream->index;
                av_packet_rescale_ts(packet, stored.time_base, stream->time_base);
                const int written = av_interleaved_write_frame(output->fmt, packet);
                av_packet_free(&packet);
                if (written < 0) {
                    result.error = "Could not write replay clip: " + av_err(written);
                    break;
                }
            }
            std::string closed;
            if (!encoder_remove_output(*output, 0, closed)) {
                if (!result.error.empty())
                    result.error += " ";
                result.error += closed;
            }
            return result;
        });
    } catch (const std::exception& e) {
        error = std::string("Cannot start replay writer: ") + e.what();
        return false;
    }
    return true;
}

bool encoder_output_active(const Encoder& enc, std::size_t id) {
    if (id != 0)
        return id <= enc.outputs.size() && enc.outputs[id - 1] &&
               encoder_output_active(*enc.outputs[id - 1], 0);
    std::lock_guard lock(enc.output_mutex);
    return enc.header_written && !enc.output_finishing && enc.output_error.empty();
}

std::string encoder_output_error(const Encoder& enc, std::size_t id) {
    if (id != 0)
        return id <= enc.outputs.size() && enc.outputs[id - 1]
                   ? encoder_output_error(*enc.outputs[id - 1], 0)
                   : std::string{};
    std::lock_guard lock(enc.output_mutex);
    return enc.output_error;
}

bool encoder_remove_output(Encoder& enc, std::size_t id, std::string& error) {
    Encoder* output = id == 0                    ? &enc
                      : id <= enc.outputs.size() ? enc.outputs[id - 1].get()
                                                 : nullptr;
    if (!output)
        return true;
    {
        std::lock_guard lock(output->output_mutex);
        output->output_finishing = true;
    }
    output->output_ready.notify_one();
    if (output->output_writer.joinable()) {
        output->output_writer.join();
    } else {
        std::string closed;
        close_muxer(*output, closed);
        std::lock_guard lock(output->output_mutex);
        output->output_packets.clear();
        output->output_bytes = 0;
        if (output->output_error.empty())
            output->output_error = std::move(closed);
    }
    const auto failure = encoder_output_error(*output, 0);
    if (!failure.empty())
        error = failure;
    return failure.empty();
}

bool encoder_video(Encoder& enc, const std::uint8_t* bgra, std::size_t stride, std::int64_t pts_ns,
                   std::string& error, bool wide) {
    const std::size_t bytes_per_pixel = wide ? 8u : 4u;
    if (!bgra || stride < static_cast<std::size_t>(enc.cfg.source_width) * bytes_per_pixel ||
        stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "invalid video frame stride or pixels";
        return false;
    }
    if (enc.t0_ns == AV_NOPTS_VALUE)
        enc.t0_ns = pts_ns;

    if (av_frame_make_writable(enc.vframe) < 0) {
        error = "av_frame_make_writable (video) failed";
        return false;
    }
    // Timed separately. An earlier version started the clock *after* sws_scale and still called
    // the result "encode+convert", so the colour conversion - the more expensive of the two - was
    // not in the number at all. A stage that is not measured must not appear in the label.
    const auto t_convert = std::chrono::steady_clock::now();
    AVFrame source{};
    // RGBA64LE is the only 16-bit order worth carrying: it is what Vulkan's
    // R16G16B16A16_UNORM reads back as on a little-endian host, so the wide path needs no
    // byte-swap anywhere. There is no BGRA64 equivalent in swscale, so a wide frame is always
    // RGBA order and the compositor writes it that way.
    source.format = wide ? AV_PIX_FMT_RGBA64LE
                         : enc.cfg.source_rgba ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
    source.width = static_cast<int>(enc.cfg.source_width);
    source.height = static_cast<int>(enc.cfg.source_height);
    source.data[0] = const_cast<std::uint8_t*>(bgra);
    source.linesize[0] = static_cast<int>(stride);
    source.colorspace = AVCOL_SPC_BT709;
    source.color_range = AVCOL_RANGE_JPEG;
    source.color_primaries = enc.vframe->color_primaries;
    source.color_trc = enc.vframe->color_trc;
    const int converted = sws_scale_frame(enc.sws, enc.vframe, &source);
    if (converted < 0) {
        error = std::format("video color conversion: {}", av_err(converted));
        return false;
    }
    enc.stats.convert_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_convert)
            .count());

    enc.vframe->pts =
        av_rescale_q(pts_ns - enc.t0_ns, AVRational{1, 1'000'000'000}, enc.vctx->time_base);

    const auto t_begin = std::chrono::steady_clock::now();
    enc.vframe->pict_type = enc.force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    enc.force_keyframe = false;
    // An encoder opened for GPU frames still takes the occasional host frame (odd canvas size,
    // or every GPU slot still queued in NVENC): upload it rather than fail the output.
    AVFrame* upload = nullptr;
    if (encoder_takes_device_frames(enc)) {
        upload = av_frame_alloc();
        if (!upload || av_hwframe_get_buffer(enc.cuda_frames, upload, 0) < 0 ||
            av_hwframe_transfer_data(upload, enc.vframe, 0) < 0 ||
            av_frame_copy_props(upload, enc.vframe) < 0) {
            av_frame_free(&upload);
            error = "cannot upload the frame to the GPU encoder";
            return false;
        }
    }
    const int r = avcodec_send_frame(enc.vctx, upload ? upload : enc.vframe);
    av_frame_free(&upload);
    if (r < 0) {
        error = std::format("avcodec_send_frame (video): {}", av_err(r));
        return false;
    }
    if (!drain(enc, enc.vctx, enc.vst, enc.stats.video_bytes, error))
        return false;
    enc.stats.video_encode_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin)
            .count());
    ++enc.stats.video_frames;
    return true;
}

bool encoder_audio(Encoder& enc, const float* interleaved, std::uint32_t frames,
                   std::int64_t pts_ns, std::string& error) {
    if (enc.actx == nullptr)
        return true;
    if (enc.t0_ns == AV_NOPTS_VALUE)
        enc.t0_ns = pts_ns;

    const std::uint8_t* in[1] = {reinterpret_cast<const std::uint8_t*>(interleaved)};
    // Convert interleaved float into the planar layout AAC wants, straight into the FIFO.
    AVFrame* tmp = av_frame_alloc();
    tmp->format = enc.actx->sample_fmt;
    tmp->nb_samples = static_cast<int>(frames);
    av_channel_layout_copy(&tmp->ch_layout, &enc.actx->ch_layout);
    if (av_frame_get_buffer(tmp, 0) < 0) {
        av_frame_free(&tmp);
        error = "av_frame_get_buffer (audio scratch) failed";
        return false;
    }
    const int converted =
        swr_convert(enc.swr, tmp->data, static_cast<int>(frames), in, static_cast<int>(frames));
    if (converted < 0) {
        av_frame_free(&tmp);
        error = "swr_convert failed";
        return false;
    }
    av_audio_fifo_write(enc.fifo, reinterpret_cast<void**>(tmp->data), converted);
    av_frame_free(&tmp);

    // Drain the FIFO in exact encoder-frame units. AAC will not accept a partial frame, and
    // padding one would inject silence that accumulates as drift.
    while (av_audio_fifo_size(enc.fifo) >= enc.actx->frame_size) {
        if (av_frame_make_writable(enc.aframe) < 0) {
            error = "av_frame_make_writable (audio) failed";
            return false;
        }
        av_audio_fifo_read(enc.fifo, reinterpret_cast<void**>(enc.aframe->data),
                           enc.actx->frame_size);
        // The master clock: an exact running sample count, never a wall-clock reading.
        enc.aframe->pts = enc.audio_samples;
        enc.audio_samples += enc.actx->frame_size;

        const int r = avcodec_send_frame(enc.actx, enc.aframe);
        if (r < 0) {
            error = std::format("avcodec_send_frame (audio): {}", av_err(r));
            return false;
        }
        if (!drain(enc, enc.actx, enc.ast, enc.stats.audio_bytes, error))
            return false;
        ++enc.stats.audio_frames;
    }
    return true;
}

bool encoder_finish(Encoder& enc, std::string& error) {
    if (enc.vctx != nullptr) {
        avcodec_send_frame(enc.vctx, nullptr);
        if (!drain(enc, enc.vctx, enc.vst, enc.stats.video_bytes, error))
            return false;
    }
    if (enc.actx != nullptr) {
        avcodec_send_frame(enc.actx, nullptr);
        if (!drain(enc, enc.actx, enc.ast, enc.stats.audio_bytes, error))
            return false;
    }
    bool success = encoder_remove_output(enc, 0, error);
    for (std::size_t i = 0; i < enc.outputs.size(); ++i) {
        std::string output_error;
        if (!encoder_remove_output(enc, i + 1, output_error)) {
            if (!error.empty())
                error += " ";
            error += output_error;
            success = false;
        }
    }
    return success;
}

bool encoder_video_nv12(Encoder& enc, const std::uint8_t* y, int y_stride, const std::uint8_t* uv,
                        int uv_stride, std::int64_t pts_ns, std::string& error) {
    if (enc.vctx->pix_fmt != AV_PIX_FMT_NV12) {
        error = "encoder was not opened with NV12; the GPU convert path cannot feed it";
        return false;
    }
    if (enc.t0_ns == AV_NOPTS_VALUE)
        enc.t0_ns = pts_ns;

    if (av_frame_make_writable(enc.vframe) < 0) {
        error = "av_frame_make_writable (video) failed";
        return false;
    }

    // Not a conversion - the pixels are already in the encoder's layout. This is only the copy
    // into FFmpeg's own buffer, which exists because the frame must stay valid until the encoder
    // is done with it. Timed separately so it cannot hide inside a number labelled "encode".
    const auto t_copy = std::chrono::steady_clock::now();
    av_image_copy_plane(enc.vframe->data[0], enc.vframe->linesize[0], y, y_stride, enc.vctx->width,
                        enc.vctx->height);
    av_image_copy_plane(enc.vframe->data[1], enc.vframe->linesize[1], uv, uv_stride,
                        enc.vctx->width, enc.vctx->height / 2);
    enc.stats.copy_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_copy)
            .count());

    enc.vframe->pts =
        av_rescale_q(pts_ns - enc.t0_ns, AVRational{1, 1'000'000'000}, enc.vctx->time_base);

    const auto t_begin = std::chrono::steady_clock::now();
    enc.vframe->pict_type = enc.force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    enc.force_keyframe = false;
    const int r = avcodec_send_frame(enc.vctx, enc.vframe);
    if (r < 0) {
        error = std::format("avcodec_send_frame (video): {}", av_err(r));
        return false;
    }
    if (!drain(enc, enc.vctx, enc.vst, enc.stats.video_bytes, error))
        return false;
    enc.stats.video_encode_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin)
            .count());
    ++enc.stats.video_frames;
    return true;
}

bool encoder_is_nv12(Encoder& enc) {
    return enc.vctx->pix_fmt == AV_PIX_FMT_NV12;
}

bool encoder_wants_wide_pixels(const Encoder& enc) {
    if (!enc.vctx)
        return false;
    const auto* d = av_pix_fmt_desc_get(enc.vctx->pix_fmt == AV_PIX_FMT_CUDA ? AV_PIX_FMT_NV12
                                                                             : enc.vctx->pix_fmt);
    // Asked of the descriptor rather than listed by name, so a codec that falls back from P010
    // to YUV420P10 -- or to plain NV12 because it could do neither -- answers correctly without
    // this function knowing which formats the ladder can land on.
    return d != nullptr && d->comp[0].depth > 8;
}

bool encoder_takes_device_frames(const Encoder& enc) {
    return enc.vctx && enc.vctx->pix_fmt == AV_PIX_FMT_CUDA;
}

#if MUNKICAST_HAVE_FFNVCODEC
bool encoder_video_device(Encoder& enc, int fd, std::uint64_t size, std::uint64_t id,
                          std::uint32_t pitch, std::shared_ptr<const void> owner,
                          std::int64_t pts_ns, std::string& error) {
    if (!encoder_takes_device_frames(enc)) {
        error = "the encoder was not opened for GPU frames";
        return false;
    }
    const auto height = static_cast<std::uint64_t>(enc.vctx->height);
    if (fd < 0 || pitch < static_cast<std::uint32_t>(enc.vctx->width) ||
        size < std::uint64_t{pitch} * height * 3 / 2) {
        error = "invalid GPU frame";
        return false;
    }
    auto* device = reinterpret_cast<AVHWDeviceContext*>(enc.cuda_device->data);
    auto* cuda = static_cast<AVCUDADeviceContext*>(device->hwctx);
    CUcontext ignored = nullptr;
    if (enc.cu->cuCtxPushCurrent(cuda->cuda_ctx) != CUDA_SUCCESS) {
        error = "cannot enter the CUDA context";
        return false;
    }
    auto found = std::ranges::find(enc.imported, id, &Encoder::Imported::id);
    if (found == enc.imported.end()) {
        // CUDA takes ownership of the descriptor it imports, so give it a duplicate.
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle{};
        handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        handle.handle.fd = ::dup(fd);
        handle.size = size;
        handle.flags = 1; // CUDA_EXTERNAL_MEMORY_DEDICATED: the export is a dedicated allocation.
        Encoder::Imported entry{id};
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer{};
        buffer.size = size;
        if (handle.handle.fd < 0 ||
            enc.cu->cuImportExternalMemory(&entry.memory, &handle) != CUDA_SUCCESS) {
            if (handle.handle.fd >= 0)
                ::close(handle.handle.fd);
            enc.cu->cuCtxPopCurrent(&ignored);
            error = "CUDA could not import the GPU frame";
            return false;
        }
        if (enc.cu->cuExternalMemoryGetMappedBuffer(&entry.pointer, entry.memory, &buffer) !=
            CUDA_SUCCESS) {
            enc.cu->cuDestroyExternalMemory(entry.memory);
            enc.cu->cuCtxPopCurrent(&ignored);
            error = "CUDA could not map the GPU frame";
            return false;
        }
        // ponytail: the producer keeps at most eight slots; imports for retired ids are freed
        // only when the encoder closes. Resizing a canvas mid-output stops the output anyway.
        enc.imported.push_back(entry);
        found = std::prev(enc.imported.end());
    }
    enc.cu->cuCtxPopCurrent(&ignored);
    if (enc.t0_ns == AV_NOPTS_VALUE)
        enc.t0_ns = pts_ns;

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        error = "cannot allocate a GPU frame";
        return false;
    }
    frame->format = AV_PIX_FMT_CUDA;
    frame->width = enc.vctx->width;
    frame->height = enc.vctx->height;
    frame->hw_frames_ctx = av_buffer_ref(enc.cuda_frames);
    frame->data[0] = reinterpret_cast<std::uint8_t*>(found->pointer);
    frame->data[1] = frame->data[0] + std::uint64_t{pitch} * height;
    frame->linesize[0] = frame->linesize[1] = static_cast<int>(pitch);
    // NVENC holds a reference to this buffer until it has read the frame; releasing it releases
    // `owner`, which is what returns the slot to the compositor.
    auto* held = new std::shared_ptr<const void>(std::move(owner));
    frame->buf[0] = av_buffer_create(
        nullptr, 0, [](void* opaque, std::uint8_t*) { delete static_cast<std::shared_ptr<const void>*>(opaque); },
        held, 0);
    if (!frame->buf[0] || !frame->hw_frames_ctx) {
        if (!frame->buf[0])
            delete held;
        av_frame_free(&frame);
        error = "cannot wrap the GPU frame";
        return false;
    }
    frame->colorspace = enc.vctx->colorspace;
    frame->color_range = enc.vctx->color_range;
    frame->color_primaries = enc.vctx->color_primaries;
    frame->color_trc = enc.vctx->color_trc;
    frame->pts = av_rescale_q(pts_ns - enc.t0_ns, AVRational{1, 1'000'000'000}, enc.vctx->time_base);
    frame->pict_type = enc.force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    enc.force_keyframe = false;

    const auto t_begin = std::chrono::steady_clock::now();
    const int r = avcodec_send_frame(enc.vctx, frame);
    av_frame_free(&frame);
    if (r < 0) {
        error = std::format("avcodec_send_frame (video): {}", av_err(r));
        return false;
    }
    if (!drain(enc, enc.vctx, enc.vst, enc.stats.video_bytes, error))
        return false;
    enc.stats.video_encode_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin)
            .count());
    ++enc.stats.video_frames;
    return true;
}
#else
bool encoder_video_device(Encoder&, int, std::uint64_t, std::uint64_t, std::uint32_t,
                          std::shared_ptr<const void>, std::int64_t, std::string& error) {
    error = "this build has no CUDA support";
    return false;
}
#endif

const EncoderStats& encoder_stats(Encoder& enc) {
    return enc.stats;
}

void encoder_close(Encoder* enc) {
    if (enc == nullptr)
        return;
    std::string ignored;
    encoder_remove_output(*enc, 0, ignored);
    // A secondary writer owns only its muxer and packet references, but join it
    // before releasing this encoder's streams and the rest of its resources.
    enc->outputs.clear();
    if (enc->pkt != nullptr)
        av_packet_free(&enc->pkt);
    if (enc->sws != nullptr)
        sws_freeContext(enc->sws);
    if (enc->swr != nullptr)
        swr_free(&enc->swr);
    if (enc->fifo != nullptr)
        av_audio_fifo_free(enc->fifo);
    if (enc->vframe != nullptr)
        av_frame_free(&enc->vframe);
    if (enc->aframe != nullptr)
        av_frame_free(&enc->aframe);
    if (enc->vctx != nullptr)
        avcodec_free_context(&enc->vctx);
    // After NVENC is gone: it may still have held mapped imports until its context closed.
#if MUNKICAST_HAVE_FFNVCODEC
    if (enc->cu) {
        auto* device = reinterpret_cast<AVHWDeviceContext*>(enc->cuda_device->data);
        CUcontext previous = nullptr;
        enc->cu->cuCtxPushCurrent(static_cast<AVCUDADeviceContext*>(device->hwctx)->cuda_ctx);
        for (const auto& entry : enc->imported) {
            enc->cu->cuMemFree(entry.pointer); // A mapped buffer is freed before its memory.
            enc->cu->cuDestroyExternalMemory(entry.memory);
        }
        enc->cu->cuCtxPopCurrent(&previous);
        cuda_free_functions(&enc->cu);
    }
#endif
    av_buffer_unref(&enc->cuda_frames);
    av_buffer_unref(&enc->cuda_device);
    if (enc->actx != nullptr)
        avcodec_free_context(&enc->actx);
    if (enc->fmt != nullptr) {
        if (enc->network) {
            avio_closep(&enc->fmt->pb);
        } else if (enc->fmt->pb != nullptr) {
            av_freep(&enc->fmt->pb->buffer);
            avio_context_free(&enc->fmt->pb);
        }
        avformat_free_context(enc->fmt);
    }
    if (enc->file_fd >= 0)
        ::close(enc->file_fd);
    delete enc;
}

} // namespace munkicast::encode

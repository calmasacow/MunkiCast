// SPDX-FileCopyrightText: Libertas-Studio contributors
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
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <format>
#include <future>
#include <lcore/log.hpp>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace libertas::encode {

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
    bool header_written = false;
    std::vector<EncoderPtr> outputs;
    std::string output_error;
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
            if (!output.header_written)
                return;
            NetworkOperation operation(output);
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
            int result = AVERROR(ENOMEM);
            if (packet) {
                auto* stream = video ? output.vst : output.ast;
                packet->pts -= origin;
                packet->dts -= origin;
                packet->stream_index = stream->index;
                av_packet_rescale_ts(packet, ctx->time_base, stream->time_base);
                result = av_interleaved_write_frame(output.fmt, packet);
                if (result >= 0 && output.fmt->pb && output.fmt->pb->error < 0)
                    result = output.fmt->pb->error;
                av_packet_free(&packet);
            }
            if (result < 0) {
                output.output_error = std::format("Output write failed: {}", av_err(result));
                std::string ignored;
                encoder_remove_output(output, 0, ignored);
            }
        };
        write(enc);
        for (auto& output : enc.outputs)
            if (output)
                write(*output);
        av_packet_unref(enc.pkt);
        if (!enc.replay_seconds && !enc.header_written &&
            std::ranges::none_of(
                enc.outputs, [](const auto& output) { return output && output->header_written; })) {
            error = enc.output_error.empty() ? "No output destinations remain" : enc.output_error;
            return false;
        }
    }
}

/// Tries one video encoder by name. Returns nullptr when the encoder is absent or refuses to open,
/// which is the ladder's whole mechanism - "not available" and "present but unusable here" must
/// both fall through to the next rung rather than abort the run.
AVCodecContext* try_video(const char* name, const RuntimeConfig& cfg, AVFormatContext* fmt,
                          std::string& why) {
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
    c->pix_fmt =
        cfg.video_options.contains("profile") && cfg.video_options.at("profile") == "main10"
            ? AV_PIX_FMT_P010LE
            : AV_PIX_FMT_NV12;
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
    enc->vframe->format = enc->vctx->pix_fmt;
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
    if (id == 0)
        return enc.header_written;
    return id <= enc.outputs.size() && enc.outputs[id - 1] && enc.outputs[id - 1]->header_written;
}

std::string encoder_output_error(const Encoder& enc, std::size_t id) {
    if (id == 0)
        return enc.output_error;
    return id <= enc.outputs.size() && enc.outputs[id - 1] ? enc.outputs[id - 1]->output_error
                                                           : std::string{};
}

bool encoder_remove_output(Encoder& enc, std::size_t id, std::string& error) {
    Encoder* output = id == 0                    ? &enc
                      : id <= enc.outputs.size() ? enc.outputs[id - 1].get()
                                                 : nullptr;
    if (!output || !output->header_written)
        return true;
    NetworkOperation operation(*output);
    int result = av_write_trailer(output->fmt);
    output->header_written = false;
    if (output->fmt->pb) {
        avio_flush(output->fmt->pb);
        if (result >= 0 && output->fmt->pb->error < 0)
            result = output->fmt->pb->error;
    }
    if (result >= 0 && output->file_fd >= 0 && ::fsync(output->file_fd) < 0)
        result = AVERROR(errno);
    if (output->network) {
        const int closed = avio_closep(&output->fmt->pb);
        if (result >= 0)
            result = closed;
    } else if (output->fmt->pb) {
        av_freep(&output->fmt->pb->buffer);
        avio_context_free(&output->fmt->pb);
    }
    if (output->file_fd >= 0) {
        if (::close(output->file_fd) < 0 && result >= 0)
            result = AVERROR(errno);
        output->file_fd = -1;
    }
    if (result < 0) {
        error = std::format("Output did not close cleanly: {}", av_err(result));
        return false;
    }
    return true;
}

bool encoder_video(Encoder& enc, const std::uint8_t* bgra, std::size_t stride, std::int64_t pts_ns,
                   std::string& error) {
    if (!bgra || stride < static_cast<std::size_t>(enc.cfg.source_width) * 4 ||
        stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "invalid video frame stride or pixels";
        return false;
    }
    NetworkOperation operation(enc);
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
    source.format = enc.cfg.source_rgba ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
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

bool encoder_audio(Encoder& enc, const float* interleaved, std::uint32_t frames,
                   std::int64_t pts_ns, std::string& error) {
    NetworkOperation operation(enc);
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
    NetworkOperation operation(enc);
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
    NetworkOperation operation(enc);
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

const EncoderStats& encoder_stats(Encoder& enc) {
    return enc.stats;
}

void encoder_close(Encoder* enc) {
    if (enc == nullptr)
        return;
    NetworkOperation operation(*enc);
    if (enc->header_written)
        av_write_trailer(enc->fmt);
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

} // namespace libertas::encode

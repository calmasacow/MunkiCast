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
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <format>
#include <lcore/log.hpp>

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

struct Encoder {
    RuntimeConfig cfg;
    EncoderStats stats;

    AVFormatContext* fmt = nullptr;
    int file_fd = -1;

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
};

namespace {

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
        struct stat info {};
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

/// Drains packets from an encoder into the muxer.
bool drain(Encoder& enc, AVCodecContext* ctx, AVStream* st, std::uint64_t& bytes,
           std::string& error) {
    for (;;) {
        const int r = avcodec_receive_packet(ctx, enc.pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return true;
        if (r < 0) {
            error = std::format("avcodec_receive_packet: {}", av_err(r));
            return false;
        }
        enc.pkt->stream_index = st->index;
        av_packet_rescale_ts(enc.pkt, ctx->time_base, st->time_base);
        bytes += static_cast<std::uint64_t>(enc.pkt->size);
        int w = av_interleaved_write_frame(enc.fmt, enc.pkt);
        if (w >= 0 && enc.fmt->pb && enc.fmt->pb->error < 0)
            w = enc.fmt->pb->error;
        av_packet_unref(enc.pkt);
        if (w < 0) {
            error = std::format("av_interleaved_write_frame: {}", av_err(w));
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
    if (cfg.path.find('\0') != std::string::npos) {
        error = "the recording path contains a null character";
        return nullptr;
    }
    if (enc->cfg.source_width == 0)
        enc->cfg.source_width = cfg.width;
    if (enc->cfg.source_height == 0)
        enc->cfg.source_height = cfg.height;

    int r = avformat_alloc_output_context2(&enc->fmt, nullptr, nullptr, cfg.path.c_str());
    if (r < 0 || enc->fmt == nullptr) {
        error = std::format("avformat_alloc_output_context2({}): {}", cfg.path, av_err(r));
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

    enc->sws = sws_getContext(
        static_cast<int>(enc->cfg.source_width), static_cast<int>(enc->cfg.source_height),
        cfg.source_rgba ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA, static_cast<int>(cfg.width),
        static_cast<int>(cfg.height), enc->vctx->pix_fmt,
        cfg.rescale_filter == "lanczos"   ? SWS_LANCZOS
        : cfg.rescale_filter == "bicubic" ? SWS_BICUBIC
                                          : SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    if (enc->sws == nullptr) {
        error = "sws_getContext failed";
        return nullptr;
    }
    // ADR-0014 again: say which matrix and which range, do not accept a default that varies.
    const int* tbl = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(enc->sws, tbl, 1 /* src is full-range RGB */, tbl,
                             0 /* dst is limited-range YUV */, 0, 1 << 16, 1 << 16);

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

    // Create exclusively: neither an existing take nor a symlink may be overwritten.
    enc->file_fd = ::open(cfg.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (enc->file_fd < 0) {
        error = std::format("Cannot create recording '{}': {}", cfg.path, std::strerror(errno));
        return nullptr;
    }
    constexpr int buffer_size = 32768;
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(buffer_size));
    if (!buffer) {
        error = "cannot allocate the recording write buffer";
        return nullptr;
    }
    enc->fmt->pb = avio_alloc_context(buffer, buffer_size, 1, enc.get(), nullptr,
                                      file_write, file_seek);
    if (!enc->fmt->pb) {
        av_free(buffer);
        error = "cannot allocate the recording file writer";
        return nullptr;
    }
    enc->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Always write a fragmented container, so an interrupted recording is still a valid file.
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    if ((r = avformat_write_header(enc->fmt, &opts)) < 0) {
        av_dict_free(&opts);
        error = std::format("avformat_write_header: {}", av_err(r));
        return nullptr;
    }
    av_dict_free(&opts);
    enc->header_written = true;
    enc->pkt = av_packet_alloc();
    return enc;
}

bool encoder_video(Encoder& enc, const std::uint8_t* bgra, std::size_t stride, std::int64_t pts_ns,
                   std::string& error) {
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
    const std::uint8_t* src[4] = {bgra, nullptr, nullptr, nullptr};
    const int src_stride[4] = {static_cast<int>(stride), 0, 0, 0};
    sws_scale(enc.sws, src, src_stride, 0, static_cast<int>(enc.cfg.source_height),
              enc.vframe->data, enc.vframe->linesize);
    enc.stats.convert_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_convert)
            .count());

    enc.vframe->pts =
        av_rescale_q(pts_ns - enc.t0_ns, AVRational{1, 1'000'000'000}, enc.vctx->time_base);

    const auto t_begin = std::chrono::steady_clock::now();
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
    int r = av_write_trailer(enc.fmt);
    enc.header_written = false;
    avio_flush(enc.fmt->pb);
    if (r >= 0 && enc.fmt->pb->error < 0)
        r = enc.fmt->pb->error;
    if (r >= 0 && enc.file_fd >= 0 && ::fsync(enc.file_fd) < 0)
        r = AVERROR(errno);
    if (r < 0) {
        error = std::format("av_write_trailer: {}", av_err(r));
        return false;
    }
    enc.header_written = false;
    return true;
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
        if (enc->fmt->pb != nullptr) {
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

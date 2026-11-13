// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFile>
#include <QImage>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string_view>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

// WebM alpha lives in block side data. FFmpeg's native VP8/VP9 decoders discard
// it; libvpx consumes it and returns YUVA, which must remain RGBA through IPC.
class AlphaVideo {
    AVFormatContext* input_ = nullptr;
    AVCodecContext* decoder_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* convert_ = nullptr;
    int stream_ = -1;
    bool draining_ = false, paused_ = false;
    QTimer timer_;
    std::function<qint64()> position_;
    double speed_ = 1;
    QImage pending_;
    qint64 due_ = 0;
    qint64 end_ = 0;
    std::function<void(const QImage&)> publish_;
    std::function<void()> finished_;
    // Short clips (stingers, overlays) are read once into memory so playback never waits on
    // the file's storage -- a network share stalling mid-clip froze every source reading it.
    QByteArray bytes_;
    qint64 offset_ = 0;
    AVIOContext* io_ = nullptr;

    static int read_memory(void* opaque, uint8_t* buffer, int size) {
        auto* self = static_cast<AlphaVideo*>(opaque);
        const auto left = self->bytes_.size() - self->offset_;
        if (left <= 0) return AVERROR_EOF;
        const auto n = static_cast<int>(std::min<qint64>(size, left));
        std::memcpy(buffer, self->bytes_.constData() + self->offset_, static_cast<std::size_t>(n));
        self->offset_ += n;
        return n;
    }
    static int64_t seek_memory(void* opaque, int64_t offset, int whence) {
        auto* self = static_cast<AlphaVideo*>(opaque);
        const qint64 size = self->bytes_.size();
        if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return size;
        const qint64 base = whence == SEEK_CUR ? self->offset_ : whence == SEEK_END ? size : 0;
        const qint64 target = base + offset;
        if (target < 0 || target > size) return AVERROR(EINVAL);
        self->offset_ = target;
        return target;
    }

    qint64 position() const {
        return static_cast<qint64>(static_cast<double>(position_()) / speed_);
    }

    void tick() {
        try {
            if (!pending_.isNull()) {
                if (position() < due_) {
                    timer_.start(static_cast<int>(std::min<qint64>(due_ - position(), 1000)));
                    return;
                }
                publish_(pending_);
                pending_ = {};
                if (paused_) return;
            }
            for (int tries = 0; tries < 64; ++tries) {
                const int received = avcodec_receive_frame(decoder_, frame_);
                if (received == 0) {
                    const int w = frame_->width, h = frame_->height;
                    if (w < 1 || h < 1 || w > 8192 || h > 8192 ||
                        static_cast<std::int64_t>(w) * h * 4 > 128 * 1024 * 1024)
                        throw std::runtime_error("alpha video dimensions exceed image budget");
                    // swscale SIMD writes whole pixel blocks; keep row ends inside the allocation.
                    QImage converted((w + 15) & ~15, h, QImage::Format_RGBA8888);
                    if (converted.isNull())
                        throw std::runtime_error("alpha video allocation failed");
                    convert_ = sws_getCachedContext(
                        convert_, w, h, static_cast<AVPixelFormat>(frame_->format), w, h,
                        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!convert_)
                        throw std::runtime_error("alpha video conversion unavailable");
                    uint8_t* dst[] = {converted.bits()};
                    int stride[] = {static_cast<int>(converted.bytesPerLine())};
                    if (sws_scale(convert_, frame_->data, frame_->linesize, 0, h, dst, stride) != h)
                        throw std::runtime_error("alpha video conversion failed");
                    pending_ = converted.width() == w ? converted : converted.copy(0, 0, w, h);
                    if (pending_.isNull())
                        throw std::runtime_error("alpha video allocation failed");
                    const auto* stream = input_->streams[stream_];
                    const auto start =
                        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
                    const auto pts = frame_->best_effort_timestamp;
                    due_ = pts == AV_NOPTS_VALUE
                               ? position()
                               : static_cast<qint64>(
                                     static_cast<double>(av_rescale_q(
                                         pts - start, stream->time_base, AVRational{1, 1000})) /
                                     speed_);
                    const auto duration =
                        av_rescale_q(frame_->duration, stream->time_base, AVRational{1, 1000});
                    end_ = due_ + static_cast<qint64>(static_cast<double>(duration) / speed_);
                    av_frame_unref(frame_);
                    timer_.start(0);
                    return;
                }
                if (received == AVERROR_EOF) {
                    if (position() < end_)
                        timer_.start(static_cast<int>(std::min<qint64>(end_ - position(), 1000)));
                    else
                        finished_();
                    return;
                }
                if (received != AVERROR(EAGAIN))
                    throw std::runtime_error("alpha video decode failed");
                if (draining_)
                    throw std::runtime_error("alpha video decoder stalled");
                const int read = av_read_frame(input_, packet_);
                if (read == AVERROR_EOF) {
                    draining_ = true;
                    if (avcodec_send_packet(decoder_, nullptr) < 0)
                        throw std::runtime_error("alpha video flush failed");
                } else if (read < 0) {
                    throw std::runtime_error("alpha video read failed");
                } else {
                    const int sent = packet_->stream_index == stream_
                                         ? avcodec_send_packet(decoder_, packet_)
                                         : 0;
                    av_packet_unref(packet_);
                    if (sent < 0)
                        throw std::runtime_error("alpha video packet rejected");
                }
            }
            timer_.start(0);
        } catch (const std::exception& error) {
            qWarning("media source: %s", error.what());
            publish_({});
        }
    }

  public:
    // The custom I/O callbacks hold `this`.
    AlphaVideo(const AlphaVideo&) = delete;
    AlphaVideo& operator=(const AlphaVideo&) = delete;
    AlphaVideo() {
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] { tick(); });
    }

    ~AlphaVideo() {
        timer_.stop();
        sws_freeContext(convert_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&decoder_);
        avformat_close_input(&input_);
        if (io_) av_freep(&io_->buffer);
        avio_context_free(&io_);
    }

    bool open(const QString& path) {
        // ponytail: whole-file read up to 64 MB; longer clips still stream from disk.
        if (QFile file(path); file.size() > 0 && file.size() <= 64 * 1024 * 1024 &&
                              file.open(QIODevice::ReadOnly)) {
            bytes_ = file.readAll();
            constexpr int chunk = 64 * 1024;
            auto* buffer = static_cast<unsigned char*>(av_malloc(chunk));
            if (bytes_.size() == file.size() && buffer &&
                (io_ = avio_alloc_context(buffer, chunk, 0, this, read_memory, nullptr, seek_memory))) {
                input_ = avformat_alloc_context();
                if (!input_) return false;
                input_->pb = io_;
                input_->flags |= AVFMT_FLAG_CUSTOM_IO;
            } else {
                av_free(buffer);
                bytes_.clear();
            }
        }
        if (avformat_open_input(&input_, io_ ? nullptr : path.toUtf8().constData(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(input_, nullptr) < 0)
            return false;
        stream_ = av_find_best_stream(input_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_ < 0)
            return false;
        const auto* stream = input_->streams[stream_];
        const auto* alpha = av_dict_get(stream->metadata, "alpha_mode", nullptr, 0);
        if (!alpha || std::string_view(alpha->value) != "1")
            return false;
        const auto codec = stream->codecpar->codec_id;
        const auto* implementation =
            avcodec_find_decoder_by_name(codec == AV_CODEC_ID_VP9   ? "libvpx-vp9"
                                         : codec == AV_CODEC_ID_VP8 ? "libvpx"
                                                                    : "");
        if (!implementation)
            throw std::runtime_error("WebM alpha requires FFmpeg libvpx decoder");
        decoder_ = avcodec_alloc_context3(implementation);
        if (!decoder_ || avcodec_parameters_to_context(decoder_, stream->codecpar) < 0)
            throw std::runtime_error("alpha video decoder allocation failed");
        decoder_->thread_count = 2;
        decoder_->max_pixels = 128 * 1024 * 1024 / 4;
        if (avcodec_open2(decoder_, implementation, nullptr) < 0)
            throw std::runtime_error("alpha video decoder could not open");
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!packet_ || !frame_)
            throw std::runtime_error("alpha video frame allocation failed");
        return true;
    }

    void pause() { paused_ = true; timer_.stop(); }
    void resume() { paused_ = false; timer_.start(0); }
    void rewind() {
        paused_ = false;
        timer_.stop();
        const auto* stream = input_->streams[stream_];
        const auto start = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        if (av_seek_frame(input_, stream_, start, AVSEEK_FLAG_BACKWARD) < 0)
            throw std::runtime_error("alpha video seek failed");
        avcodec_flush_buffers(decoder_);
        pending_ = {};
        draining_ = false;
        due_ = end_ = 0;
        timer_.start(0);
    }

    bool has_audio() const {
        return av_find_best_stream(input_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
    }

    void start(double speed, std::function<qint64()> position,
               std::function<void(const QImage&)> publish, std::function<void()> finished) {
        speed_ = speed;
        publish_ = std::move(publish);
        finished_ = std::move(finished);
        position_ = std::move(position);
        timer_.start(0);
    }
};

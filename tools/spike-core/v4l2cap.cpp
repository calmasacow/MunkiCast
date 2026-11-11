// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// V4L2 capture-card source.
//
// Written against an Elgato 4K X, which matters because of what it is NOT: it presents as a
// generic UVC "Camera", reports input Status: ok whether or not an HDMI source is attached, and
// does not implement VIDIOC_QUERY_DV_TIMINGS. So there is no ioctl that answers "is there a
// signal?" - with no input it simply streams black frames at full rate. Signal state has to be
// inferred, and this probe measures the luma needed to do it.

#include "v4l2cap.hpp"

#include <lcore/log.hpp>

#include <chrono>
#include <algorithm>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

namespace munkicast::spike {

namespace {

int xioctl(int fd, unsigned long req, void* arg) {
    int r = 0;
    do {
        r = ::ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

struct Buffer {
    void* start = nullptr;
    std::size_t length = 0;
    int dmabuf_fd = -1;
};

/// Statistics over the LUMA plane only.
///
/// The first version of this sampled the whole buffer, and that was wrong in a way that defeated
/// the entire purpose of the function: in NV12/YU12/P010 the chroma plane sits after the luma
/// plane and neutral chroma is 128, so a frame that is genuinely black averages 42.67 over the
/// whole buffer, never 0. Any "is it black?" threshold applied to that number reports signal on a
/// disconnected card, forever. Plane 0 must be bounded explicitly.
struct PlaneStats {
    double mean = 0.0;
    std::uint8_t min = 255;
    std::uint8_t max = 0;
};

PlaneStats plane0_stats(const std::uint8_t* p, std::size_t len, std::uint32_t fourcc) {
    PlaneStats st;
    if (len == 0)
        return st;
    std::size_t off = 0;
    std::size_t step = 1;
    if (fourcc == V4L2_PIX_FMT_YUYV) {
        step = 2; // Y0 U Y1 V - sample only luma
    } else if (fourcc == V4L2_PIX_FMT_P010) {
        // 10 bits in the HIGH end of a 16-bit little-endian word, so the top 8 bits live at +1.
        off = 1;
        step = 2;
    } else if (fourcc == V4L2_PIX_FMT_BGR24) {
        step = 3; // blue channel: a presence check, not a luma measurement
    }
    // Sample rather than scan: this runs per frame and only needs to answer a yes/no question.
    const std::size_t stride = step * 97;
    std::uint64_t sum = 0;
    std::size_t n = 0;
    for (std::size_t i = off; i < len; i += stride) {
        const std::uint8_t v = p[i];
        sum += v;
        st.min = std::min(st.min, v);
        st.max = std::max(st.max, v);
        ++n;
    }
    st.mean = n > 0 ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    return st;
}

/// Bytes of plane 0, which is NOT the whole buffer for planar formats.
std::size_t plane0_len(const v4l2_format& fmt) {
    const std::uint32_t fourcc = fmt.fmt.pix.pixelformat;
    const std::size_t bpl = fmt.fmt.pix.bytesperline;
    const std::size_t h = fmt.fmt.pix.height;
    switch (fourcc) {
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_P010:
        return bpl * h;
    default:
        return fmt.fmt.pix.sizeimage; // packed: plane 0 is the frame
    }
}

} // namespace

std::string v4l2_fourcc_str(std::uint32_t f) {
    char s[5] = {static_cast<char>(f & 0xff), static_cast<char>((f >> 8) & 0xff),
                 static_cast<char>((f >> 16) & 0xff), static_cast<char>((f >> 24) & 0xff), 0};
    return std::string(s);
}

std::string v4l2_quantization_str(std::uint32_t q) {
    switch (q) {
    case V4L2_QUANTIZATION_DEFAULT: return "default";
    case V4L2_QUANTIZATION_FULL_RANGE: return "full";
    case V4L2_QUANTIZATION_LIM_RANGE: return "limited";
    default: return std::format("?{}", q);
    }
}

std::string v4l2_colorspace_str(std::uint32_t c) {
    switch (c) {
    case V4L2_COLORSPACE_DEFAULT: return "default";
    case V4L2_COLORSPACE_SRGB: return "sRGB";
    case V4L2_COLORSPACE_REC709: return "Rec.709";
    case V4L2_COLORSPACE_BT2020: return "BT.2020";
    case V4L2_COLORSPACE_SMPTE170M: return "SMPTE170M";
    case V4L2_COLORSPACE_RAW: return "raw";
    default: return std::format("?{}", c);
    }
}

std::string v4l2_ycbcr_enc_str(std::uint32_t e) {
    switch (e) {
    case V4L2_YCBCR_ENC_DEFAULT: return "default";
    case V4L2_YCBCR_ENC_601: return "BT.601";
    case V4L2_YCBCR_ENC_709: return "BT.709";
    case V4L2_YCBCR_ENC_BT2020: return "BT.2020";
    default: return std::format("?{}", e);
    }
}

std::string v4l2_xfer_func_str(std::uint32_t x) {
    switch (x) {
    case V4L2_XFER_FUNC_DEFAULT: return "default";
    case V4L2_XFER_FUNC_709: return "Rec.709";
    case V4L2_XFER_FUNC_SRGB: return "sRGB";
    case V4L2_XFER_FUNC_SMPTE2084: return "PQ";
    case V4L2_XFER_FUNC_NONE: return "linear";
    default: return std::format("?{}", x);
    }
}

std::uint32_t v4l2_fourcc_parse(const std::string& s) {
    std::uint32_t f = 0;
    for (std::size_t i = 0; i < 4; ++i)
        f |= static_cast<std::uint32_t>(i < s.size() ? s[i] : ' ') << (8 * i);
    return f;
}

bool v4l2_capture(const V4l2Config& cfg, V4l2Result& out, std::string& error) {
    const int fd = ::open(cfg.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        error = std::format("open {}: {}", cfg.device, std::strerror(errno));
        return false;
    }
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard{fd};

    v4l2_capability capability{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        error = std::format("VIDIOC_QUERYCAP: {}", std::strerror(errno));
        return false;
    }
    if ((capability.device_caps & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        error = "device does not support VIDEO_CAPTURE";
        return false;
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        error = std::format("VIDIOC_G_FMT: {}", std::strerror(errno));
        return false;
    }
    if (cfg.width != 0)
        fmt.fmt.pix.width = cfg.width;
    if (cfg.height != 0)
        fmt.fmt.pix.height = cfg.height;
    if (cfg.fourcc != 0)
        fmt.fmt.pix.pixelformat = cfg.fourcc;
    // Colour signalling is a request, not a command. UVC drivers commonly ignore it outright.
    if (cfg.quantization != 0)
        fmt.fmt.pix.quantization = cfg.quantization;
    if (cfg.colorspace != 0)
        fmt.fmt.pix.colorspace = cfg.colorspace;
    if (cfg.ycbcr_enc != 0)
        fmt.fmt.pix.ycbcr_enc = cfg.ycbcr_enc;
    if (cfg.xfer_func != 0)
        fmt.fmt.pix.xfer_func = cfg.xfer_func;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        error = std::format("VIDIOC_S_FMT: {}", std::strerror(errno));
        return false;
    }
    // S_FMT negotiates: the driver may hand back something other than what was asked for, and
    // believing the request rather than the reply is how a capture path ends up misreading memory.
    out.width = fmt.fmt.pix.width;
    out.height = fmt.fmt.pix.height;
    out.fourcc = fmt.fmt.pix.pixelformat;
    out.quantization = fmt.fmt.pix.quantization;
    out.colorspace = fmt.fmt.pix.colorspace;
    out.ycbcr_enc = fmt.fmt.pix.ycbcr_enc;
    out.xfer_func = fmt.fmt.pix.xfer_func;
    if (cfg.quantization != 0 && fmt.fmt.pix.quantization != cfg.quantization)
        lc::warn("requested quantization {} but the driver replied {} - it is not negotiable here",
                 cfg.quantization, fmt.fmt.pix.quantization);

    if (cfg.fps_num != 0) {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = cfg.fps_den;
        parm.parm.capture.timeperframe.denominator = cfg.fps_num;
        if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
            lc::warn("VIDIOC_S_PARM failed: {} (continuing at the device's own rate)",
                     std::strerror(errno));
    }

    v4l2_requestbuffers req{};
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        error = std::format("VIDIOC_REQBUFS: {}", std::strerror(errno));
        return false;
    }

    std::vector<Buffer> buffers(req.count);
    for (std::uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            error = std::format("VIDIOC_QUERYBUF {}: {}", i, std::strerror(errno));
            return false;
        }
        buffers[i].length = buf.length;
        buffers[i].start =
            ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            error = std::format("mmap {}: {}", i, std::strerror(errno));
            return false;
        }

        if (cfg.export_dmabuf) {
            // The zero-copy question for capture cards: can the driver hand us a dmabuf we can
            // import into Vulkan, instead of a memcpy out of an mmap?
            v4l2_exportbuffer exp{};
            exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            exp.index = i;
            exp.flags = O_CLOEXEC | O_RDONLY;
            if (xioctl(fd, VIDIOC_EXPBUF, &exp) == 0) {
                buffers[i].dmabuf_fd = exp.fd;
                out.dmabuf_exported = true;
            } else if (i == 0) {
                lc::warn("VIDIOC_EXPBUF unsupported ({}); falling back to mmap",
                         std::strerror(errno));
            }
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            error = std::format("VIDIOC_QBUF {}: {}", i, std::strerror(errno));
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        error = std::format("VIDIOC_STREAMON: {}", std::strerror(errno));
        return false;
    }

    const std::size_t p0 = plane0_len(fmt);

    out.driver_ms.reserve(cfg.frames);
    out.wall_ms.reserve(cfg.frames);
    out.luma.reserve(cfg.frames);

    double prev_driver = 0;
    double first_driver = 0;
    std::uint32_t prev_seq = 0;
    auto prev_wall = std::chrono::steady_clock::now();
    bool first = true;

    while (out.captured < cfg.frames) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv{};
        tv.tv_sec = 5;
        const int sel = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            error = std::format("select: {}", std::strerror(errno));
            break;
        }
        if (sel == 0) {
            error = "timed out waiting for a frame (5 s)";
            break;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN)
                continue;
            error = std::format("VIDIOC_DQBUF: {}", std::strerror(errno));
            break;
        }

        const double driver_ms = static_cast<double>(buf.timestamp.tv_sec) * 1000.0 +
                                 static_cast<double>(buf.timestamp.tv_usec) / 1000.0;
        const auto wall = std::chrono::steady_clock::now();
        if (!first) {
            out.driver_ms.push_back(driver_ms - prev_driver);
            out.wall_ms.push_back(
                std::chrono::duration<double, std::milli>(wall - prev_wall).count());
            // A gap of more than 1 means the driver produced frames we never saw. That is a real
            // drop, and it is invisible to every timing statistic computed from what we did see.
            const std::uint32_t step_seq = buf.sequence - prev_seq;
            if (step_seq != 1) {
                ++out.seq_breaks;
                out.seq_missing += step_seq > 1 ? step_seq - 1 : 0;
            }
        } else {
            out.seq_first = buf.sequence;
            first_driver = driver_ms;
        }
        out.seq_last = buf.sequence;
        out.span_ms = driver_ms - first_driver;
        prev_seq = buf.sequence;
        first = false;
        prev_driver = driver_ms;
        prev_wall = wall;

        const std::size_t avail = buf.bytesused != 0 ? buf.bytesused : buffers[buf.index].length;
        const PlaneStats st = plane0_stats(
            static_cast<const std::uint8_t*>(buffers[buf.index].start), std::min(p0, avail),
            out.fourcc);
        out.luma.push_back(st.mean);
        out.luma_min.push_back(st.min);
        out.luma_max.push_back(st.max);
        ++out.captured;

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            error = std::format("VIDIOC_QBUF (requeue): {}", std::strerror(errno));
            break;
        }
    }

    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& b : buffers) {
        if (b.dmabuf_fd >= 0)
            ::close(b.dmabuf_fd);
        if (b.start != nullptr && b.start != MAP_FAILED)
            ::munmap(b.start, b.length);
    }
    return out.captured > 0;
}

} // namespace munkicast::spike

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "image.hpp"

#include "lcomp/composite.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
}

namespace lcomp {
namespace {

/// The same ceiling `composite_canvas` uses for a canvas. A still past it is refused rather
/// than allocated: the refusal is the result, and a smaller stand-in would be a different one.
constexpr std::uint64_t kMaxImageBytes = 128ull * 1024ull * 1024ull;

bool size_ok(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0)
        return false;
    if (width > static_cast<std::uint32_t>(kMaxPreviewEdge) ||
        height > static_cast<std::uint32_t>(kMaxPreviewEdge))
        return false;
    const auto bytes = static_cast<std::uint64_t>(width) * height * 4u;
    return bytes <= kMaxImageBytes &&
           bytes <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

struct PngFailed : std::exception {};

void png_throw(png_structp, png_const_charp) {
    throw PngFailed();
}

void png_quiet(png_structp, png_const_charp) {}

std::optional<DecodedImage> decode_png(const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr)
        return std::nullopt;
    png_structp png = nullptr;
    png_infop info = nullptr;
    try {
        png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, png_throw, png_quiet);
        if (png == nullptr) {
            std::fclose(fp);
            return std::nullopt;
        }
        info = png_create_info_struct(png);
        if (info == nullptr) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            std::fclose(fp);
            return std::nullopt;
        }
        png_init_io(png, fp);
        png_read_info(png, info);

        png_uint_32 width = 0;
        png_uint_32 height = 0;
        int bit_depth = 0;
        int color_type = 0;
        png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, nullptr, nullptr,
                     nullptr);
        if (!size_ok(width, height)) {
            png_destroy_read_struct(&png, &info, nullptr);
            std::fclose(fp);
            return std::nullopt;
        }

        if (color_type == PNG_COLOR_TYPE_PALETTE)
            png_set_palette_to_rgb(png);
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
            png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS) != 0)
            png_set_tRNS_to_alpha(png);
        if (bit_depth == 16)
            png_set_strip_16(png);
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
            png_set_gray_to_rgb(png);
        const bool have_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
                                png_get_valid(png, info, PNG_INFO_tRNS) != 0;
        if (!have_alpha)
            png_set_filler(png, 0xff, PNG_FILLER_AFTER);
        png_set_interlace_handling(png);
        png_read_update_info(png, info);

        const png_size_t row_bytes = png_get_rowbytes(png, info);
        if (png_get_channels(png, info) != 4 || png_get_bit_depth(png, info) != 8 ||
            row_bytes != static_cast<png_size_t>(width) * 4u) {
            png_destroy_read_struct(&png, &info, nullptr);
            std::fclose(fp);
            return std::nullopt;
        }

        DecodedImage image;
        image.width = static_cast<std::int32_t>(width);
        image.height = static_cast<std::int32_t>(height);
        image.pixels.resize(static_cast<std::size_t>(row_bytes) * height);
        std::vector<png_bytep> rows(height);
        for (png_uint_32 y = 0; y < height; ++y)
            rows[y] = image.pixels.data() + static_cast<std::size_t>(y) * row_bytes;
        png_read_image(png, rows.data());
        png_read_end(png, nullptr);
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return image;
    } catch (const PngFailed&) {
        if (png != nullptr)
            png_destroy_read_struct(&png, info != nullptr ? &info : nullptr, nullptr);
        std::fclose(fp);
        return std::nullopt;
    }
}

struct JpegFailed : std::exception {};

void jpeg_throw(j_common_ptr) {
    throw JpegFailed();
}

void jpeg_quiet(j_common_ptr) {}

std::optional<DecodedImage> decode_jpeg(const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr)
        return std::nullopt;

    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = jpeg_throw;
    jerr.output_message = jpeg_quiet;

    try {
        jpeg_create_decompress(&cinfo);
        jpeg_stdio_src(&cinfo, fp);
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        if (cinfo.output_components != 3 || !size_ok(cinfo.output_width, cinfo.output_height))
            throw JpegFailed();

        DecodedImage image;
        image.width = static_cast<std::int32_t>(cinfo.output_width);
        image.height = static_cast<std::int32_t>(cinfo.output_height);
        image.pixels.assign(static_cast<std::size_t>(image.width) *
                                static_cast<std::size_t>(image.height) * 4u,
                            255);
        std::vector<JSAMPLE> row(static_cast<std::size_t>(image.width) * 3u);
        JSAMPROW rows[1] = {row.data()};
        std::size_t at = 0;
        while (cinfo.output_scanline < cinfo.output_height) {
            jpeg_read_scanlines(&cinfo, rows, 1);
            for (int x = 0; x < image.width; ++x) {
                image.pixels[at++] = row[static_cast<std::size_t>(x) * 3u];
                image.pixels[at++] = row[static_cast<std::size_t>(x) * 3u + 1u];
                image.pixels[at++] = row[static_cast<std::size_t>(x) * 3u + 2u];
                image.pixels[at++] = 255;
            }
        }
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return image;
    } catch (const JpegFailed&) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return std::nullopt;
    }
}

std::optional<DecodedImage> decode_webp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > kMaxImageBytes)
        return std::nullopt;
    in.seekg(0);
    std::vector<std::uint8_t> file(static_cast<std::size_t>(length));
    in.read(reinterpret_cast<char*>(file.data()), length);
    if (!in)
        return std::nullopt;

    int width = 0;
    int height = 0;
    if (WebPGetInfo(file.data(), file.size(), &width, &height) == 0)
        return std::nullopt;
    if (width <= 0 || height <= 0 ||
        !size_ok(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)))
        return std::nullopt;

    std::uint8_t* decoded = WebPDecodeRGBA(file.data(), file.size(), &width, &height);
    if (decoded == nullptr)
        return std::nullopt;
    DecodedImage image;
    image.width = width;
    image.height = height;
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    image.pixels.assign(decoded, decoded + bytes);
    WebPFree(decoded);
    return image;
}

} // namespace

std::optional<DecodedImage> decode_image(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    unsigned char magic[12] = {};
    in.read(reinterpret_cast<char*>(magic), sizeof magic);
    const auto got = in.gcount();
    if (got >= 8 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
        return decode_png(path);
    if (got >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
        return decode_jpeg(path);
    if (got >= 12 && std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "WEBP", 4) == 0)
        return decode_webp(path);
    return std::nullopt;
}

namespace {
std::optional<FileStamp> stat_file(const std::string& path) {
    std::error_code ec;
    FileStamp stamp;
    stamp.written = std::filesystem::last_write_time(path, ec);
    if (ec)
        return std::nullopt;
    stamp.size = std::filesystem::file_size(path, ec);
    if (ec)
        return std::nullopt;
    return stamp;
}

struct Watched {
    std::optional<FileStamp> stamp;
    bool slow = false; // A stat took long enough to cost a frame; only the watcher stats it.
};

struct StampWatcher {
    std::mutex mu;
    std::map<std::string, Watched> paths;
    std::jthread thread{[this](std::stop_token stop) {
        std::mutex sleep_mu;
        std::condition_variable_any sleep;
        while (!stop.stop_requested()) {
            std::vector<std::string> slow;
            {
                std::lock_guard lock(mu);
                for (const auto& [path, watched] : paths)
                    if (watched.slow)
                        slow.push_back(path);
            }
            for (const auto& path : slow) {
                auto stamp = stat_file(path);
                std::lock_guard lock(mu);
                paths[path].stamp = stamp;
            }
            std::unique_lock sleep_lock(sleep_mu);
            sleep.wait_for(sleep_lock, stop, std::chrono::milliseconds(500), [] { return false; });
        }
    }};
};
} // namespace

std::optional<FileStamp> file_stamp(const std::string& path) {
    // ponytail: watched paths are never forgotten; a show references a bounded set of files.
    static StampWatcher watcher;
    {
        std::lock_guard lock(watcher.mu);
        if (const auto it = watcher.paths.find(path); it != watcher.paths.end() && it->second.slow)
            return it->second.stamp;
    }
    const auto started = std::chrono::steady_clock::now();
    auto stamp = stat_file(path);
    const bool slow = std::chrono::steady_clock::now() - started > std::chrono::milliseconds(2);
    std::lock_guard lock(watcher.mu);
    watcher.paths[path] = {stamp, slow};
    return stamp;
}

} // namespace lcomp

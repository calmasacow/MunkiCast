// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lut.hpp"

#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace lcomp {
std::shared_ptr<const Lut> load_lut(const std::string& path) {
    struct Cached {
        FileStamp stamp;
        std::shared_ptr<Lut> lut;
    };

    static thread_local std::map<std::string, Cached> cache;
    const auto stamp = file_stamp(path);
    if (!stamp || stamp->size > 64 * 1024 * 1024)
        return {};
    if (const auto it = cache.find(path); it != cache.end() && it->second.stamp == *stamp)
        return it->second.lut;
    auto lut = std::make_shared<Lut>();
    lut->image = std::make_shared<Picture>();
    std::vector<std::array<float, 3>> entries;
    bool one = false;
    if (std::filesystem::path(path).extension() == ".cube") {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            if (line.size() > 4096)
                return {};
            std::istringstream stream(line);
            std::string token;
            if (!(stream >> token) || token[0] == '#')
                continue;
            if (token == "TITLE")
                continue;
            if (token == "LUT_3D_SIZE" || token == "LUT_1D_SIZE") {
                if (lut->size || !(stream >> lut->size) || lut->size < 2 || lut->size > 64)
                    return {};
                one = token == "LUT_1D_SIZE";
                continue;
            }
            if (token == "DOMAIN_MIN" || token == "DOMAIN_MAX") {
                auto& values = token == "DOMAIN_MIN" ? lut->minimum : lut->maximum;
                for (auto& value : values)
                    if (!(stream >> value) || !std::isfinite(value))
                        return {};
                continue;
            }
            std::array<float, 3> value{};
            std::istringstream values(line);
            for (auto& channel : value)
                if (!(values >> channel) || !std::isfinite(channel) || channel < 0 || channel > 1)
                    return {};
            entries.push_back(value);
            if (entries.size() > 64 * 64 * 64)
                return {};
        }
        const auto n = static_cast<std::size_t>(lut->size);
        if (n < 2 || entries.size() != (one ? n : n * n * n))
            return {};
    } else {
        const auto decoded = decode_image(path);
        if (!decoded)
            return {};
        lut->size = 64;
        if (decoded->width % 64 || decoded->height % 64 ||
            decoded->width * decoded->height != 64 * 64 * 64)
            return {};
        const int tiles = decoded->width / 64;
        entries.reserve(64 * 64 * 64);
        for (int z = 0; z < 64; ++z)
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    const auto index =
                        static_cast<std::size_t>(((z / tiles) * 64 + y) * decoded->width +
                                                 (z % tiles) * 64 + x) *
                        4;
                    entries.push_back({decoded->pixels[index] / 255.0f,
                                       decoded->pixels[index + 1] / 255.0f,
                                       decoded->pixels[index + 2] / 255.0f});
                }
    }
    for (std::size_t i = 0; i < 3; ++i)
        if (lut->minimum[i] >= lut->maximum[i])
            return {};
    const auto n = static_cast<std::size_t>(lut->size);
    auto& image = *lut->image;
    image.width = lut->size * lut->size;
    image.height = lut->size;
    image.stride = static_cast<unsigned>(image.width) * 4;
    image.pixels.resize(n * n * n * 4);
    for (std::size_t b = 0; b < n; ++b)
        for (std::size_t g = 0; g < n; ++g)
            for (std::size_t r = 0; r < n; ++r) {
                const auto dst = (g * n * n + b * n + r) * 4;
                const auto value =
                    one ? std::array<float, 3>{entries[r][0], entries[g][1], entries[b][2]}
                        : entries[b * n * n + g * n + r];
                for (std::size_t k = 0; k < 3; ++k)
                    image.pixels[dst + k] = static_cast<std::uint8_t>(std::lround(value[k] * 255));
                image.pixels[dst + 3] = 255;
            }
    if (cache.size() >= 8)
        cache.clear();
    cache[path] = {*stamp, lut};
    return lut;
}
} // namespace lcomp

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lcomp {

/// A still, straight sRGB8, tightly packed. The same pixel layout as `Picture`.
struct DecodedImage {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

/// PNG, JPEG, or WebP, recognised from the bytes rather than the name.
///
/// Empty when there is nothing to draw: the file is missing, it is not one of those, or it is
/// larger than the preview will allocate. A stand-in would be a picture the operator did not
/// choose.
[[nodiscard]] std::optional<DecodedImage> decode_image(const std::string& path);

/// A file's change stamp: modification time and size. Unset when it cannot be read.
struct FileStamp {
    std::filesystem::file_time_type written{};
    std::uintmax_t size = 0;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

/// The stamp caches use to notice a replaced file.
///
/// A stat on a network or FUSE mount (gvfs) sometimes takes 10+ ms, which stalled whole frames
/// once a second. Paths stat inline while that stays under 2 ms. Once a stat is slower, the path
/// is handed to a background thread that refreshes it twice a second, and this returns its last
/// answer, so a file replaced on a slow mount shows up within about half a second.
[[nodiscard]] std::optional<FileStamp> file_stamp(const std::string& path);

} // namespace lcomp

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcapture/dmabuf.hpp>

namespace munkicast::capture {

std::string fourcc_str(std::uint32_t fourcc) {
    if (fourcc == 0)
        return "(none)";
    char b[5] = {static_cast<char>(fourcc & 0xff), static_cast<char>((fourcc >> 8) & 0xff),
                 static_cast<char>((fourcc >> 16) & 0xff), static_cast<char>((fourcc >> 24) & 0xff),
                 '\0'};
    for (char& c : b)
        if (c != '\0' && (c < 32 || c > 126))
            c = '?';
    return std::string(b);
}

} // namespace munkicast::capture

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <lcomp/composite.hpp>
#include <memory>
#include <string>

namespace lcomp {
struct Lut {
    int size = 0;
    std::array<float, 3> minimum{}, maximum{1, 1, 1};
    std::shared_ptr<Picture> image;
};

std::shared_ptr<const Lut> load_lut(const std::string& path);
} // namespace lcomp

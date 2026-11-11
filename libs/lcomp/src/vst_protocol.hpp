// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace lcomp::vst {
inline constexpr std::uint32_t magic = 0x4c565354;

struct Header {
    std::uint32_t magic_value = magic;
    std::int32_t parameters = 0, programs = 0, latency = 0;
};

struct Parameter {
    std::array<char, 64> name{}, unit{};
    float value = 0;
};

struct Block {
    std::uint32_t operation = 0;
    std::array<float, 512> parameters;
    std::array<float, 960> samples;
};
} // namespace lcomp::vst

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace vst2 {
struct Effect;
using Callback = std::intptr_t (*)(Effect*, std::int32_t, std::int32_t, std::intptr_t, void*,
                                   float);
using Process = void (*)(Effect*, float**, float**, std::int32_t);

struct Effect {
    std::int32_t magic;
    Callback dispatcher;
    Process process;
    void (*set_parameter)(Effect*, std::int32_t, float);
    float (*get_parameter)(Effect*, std::int32_t);
    std::int32_t programs, parameters, inputs, outputs, flags;
    std::intptr_t reserved1, reserved2;
    std::int32_t latency, quality1, quality2;
    float ratio;
    void* object;
    void* user;
    std::int32_t unique_id, version;
    Process replacing;
    void (*double_replacing)(Effect*, double**, double**, std::int32_t);
    char future[56];
};
} // namespace vst2

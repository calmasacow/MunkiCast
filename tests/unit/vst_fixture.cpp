// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../apps/munkicast-vst/abi.hpp"

#include <cstring>
#include <unistd.h>

namespace {
float gain = 0.5f;

std::intptr_t dispatch(vst2::Effect*, std::int32_t opcode, std::int32_t, std::intptr_t, void* out,
                       float) {
    if (opcode == 8 && out)
        std::strcpy(static_cast<char*>(out), "Gain");
    if (opcode == 6 && out)
        std::strcpy(static_cast<char*>(out), "linear");
    return 0;
}

void set(vst2::Effect*, std::int32_t, float value) {
    gain = value;
}

float get(vst2::Effect*, std::int32_t) {
    return gain;
}

void process(vst2::Effect*, float** input, float** output, std::int32_t frames) {
    if (gain == 1)
        _exit(42);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < frames; ++i)
            output[ch][i] = input[ch][i] * gain;
}
} // namespace

extern "C" __attribute__((visibility("default"))) vst2::Effect* VSTPluginMain(vst2::Callback host) {
    static vst2::Effect effect{};
    if (host(nullptr, 1, 0, 0, nullptr, 0) != 2400)
        return nullptr;
    effect.magic = 0x56737450;
    effect.dispatcher = dispatch;
    effect.set_parameter = set;
    effect.get_parameter = get;
    effect.parameters = 1;
    effect.programs = 1;
    effect.inputs = effect.outputs = 2;
    effect.flags = 16;
    effect.replacing = process;
    return &effect;
}

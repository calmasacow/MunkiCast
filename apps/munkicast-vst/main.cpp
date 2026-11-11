// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../libs/lcomp/src/vst_protocol.hpp"
#include "abi.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <signal.h>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
bool transfer(void* data, std::size_t size, bool sending) {
    auto* p = static_cast<char*>(data);
    while (size) {
        const auto n = sending ? ::send(3, p, size, MSG_NOSIGNAL) : ::recv(3, p, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

std::intptr_t host(vst2::Effect*, std::int32_t opcode, std::int32_t, std::intptr_t, void* pointer,
                   float) {
    switch (opcode) {
    case 1:
        return 2400;
    case 16:
        return 48000;
    case 17:
        return 480;
    case 32:
        if (pointer)
            std::strcpy(static_cast<char*>(pointer), "MunkiCast");
        return 1;
    case 33:
        if (pointer)
            std::strcpy(static_cast<char*>(pointer), "MunkiCast");
        return 1;
    case 34:
        return 1;
    case 37:
        return 0;
    default:
        return 0;
    }
}

std::vector<unsigned char> decode(const std::string& text) {
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    unsigned value = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=')
            break;
        const auto index = alphabet.find(c);
        if (index == std::string::npos)
            return {};
        value = (value << 6) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((value >> bits) & 255));
        }
    }
    return out;
}

std::string encode(const unsigned char* data, std::size_t size) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned v = (unsigned(data[i]) << 16) |
                           (i + 1 < size ? unsigned(data[i + 1]) << 8 : 0) |
                           (i + 2 < size ? unsigned(data[i + 2]) : 0);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < size ? alphabet[v & 63] : '=';
    }
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    const int program = std::atoi(argv[2]);
    void* library = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return 3;
    using Entry = vst2::Effect* (*)(vst2::Callback);
    auto entry = reinterpret_cast<Entry>(::dlsym(library, "VSTPluginMain"));
    if (!entry)
        entry = reinterpret_cast<Entry>(::dlsym(library, "main"));
    if (!entry)
        return 4;
    auto* e = entry(host);
    if (!e || e->magic != 0x56737450 || !e->dispatcher || !e->replacing || !(e->flags & 16) ||
        e->inputs < 1 || e->inputs > 2 || e->outputs < 1 || e->outputs > 2 || e->parameters < 0 ||
        e->parameters > 512 || e->programs < 0 || e->programs > 100000 ||
        (e->parameters && (!e->set_parameter || !e->get_parameter)))
        return 5;
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, 48000);
    e->dispatcher(e, 11, 0, 480, nullptr, 0);
    if (program < 0 || (e->programs && program >= e->programs))
        return 6;
    if (e->programs)
        e->dispatcher(e, 2, 0, program, nullptr, 0);
    std::uint32_t size = 0;
    if (!transfer(&size, sizeof(size), false) || size > 1024 * 1024)
        return 7;
    std::string encoded(size, '\0');
    if (size && !transfer(encoded.data(), size, false))
        return 7;
    if (size) {
        const auto chunk = decode(encoded);
        if (chunk.empty())
            return 7;
        if (e->flags & 32)
            e->dispatcher(e, 24, 1, static_cast<std::intptr_t>(chunk.size()),
                          const_cast<unsigned char*>(chunk.data()), 0);
        else {
            if (chunk.size() != static_cast<std::size_t>(e->parameters) * sizeof(float))
                return 7;
            for (int i = 0; i < e->parameters; ++i) {
                float value = 0;
                std::memcpy(&value, chunk.data() + static_cast<std::size_t>(i) * sizeof(float),
                            sizeof(float));
                if (!std::isfinite(value) || value < 0 || value > 1)
                    return 7;
                e->set_parameter(e, i, value);
            }
        }
    }
    lcomp::vst::Header header;
    header.parameters = e->parameters;
    header.programs = e->programs;
    header.latency = e->latency;
    if (!transfer(&header, sizeof(header), true))
        return 8;
    for (int i = 0; i < e->parameters; ++i) {
        lcomp::vst::Parameter parameter;
        e->dispatcher(e, 8, i, 0, parameter.name.data(), 0);
        e->dispatcher(e, 6, i, 0, parameter.unit.data(), 0);
        parameter.name.back() = parameter.unit.back() = '\0';
        parameter.value = e->get_parameter(e, i);
        if (!transfer(&parameter, sizeof(parameter), true))
            return 8;
    }
    e->dispatcher(e, 12, 0, 1, nullptr, 0);
    e->dispatcher(e, 71, 0, 0, nullptr, 0);
    lcomp::vst::Block packet{};
    std::array<std::array<float, 480>, 2> input{}, output{};
    float* in[] = {input[0].data(), input[1].data()};
    float* out[] = {output[0].data(), output[1].data()};
    while (transfer(&packet, sizeof(packet), false)) {
        for (int i = 0; i < e->parameters; ++i)
            if (std::isfinite(packet.parameters[static_cast<std::size_t>(i)])) {
                const float v = packet.parameters[static_cast<std::size_t>(i)];
                if (v < 0 || v > 1)
                    return 9;
                e->set_parameter(e, i, v);
            }
        if (packet.operation == 1) {
            std::string state;
            if (e->flags & 32) {
                void* data = nullptr;
                const auto length = e->dispatcher(e, 23, 1, 0, &data, 0);
                if (length < 0 || length > 768 * 1024 || (length && !data))
                    return 10;
                state = encode(static_cast<const unsigned char*>(data),
                               static_cast<std::size_t>(length));
            } else {
                std::vector<float> parameters(static_cast<std::size_t>(e->parameters));
                for (int i = 0; i < e->parameters; ++i)
                    parameters[static_cast<std::size_t>(i)] = e->get_parameter(e, i);
                state = encode(reinterpret_cast<const unsigned char*>(parameters.data()),
                               parameters.size() * sizeof(float));
            }
            auto length = static_cast<std::uint32_t>(state.size());
            if (!transfer(&length, sizeof(length), true) ||
                (length && !transfer(state.data(), length, true)))
                break;
            continue;
        }
        if (packet.operation != 0)
            return 11;
        for (std::size_t i = 0; i < 480; ++i) {
            input[0][i] = e->inputs == 1
                              ? (packet.samples[i * 2] + packet.samples[i * 2 + 1]) * 0.5f
                              : packet.samples[i * 2];
            input[1][i] = packet.samples[i * 2 + 1];
        }
        for (auto& channel : output)
            channel.fill(0);
        e->replacing(e, in, out, 480);
        for (std::size_t i = 0; i < 480; ++i) {
            packet.samples[i * 2] = output[0][i];
            packet.samples[i * 2 + 1] = output[e->outputs == 1 ? 0 : 1][i];
        }
        if (!transfer(packet.samples.data(), sizeof(packet.samples), true))
            break;
    }
    e->dispatcher(e, 72, 0, 0, nullptr, 0);
    e->dispatcher(e, 12, 0, 0, nullptr, 0);
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    ::dlclose(library);
    return 0;
}

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "vst_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <lcomp/filters.hpp>
#include <lcomp/vst.hpp>
#include <limits>
#include <map>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;

namespace lcomp {
struct VstWorker::Impl {
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    VstInfo metadata;
    std::deque<vst::Block> pending;
    std::deque<std::array<float, 960>> outputs;
    std::jthread thread;

    explicit Impl(ldoc::Effect effect)
        : thread([this, effect = std::move(effect)](std::stop_token stop) { run(effect, stop); }) {}

    ~Impl() {
        thread.request_stop();
        changed.notify_all();
        if (thread.joinable())
            thread.join();
    }

    void run(const ldoc::Effect& effect, std::stop_token stop) {
        int sockets[2] = {-1, -1};
        pid_t child = -1;
        int fd = -1;
        const auto close_all = [&] {
            if (fd >= 0)
                ::close(fd);
            if (child > 0) {
                ::kill(child, SIGKILL);
                while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
                }
            }
        };
        const auto fail = [&](const char* message) {
            std::lock_guard lock(mutex);
            metadata.error = message;
            outputs.clear();
            pending.clear();
        };
        const auto transfer = [&](void* buffer, std::size_t bytes, bool send, int timeout) {
            auto* cursor = static_cast<char*>(buffer);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            while (bytes && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                pollfd p{fd, static_cast<short>(send ? POLLOUT : POLLIN), 0};
                const auto ready = ::poll(&p, 1, 20);
                if (ready < 0 && errno == EINTR)
                    continue;
                if (ready < 0)
                    return false;
                if (!ready)
                    continue;
                const auto n = send ? ::send(fd, cursor, bytes, MSG_NOSIGNAL | MSG_DONTWAIT)
                                    : ::recv(fd, cursor, bytes, MSG_DONTWAIT);
                if (n < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                if (n <= 0)
                    return false;
                bytes -= static_cast<std::size_t>(n);
                cursor += n;
            }
            return bytes == 0;
        };
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
            fail("Cannot create VST worker channel");
            return;
        }
        const auto path = filter_text(effect, "path"),
                   program = std::to_string(static_cast<int>(filter_number(effect, "program")));
        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_addclose(&actions, sockets[0]);
        ::posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
        if (sockets[1] != 3)
            ::posix_spawn_file_actions_addclose(&actions, sockets[1]);
        std::array<char*, 4> argv{const_cast<char*>(MUNKICAST_VST_EXE),
                                  const_cast<char*>(path.c_str()),
                                  const_cast<char*>(program.c_str()), nullptr};
        const int status =
            ::posix_spawn(&child, MUNKICAST_VST_EXE, &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(sockets[1]);
        fd = sockets[0];
        if (status) {
            child = -1;
            fail("Cannot start VST worker");
            close_all();
            return;
        }
        auto chunk = filter_text(effect, "chunk_data");
        auto length = static_cast<std::uint32_t>(chunk.size());
        if (chunk.size() > 1024 * 1024 || !transfer(&length, sizeof(length), true, 3000) ||
            (length && !transfer(chunk.data(), length, true, 3000))) {
            fail("Cannot load VST state");
            close_all();
            return;
        }
        vst::Header header;
        if (!transfer(&header, sizeof(header), false, 3000) || header.magic_value != vst::magic ||
            header.parameters < 0 || header.parameters > 512 || header.programs < 0 ||
            header.latency < 0 || header.latency > 480000) {
            fail("Plugin unavailable, incompatible, or failed during initialization");
            close_all();
            return;
        }
        VstInfo ready;
        ready.error.clear();
        ready.programs = header.programs;
        ready.latency = header.latency;
        for (int i = 0; i < header.parameters; ++i) {
            vst::Parameter parameter;
            if (!transfer(&parameter, sizeof(parameter), false, 3000) ||
                !std::isfinite(parameter.value) || parameter.value < 0 || parameter.value > 1) {
                fail("Invalid VST parameter metadata");
                close_all();
                return;
            }
            parameter.name.back() = parameter.unit.back() = '\0';
            ready.parameters.push_back(
                {parameter.name.data(), parameter.unit.data(), parameter.value});
        }
        {
            std::lock_guard lock(mutex);
            metadata = std::move(ready);
        }
        while (!stop.stop_requested()) {
            vst::Block packet;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, stop, [&] { return !pending.empty(); });
                if (stop.stop_requested())
                    break;
                packet = std::move(pending.front());
                pending.pop_front();
            }
            if (!transfer(&packet, sizeof(packet), true, 100)) {
                fail("VST processing stopped or exceeded 100 ms; toggle filter to restart");
                break;
            }
            if (packet.operation == 1) {
                std::uint32_t size = 0;
                if (!transfer(&size, sizeof(size), false, 100) || size > 1024 * 1024) {
                    fail("Invalid VST state snapshot");
                    break;
                }
                std::string state(size, '\0');
                if (size && !transfer(state.data(), size, false, 100)) {
                    fail("VST state snapshot timed out");
                    break;
                }
                std::lock_guard lock(mutex);
                metadata.snapshot = std::move(state);
                metadata.snapshot_ready = true;
                continue;
            }
            if (!transfer(packet.samples.data(), sizeof(packet.samples), false, 100)) {
                fail("VST processing stopped or exceeded 100 ms; toggle filter to restart");
                break;
            }
            for (auto& sample : packet.samples)
                if (!std::isfinite(sample))
                    sample = 0;
            {
                std::lock_guard lock(mutex);
                if (outputs.size() >= 3)
                    outputs.pop_front();
                outputs.push_back(packet.samples);
            }
        }
        close_all();
    }
};

VstWorker::VstWorker(const ldoc::Effect& effect) : impl_(std::make_unique<Impl>(effect)) {}

VstWorker::~VstWorker() = default;

VstInfo VstWorker::info() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->metadata;
}

bool VstWorker::request_state(const ldoc::ParamMap& parameters) {
    vst::Block packet{};
    packet.operation = 1;
    packet.parameters.fill(std::numeric_limits<float>::quiet_NaN());
    for (const auto& [key, v] : parameters)
        if (key.starts_with("parameter.")) {
            unsigned index = 0;
            const auto parsed = std::from_chars(key.data() + 10, key.data() + key.size(), index);
            const auto* n = std::get_if<double>(&v);
            if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index >= 512 ||
                !n || !std::isfinite(*n) || *n < 0 || *n > 1)
                return false;
            packet.parameters[index] = static_cast<float>(*n);
        }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->metadata.error.empty() || impl_->pending.size() >= 3)
        return false;
    for (std::size_t i = impl_->metadata.parameters.size(); i < packet.parameters.size(); ++i)
        if (!std::isnan(packet.parameters[i]))
            return false;
    impl_->metadata.snapshot_ready = false;
    impl_->pending.push_back(std::move(packet));
    impl_->changed.notify_one();
    return true;
}

std::expected<std::string, std::string> vst_snapshot(const ldoc::Effect& effect) {
    if (auto valid = validate_filter(effect); !valid)
        return std::unexpected(valid.error());
    VstWorker worker(effect);
    for (int i = 0; i < 320 && worker.info().error == "Loading plugin"; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!worker.info().error.empty())
        return std::unexpected(worker.info().error);
    if (!worker.request_state(effect.params))
        return std::unexpected("Cannot request VST state snapshot");
    for (int i = 0; i < 30 && !worker.info().snapshot_ready && worker.info().error.empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto info = worker.info();
    if (!info.snapshot_ready)
        return std::unexpected(info.error.empty() ? "VST state snapshot timed out" : info.error);
    return info.snapshot;
}

bool VstWorker::process(std::span<float> samples, const ldoc::ParamMap& parameters) {
    if (samples.size() != 960)
        return false;
    vst::Block packet;
    std::ranges::copy(samples, packet.samples.begin());
    packet.parameters.fill(std::numeric_limits<float>::quiet_NaN());
    for (const auto& [key, value] : parameters)
        if (key.starts_with("parameter.")) {
            unsigned index = 0;
            const auto parsed = std::from_chars(key.data() + 10, key.data() + key.size(), index);
            const auto* number = std::get_if<double>(&value);
            if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index >= 512 ||
                !number || !std::isfinite(*number) || *number < 0 || *number > 1)
                return false;
            packet.parameters[index] = static_cast<float>(*number);
        }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->metadata.error.empty())
        return false;
    for (std::size_t i = impl_->metadata.parameters.size(); i < packet.parameters.size(); ++i)
        if (!std::isnan(packet.parameters[i]))
            return false;
    if (impl_->pending.size() >= 3)
        return false;
    impl_->pending.push_back(std::move(packet));
    impl_->changed.notify_one();
    if (impl_->outputs.empty())
        return false;
    std::ranges::copy(impl_->outputs.front(), samples.begin());
    impl_->outputs.pop_front();
    return true;
}

VstInfo vst_probe(const ldoc::Effect& effect) {
    struct Entry {
        std::string path, chunk;
        int program;
        std::unique_ptr<VstWorker> worker;
    };

    static std::map<ldoc::Id, Entry> cache;
    const auto path = filter_text(effect, "path"), chunk = filter_text(effect, "chunk_data");
    const int program = static_cast<int>(filter_number(effect, "program"));
    if (path.empty())
        return {"Choose a Linux VST2 .so plugin", 0, 0, {}};
    if (!cache.contains(effect.id) && cache.size() >= 4)
        cache.erase(cache.begin());
    auto& entry = cache[effect.id];
    if (!entry.worker || entry.path != path || entry.chunk != chunk || entry.program != program)
        entry = {path, chunk, program, std::make_unique<VstWorker>(effect)};
    return entry.worker->info();
}
} // namespace lcomp

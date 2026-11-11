// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <lcapture/audio.hpp>
#include <pulse/pulseaudio.h>
#include <thread>

namespace munkicast::capture {
namespace {
std::string physical_device(const std::filesystem::path& device) {
    std::error_code error;
    auto path = std::filesystem::canonical(device, error);
    if (error)
        return {};
    for (; path.has_relative_path(); path = path.parent_path()) {
        if ((std::filesystem::exists(path / "idVendor", error) &&
             std::filesystem::exists(path / "idProduct", error)) ||
            (std::filesystem::exists(path / "vendor", error) &&
             std::filesystem::exists(path / "device", error)))
            return path.string();
    }
    return {};
}

struct Connection {
    pa_threaded_mainloop* loop = nullptr;
    pa_context* context = nullptr;
    bool started = false;

    ~Connection() {
        if (started)
            pa_threaded_mainloop_stop(loop);
        if (context) {
            pa_context_disconnect(context);
            pa_context_unref(context);
        }
        if (loop)
            pa_threaded_mainloop_free(loop);
    }

    template <class Ready>
    bool wait(Ready ready) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end) {
            pa_threaded_mainloop_lock(loop);
            const bool done = ready();
            const bool good = PA_CONTEXT_IS_GOOD(pa_context_get_state(context));
            pa_threaded_mainloop_unlock(loop);
            if (done)
                return true;
            if (!good)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    bool connect(std::string& error) {
        loop = pa_threaded_mainloop_new();
        if (!loop) {
            error = "cannot allocate audio main loop";
            return false;
        }
        context = pa_context_new(pa_threaded_mainloop_get_api(loop), "MunkiCast");
        if (!context || pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 ||
            !(started = pa_threaded_mainloop_start(loop) == 0) ||
            !wait([&] { return pa_context_get_state(context) == PA_CONTEXT_READY; })) {
            error = "cannot connect to the PulseAudio/PipeWire server";
            return false;
        }
        return true;
    }
};
} // namespace

std::vector<AudioDevice> list_audio_devices(std::string& error) {
    Connection connection;
    if (!connection.connect(error))
        return {};
    std::vector<AudioDevice> devices;
    pa_threaded_mainloop_lock(connection.loop);
    auto* operation = pa_context_get_source_info_list(
        connection.context,
        [](pa_context*, const pa_source_info* info, int, void* data) {
            if (info) {
                const char* card = pa_proplist_gets(info->proplist, "api.alsa.card");
                if (!card)
                    card = pa_proplist_gets(info->proplist, "alsa.card");
                const std::string index = card ? card : "";
                const auto hardware =
                    !index.empty() && index.find_first_not_of("0123456789") == std::string::npos
                        ? physical_device("/sys/class/sound/card" + index + "/device")
                        : "";
                static_cast<std::vector<AudioDevice>*>(data)->push_back(
                    {info->name, info->description, info->monitor_of_sink != PA_INVALID_INDEX,
                     hardware});
            }
        },
        &devices);
    pa_threaded_mainloop_unlock(connection.loop);
    if (!operation) {
        error = "cannot enumerate audio devices";
        return {};
    }
    const bool done =
        connection.wait([&] { return pa_operation_get_state(operation) != PA_OPERATION_RUNNING; });
    pa_threaded_mainloop_lock(connection.loop);
    if (!done)
        pa_operation_cancel(operation);
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(connection.loop);
    if (!done) {
        error = "audio enumeration timed out";
        return {};
    }
    return devices;
}

std::string video_audio_device(const std::string& video, std::string& error) {
    std::error_code ec;
    const auto path = std::filesystem::canonical(video, ec);
    const auto hardware = ec ? ""
                             : physical_device(std::filesystem::path("/sys/class/video4linux") /
                                               path.filename() / "device");
    if (hardware.empty()) {
        error = "video device has no physical identity for audio matching";
        return {};
    }
    std::string match;
    for (const auto& input : list_audio_devices(error)) {
        if (input.monitor || input.hardware_path != hardware)
            continue;
        if (!match.empty()) {
            error = "multiple audio inputs match video device; select one explicitly";
            return {};
        }
        match = input.id;
    }
    if (match.empty() && error.empty())
        error = "no audio input matches video device";
    return match;
}

std::vector<AudioDevice> list_audio_sinks(std::string& error) {
    Connection connection;
    if (!connection.connect(error))
        return {};
    std::vector<AudioDevice> devices;
    pa_threaded_mainloop_lock(connection.loop);
    auto* operation = pa_context_get_sink_info_list(
        connection.context,
        [](pa_context*, const pa_sink_info* info, int, void* data) {
            if (info)
                static_cast<std::vector<AudioDevice>*>(data)->push_back(
                    {info->name, info->description, false, {}});
        },
        &devices);
    pa_threaded_mainloop_unlock(connection.loop);
    if (!operation) {
        error = "cannot enumerate audio outputs";
        return {};
    }
    const bool done =
        connection.wait([&] { return pa_operation_get_state(operation) != PA_OPERATION_RUNNING; });
    pa_threaded_mainloop_lock(connection.loop);
    if (!done)
        pa_operation_cancel(operation);
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(connection.loop);
    if (!done) {
        error = "audio output enumeration timed out";
        return {};
    }
    return devices;
}

struct AudioCapture::State {
    Connection connection;
    pa_stream* stream = nullptr;
    Callback callback;
    bool verified = false;

    ~State() {
        if (stream) {
            pa_threaded_mainloop_lock(connection.loop);
            pa_stream_set_read_callback(stream, nullptr, nullptr);
            pa_stream_set_state_callback(stream, nullptr, nullptr);
            pa_stream_disconnect(stream);
            pa_stream_unref(stream);
            pa_threaded_mainloop_unlock(connection.loop);
        }
    }
};

AudioCapture::AudioCapture(std::unique_ptr<State> state) : state_(std::move(state)) {}

AudioCapture::~AudioCapture() = default;

struct AudioSink::State {
    Connection connection;
    std::uint32_t module = PA_INVALID_INDEX;

    ~State() {
        if (module == PA_INVALID_INDEX)
            return;
        pa_threaded_mainloop_lock(connection.loop);
        auto* operation = pa_context_unload_module(connection.context, module, nullptr, nullptr);
        pa_threaded_mainloop_unlock(connection.loop);
        if (!operation)
            return;
        connection.wait([&] { return pa_operation_get_state(operation) != PA_OPERATION_RUNNING; });
        pa_threaded_mainloop_lock(connection.loop);
        pa_operation_cancel(operation);
        pa_operation_unref(operation);
        pa_threaded_mainloop_unlock(connection.loop);
    }
};

AudioSink::AudioSink(std::unique_ptr<State> state) : state_(std::move(state)) {}

AudioSink::~AudioSink() = default;

std::unique_ptr<AudioSink> AudioSink::open(const std::string& name, std::string& error) {
    if (name.empty() || name.size() > 128 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos) {
        error = "invalid private audio sink name";
        return {};
    }
    auto state = std::make_unique<State>();
    if (!state->connection.connect(error))
        return {};
    auto& connection = state->connection;
    const std::string args = "sink_name=" + name + " format=float32le rate=48000 channels=2";
    pa_threaded_mainloop_lock(connection.loop);
    auto* operation = pa_context_load_module(
        connection.context, "module-null-sink", args.c_str(),
        [](pa_context*, std::uint32_t index, void* data) {
            static_cast<State*>(data)->module = index;
        },
        state.get());
    pa_threaded_mainloop_unlock(connection.loop);
    if (!operation) {
        error = "cannot create private browser audio sink";
        return {};
    }
    const bool done =
        connection.wait([&] { return pa_operation_get_state(operation) != PA_OPERATION_RUNNING; });
    pa_threaded_mainloop_lock(connection.loop);
    if (!done)
        pa_operation_cancel(operation);
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(connection.loop);
    if (!done || state->module == PA_INVALID_INDEX) {
        error = "private browser audio sink unavailable";
        return {};
    }
    return std::unique_ptr<AudioSink>(new AudioSink(std::move(state)));
}

struct AudioMonitor::State {
    Connection connection;
    pa_stream* stream = nullptr;
    std::deque<float> pending;

    ~State() {
        if (stream) {
            pa_threaded_mainloop_lock(connection.loop);
            pa_stream_set_write_callback(stream, nullptr, nullptr);
            pa_stream_disconnect(stream);
            pa_stream_unref(stream);
            pa_threaded_mainloop_unlock(connection.loop);
        }
    }
};

AudioMonitor::AudioMonitor(std::unique_ptr<State> state) : state_(std::move(state)) {}

AudioMonitor::~AudioMonitor() = default;

std::unique_ptr<AudioMonitor> AudioMonitor::open(std::string& error, const std::string& device) {
    if (device.size() > 1024 || device.find('\0') != std::string::npos) {
        error = "invalid monitoring output identity";
        return {};
    }
    if (!device.empty()) {
        const auto sinks = list_audio_sinks(error);
        if (std::ranges::none_of(sinks, [&](const auto& sink) { return sink.id == device; })) {
            error = "selected monitoring output is unavailable";
            return {};
        }
    }
    auto state = std::make_unique<State>();
    if (!state->connection.connect(error))
        return {};
    auto& connection = state->connection;
    pa_sample_spec format{PA_SAMPLE_FLOAT32NE, 48000, 2};
    pa_threaded_mainloop_lock(connection.loop);
    state->stream = pa_stream_new(connection.context, "MunkiCast Monitor", &format, nullptr);
    if (!state->stream) {
        pa_threaded_mainloop_unlock(connection.loop);
        error = "cannot create monitor stream";
        return {};
    }
    pa_stream_set_write_callback(
        state->stream,
        [](pa_stream* stream, std::size_t requested, void* data) {
            auto& self = *static_cast<State*>(data);
            std::array<float, 960> block{};
            while (requested >= sizeof(float) * 2) {
                const auto count = std::min(requested / (sizeof(float) * 2) * 2, block.size());
                std::fill(block.begin(), block.end(), 0);
                for (std::size_t i = 0; i < count && !self.pending.empty(); ++i) {
                    block[i] = self.pending.front();
                    self.pending.pop_front();
                }
                if (pa_stream_write(stream, block.data(), count * sizeof(float), nullptr, 0,
                                    PA_SEEK_RELATIVE) < 0)
                    break;
                requested -= count * sizeof(float);
            }
        },
        state.get());
    pa_buffer_attr attr{static_cast<std::uint32_t>(-1), 1920 * sizeof(float),
                        static_cast<std::uint32_t>(-1), 480 * sizeof(float),
                        static_cast<std::uint32_t>(-1)};
    const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY |
                                                      (device.empty() ? 0 : PA_STREAM_DONT_MOVE));
    const int connected = pa_stream_connect_playback(
        state->stream, device.empty() ? nullptr : device.c_str(), &attr, flags, nullptr, nullptr);
    pa_threaded_mainloop_unlock(connection.loop);
    if (connected < 0 ||
        !connection.wait([&] { return pa_stream_get_state(state->stream) == PA_STREAM_READY; })) {
        error = "cannot open monitor output";
        return {};
    }
    pa_threaded_mainloop_lock(connection.loop);
    const char* actual = pa_stream_get_device_name(state->stream);
    const bool matches = device.empty() || (actual && device == actual);
    pa_threaded_mainloop_unlock(connection.loop);
    if (!matches) {
        error = "audio server substituted a different monitoring output";
        return {};
    }
    return std::unique_ptr<AudioMonitor>(new AudioMonitor(std::move(state)));
}

void AudioMonitor::write(std::span<const float> samples) {
    pa_threaded_mainloop_lock(state_->connection.loop);
    for (float sample : samples)
        state_->pending.push_back(std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0);
    while (state_->pending.size() > 9600)
        state_->pending.pop_front();
    pa_threaded_mainloop_unlock(state_->connection.loop);
}

std::unique_ptr<AudioCapture> AudioCapture::open(const std::string& device, bool output,
                                                 Callback callback, std::string& error,
                                                 bool exact_device) {
    if (device.size() > 1024 || device.find('\0') != std::string::npos) {
        error = "invalid audio device identity";
        return {};
    }
    if (exact_device) {
        const auto inputs = list_audio_devices(error);
        if (device.empty() || device == "default" ||
            std::ranges::none_of(inputs, [&](const auto& input) { return input.id == device; })) {
            error = "selected capture audio input is unavailable";
            return {};
        }
    }
    auto state = std::make_unique<State>();
    if (!state->connection.connect(error))
        return {};
    state->callback = std::move(callback);
    auto& connection = state->connection;
    pa_sample_spec format{PA_SAMPLE_FLOAT32NE, 48000, 2};
    pa_threaded_mainloop_lock(connection.loop);
    state->stream = pa_stream_new(connection.context, output ? "Desktop capture" : "Input capture",
                                  &format, nullptr);
    if (!state->stream) {
        pa_threaded_mainloop_unlock(connection.loop);
        error = "cannot create audio stream";
        return {};
    }
    pa_stream_set_read_callback(
        state->stream,
        [](pa_stream* stream, std::size_t, void* userdata) {
            auto& self = *static_cast<State*>(userdata);
            const void* data = nullptr;
            std::size_t size = 0;
            if (pa_stream_peek(stream, &data, &size) < 0) {
                self.callback({});
                return;
            }
            if (self.verified && data && size % (2 * sizeof(float)) == 0 && size <= 1024 * 1024)
                self.callback({static_cast<const float*>(data), size / sizeof(float)});
            if (size)
                pa_stream_drop(stream);
        },
        state.get());
    pa_stream_set_state_callback(
        state->stream,
        [](pa_stream* stream, void* userdata) {
            if (pa_stream_get_state(stream) == PA_STREAM_FAILED ||
                pa_stream_get_state(stream) == PA_STREAM_TERMINATED)
                static_cast<State*>(userdata)->callback({});
        },
        state.get());
    const std::string name = device.empty() || device == "default"
                                 ? (output ? "@DEFAULT_MONITOR@" : "@DEFAULT_SOURCE@")
                                 : device;
    pa_buffer_attr attr{static_cast<std::uint32_t>(-1), static_cast<std::uint32_t>(-1),
                        static_cast<std::uint32_t>(-1), static_cast<std::uint32_t>(-1),
                        480 * 2 * sizeof(float)};
    const int connected = pa_stream_connect_record(
        state->stream, name.c_str(), &attr,
        static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY |
                                       (exact_device ? PA_STREAM_DONT_MOVE : 0)));
    pa_threaded_mainloop_unlock(connection.loop);
    if (connected < 0 ||
        !connection.wait([&] { return pa_stream_get_state(state->stream) == PA_STREAM_READY; })) {
        error = "cannot open selected audio device";
        return {};
    }
    if (exact_device) {
        pa_threaded_mainloop_lock(connection.loop);
        const char* actual = pa_stream_get_device_name(state->stream);
        const bool matches = actual && device == actual;
        state->verified = matches;
        pa_threaded_mainloop_unlock(connection.loop);
        if (!matches) {
            error = "audio server substituted a different capture input";
            return {};
        }
    } else {
        pa_threaded_mainloop_lock(connection.loop);
        state->verified = true;
        pa_threaded_mainloop_unlock(connection.loop);
    }
    return std::unique_ptr<AudioCapture>(new AudioCapture(std::move(state)));
}
} // namespace munkicast::capture

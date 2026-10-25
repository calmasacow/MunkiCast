// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <lcapture/audio.hpp>
#include <lcomp/audio_filters.hpp>
#include <lcomp/filters.hpp>
#include <lcomp/preview.hpp>
#include <lcore/log.hpp>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

class AudioSources {
    struct Session {
        ldoc::Source source;
        std::mutex mu;
        float left = 0, right = 0;
        bool available = false;
        std::deque<float> pcm;
        std::chrono::steady_clock::time_point received;

        void ingest(std::span<const float> samples) {
            std::lock_guard lock(mu);
            available = !samples.empty();
            left = right = 0;
            for (std::size_t i = 0; i + 1 < samples.size(); i += 2) {
                if (std::isfinite(samples[i]))
                    left = std::max(left, std::abs(samples[i]));
                if (std::isfinite(samples[i + 1]))
                    right = std::max(right, std::abs(samples[i + 1]));
            }
            for (float sample : samples)
                pcm.push_back(std::isfinite(sample) ? sample : 0);
            while (pcm.size() > 9600)
                pcm.pop_front();
            received = std::chrono::steady_clock::now();
        }

        std::jthread thread;

        explicit Session(const ldoc::Source& src)
            : source(src), thread([this](std::stop_token stop) {
                  if (source.type != "source.audio-input" && source.type != "source.audio-output" &&
                      source.type != "source.v4l2")
                      return;
                  const bool video = source.type == "source.v4l2";
                  std::string device = "default", error;
                  if (auto it = source.settings.find(video ? "audioDevice" : "device");
                      it != source.settings.end())
                      if (auto value = std::get_if<std::string>(&it->second))
                          device = *value;
                  while (!stop.stop_requested()) {
                      error.clear();
                      std::string selected = device;
                      if (video && device == "default") {
                          const auto it = source.settings.find("device");
                          const auto* path = it == source.settings.end()
                                                 ? nullptr
                                                 : std::get_if<std::string>(&it->second);
                          selected =
                              libertas::capture::video_audio_device(path ? *path : "", error);
                      }
                      std::unique_ptr<libertas::capture::AudioCapture> capture;
                      if (!video || !selected.empty())
                          capture = libertas::capture::AudioCapture::open(
                              selected, source.type == "source.audio-output",
                              [this](std::span<const float> samples) { ingest(samples); }, error,
                              video);
                      if (!capture) {
                          lc::warn("audio {}: {}", source.name, error);
                          if (!video)
                              return;
                          for (int i = 0; i < 150 && !stop.stop_requested(); ++i)
                              std::this_thread::sleep_for(std::chrono::milliseconds(20));
                          continue;
                      }
                      const auto opened = std::chrono::steady_clock::now();
                      while (!stop.stop_requested()) {
                          std::this_thread::sleep_for(std::chrono::milliseconds(20));
                          if (video) {
                              std::lock_guard lock(mu);
                              if (std::chrono::steady_clock::now() - std::max(opened, received) >
                                  std::chrono::seconds(3))
                                  break;
                          }
                      }
                  }
              }) {}

        ~Session() {
            thread.request_stop();
            if (thread.joinable())
                thread.join();
        }
    };

    std::map<ldoc::Id, std::shared_ptr<Session>> sessions_;

    using FilterPath = std::vector<std::pair<ldoc::Id, std::vector<ldoc::Effect>>>;

    struct Route {
        ldoc::Id node;
        std::shared_ptr<Session> session;
        ldoc::AudioRouting audio;
        std::vector<ldoc::Effect> source_effects;
        std::vector<ldoc::Effect> node_effects;
        FilterPath parents;
        ldoc::Id source;
    };

    std::mutex mu_;
    std::vector<Route> routes_;
    std::vector<ldoc::Bus> buses_;
    std::string monitor_output_ = "none";
    std::map<ldoc::Id, lcomp::AudioLevel> bus_levels_;
    /// Which bus the encoder is following, and what it has not drained yet.
    ldoc::Id tapped_ = ldoc::Id::nil();
    std::vector<float> tap_;
    std::jthread mixer_{[this](std::stop_token stop) { mix(stop); }};

    static float gain(double db) {
        return std::isfinite(db)
                   ? static_cast<float>(std::pow(10.0, std::clamp(db, -120.0, 24.0) / 20))
                   : 0;
    }

    void mix(std::stop_token stop) {
        using Block = std::array<float, 960>;
        std::unique_ptr<libertas::capture::AudioMonitor> monitor;
        std::string opened_output;
        auto retry_monitor = std::chrono::steady_clock::time_point{};

        struct Delay {
            std::size_t length = 0;
            std::deque<float> samples;
        };

        std::map<ldoc::Id, Delay> delays;
        std::map<std::string, std::unique_ptr<lcomp::AudioFilters>> processors;
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            next += std::chrono::milliseconds(10);
            std::vector<Route> routes;
            std::vector<ldoc::Bus> buses;
            std::string monitor_output;
            {
                std::lock_guard lock(mu_);
                routes = routes_;
                buses = buses_;
                monitor_output = monitor_output_;
            }
            std::map<ldoc::Id, Block> inputs, processed, mixed;
            std::map<ldoc::Id, bool> available, processed_ready;
            std::map<ldoc::Id, lcomp::AudioLevel> levels;
            std::set<std::string> used_processors;
            const auto process = [&](Block& block, const std::vector<ldoc::Effect>& effects,
                                     const std::string& key) {
                used_processors.insert(key);
                auto& processor = processors[key];
                if (!processor)
                    processor = std::make_unique<lcomp::AudioFilters>();
                return processor
                    ->process(block, effects,
                              [&](const ldoc::Id& source) -> std::span<const float> {
                                  const auto it = inputs.find(source);
                                  return it == inputs.end() ? std::span<const float>{}
                                                            : std::span<const float>{it->second};
                              })
                    .has_value();
            };
            Block listening{};
            bool monitoring = false;
            auto add = [](Block& dst, const Block& src, float volume) {
                for (std::size_t i = 0; i < dst.size(); ++i)
                    dst[i] += src[i] * volume;
            };
            for (const auto& route : routes) {
                auto& session = *route.session;
                if (inputs.contains(session.source.id))
                    continue;
                auto& input = inputs[session.source.id];
                std::lock_guard lock(session.mu);
                available[session.source.id] =
                    session.available &&
                    std::chrono::steady_clock::now() - session.received < std::chrono::seconds(1);
                for (auto& sample : input) {
                    if (session.pcm.empty())
                        break;
                    sample = session.pcm.front();
                    session.pcm.pop_front();
                }
            }
            using Path = std::vector<ldoc::Id>;
            std::map<Path, std::map<ldoc::Id, Block>> groups;
            std::map<ldoc::Id, std::vector<ldoc::Effect>> group_filters;
            for (const auto& route : routes) {
                const auto source = route.source;
                const auto input_id = route.session->source.id;
                if (!processed.contains(source)) {
                    processed[source] = inputs[input_id];
                    processed_ready[source] = process(processed[source], route.source_effects,
                                                       "source:" + source.to_string()) &&
                                              available[input_id];
                }
                Block input = processed[source];
                const bool ready =
                    process(input, route.node_effects, "node:" + route.node.to_string()) &&
                    processed_ready[source];
                const float volume = route.audio.mute ? 0 : gain(route.audio.gain_db);
                lcomp::AudioLevel level{route.node, 0, 0, ready};
                for (std::size_t i = 0; i < input.size(); i += 2) {
                    level.left = std::max(level.left, std::abs(input[i] * volume));
                    level.right = std::max(level.right, std::abs(input[i + 1] * volume));
                }
                levels[route.node] = level;
                Path path;
                for (const auto& [id, effects] : route.parents) {
                    path.push_back(id);
                    group_filters[id] = effects;
                }
                auto& destinations = groups[path];
                if (route.audio.bus)
                    add(destinations[*route.audio.bus], input, volume);
                for (const auto& send : route.audio.sends)
                    add(destinations[send.to], input,
                        (send.pre_fader ? (route.audio.mute ? 0 : 1) : volume) *
                            gain(send.gain_db));
                if (route.audio.monitor) {
                    monitoring = true;
                    add(destinations[ldoc::Id::nil()], input, volume);
                }
            }
            // Fold deepest groups first so nonlinear filters see the sum of their children.
            while (!groups.empty()) {
                auto it = std::max_element(
                    groups.begin(), groups.end(),
                    [](const auto& a, const auto& b) { return a.first.size() < b.first.size(); });
                auto path = it->first;
                auto destinations = std::move(it->second);
                groups.erase(it);
                if (path.empty()) {
                    for (const auto& [destination, block] : destinations)
                        if (destination.is_nil())
                            add(listening, block, 1);
                        else
                            add(mixed[destination], block, 1);
                    continue;
                }
                const auto owner = path.back();
                path.pop_back();
                for (auto& [destination, block] : destinations) {
                    process(block, group_filters[owner],
                            "group:" + owner.to_string() + ":" + destination.to_string());
                    add(groups[path][destination], block, 1);
                }
            }
            std::set<ldoc::Id> finished;
            for (std::size_t pass = 0; pass < buses.size(); ++pass) {
                bool progress = false;
                for (const auto& bus : buses) {
                    if (finished.contains(bus.id))
                        continue;
                    bool waiting = false;
                    for (const auto& upstream : buses)
                        if (!finished.contains(upstream.id))
                            for (const auto& send : upstream.sends)
                                waiting = waiting || send.to == bus.id;
                    if (waiting)
                        continue;
                    auto& output = mixed[bus.id];
                    const bool unsupported =
                        !process(output, bus.inserts, "bus:" + bus.id.to_string());
                    const auto input = output;
                    const float volume = bus.mute || unsupported ? 0 : gain(bus.gain_db);
                    const double pan = std::isfinite(bus.pan) ? std::clamp(bus.pan, -1.0, 1.0) : 0;
                    lcomp::AudioLevel level{bus.id, 0, 0, true};
                    for (std::size_t i = 0; i < output.size(); i += 2) {
                        output[i] *= volume * static_cast<float>(std::min(1.0, 1.0 - pan));
                        output[i + 1] *= volume * static_cast<float>(std::min(1.0, 1.0 + pan));
                        if (bus.channels == 1)
                            output[i] = output[i + 1] = (output[i] + output[i + 1]) * 0.5f;
                        level.left = std::max(level.left, std::abs(output[i]));
                        level.right = std::max(level.right, std::abs(output[i + 1]));
                    }
                    auto& delay = delays[bus.id];
                    const auto length = std::isfinite(bus.delay_ms)
                                            ? static_cast<std::size_t>(
                                                  std::clamp(bus.delay_ms, 0.0, 10000.0) * 48) *
                                                  2
                                            : 0;
                    if (delay.length != length) {
                        delay.length = length;
                        delay.samples.assign(length, 0);
                    }
                    if (length)
                        for (auto& sample : output) {
                            delay.samples.push_back(sample);
                            sample = delay.samples.front();
                            delay.samples.pop_front();
                        }
                    level.available = !unsupported;
                    levels[bus.id] = level;
                    for (const auto& send : bus.sends)
                        add(mixed[send.to], send.pre_fader ? input : output,
                            bus.mute ? 0 : gain(send.gain_db));
                    if (bus.monitor) {
                        monitoring = true;
                        add(listening, output, 1);
                    }
                    finished.insert(bus.id);
                    progress = true;
                }
                if (!progress)
                    break;
            }
            std::erase_if(processors,
                          [&](const auto& item) { return !used_processors.contains(item.first); });
            std::erase_if(delays, [&](const auto& item) {
                return std::ranges::none_of(buses,
                                            [&](const auto& bus) { return bus.id == item.first; });
            });
            {
                std::lock_guard lock(mu_);
                bus_levels_ = std::move(levels);
                // Keep the finished mix of whichever bus is being recorded. Taken here, after
                // inserts, fader, pan and delay, because that is what the operator hears and
                // so is what the file has to carry.
                if (!tapped_.is_nil()) {
                    const auto it = mixed.find(tapped_);
                    if (it != mixed.end()) {
                        tap_.insert(tap_.end(), it->second.begin(), it->second.end());
                        // A second of slack. If the encoder has not drained by then it is not
                        // keeping up, and holding more would only delay saying so.
                        constexpr std::size_t kMaxTap = 48000 * 2;
                        if (tap_.size() > kMaxTap)
                            tap_.erase(tap_.begin(),
                                       tap_.begin() +
                                           static_cast<std::ptrdiff_t>(tap_.size() - kMaxTap));
                    }
                }
            }
            if (monitor_output != opened_output) {
                monitor.reset();
                opened_output = monitor_output;
                retry_monitor = {};
            }
            if (!monitoring || monitor_output == "none") {
                monitor.reset();
            } else if (!monitor && std::chrono::steady_clock::now() >= retry_monitor) {
                std::string error;
                monitor = libertas::capture::AudioMonitor::open(
                    error, monitor_output == "default" ? "" : monitor_output.substr(5));
                if (!monitor) {
                    retry_monitor = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    lc::warn("audio monitor: {}", error);
                } else {
                    retry_monitor = {};
                }
            }
            if (monitor)
                monitor->write(listening);
            if (next < std::chrono::steady_clock::now())
                next = std::chrono::steady_clock::now();
            std::this_thread::sleep_until(next);
        }
    }

  public:
    ~AudioSources() {
        mixer_.request_stop();
        if (mixer_.joinable())
            mixer_.join();
    }

    void ingest(const ldoc::Id& source, std::span<const float> samples) {
        std::lock_guard lock(mu_);
        for (const auto& route : routes_)
            if (route.session->source.id == source) {
                route.session->ingest(samples);
                break;
            }
    }

    /// Follow one bus for the encoder, or none when the id is nil. Switching buses drops
    /// whatever was held: a file that spliced two different mixes together would be worse
    /// than one that is a few milliseconds short.
    void tap(const ldoc::Id& bus) {
        std::lock_guard lock(mu_);
        if (tapped_ == bus)
            return;
        tapped_ = bus;
        tap_.clear();
    }

    /// Everything the tapped bus has produced since the last call, interleaved stereo float32
    /// at 48 kHz.
    std::vector<float> drain_tap() {
        std::lock_guard lock(mu_);
        return std::exchange(tap_, {});
    }

    std::vector<lcomp::AudioLevel> meters(const ldoc::Project& project) {
        std::set<ldoc::Id> wanted, visited;
        std::vector<lcomp::AudioLevel> result;
        std::vector<Route> routes;
        const ldoc::Scene* owner = nullptr;
        FilterPath inherited;
        const auto add_filters = [&](const ldoc::Id& id, const std::vector<ldoc::Effect>& effects) {
            if (std::ranges::any_of(effects, [](const auto& e) {
                    const auto* info = lcomp::filter_info(e.type);
                    return e.enabled && (!info || info->audio || e.type == "filter.video-delay");
                }))
                inherited.push_back({id, effects});
        };
        const auto session_for = [&](const ldoc::Source& source) {
            wanted.insert(source.id);
            auto& session = sessions_[source.id];
            const auto setting = [](const ldoc::Source& src, const char* key) {
                const auto it = src.settings.find(key);
                return it == src.settings.end() ? ldoc::ParamValue{} : it->second;
            };
            if (session &&
                (session->source.type != source.type ||
                 (source.type == "source.v4l2"
                      ? setting(session->source, "audioDevice") != setting(source, "audioDevice") ||
                            setting(session->source, "device") != setting(source, "device")
                      : session->source.settings != source.settings)))
                session.reset();
            if (!session)
                session = std::make_shared<Session>(source);
            return session;
        };
        for (const auto& source : project.sources) {
            if (source.type != "source.v4l2" || !ldoc::has_audio(source))
                continue;
            const auto it = source.settings.find("deactivateWhenHidden");
            const auto* deactivate =
                it == source.settings.end() ? nullptr : std::get_if<bool>(&it->second);
            if (!deactivate || !*deactivate)
                session_for(source);
        }
        std::function<void(const std::vector<ldoc::Node>&)> walk;
        walk = [&](const std::vector<ldoc::Node>& nodes) {
            for (const auto& node : nodes) {
                if (!node.visible || !visited.insert(node.id).second)
                    continue;
                const auto previous_depth = inherited.size();
                if (node.kind() != ldoc::NodeKind::Source)
                    add_filters(node.id, node.effects);
                if (const auto* group = std::get_if<ldoc::GroupData>(&node.data))
                    walk(group->children);
                else if (const auto* pre = std::get_if<ldoc::PrecompData>(&node.data)) {
                    if (const auto* scene = project.scene(pre->scene)) {
                        const auto* previous = owner; owner = scene;
                        add_filters(scene->id, scene->effects);
                        walk(scene->roots);
                        owner = previous;
                    }
                } else if (const auto* data = std::get_if<ldoc::SourceData>(&node.data)) {
                    const auto* source = data->source ? project.source(*data->source) : nullptr;
                    const auto source_id = source ? source->id : ldoc::Id::nil();
                    const bool audio_enabled = source && ldoc::has_audio(*source);
                    const auto resolved = source ? ldoc::resolve_source(project, *source, true) : std::nullopt;
                    source = resolved ? &*resolved : nullptr;
                    if (!audio_enabled || !source || !ldoc::has_audio(*source) ||
                        (source->type != "source.audio-input" &&
                         source->type != "source.audio-output" && source->type != "source.media" &&
                         source->type != "source.playlist" && source->type != "source.blackmagic" &&
                         source->type != "source.browser" && source->type != "source.v4l2")) {
                        inherited.resize(previous_depth);
                        continue;
                    }
                    auto session = session_for(*source);
                    auto routing = data->audio;
                    if (owner) if (auto found = owner->audio_overrides.find(node.id); found != owner->audio_overrides.end()) {
                        const auto& change = found->second;
                        if (change.bus) routing.bus = change.bus;
                        if (change.gain_db) routing.gain_db = *change.gain_db;
                        if (change.mute) routing.mute = *change.mute;
                        if (change.sends) routing.sends = *change.sends;
                    }
                    routes.push_back({node.id, session, std::move(routing), source->effects,
                                      node.effects, inherited, source_id});
                }
                inherited.resize(previous_depth);
            }
        };
        for (const auto& deck : project.decks) {
            const auto preview = project.settings.extra.find("ui.filter-preview-deck");
            if (preview != project.settings.extra.end())
                if (const auto* id = std::get_if<std::string>(&preview->second);
                    id && *id == deck.id.to_string())
                    continue;
            if (deck.program_scene)
                if (const auto* scene = project.scene(*deck.program_scene)) {
                    owner = scene;
                    inherited.clear();
                    add_filters(scene->id, scene->effects);
                    walk(scene->roots);
                }
        }
        std::erase_if(sessions_, [&](const auto& item) { return !wanted.contains(item.first); });
        {
            std::lock_guard lock(mu_);
            routes_ = std::move(routes);
            buses_ = project.buses;
            monitor_output_ = "none";
            if (const auto it = project.settings.extra.find("audio.monitor.output");
                it != project.settings.extra.end()) {
                if (const auto* value = std::get_if<std::string>(&it->second);
                    value && !value->empty() && value->size() <= 1024 &&
                    value->find('\0') == std::string::npos &&
                    (*value == "default" || (value->starts_with("sink:") && value->size() > 5)))
                    monitor_output_ = *value;
            }
            for (const auto& [id, level] : bus_levels_)
                result.push_back(level);
        }
        return result;
    }
};

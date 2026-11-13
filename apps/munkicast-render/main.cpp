// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The render process owns capture and the device. Both backends consume lcomp's shared plan.
// Offscreen checks select CPU; normal runs try GPU and retain the CPU fallback (ADR-0042).

#include "lcomp/preview.hpp"
#include <lcomp/countdown.hpp>
#include "lipc/fd.hpp"
#include "source_worker.hpp"
#include "audio_sources.hpp"
#include "recorder.hpp"
#include "projectors.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <lcapture/screen.hpp>
#include <lcapture/video.hpp>
#include <lcapture/video_buffer.hpp>
#include <lcore/log.hpp>
#include <lrhi/composite.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

int channel_fd() {
    const char* spec = std::getenv("MUNKICAST_CHANNEL_FD");
    if (spec == nullptr || *spec == '\0')
        return -1;
    char* end = nullptr;
    errno = 0;
    const long fd = std::strtol(spec, &end, 10);
    if (errno != 0 || end == spec || *end != '\0' || fd < 0 || fd > 1'000'000L)
        return -1;
    return static_cast<int>(fd);
}

/// One capture snapshot per connector and commit, shared by every placement and canvas.
class Renderer {
  public:
    explicit Renderer(AudioSources& audio) : audio_(audio) {
        const char* mode = std::getenv("MUNKICAST_RENDER_BACKEND");
        require_gpu_ = mode && std::strcmp(mode, "gpu") == 0;
        const char* platform = std::getenv("QT_QPA_PLATFORM");
        cpu_ = (mode && std::strcmp(mode, "cpu") == 0) ||
               (!require_gpu_ && platform && std::strcmp(platform, "offscreen") == 0);
    }

    ~Renderer() {
        for (const auto& [id, session] : workers_) session->worker->stop();
        workers_.clear();
    }

    /// MUNKICAST_BENCHMARK: logs a per-frame step that took long enough to be a visible hitch.
    struct SlowStep {
        const char* name;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~SlowStep() {
            static const bool bench = std::getenv("MUNKICAST_BENCHMARK") != nullptr;
            const auto ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            if (bench && ms > 40) std::fprintf(stderr, "SLOW_STEP %s ms=%.1f\n", name, ms);
        }
    };

    const ldoc::Project& prepare(const ldoc::Project& project) {
        const SlowStep slow{"prepare"};
        effective_.reset();
        transition_dirty_ = false;
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(transitions_, [&](const auto& entry) {
            return std::ranges::none_of(project.decks, [&](const auto& deck) { return deck.id == entry.first; });
        });
        for (const auto& deck : project.decks) {
            auto [it, inserted] = transitions_.try_emplace(deck.id);
            auto& state = it->second;
            if (inserted) state.target = deck.program_scene;
            const auto manual = project.settings.extra.find("ui.manual." + deck.id.to_string());
            if (manual != project.settings.extra.end()) {
                const auto* progress = std::get_if<double>(&manual->second);
                if (progress && std::isfinite(*progress)) {
                    state.target = deck.program_scene;
                    state.from = deck.preview_scene;
                    state.playing = false;
                    state.manual = true;
                    if (!effective_) effective_ = project;
                    mix_transition(*effective_, deck, state, deck.preview_scene, deck.program_scene,
                                   deck.transition == "transition.fade-to-black", *progress);
                    continue;
                }
            }
            const bool immediate = lcomp::timer_flag(project.settings.extra,
                ("ui.immediate." + deck.id.to_string()).c_str());
            if (state.manual && !immediate) {
                state.manual = false;
                state.target = state.from;
            }
            if (immediate) {
                transition_dirty_ = transition_dirty_ || state.manual || state.playing ||
                                    state.target != deck.program_scene;
                state.manual = state.playing = false;
                state.target = deck.program_scene;
                continue;
            }
            if (inserted) continue;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.start).count();
            auto previous = state.playing && elapsed < state.cut_ms ? state.from : state.target;
            if (state.target != deck.program_scene) {
                transition_dirty_ = true;
                state.target = deck.program_scene;
                state.playing = false;
                state.kind = deck.transition;
                if (previous && state.target && project.scene(*previous) &&
                    (deck.transition == "transition.fade" || deck.transition == "transition.fade-to-black")) {
                    state.from = previous;
                    state.start = now;
                    state.duration_ms = std::clamp<std::int64_t>(deck.transition_ms, 0, 600000);
                    state.playing = state.duration_ms > 0;
                }
                if (previous && state.target && project.scene(*previous) &&
                    deck.transition.starts_with("transition.stinger:")) {
                    const auto id = ldoc::Id::parse(deck.transition.substr(19));
                    const auto* source = id ? project.source(*id) : nullptr;
                    if (source && source->type == "source.media" &&
                        lcomp::timer_flag(source->settings, "transitionStinger")) {
                        auto& worker = worker_session(*source);
                        std::lock_guard lock(worker.mu);
                        if (worker.last) {
                            state.source = *id;
                            state.from = previous;
                            state.start = now;
                            state.cut_ms = std::clamp<std::int64_t>(
                                lcomp::timer_number(source->settings, "stingerCutMs", 500), 0, 600000);
                            state.duration_ms = std::clamp<std::int64_t>(
                                lcomp::timer_number(source->settings, "stingerDurationMs", 1000), 1, 600000);
                            state.playing = state.cut_ms <= state.duration_ms;
                            if (state.playing) worker.worker->restart();
                        } else lc::warn("stinger {} is not preloaded; using Cut", source->name);
                    } else lc::warn("stinger source missing; using Cut");
                }
            }
            if (!state.playing) continue;
            const auto position = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.start).count();
            const bool fade = state.kind == "transition.fade" || state.kind == "transition.fade-to-black";
            if (position >= state.duration_ms || (!fade && !project.source(state.source))) {
                state.playing = false;
                transition_dirty_ = true;
                continue;
            }
            if (!effective_) effective_ = project;
            if (fade) {
                mix_transition(*effective_, deck, state, state.from, state.target,
                               state.kind == "transition.fade-to-black",
                               static_cast<double>(position) / static_cast<double>(state.duration_ms));
                continue;
            }
            auto selected = std::ranges::find(effective_->decks, deck.id, &ldoc::Deck::id);
            const auto scene_id = position < state.cut_ms ? state.from : state.target;
            if (position < state.cut_ms && state.target) warm_scene(project, *state.target);
            const auto* source = effective_->source(state.source);
            const auto* canvas = effective_->canvas(deck.canvas);
            if (!scene_id || !effective_->scene(*scene_id) || !source || !canvas) continue;
            auto overlay = ldoc::make_node(ldoc::NodeKind::Source, source->name);
            overlay.id = state.overlay;
            auto& data = std::get<ldoc::SourceData>(overlay.data);
            data.source = state.source;
            if (!effective_->buses.empty()) data.audio.bus = effective_->buses.front().id;
            overlay.transform.scale = {
                static_cast<double>(canvas->width) / static_cast<double>(std::max<std::int64_t>(1, lcomp::timer_number(source->settings, "width", 1920))),
                static_cast<double>(canvas->height) / static_cast<double>(std::max<std::int64_t>(1, lcomp::timer_number(source->settings, "height", 1080)))};
            auto underlay = ldoc::make_node(ldoc::NodeKind::Precomp, "Transition scene");
            underlay.id = state.underlay;
            std::get<ldoc::PrecompData>(underlay.data).scene = *scene_id;
            ldoc::Scene scene;
            scene.id = state.scene;
            scene.name = "Stinger transition";
            scene.canvas = deck.canvas;
            scene.roots = {std::move(overlay), std::move(underlay)};
            selected->program_scene = scene.id;
            effective_->scenes.push_back(std::move(scene));
        }
        return effective_ ? *effective_ : project;
    }

    lcomp::Picture render(const ldoc::Project& original, const ldoc::Id& canvas,
                          std::uint32_t max_edge, lcomp::Picture* full_output, bool display_output,
                          std::chrono::steady_clock::time_point frame_time) {
        const auto t_entry = std::chrono::steady_clock::now();
        const auto& project = effective_ ? *effective_ : original;
        sample_time_ = frame_time;
        std::erase_if(videos_, [&](auto& item) {
            const auto* source = project.source(item.second->source.id);
            if (!source || source->type != "source.v4l2" ||
                !same_video_mode(item.second->source, *source) ||
                (item.second->capture && item.second->capture->signal_changed())) {
                video_held_.erase(item.first);
                audio_.set_capture_delay(item.second->source, {});
                return true;
            }
            item.second->source = *source;
            return false;
        });
        for (const auto& source : project.sources)
            if (source.type == "source.v4l2" && !deactivate_video_when_hidden(source))
                video_session(source);
        std::erase_if(workers_, [&](const auto& item) {
            const auto* source = project.source(item.second->source.id);
            return !source || source->settings != item.second->source.settings ||
                   source->type != item.second->source.type;
        });
        // Keep-running backgrounds get ready before paused standby clips.
        for (bool restart : {false, true})
            for (const auto& source : project.sources)
                if (retained_media(source) &&
                    lcomp::timer_flag(source.settings, "restartOnActivate", true) == restart)
                    worker_session(source);
        std::erase_if(gpus_, [&](const auto& entry) { return !project.canvas(entry.first); });
        auto& gpu = gpus_[canvas];
        if (gpu_failed_)
            gpu.reset();
        if (!cpu_ && !gpu_failed_ && !gpu) {
            if (device()) {
                std::string error;
                gpu = munkicast::rhi::GpuCompositor::create(*device_, error);
                if (!gpu)
                    fail_gpu(error);
                else
                    lc::info("GPU composite: {}", munkicast::rhi::vk_device_name(*device_));
            } else
                fail_gpu("no usable Vulkan device");
        }
        auto frames = [this, &gpu](const ldoc::Source& source) {
            return source_frame(source, bool(gpu));
        };
        const auto t_plan = std::chrono::steady_clock::now();
        auto plan = lcomp::plan_canvas(project, canvas, frames);
        next_animated_ = next_animated_ || lcomp::animated_plan(plan);
        if (gpu) {
            std::string error;
            const auto t_acquire = std::chrono::steady_clock::now();
            const auto displays = display_output ? projectors_.acquire(canvas, device_.get()) :
                std::vector<munkicast::rhi::PresentationTarget>{};
            const auto t_render = std::chrono::steady_clock::now();
            auto result = gpu->render(plan, max_edge, error, full_output, displays,
                                      full_output && device_output && device_output());
            const auto t_present = std::chrono::steady_clock::now();
            projectors_.present(canvas, result && result->status != lcomp::PictureStatus::Unsupported);
            if (static const bool bench = std::getenv("MUNKICAST_BENCHMARK") != nullptr; bench) {
                const auto t_end = std::chrono::steady_clock::now();
                const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
                // Only frames slow enough to be a visible hitch.
                if (ms(t_entry, t_end) > 40)
                    std::fprintf(stderr, "SLOW_FRAME canvas=%s total_ms=%.1f setup_ms=%.1f plan_ms=%.1f "
                                 "acquire_ms=%.1f gpu_ms=%.1f present_ms=%.1f\n",
                                 canvas.to_string().c_str(), ms(t_entry, t_end), ms(t_entry, t_plan),
                                 ms(t_plan, t_acquire), ms(t_acquire, t_render), ms(t_render, t_present),
                                 ms(t_present, t_end));
            }
            if (result)
                return std::move(*result);
            // Work still in flight cannot have its resources or capture leases recycled.
            // Exiting lets the OS reclaim them; the UI reports the lost process.
            if (error.starts_with("GPU fence failed")) {
                lc::error("{}; stopping renderer", error);
                std::_Exit(1);
            }
            fail_gpu(error);
            gpu.reset();
            plan = lcomp::plan_canvas(project, canvas, [this](const ldoc::Source& source) {
                return source_frame(source, false);
            });
        }
        if (display_output) (void)projectors_.acquire(canvas, nullptr);
        auto picture = lcomp::execute_cpu(plan);
        if (full_output)
            *full_output = picture;
        return lcomp::preview_size(std::move(picture), max_edge);
    }

    Projectors& projectors() { return projectors_; }

    /// Whether full-size output should stay on the GPU as NV12 for the encoder.
    std::function<bool()> device_output;

    std::array<std::uint8_t, 16> gpu_uuid() {
        return !cpu_ && !gpu_failed_ && device() ? munkicast::rhi::vk_device_uuid(*device_)
                                                 : std::array<std::uint8_t, 16>{};
    }

    bool fresh() const {
        if (projectors_.dirty()) return true;
        if (transition_dirty_ || std::ranges::any_of(transitions_, [](const auto& entry) { return entry.second.playing; }))
            return true;
        if (animated_)
            return true;
        for (const auto& [id, session] : workers_) {
            if (!session->visible) continue;
            std::lock_guard lock(session->mu);
            if (session->sequence != session->drawn) return true;
        }
        for (const auto& [id, session] : videos_) {
            if (!session->visible) continue;
            std::lock_guard lock(session->mu);
            if (session->buffer && session->last ? session->buffer->ready(std::chrono::steady_clock::now()) :
                session->sequence != session->drawn) return true;
        }
        for (const auto& [name, session] : sessions_) {
            std::lock_guard lock(session->mu);
            if (session->sequence != session->drawn ||
                (session->retry && std::chrono::steady_clock::now() >= session->retry_at))
                return true;
        }
        return false;
    }

    bool animated_ = false;
    bool next_animated_ = false;

    void done() {
        const SlowStep slow{"done"};
        animated_ = next_animated_;
        next_animated_ = false;
        for (const auto& [id, snapshot] : worker_held_) {
            auto& session = *workers_.at(id);
            std::lock_guard lock(session.mu);
            session.drawn = snapshot.second;
        }
        worker_held_.clear();
        std::erase_if(workers_, [this](const auto& item) {
            auto& session = *item.second;
            session.visible = worker_wanted_.contains(item.first);
            session.active.store(session.visible);
            session.worker->set_active(session.visible);
            return !session.visible && !retained_media(session.source) &&
                   !(session.source.type == "source.browser" &&
                     !lcomp::timer_flag(session.source.settings, "shutdownWhenHidden", false));
        });
        worker_wanted_.clear();
        for (const auto& [id, snapshot] : video_held_) {
            auto& session = *videos_.at(id);
            std::lock_guard lock(session.mu);
            session.drawn = snapshot.second;
        }
        video_held_.clear();
        std::erase_if(videos_, [this](const auto& item) {
            auto& session = *item.second;
            session.visible = video_wanted_.contains(item.first);
            const bool remove = reopen_video_ ||
                (!session.visible && deactivate_video_when_hidden(session.source));
            if (remove) audio_.set_capture_delay(session.source, {});
            return remove;
        });
        reopen_video_ = false;
        video_wanted_.clear();
        for (auto& [name, snapshot] : held_) {
            auto& s = *sessions_.at(name);
            std::lock_guard lock(s.mu);
            s.drawn = snapshot.sequence;
        }
        held_.clear();
        std::erase_if(sessions_,
                      [this](const auto& entry) { return !wanted_.contains(entry.first); });
        wanted_.clear();
    }

    std::vector<lcomp::CaptureState> capture_stats() {
        std::vector<lcomp::CaptureState> states;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [id, session] : videos_) {
            std::lock_guard lock(session->mu);
            auto error = session->error;
            if (session->capture && (!session->last ||
                now - session->arrival > std::chrono::seconds(1)))
                error = "No capture frames arriving";
            states.push_back({session->source.id,
                session->capture ? static_cast<std::uint32_t>(
                    session->capture->negotiated_fps() * 1000) : 0,
                error.empty() ? session->received : 0, std::move(error)});
        }
        return states;
    }

  private:
    struct Transition {
        std::optional<ldoc::Id> target, from;
        ldoc::Id source, overlay = ldoc::Id::generate(),
                 underlay = ldoc::Id::generate(), scene = ldoc::Id::generate();
        std::chrono::steady_clock::time_point start{};
        std::int64_t cut_ms = 500, duration_ms = 1000;
        bool playing = false, manual = false;
        std::string kind;
    };

    static void mix_transition(ldoc::Project& project, const ldoc::Deck& deck,
                               const Transition& state, std::optional<ldoc::Id> from,
                               std::optional<ldoc::Id> to, bool black, double progress) {
        progress = std::clamp(progress, 0.0, 1.0);
        const double incoming = black ? std::max(0.0, progress * 2 - 1) : progress;
        const double outgoing = black ? std::max(0.0, 1 - progress * 2) : 1;
        ldoc::Scene scene;
        scene.id = state.scene;
        scene.canvas = deck.canvas;
        scene.name = "Live transition";
        const auto add = [&](std::optional<ldoc::Id> target, const ldoc::Id& id,
                             double opacity, double audio_gain) {
            if (!target || !project.scene(*target)) return;
            auto node = ldoc::make_node(ldoc::NodeKind::Precomp, "Transition scene");
            node.id = id;
            std::get<ldoc::PrecompData>(node.data).scene = *target;
            node.blend.opacity = opacity;
            project.settings.extra["ui.transition-gain." + id.to_string()] = audio_gain;
            scene.roots.push_back(std::move(node));
        };
        add(to, state.overlay, incoming, black ? incoming : progress);
        add(from, state.underlay, outgoing, black ? outgoing : 1 - progress);
        const auto selected = std::ranges::find(project.decks, deck.id, &ldoc::Deck::id);
        if (selected != project.decks.end()) selected->program_scene = scene.id;
        project.scenes.push_back(std::move(scene));
    }

    std::map<ldoc::Id, Transition> transitions_;
    std::optional<ldoc::Project> effective_;
    bool transition_dirty_ = false;
    AudioSources& audio_;
    struct WorkerSession {
        ldoc::Source source;
        std::mutex mu;
        std::shared_ptr<const lcomp::PreviewFrame> last;
        std::uint64_t sequence = 0, drawn = 0;
        bool visible = false;
        std::atomic<bool> active{false};
        std::unique_ptr<SourceWorker> worker;
        ~WorkerSession() { worker.reset(); }
    };
    std::map<std::string, std::unique_ptr<WorkerSession>> workers_;
    std::map<std::string, std::pair<std::shared_ptr<const lcomp::PreviewFrame>, std::uint64_t>> worker_held_;
    std::set<std::string> worker_wanted_;

    /// Start a scene's retained media before it is drawn, so a clip behind a stinger is already
    /// playing when the cut reveals it instead of resuming from paused under the overlay.
    /// Follows visible precomps; `depth` bounds a precomp cycle.
    void warm_scene(const ldoc::Project& project, const ldoc::Id& scene_id, int depth = 0) {
        const auto* scene = project.scene(scene_id);
        if (!scene || depth > 8) return;
        const auto walk = [&](this const auto& self, const std::vector<ldoc::Node>& nodes) -> void {
            for (const auto& node : nodes) {
                if (!node.visible) continue;
                if (const auto* children = node.children()) self(*children);
                else if (const auto* pre = std::get_if<ldoc::PrecompData>(&node.data))
                    warm_scene(project, pre->scene, depth + 1);
                else if (const auto* src = std::get_if<ldoc::SourceData>(&node.data); src && src->source)
                    if (const auto* source = project.source(*src->source); source && retained_media(*source)) {
                        auto& session = worker_session(*source);
                        session.worker->set_active(true);
                        worker_wanted_.insert(source->id.to_string());
                    }
            }
        };
        walk(scene->roots);
    }

    static bool retained_media(const ldoc::Source& source) {
        const auto it = source.settings.find("closeWhenInactive");
        const auto* close = it == source.settings.end() ? nullptr : std::get_if<bool>(&it->second);
        return (source.type == "source.media" || source.type == "source.playlist" ||
                (source.type == "source.text" && lcomp::timer_flag(source.settings, "countdown.enabled"))) &&
               !(close && *close);
    }

    WorkerSession& worker_session(const ldoc::Source& source) {
        const auto id = source.id.to_string();
        auto existing = workers_.find(id);
        if (existing != workers_.end() &&
            (existing->second->source.settings != source.settings || existing->second->source.type != source.type)) {
            worker_held_.erase(id);
            workers_.erase(existing);
        }
        if (!workers_.contains(id)) {
            auto session = std::make_unique<WorkerSession>();
            session->source = source;
            auto* ptr = session.get();
            session->worker = std::make_unique<SourceWorker>(source, [ptr](auto frame) {
                std::lock_guard lock(ptr->mu);
                ptr->last = std::move(frame);
                ++ptr->sequence;
            }, [this, ptr, id = source.id](auto pcm) {
                if (ptr->active.load()) audio_.ingest(id, pcm);
            }, false);
            workers_.emplace(id, std::move(session));
        }
        return *workers_.at(id);
    }

    lcomp::CapturedPicture worker_frame(const ldoc::Source& source) {
        const auto id = source.id.to_string();
        auto& current = worker_session(source);
        current.active.store(true);
        current.worker->set_active(true);
        worker_wanted_.insert(id);
        if (!worker_held_.contains(id)) {
            auto& session = *workers_.at(id);
            std::lock_guard lock(session.mu);
            worker_held_.emplace(id, std::make_pair(session.last, session.sequence));
        }
        const auto& image = worker_held_.at(id).first;
        if (!image) return {};
        return {image->width, image->height, image->bytes().data(), image->bytes().size(), nullptr,
                false,        image};
    }

    struct VideoSession {
        ldoc::Source source;
        std::string error;
        std::chrono::steady_clock::time_point window{}, arrival{};
        std::uint32_t arrivals = 0, received = 0;
        bool visible = false;
        std::mutex mu;
        std::shared_ptr<const munkicast::capture::VideoFrame> last;
        std::optional<munkicast::capture::VideoBuffer> buffer;
        std::uint64_t sequence = 0, drawn = 0;
        std::unique_ptr<munkicast::capture::VideoCapture> capture;
        ~VideoSession() { if (capture) capture->stop(); }
    };
    std::map<std::string, std::unique_ptr<VideoSession>> videos_;
    std::map<std::string, std::pair<std::shared_ptr<const munkicast::capture::VideoFrame>, std::uint64_t>> video_held_;
    std::set<std::string> video_wanted_;
    std::chrono::steady_clock::time_point sample_time_{};

    static std::string setting(const ldoc::Source& source, const std::string& key) {
        const auto it = source.settings.find(key);
        if (it != source.settings.end())
            if (const auto* text = std::get_if<std::string>(&it->second)) return *text;
        return {};
    }
    static munkicast::capture::VideoOptions video_options(const ldoc::Source& source) {
        munkicast::capture::VideoOptions options;
        options.format = setting(source, "videoFormat");
        for (auto [key, value] : {std::pair{"buffering", &options.buffering},
                                 std::pair{"autoReset", &options.auto_reset}}) {
            const auto it = source.settings.find(key);
            if (it != source.settings.end())
                if (const auto* flag = std::get_if<bool>(&it->second)) *value = *flag;
        }
        if (const auto it = source.settings.find("timeoutFrames"); it != source.settings.end()) {
            const auto* frames = std::get_if<std::int64_t>(&it->second);
            options.timeout_frames = frames && *frames >= 2 && *frames <= 120 ? static_cast<int>(*frames) : 0;
        }
        return options;
    }
    static bool same_video_mode(const ldoc::Source& a, const ldoc::Source& b) {
        for (const char* key : {"device", "resolution", "frameRate", "colourRange"})
            if (setting(a, key) != setting(b, key)) return false;
        return video_options(a) == video_options(b);
    }
    // GPU compositing converts camera YUV itself; the CPU path still needs BGRA.
    munkicast::capture::VideoOptions capture_options(const ldoc::Source& source) const {
        auto options = video_options(source);
        options.raw_yuv = !cpu_ && !gpu_failed_;
        return options;
    }
    static bool deactivate_video_when_hidden(const ldoc::Source& source) {
        const auto option = source.settings.find("deactivateWhenHidden");
        const auto* enabled = option == source.settings.end()
            ? nullptr : std::get_if<bool>(&option->second);
        return enabled && *enabled;
    }
    VideoSession& video_session(const ldoc::Source& source) {
        const auto id = source.id.to_string();
        if (!videos_.contains(id)) {
            auto session = std::make_unique<VideoSession>();
            session->source = source;
            std::string error;
            session->capture = munkicast::capture::VideoCapture::open(setting(source, "device"),
                setting(source, "resolution"), setting(source, "frameRate"), setting(source, "colourRange"), error,
                capture_options(source));
            session->error = error;
            if (!session->capture) lc::warn("cannot capture {}: {}", setting(source, "device"), error);
            else {
                if (video_options(source).buffering) {
                    const auto period = std::chrono::nanoseconds(std::max<std::int64_t>(1,
                        static_cast<std::int64_t>(1e9 / session->capture->negotiated_fps())));
                    if (period <= std::chrono::seconds(10)) session->buffer.emplace(period);
                }
                auto* ptr = session.get();
                const bool trace = std::getenv("MUNKICAST_TRACE_OUTPUT") != nullptr;
                session->capture->start([ptr, trace, source_id = id](auto frame) {
                    const auto now = std::chrono::steady_clock::now();
                    if (trace && frame) {
                        // Opt-in, before the render mutex: compare actual capture pictures,
                        // not just callback counts. Hashes are comparable within this run;
                        // hashing/logging costs capture time and is not a performance benchmark.
                        const auto hash = std::hash<std::string_view>{}(std::string_view{
                            reinterpret_cast<const char*>(frame->pixels.data()),
                            frame->pixels.size()});
                        std::fprintf(stderr, "CAPTURE_INPUT source_id=%s driver_sequence=%u "
                            "captured_ns=%lld arrival_ns=%lld pixel_hash=%llu bytes=%zu\n",
                            source_id.c_str(), frame->sequence,
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                frame->captured_at.time_since_epoch()).count()),
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                now.time_since_epoch()).count()),
                            static_cast<unsigned long long>(hash), frame->pixels.size());
                    }
                    std::lock_guard lock(ptr->mu);
                    if (frame) {
                        if (ptr->window == std::chrono::steady_clock::time_point{}) ptr->window = now;
                        else ++ptr->arrivals;
                        const double elapsed = std::chrono::duration<double>(now - ptr->window).count();
                        if (elapsed >= 1.0) {
                            ptr->received = static_cast<std::uint32_t>(ptr->arrivals * 1000 / elapsed);
                            ptr->arrivals = 0;
                            ptr->window = now;
                        }
                        ptr->arrival = now;
                    }
                    if (ptr->buffer)
                        ptr->buffer->push(frame, ptr->sequence + 1, ptr->arrival);
                    ptr->last = std::move(frame);
                    ++ptr->sequence;
                });
            }
            videos_.emplace(id, std::move(session));
        }
        auto& session = *videos_.at(id);
        audio_.set_capture_delay(source, session.buffer ? session.buffer->delay() :
                                 std::chrono::nanoseconds{});
        return session;
    }
    lcomp::CapturedPicture source_frame(const ldoc::Source& source, bool gpu) {
        if (source.type == "source.text" || source.type == "source.media" ||
            source.type == "source.slideshow" || source.type == "source.playlist" || source.type == "source.browser" || source.type == "source.blackmagic")
            return worker_frame(source);
        if (source.type != "source.v4l2") {
            const bool window = source.type == "source.window" || source.type == "source.game";
            const auto cursor = source.settings.find("showCursor");
            const auto* enabled = cursor == source.settings.end() ? nullptr : std::get_if<bool>(&cursor->second);
            return frame(setting(source, window ? "window" : "output"), gpu, window, !enabled || *enabled);
        }
        const auto id = source.id.to_string();
        video_wanted_.insert(id);
        auto& session = video_session(source);
        if (!video_held_.contains(id)) {
            std::lock_guard lock(session.mu);
            auto sample = session.buffer ? session.buffer->sample(sample_time_) :
                munkicast::capture::VideoBuffer::Sample{session.last, session.sequence, session.arrival};
            if (!sample.frame) sample.sequence = session.sequence;
            if (std::getenv("MUNKICAST_TRACE_OUTPUT") ||
                std::getenv("MUNKICAST_TRACE_SAMPLING"))
                std::fprintf(stderr, "CAPTURE_SAMPLE source=%s sequence=%llu advance=%llu "
                    "age_ms=%.3f driver_sequence=%u source_id=%s sample_ns=%lld\n",
                    source.name.c_str(), static_cast<unsigned long long>(sample.sequence),
                    static_cast<unsigned long long>(sample.sequence - session.drawn),
                    sample.frame ? std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - sample.time).count() : -1.0,
                    sample.frame ? sample.frame->sequence : 0, id.c_str(),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        sample_time_.time_since_epoch()).count()));
            video_held_.emplace(id, std::make_pair(sample.frame, sample.sequence));
        }
        const auto& image = video_held_.at(id).first;
        if (!image) return {};
        lcomp::CapturedPicture picture{image->width, image->height, image->pixels.data(),
                                       image->pixels.size(), nullptr};
        if (image->layout != munkicast::capture::VideoLayout::Bgra) {
            // A frame from before a GPU failure cannot be drawn by the CPU; the session reopens.
            if (!gpu) return {};
            picture.yuv = static_cast<std::uint8_t>(image->layout);
            picture.yuv_matrix = static_cast<std::uint8_t>((image->bt709 ? 1 : 0) | (image->full_range ? 2 : 0));
        }
        return picture;
    }

    struct Session {
        std::mutex mu;
        std::shared_ptr<const munkicast::capture::DmabufImage> last;
        std::uint64_t sequence = 0, drawn = 0;
        bool retry = false;
        std::chrono::steady_clock::time_point retry_at;
        std::unique_ptr<munkicast::capture::ScreenCapture> capture;

        ~Session() {
            if (capture)
                capture->stop();
        }
    };

    struct Snapshot {
        std::shared_ptr<const munkicast::capture::DmabufImage> image;
        std::uint64_t sequence = 0;
        std::vector<std::uint8_t> pixels;
    };

    munkicast::rhi::VkCtxPtr device_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::map<std::string, Snapshot> held_;
    std::set<std::string> wanted_;
    // Destroy GPU views before capture images, and capture images before their device.
    // Each canvas keeps its own scratch images; alternating dimensions must not reallocate.
    std::map<ldoc::Id, std::unique_ptr<munkicast::rhi::GpuCompositor>> gpus_;
    Projectors projectors_; // Must release exported display images before device_.
    bool reopen_video_ = false;
    bool cpu_ = false, require_gpu_ = false, device_failed_ = false, gpu_failed_ = false,
         capture_failed_ = false;

    void fail_gpu(const std::string& error) {
        lc::warn("GPU composite unavailable: {}; using CPU", error);
        gpu_failed_ = true;
        reopen_video_ = true; // Raw YUV cameras reopen delivering BGRA for the CPU path.
        capture_failed_ = error.find("(-4)") != std::string::npos;
        if (require_gpu_)
            std::_Exit(1);
    }

    bool device() {
        if (device_ || device_failed_)
            return bool(device_);
        const char* selected = std::getenv("MUNKICAST_RENDER_DEVICE");
        std::string error;
        device_ = munkicast::rhi::vk_init(selected ? selected : "", error);
        if (!device_) {
            lc::warn("no capture device: {}", error);
            device_failed_ = true;
        }
        return bool(device_);
    }

    lcomp::CapturedPicture frame(const std::string& target, bool gpu, bool window, bool cursor) {
        const std::string connector = (window ? "window:" : "screen:") + target + (cursor ? ":cursor" : ":bare");
        if (capture_failed_)
            return {};
        wanted_.insert(connector);
        if (auto it = sessions_.find(connector); it != sessions_.end()) {
            bool reopen = false;
            {
                std::lock_guard lock(it->second->mu);
                reopen = it->second->retry && std::chrono::steady_clock::now() >= it->second->retry_at;
            }
            if (reopen) { held_.erase(connector); sessions_.erase(it); }
        }
        if (!sessions_.contains(connector)) {
            auto session = std::make_unique<Session>();
            if (device()) {
                std::string error;
                session->capture =
                    munkicast::capture::ScreenCapture::open(*device_, target, cursor, error, window);
                if (!session->capture) {
                    lc::warn("cannot capture {}: {}", connector, error);
                    session->retry = window;
                    session->retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                } else {
                    Session* ptr = session.get();
                    session->capture->start_images([ptr, window](auto image) {
                        std::lock_guard lock(ptr->mu);
                        ptr->retry = window && !image;
                        ptr->retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                        ptr->last = std::move(image);
                        ++ptr->sequence;
                    });
                }
            }
            sessions_.emplace(connector, std::move(session));
        }
        if (!held_.contains(connector)) {
            auto& session = *sessions_.at(connector);
            std::lock_guard lock(session.mu);
            held_.emplace(connector, Snapshot{session.last, session.sequence, {}});
        }
        auto& snap = held_.at(connector);
        if (!snap.image)
            return {};
        lcomp::CapturedPicture result;
        result.width = static_cast<int>(snap.image->width);
        result.height = static_cast<int>(snap.image->height);
        if (gpu)
            result.device_image = snap.image.get();
        else {
            if (snap.pixels.empty()) {
                std::string error;
                if (!munkicast::rhi::vk_readback_exported(*device_, *snap.image, true, snap.pixels,
                                                         error)) {
                    lc::warn("capture readback failed: {}", error);
                    return {};
                }
            }
            result.pixels = snap.pixels.data();
            result.size = snap.pixels.size();
        }
        return result;
    }
};

} // namespace

int main() {
    const int fd = channel_fd();
    if (fd < 0) {
        std::fprintf(stderr, "munkicast-render: MUNKICAST_CHANNEL_FD is not a descriptor\n");
        return 2;
    }

    AudioSources audio;
    Renderer renderer(audio);
    Recorder recorder(audio);
    recorder.gpu_uuid = [&renderer] { return renderer.gpu_uuid(); };
    renderer.device_output = [&recorder] { return recorder.takes_device_frames(); };
    lcomp::RecordHooks record;
    record.control = [&recorder](const lcomp::RecordRequest& request, const ldoc::Project& p) {
        return recorder.control(request, p);
    };
    record.wants = [&recorder](const ldoc::Id& canvas) {
        return recorder.running() && recorder.canvas() == canvas;
    };
    record.frame = [&recorder](const ldoc::Id& canvas, const lcomp::Picture& picture,
                               std::chrono::steady_clock::time_point frame_time) {
        return recorder.frame(canvas, picture, frame_time);
    };
    lcomp::ProjectorHooks projectors;
    projectors.control = [&renderer](const auto& request, const auto& p) { return renderer.projectors().control(request, p); };
    projectors.pump = [&renderer](const auto& p) { return renderer.projectors().pump(p); };
    projectors.wants = [&renderer](const auto& id) { return renderer.projectors().wants(id); };
    auto served = lcomp::serve_preview(
        lipc::Channel{lipc::Fd{fd}}, {}, [&renderer] { renderer.done(); },
        [&renderer] { return renderer.fresh(); },
        [&renderer](const ldoc::Project& p, const ldoc::Id& id, std::uint32_t edge,
                    lcomp::Picture* full_output, bool display_output, auto frame_time) {
            return renderer.render(p, id, edge, full_output, display_output, frame_time);
        },
        [&audio, &renderer](const ldoc::Project& p) { return audio.meters(renderer.prepare(p)); },
        {}, std::move(record), [&renderer] { return renderer.capture_stats(); }, std::move(projectors));
    if (!served) {
        std::fprintf(stderr, "munkicast-render: %s\n", served.error().c_str());
        return 1;
    }
    return 0;
}

// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The render process owns capture and the device. Both backends consume lcomp's shared plan.
// Offscreen checks select CPU; normal runs try GPU and retain the CPU fallback (ADR-0042).

#include "lcomp/preview.hpp"
#include <lcomp/countdown.hpp>
#include "lipc/fd.hpp"
#include "source_worker.hpp"
#include "audio_sources.hpp"
#include "recorder.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lcapture/screen.hpp>
#include <lcapture/video.hpp>
#include <lcore/log.hpp>
#include <lrhi/composite.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

int channel_fd() {
    const char* spec = std::getenv("LIBERTAS_CHANNEL_FD");
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
        const char* mode = std::getenv("LIBERTAS_RENDER_BACKEND");
        require_gpu_ = mode && std::strcmp(mode, "gpu") == 0;
        const char* platform = std::getenv("QT_QPA_PLATFORM");
        cpu_ = (mode && std::strcmp(mode, "cpu") == 0) ||
               (!require_gpu_ && platform && std::strcmp(platform, "offscreen") == 0);
    }

    ~Renderer() {
        for (const auto& [id, session] : workers_) session->worker->stop();
        workers_.clear();
    }

    const ldoc::Project& prepare(const ldoc::Project& project) {
        effective_.reset();
        transition_dirty_ = false;
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(transitions_, [&](const auto& entry) {
            return std::ranges::none_of(project.decks, [&](const auto& deck) { return deck.id == entry.first; });
        });
        for (const auto& deck : project.decks) {
            auto [it, inserted] = transitions_.try_emplace(deck.id);
            auto& state = it->second;
            if (inserted) { state.target = deck.program_scene; continue; }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.start).count();
            auto previous = state.playing && elapsed < state.cut_ms ? state.from : state.target;
            if (state.target != deck.program_scene) {
                transition_dirty_ = true;
                state.target = deck.program_scene;
                state.playing = false;
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
            if (position >= state.duration_ms || !project.source(state.source)) {
                state.playing = false;
                transition_dirty_ = true;
                continue;
            }
            if (!effective_) effective_ = project;
            auto selected = std::ranges::find(effective_->decks, deck.id, &ldoc::Deck::id);
            const auto scene_id = position < state.cut_ms ? state.from : state.target;
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
                          std::uint32_t max_edge) {
        const auto& project = effective_ ? *effective_ : original;
        std::erase_if(videos_, [&](auto& item) {
            const auto* source = project.source(item.second->source.id);
            if (!source || source->type != "source.v4l2" ||
                !same_video_mode(item.second->source, *source)) {
                video_held_.erase(item.first);
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
                gpu = libertas::rhi::GpuCompositor::create(*device_, error);
                if (!gpu)
                    fail_gpu(error);
                else
                    lc::info("GPU composite: {}", libertas::rhi::vk_device_name(*device_));
            } else
                fail_gpu("no usable Vulkan device");
        }
        auto frames = [this, &gpu](const ldoc::Source& source) {
            return source_frame(source, bool(gpu));
        };
        auto plan = lcomp::plan_canvas(project, canvas, frames);
        next_animated_ = next_animated_ || lcomp::animated_plan(plan);
        if (gpu) {
            std::string error;
            auto result = gpu->render(plan, max_edge, error);
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
        return lcomp::preview_size(lcomp::execute_cpu(plan), max_edge);
    }

    bool fresh() const {
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
            if (session->sequence != session->drawn) return true;
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
            if (reopen_video_) return true;
            auto& session = *item.second;
            session.visible = video_wanted_.contains(item.first);
            return !session.visible && deactivate_video_when_hidden(session.source);
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

  private:
    struct Transition {
        std::optional<ldoc::Id> target, from;
        ldoc::Id source, overlay = ldoc::Id::generate(),
                 underlay = ldoc::Id::generate(), scene = ldoc::Id::generate();
        std::chrono::steady_clock::time_point start{};
        std::int64_t cut_ms = 500, duration_ms = 1000;
        bool playing = false;
    };
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
        bool visible = false;
        std::mutex mu;
        std::shared_ptr<const libertas::capture::VideoFrame> last;
        std::uint64_t sequence = 0, drawn = 0;
        std::unique_ptr<libertas::capture::VideoCapture> capture;
        ~VideoSession() { if (capture) capture->stop(); }
    };
    std::map<std::string, std::unique_ptr<VideoSession>> videos_;
    std::map<std::string, std::pair<std::shared_ptr<const libertas::capture::VideoFrame>, std::uint64_t>> video_held_;
    std::set<std::string> video_wanted_;

    static std::string setting(const ldoc::Source& source, const std::string& key) {
        const auto it = source.settings.find(key);
        if (it != source.settings.end())
            if (const auto* text = std::get_if<std::string>(&it->second)) return *text;
        return {};
    }
    static libertas::capture::VideoOptions video_options(const ldoc::Source& source) {
        libertas::capture::VideoOptions options;
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
    libertas::capture::VideoOptions capture_options(const ldoc::Source& source) const {
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
            session->capture = libertas::capture::VideoCapture::open(setting(source, "device"),
                setting(source, "resolution"), setting(source, "frameRate"), setting(source, "colourRange"), error,
                capture_options(source));
            if (!session->capture) lc::warn("cannot capture {}: {}", setting(source, "device"), error);
            else {
                auto* ptr = session.get();
                session->capture->start([ptr](auto frame) {
                    std::lock_guard lock(ptr->mu);
                    ptr->last = std::move(frame);
                    ++ptr->sequence;
                });
            }
            videos_.emplace(id, std::move(session));
        }
        return *videos_.at(id);
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
            video_held_.emplace(id, std::make_pair(session.last, session.sequence));
        }
        const auto& image = video_held_.at(id).first;
        if (!image) return {};
        lcomp::CapturedPicture picture{image->width, image->height, image->pixels.data(),
                                       image->pixels.size(), nullptr};
        if (image->layout != libertas::capture::VideoLayout::Bgra) {
            // A frame from before a GPU failure cannot be drawn by the CPU; the session reopens.
            if (!gpu) return {};
            picture.yuv = static_cast<std::uint8_t>(image->layout);
            picture.yuv_matrix = static_cast<std::uint8_t>((image->bt709 ? 1 : 0) | (image->full_range ? 2 : 0));
        }
        return picture;
    }

    struct Session {
        std::mutex mu;
        std::shared_ptr<const libertas::capture::DmabufImage> last;
        std::uint64_t sequence = 0, drawn = 0;
        bool retry = false;
        std::chrono::steady_clock::time_point retry_at;
        std::unique_ptr<libertas::capture::ScreenCapture> capture;

        ~Session() {
            if (capture)
                capture->stop();
        }
    };

    struct Snapshot {
        std::shared_ptr<const libertas::capture::DmabufImage> image;
        std::uint64_t sequence = 0;
        std::vector<std::uint8_t> pixels;
    };

    libertas::rhi::VkCtxPtr device_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::map<std::string, Snapshot> held_;
    std::set<std::string> wanted_;
    // Destroy GPU views before capture images, and capture images before their device.
    // Each canvas keeps its own scratch images; alternating dimensions must not reallocate.
    std::map<ldoc::Id, std::unique_ptr<libertas::rhi::GpuCompositor>> gpus_;
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
        const char* selected = std::getenv("LIBERTAS_RENDER_DEVICE");
        std::string error;
        device_ = libertas::rhi::vk_init(selected ? selected : "", error);
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
                    libertas::capture::ScreenCapture::open(*device_, target, cursor, error, window);
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
                if (!libertas::rhi::vk_readback_exported(*device_, *snap.image, true, snap.pixels,
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
        std::fprintf(stderr, "libertas-render: LIBERTAS_CHANNEL_FD is not a descriptor\n");
        return 2;
    }

    AudioSources audio;
    Renderer renderer(audio);
    Recorder recorder(audio);
    lcomp::RecordHooks record;
    record.control = [&recorder](const lcomp::RecordRequest& request, const ldoc::Project& p) {
        return recorder.control(request, p);
    };
    record.wants = [&recorder](const ldoc::Id& canvas) {
        return recorder.running() && recorder.canvas() == canvas;
    };
    record.frame = [&recorder](const ldoc::Id& canvas, const lcomp::Picture& picture) {
        return recorder.frame(canvas, picture);
    };
    auto served = lcomp::serve_preview(
        lipc::Channel{lipc::Fd{fd}}, {}, [&renderer] { renderer.done(); },
        [&renderer] { return renderer.fresh(); },
        [&renderer](const ldoc::Project& p, const ldoc::Id& id, std::uint32_t edge) {
            return renderer.render(p, id, edge);
        }, [&audio, &renderer](const ldoc::Project& p) { return audio.meters(renderer.prepare(p)); },
        {}, std::move(record));
    if (!served) {
        std::fprintf(stderr, "libertas-render: %s\n", served.error().c_str());
        return 1;
    }
    return 0;
}

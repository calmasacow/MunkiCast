// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preview.hpp"
#include <cmath>

#include "assetcache.hpp"
#include "lcomp/countdown.hpp"
#include "lcomp/preview.hpp"

#include <QDateTime>
#include <QImage>
#include <QGuiApplication>
#include <QScreen>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQuickImageProvider>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>

namespace munkicast::ui {
namespace {

std::filesystem::path g_renderer;

QImage image_from(lcomp::PreviewFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.stride == 0)
        return {};
    if (frame.stride > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        return {};
    if (frame.stride < static_cast<std::uint32_t>(frame.width) * 4u)
        return {};
    const auto rows = static_cast<std::size_t>(frame.height);
    if (rows > std::numeric_limits<std::size_t>::max() / frame.stride)
        return {};
    const std::size_t bytes = static_cast<std::size_t>(frame.stride) * rows;
    if (frame.pixels.size() < bytes)
        return {};
    // The image owns decoded storage, never the renderer's reusable shared mapping.
    auto pixels = std::make_unique<std::vector<std::uint8_t>>(std::move(frame.pixels));
    const QImage image(static_cast<const uchar*>(pixels->data()), frame.width, frame.height,
                       static_cast<int>(frame.stride), QImage::Format_RGBA8888,
                       [](void* data) { delete static_cast<std::vector<std::uint8_t>*>(data); },
                       pixels.get());
    pixels.release();
    return image;
}

} // namespace

struct Preview::Held {
    QImage image;
    lcomp::PictureStatus status = lcomp::PictureStatus::NoProgram;
    int width = 0;
    int height = 0;
    std::uint32_t gaps = 0;
    std::uint64_t render_ns = 0, rendered_frames = 0, render_lag_frames = 0;
    std::uint32_t fps_num = 0, fps_den = 0;
};

struct Preview::Received {
    lcomp::PreviewEvent event;
    QImage image;
    QString error;
};

struct Preview::State {
    std::mutex mu;
    std::map<QString, Held> frames;

    struct Sample {
        std::chrono::steady_clock::time_point at;
        std::uint64_t ns, frames;
    };

    std::map<QString, std::deque<Sample>> recent;
    int generation = 0;
};

struct Preview::Link {
    std::optional<lcomp::PreviewLink> channel;
    pid_t child = -1;
    QSocketNotifier* write = nullptr;
    bool alive = false;
    bool in_flight = false;
    bool dirty = false;
    bool have_pending = false;
    bool refresh_only = false;
    bool refresh_pending = false;
    std::optional<lcomp::RecordRequest> record_request;
    std::deque<std::string> projector_requests;
    /// Set when this commit was a fault, so its ack does not publish a picture of the last one.
    bool failed = false;
    ldoc::Project pending;
    std::vector<QString> sent;
    std::map<QString, Held> incoming;
    /// Receives and decodes off the UI thread; the UI only handles finished events. Last, so it
    /// is joined before `channel` is destroyed.
    /// Tells a restarted link's events from its predecessor's; an address could be reused.
    std::uint64_t id = [] { static std::uint64_t next = 0; return ++next; }();
    std::jthread reader;
};

class ProgramImageProvider : public QQuickImageProvider {
  public:
    explicit ProgramImageProvider(std::shared_ptr<Preview::State> state)
        : QQuickImageProvider(QQuickImageProvider::Image), state_(std::move(state)) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& /*requested*/) override {
        const QString key = id.section(QLatin1Char('/'), 0, 0);
        std::lock_guard lock(state_->mu);
        const auto it = state_->frames.find(key);
        if (it == state_->frames.end() || it->second.image.isNull()) {
            if (size != nullptr)
                *size = QSize(1, 1);
            QImage blank(1, 1, QImage::Format_RGBA8888);
            blank.fill(0);
            return blank;
        }
        if (size != nullptr)
            *size = it->second.image.size();
        return it->second.image;
    }

  private:
    std::shared_ptr<Preview::State> state_;
};

void Preview::configure(std::filesystem::path renderer) {
    g_renderer = std::move(renderer);
}

Preview* Preview::create(QQmlEngine* engine, QJSEngine*) {
    auto* preview = new Preview(nullptr);
    if (engine != nullptr)
        engine->addImageProvider(QLatin1String("program"),
                                 new ProgramImageProvider(preview->state_));
    preview->start();
    return preview;
}

Preview::Preview(QObject* parent)
    : QObject(parent), renderer_(g_renderer), state_(std::make_shared<State>()),
      link_(std::make_unique<Link>()) {
    projector_poll_ = new QTimer(this);
    projector_poll_->setInterval(250);
    connect(projector_poll_, &QTimer::timeout, this, [this] {
        // Status only. The renderer continues projecting when this timer/UI is paused.
        if (link_ && link_->alive) {
            if (link_->projector_requests.empty())
                queueProjector(R"({"status":true})");
            else
                flushProjectors();
        }
    });
    bool valid = false;
    const int seconds = qEnvironmentVariableIntValue("MUNKICAST_MONITOR_SECONDS", &valid);
    if (!valid || seconds < 1 || seconds > 3600)
        return;
    // ponytail: opt-in session diagnostic; expose a UI duration only if operators need it.
    QTimer::singleShot(15000, this, [this, seconds] {
        const auto started = std::chrono::steady_clock::now();
        auto* monitor = new QTimer(this);
        monitor->setTimerType(Qt::PreciseTimer);
        const auto sample = [this, started](const char* event) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr,
                         "SESSION_MONITOR %s elapsed=%.3f streaming=%d pending=%d frames=%llu "
                         "missed=%llu encode_lag=%llu\n",
                         event, elapsed, streaming(), streamingPending(),
                         static_cast<unsigned long long>(output_frames_),
                         static_cast<unsigned long long>(output_missed_),
                         static_cast<unsigned long long>(output_lag_));
            std::fflush(stderr);
        };
        sample("begin");
        connect(monitor, &QTimer::timeout, this, [sample] { sample("sample"); });
        monitor->start(1000);
        QTimer::singleShot(std::chrono::seconds(seconds), Qt::PreciseTimer, this,
                           [this, monitor, sample] {
                               monitor->stop();
                               sample("stop_requested");
                               if (streaming() || streamingPending())
                                   stopStreaming();
                               monitor->deleteLater();
                           });
        connect(this, &Preview::recordingChanged, this, [this, sample] {
            if (!streaming() && !streamingPending())
                sample("stopped");
        });
    });
}

Preview::~Preview() {
    if (!link_)
        return;
    if (link_->write != nullptr)
        link_->write->setEnabled(false);
    stop_reader();
    if (link_->channel) {
        link_->channel->quit();
        link_->channel->close();
        link_->channel.reset();
    }
    reap(true);
}

int Preview::generation() const {
    return state_->generation;
}

bool Preview::rendererAttached() const { return link_ && link_->alive; }

QImage Preview::snapshot(const QString& canvasId) const {
    const auto key = resolve(canvasId);
    std::lock_guard lock(state_->mu);
    const auto found = state_->frames.find(key);
    return found == state_->frames.end() ? QImage{} : found->second.image;
}

QVariantMap Preview::renderStats(const QString& canvasId) const {
    const QString key = resolve(canvasId);
    std::lock_guard lock(state_->mu);
    const auto it = state_->frames.find(key);
    if (it == state_->frames.end())
        return {{QStringLiteral("available"), false}, {QStringLiteral("frames"), 0},
                {QStringLiteral("renderMs"), 0.0}, {QStringLiteral("missed"), 0},
                {QStringLiteral("fps"), 0.0}};
    const auto base = render_baselines_.find(key);
    const RenderBaseline zero{};
    const auto& start = base == render_baselines_.end() ? zero : base->second;
    const auto& frame = it->second;
    const std::uint64_t frames = frame.rendered_frames - start.frames;
    std::uint64_t recent_ns = 0, recent_frames = 0;
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
    if (auto found = state_->recent.find(key); found != state_->recent.end()) {
        auto& samples = found->second;
        while (!samples.empty() && samples.front().at <= cutoff)
            samples.pop_front();
        for (const auto& sample : samples) {
            recent_ns += sample.ns;
            recent_frames += sample.frames;
        }
    }
    return {{QStringLiteral("available"), true},
            {QStringLiteral("frames"), QVariant::fromValue<qulonglong>(frames)},
            {QStringLiteral("renderMs"),
             recent_frames
                 ? static_cast<double>(recent_ns) / static_cast<double>(recent_frames) / 1e6
                 : 0.0},
            {QStringLiteral("missed"),
             QVariant::fromValue<qulonglong>(frame.render_lag_frames - start.missed)},
            {QStringLiteral("fps"),
             frame.fps_den ? static_cast<double>(frame.fps_num) / frame.fps_den : 0.0}};
}

QVariantList Preview::captureStats() const {
    QVariantList result;
    if (!rendererAttached()) return result;
    for (const auto& state : capture_stats_) {
        const auto* source = latest_.source(state.source);
        if (!source) continue;
        QString requested = QStringLiteral("Leave unchanged");
        if (const auto it = source->settings.find("frameRate"); it != source->settings.end())
            if (const auto* rate = std::get_if<std::string>(&it->second))
                requested = QString::fromStdString(*rate);
        if (requested == QStringLiteral("Leave unchanged"))
            requested = QStringLiteral("Use current device mode");
        result.push_back(QVariantMap{{"sourceId", QString::fromStdString(state.source.to_string())},
            {"name", QString::fromStdString(source->name)}, {"requested", requested},
            {"negotiated", state.negotiated / 1000.0}, {"received", state.received / 1000.0},
            {"error", QString::fromStdString(state.error)}});
    }
    return result;
}

QVariantMap Preview::outputStats() const {
    return {{"available", output_stats_available_},
            {"frames", QVariant::fromValue<qulonglong>(output_frames_ - base_output_frames_)},
            {"missed", QVariant::fromValue<qulonglong>(output_missed_ - base_output_missed_)},
            {"lag", QVariant::fromValue<qulonglong>(output_lag_ - base_output_lag_)}};
}

void Preview::resetStats() {
    base_output_frames_ = output_frames_;
    base_output_missed_ = output_missed_;
    base_output_lag_ = output_lag_;
    {
        std::lock_guard lock(state_->mu);
        render_baselines_.clear();
        state_->recent.clear();
        for (const auto& canvas : latest_.canvases) {
            const QString key = QString::fromStdString(canvas.id.to_string());
            const auto frame_it = state_->frames.find(key);
            const std::uint64_t ns = frame_it == state_->frames.end() ? 0 : frame_it->second.render_ns;
            const std::uint64_t frames =
                frame_it == state_->frames.end() ? 0 : frame_it->second.rendered_frames;
            render_baselines_.insert_or_assign(
                key, RenderBaseline{ns, frames,
                                    frame_it == state_->frames.end() ? 0 : frame_it->second.render_lag_frames});
        }
    }
    emit changed();
}

QString Preview::resolve(const QString& canvasId) const {
    return canvasId.isEmpty() ? first_canvas_ : canvasId;
}

QString Preview::canvasKey(const QString& canvasId) const {
    return resolve(canvasId);
}

bool Preview::hasFrame(const QString& canvasId) const {
    const QString key = resolve(canvasId);
    if (key.isEmpty())
        return false;
    std::lock_guard lock(state_->mu);
    return state_->frames.contains(key);
}

QString Preview::caption(const QString& canvasId) const {
    const QString key = resolve(canvasId);
    if (key.isEmpty())
        return {};
    Held held;
    {
        std::lock_guard lock(state_->mu);
        const auto it = state_->frames.find(key);
        if (it == state_->frames.end())
            return {};
        held = it->second;
    }
    if (held.status == lcomp::PictureStatus::Unsupported)
        return QStringLiteral("too large to preview");
    int width = held.width, height = held.height;
    for (const auto& canvas : latest_.canvases) {
        if (QString::fromStdString(canvas.id.to_string()) == key) {
            width = canvas.width;
            height = canvas.height;
            break;
        }
    }
    const QString size = QStringLiteral("%1x%2").arg(width).arg(height);
    switch (held.status) {
    case lcomp::PictureStatus::NoProgram:
        return size + QStringLiteral(" · no program scene");
    case lcomp::PictureStatus::Empty:
        return size + QStringLiteral(" · nothing in the scene");
    case lcomp::PictureStatus::Partial:
        return size + QStringLiteral(" · %1 not drawn").arg(held.gaps);
    case lcomp::PictureStatus::Complete:
        return size;
    case lcomp::PictureStatus::Unsupported:
        break;
    }
    return QStringLiteral("too large to preview");
}

void Preview::setLive(bool live, ldoc::Fps rate) {
    program_live_ = live;
    live = live || recording_ || streaming_ || replay_ || !filter_target_.isEmpty() ||
           std::chrono::steady_clock::now() < transition_until_;
    if ((recording_ || streaming_ || replay_) && recording_rate_.valid() && (!rate.valid() || recording_rate_.hz() > rate.hz()))
        rate = recording_rate_;
    if (!live) {
        if (link_) link_->refresh_pending = false;
        if (live_ != nullptr)
            live_->stop();
        return;
    }
    if (live_ == nullptr) {
        live_ = new QTimer(this);
        live_->setObjectName(QStringLiteral("previewClock"));
        live_->setTimerType(Qt::PreciseTimer);
        live_->setSingleShot(true);
        QObject::connect(live_, &QTimer::timeout, this, [this] {
            tick();
            const auto now = std::chrono::steady_clock::now();
            live_deadline_ += live_period_;
            if (live_deadline_ <= now)
                live_deadline_ += live_period_ * ((now - live_deadline_) / live_period_ + 1);
            if (program_live_ || recording_ || streaming_ || replay_ || !filter_target_.isEmpty() ||
                now < transition_until_)
                schedule_tick();
        });
    }
    // Absolute deadlines retain fractional milliseconds and never accumulate timer drift.
    const auto period = std::chrono::nanoseconds(
        rate.valid() ? std::max<std::int64_t>(1, 1000000000LL * rate.den / rate.num) : 16666667);
    if (!live_->isActive() || live_period_ != period) {
        live_period_ = period;
        live_deadline_ = std::chrono::steady_clock::now() + period;
        schedule_tick();
    }
}

void Preview::schedule_tick() {
    const auto delay = std::chrono::ceil<std::chrono::milliseconds>(
        live_deadline_ - std::chrono::steady_clock::now()).count();
    live_->start(static_cast<int>(std::clamp<std::int64_t>(
        delay, 1, std::numeric_limits<int>::max())));
}

/// Republish the document we already have, so the renderer composites it against whatever its
/// capture has since received.
///
/// Nothing is marked dirty and no command runs: this is the same snapshot going round again.
/// Coalesce busy ticks into one refresh on acknowledgment: draw the newest frame without
/// waiting another timer interval or accumulating a backlog.
void Preview::tick() {
    if (!link_ || !link_->alive)
        return;
    if (link_->in_flight) {
        link_->refresh_pending = true;
        return;
    }
    if (latest_.canvases.empty())
        return;
    link_->refresh_only = true;
    try_send();
}

void Preview::requestSize(QObject* owner, int edge) {
    if (!owner)
        return;
    if (!preview_sizes_.contains(owner)) {
        QObject::connect(owner, &QObject::destroyed, this, [this, owner] {
            preview_sizes_.erase(owner);
            update_size();
        });
    }
    preview_sizes_[owner] = std::clamp(edge, 0, lcomp::kMaxPreviewEdge);
    update_size();
}

void Preview::update_size() {
    int largest = 1;
    for (const auto& [item, size] : preview_sizes_) {
        (void)item;
        largest = std::max(largest, size);
    }
    if (preview_edge_ == static_cast<std::uint32_t>(largest))
        return;
    preview_edge_ = static_cast<std::uint32_t>(largest);
    if (!latest_.canvases.empty())
        publish(latest_);
}

QVariantList Preview::projectorScreens() const {
    QVariantList result;
    for (const auto* screen : QGuiApplication::screens())
        if (!screen->name().isEmpty())
            result.append(QVariantMap{{"name", screen->name()},
                                      {"width", screen->size().width()},
                                      {"height", screen->size().height()},
                                      {"refreshRate", screen->refreshRate()}});
    return result;
}

bool Preview::openProjector(const QString& canvasId, bool preview, bool fullscreen,
                            const QString& screen) {
    if (!link_ || !link_->alive) {
        report(QStringLiteral("The renderer is unavailable."));
        return false;
    }
    auto key = resolve(canvasId);
    if (preview) {
        const auto deck = latest_.canvases.empty()
                              ? latest_.decks.end()
                              : std::ranges::find(latest_.decks, latest_.canvases.front().id,
                                                  &ldoc::Deck::canvas);
        if (deck == latest_.decks.end() || !deck->studio_mode) {
            report(QStringLiteral("Preview projectors require Studio Mode."));
            return false;
        }
        key = studioPreviewKey();
    }
    const auto id = ldoc::Id::parse(key.toStdString());
    if (!id || !link_->pending.canvas(*id) || screen.size() > 256) {
        report(QStringLiteral("Invalid projector canvas or monitor."));
        return false;
    }
    queueProjector(QJsonDocument(QJsonObject{{"id", int(next_projector_++)},
                                             {"canvas", key},
                                             {"preview", preview},
                                             {"fullscreen", fullscreen},
                                             {"screen", screen}})
                       .toJson(QJsonDocument::Compact));
    projector_poll_->start();
    return true;
}

void Preview::closeProjector(int id) {
    if (id > 0)
        queueProjector(QJsonDocument(QJsonObject{{"close", id}}).toJson(QJsonDocument::Compact));
}

void Preview::closeProjectors() {
    queueProjector(R"({"closeAll":true})");
}

void Preview::queueProjector(const QByteArray& request) {
    if (!link_ || !link_->alive || link_->projector_requests.size() >= 16) {
        report(QStringLiteral("Projector control unavailable or busy."));
        return;
    }
    link_->projector_requests.push_back(request.toStdString());
    projector_poll_->start(); // Also retries controls when the IPC socket is temporarily full.
    flushProjectors();
}

void Preview::flushProjectors() {
    if (!link_ || !link_->channel || link_->in_flight || link_->dirty)
        return;
    while (!link_->projector_requests.empty()) {
        const auto sent = link_->channel->send_projector(link_->projector_requests.front());
        if (!sent) {
            if (lcomp::send_would_block(sent.error()))
                return; // Next status tick/ack retries.
            report(QString::fromStdString(sent.error()));
        }
        link_->projector_requests.pop_front();
    }
}

QString Preview::studioPreviewKey() const {
    return QString::fromStdString(studio_canvas_.to_string());
}

void Preview::setManualTransition(double progress) {
    if (!std::isfinite(progress) || progress < -1 || progress > 1 || latest_.canvases.empty())
        return;
    if (progress < 0) {
        for (const auto& deck : latest_.decks)
            if (manual_targets_.contains(deck.id)) immediate_targets_[deck.id] = deck.program_scene;
        manual_targets_.clear();
        manual_progress_ = -1;
    } else {
        if (manual_progress_ < 0) {
            const auto main = std::ranges::find(latest_.decks, latest_.canvases.front().id,
                                                &ldoc::Deck::canvas);
            if (main == latest_.decks.end() || !main->studio_mode || !main->preview_scene ||
                main->preview_scene == main->program_scene ||
                (main->transition != "transition.cut" && main->transition != "transition.fade" &&
                 main->transition != "transition.fade-to-black")) return;
            manual_project_ = latest_.id;
            manual_targets_[main->id] = {main->program_scene, *main->preview_scene};
            for (const auto& link : latest_.scene_links) {
                if (link.from != *main->preview_scene) continue;
                const auto* scene = latest_.scene(link.to);
                const auto* canvas = scene ? latest_.canvas(scene->canvas) : nullptr;
                if (!scene || !canvas || canvas->emergency_scene == link.to) continue;
                if (scene->canvas == main->canvas) continue;
                const auto target = std::ranges::find(latest_.decks, scene->canvas, &ldoc::Deck::canvas);
                if (target != latest_.decks.end())
                    manual_targets_[target->id] = {target->program_scene, link.to};
                else
                    manual_targets_[ldoc::Id::generate()] = {std::nullopt, link.to};
            }
        }
        manual_progress_ = progress;
    }
    publish(latest_);
}

void Preview::publish(const ldoc::Project& project) {
    if (latest_.id != project.id && projector_poll_->isActive()) closeProjectors();
    if (latest_.id != project.id) transition_until_ = {};
    else for (const auto& deck : project.decks) {
        const auto previous = std::ranges::find(latest_.decks, deck.id, &ldoc::Deck::id);
        if (previous == latest_.decks.end() || previous->program_scene == deck.program_scene ||
            manual_targets_.contains(deck.id)) continue;
        std::int64_t duration = 0;
        if (deck.transition == "transition.fade" || deck.transition == "transition.fade-to-black")
            duration = deck.transition_ms;
        else if (deck.transition.starts_with("transition.stinger:")) {
            const auto id = ldoc::Id::parse(deck.transition.substr(19));
            if (const auto* source = id ? project.source(*id) : nullptr) {
                duration = lcomp::timer_number(source->settings, "stingerDurationMs", 1000);
            }
        }
        if (duration > 0)
            transition_until_ = std::max(transition_until_, std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::clamp<std::int64_t>(duration, 0, 600000) + 200));
    }
    latest_ = project;
    if (!project.canvases.empty()) setLive(program_live_, project.canvases.front().fps);
    if (!link_ || !link_->alive)
        return;
    link_->pending = project;
    const auto main = project.canvases.empty() ? project.decks.end() :
        std::ranges::find(project.decks, project.canvases.front().id, &ldoc::Deck::canvas);
    if (manual_project_ != project.id) {
        manual_targets_.clear();
        immediate_targets_.clear();
        manual_progress_ = -1;
        manual_project_ = project.id;
    }
    if (manual_progress_ >= 0 && (main == project.decks.end() || !main->studio_mode)) {
        for (const auto& deck : project.decks)
            if (manual_targets_.contains(deck.id)) immediate_targets_[deck.id] = deck.program_scene;
        manual_targets_.clear();
        manual_progress_ = -1;
    }
    if (manual_progress_ >= 0)
        for (const auto& [id, target] : manual_targets_) {
            const auto* scene = project.scene(target.second);
            if (!scene || std::ranges::any_of(link_->pending.decks,
                    [&](const auto& deck) { return deck.canvas == scene->canvas; })) continue;
            ldoc::Deck deck;
            deck.id = id;
            deck.canvas = scene->canvas;
            link_->pending.decks.push_back(std::move(deck));
        }
    for (auto& deck : link_->pending.decks) {
        if (const auto it = immediate_targets_.find(deck.id); it != immediate_targets_.end()) {
            if (it->second == deck.program_scene)
                link_->pending.settings.extra["ui.immediate." + deck.id.to_string()] = true;
            else immediate_targets_.erase(it);
        }
        if (manual_progress_ < 0) continue;
        const auto it = manual_targets_.find(deck.id);
        if (it == manual_targets_.end()) continue;
        const auto& [from, to] = it->second;
        if (deck.program_scene == from) {
            deck.preview_scene = from;
            deck.program_scene = to;
            link_->pending.settings.extra["ui.manual." + deck.id.to_string()] = manual_progress_;
        } else if (deck.program_scene == to) {
            link_->pending.settings.extra["ui.immediate." + deck.id.to_string()] = true;
        }
    }
    if (!project.canvases.empty()) {
        if (main != project.decks.end() && main->studio_mode) {
            auto canvas = project.canvases.front();
            canvas.id = studio_canvas_;
            canvas.name = "Main Preview";
            canvas.emergency_scene.reset();
            ldoc::Scene scene;
            scene.id = studio_scene_;
            scene.canvas = canvas.id;
            auto node = ldoc::make_node(ldoc::NodeKind::Precomp, "Main Preview");
            node.id = studio_node_;
            if (main->preview_scene) {
                std::get<ldoc::PrecompData>(node.data).scene = *main->preview_scene;
                scene.roots.push_back(std::move(node));
            }
            ldoc::Deck deck;
            deck.id = studio_deck_;
            deck.canvas = canvas.id;
            deck.program_scene = scene.id;
            link_->pending.settings.extra["ui.studio-preview-deck"] = deck.id.to_string();
            link_->pending.canvases.push_back(std::move(canvas));
            link_->pending.scenes.push_back(std::move(scene));
            link_->pending.decks.push_back(std::move(deck));
        }
    }
    // The one place a prepared path is substituted. Done on the copy that goes to the renderer
    // and nowhere else, so the saved project keeps the operator's own paths, the two worker
    // processes need no new path logic -- they receive the prepared path as if it had been
    // typed in -- and turning caching off is a flag rather than a migration.
    if (const auto* cache = AssetCache::instance(); cache && cache->enabled())
        for (ldoc::Source& source : link_->pending.sources)
            substitute_source_paths(source, *cache);
    // A countdown's text is deliberately *not* written here. The source worker computes it on
    // its own clock (munkicast-source/main.cpp, draw_text), and writing it onto this copy as
    // well would change `settings` on every tick -- which the worker reads as a reconfigure,
    // blanking the frame and rebuilding the source once a second instead of letting it count.
    // The colour rules are the one thing the worker cannot derive, so they travel as settings
    // and are resolved there too.
    if (const auto owner = ldoc::Id::parse(filter_target_.toStdString());
        owner && !project.canvases.empty()) {
        auto canvas = project.canvases.front();
        canvas.id = filter_canvas_;
        canvas.name = "Filter preview";
        canvas.emergency_scene.reset();
        ldoc::Scene scene;
        scene.id = filter_scene_;
        scene.name = "Filter preview";
        scene.canvas = canvas.id;
        if (const auto* source = project.source(*owner)) {
            auto node = ldoc::make_node(ldoc::NodeKind::Source, source->name);
            node.id = filter_node_;
            std::get<ldoc::SourceData>(node.data).source = source->id;
            node.layout.fit = ldoc::FitMode::Contain;
            node.layout.size = {double(canvas.width), double(canvas.height)};
            scene.roots.push_back(node);
        } else if (const auto* selected = project.scene(*owner)) {
            if (const auto* original = project.canvas(selected->canvas)) {
                canvas.width = original->width;
                canvas.height = original->height;
            }
            auto node = ldoc::make_node(ldoc::NodeKind::Precomp, selected->name);
            node.id = filter_node_;
            auto& data = std::get<ldoc::PrecompData>(node.data);
            data.scene = selected->id;
            data.size = {double(canvas.width), double(canvas.height)};
            scene.roots.push_back(node);
        } else {
            for (const auto& existing : project.scenes)
                if (const auto* node = ldoc::find_node(existing.roots, *owner)) {
                    scene.roots = {*node};
                    scene.roots[0].visible = true;
                    scene.roots[0].transform = {};
                    const auto remap = [&](ldoc::Id& id) {
                        const auto [it, inserted] =
                            filter_ids_.try_emplace(id, ldoc::Id::generate());
                        (void)inserted;
                        id = it->second;
                    };
                    const auto clone = [&](auto&& self, ldoc::Node& item) -> void {
                        remap(item.id);
                        for (auto& effect : item.effects)
                            remap(effect.id);
                        if (auto* group = std::get_if<ldoc::GroupData>(&item.data))
                            for (auto& child : group->children)
                                self(self, child);
                    };
                    clone(clone, scene.roots[0]);
                    break;
                }
        }
        ldoc::Deck deck;
        deck.id = filter_deck_;
        deck.name = "Filter preview";
        deck.canvas = canvas.id;
        deck.program_scene = scene.id;
        link_->pending.settings.extra["ui.filter-preview-deck"] = deck.id.to_string();
        link_->pending.canvases.push_back(canvas);
        link_->pending.scenes.push_back(scene);
        link_->pending.decks.push_back(deck);
    }
    link_->refresh_only = false;
    link_->have_pending = true;
    if (!project.canvases.empty())
        first_canvas_ = QString::fromStdString(project.canvases.front().id.to_string());
    if (link_->in_flight) {
        link_->dirty = true;
        return;
    }
    try_send();
}

std::string Preview::publishedSourceSetting(const QString& sourceId,
                                            std::string_view key) const {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    if (!id || !link_)
        return {};
    const auto* source = link_->pending.source(*id);
    if (!source)
        return {};
    const auto it = source->settings.find(key);
    if (it == source->settings.end())
        return {};
    const auto* text = std::get_if<std::string>(&it->second);
    return text ? *text : std::string{};
}

QString Preview::setFilterTarget(const QString& owner) {
    if (filter_target_ != owner) {
        filter_target_ = owner;
        filter_ids_.clear();
        {
            std::lock_guard lock(state_->mu);
            state_->frames.erase(QString::fromStdString(filter_canvas_.to_string()));
        }
        setLive(program_live_,
                latest_.canvases.empty() ? ldoc::Fps{} : latest_.canvases.front().fps);
        publish(latest_);
    }
    return owner.isEmpty() ? QString{} : QString::fromStdString(filter_canvas_.to_string());
}

/// Posted, not emitted directly. `start` runs while the singleton is being created, which is
/// before main has connected `notice`, and a direct emit there would be a failure nobody heard.
void Preview::report(const QString& text) {
    QMetaObject::invokeMethod(this, [this, text] { emit notice(text); }, Qt::QueuedConnection);
}

void Preview::start() {
    if (renderer_.empty()) {
        report(QStringLiteral("No render process is configured."));
        return;
    }
    auto ends = lipc::Channel::pair();
    if (!ends) {
        report(QString::fromStdString(ends.error()));
        return;
    }
    auto pid = lcomp::spawn_renderer(renderer_, std::move(ends->second).take_socket());
    if (!pid) {
        report(QString::fromStdString(pid.error()));
        return;
    }
    link_->child = *pid;
    link_->channel.emplace(std::move(ends->first));
    // Before the notifier exists, so it cannot take the hello this call is waiting for.
    const auto hello = link_->channel->take_hello(2000);
    if (!hello) {
        lose(QString::fromStdString(hello.error()));
        return;
    }
    link_->alive = true;
    link_->write = new QSocketNotifier(link_->channel->poll_fd(), QSocketNotifier::Write, this);
    link_->write->setEnabled(false);
    QObject::connect(link_->write, &QSocketNotifier::activated, this, [this] { on_writable(); });
    // Receive, decode and copy pixels here, off the UI thread: a 4K frame copy and its QImage
    // used to sit between the UI's own paints. Events are posted back in order. `link` guards
    // against delivery after lose()/restart() replaced the link.
    Link* link = link_.get();
    const auto id = link->id;
    link_->reader = std::jthread([this, link, id](std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto event = link->channel->receive(50);
            if (event && !event->has_value())
                continue;
            auto item = std::make_shared<Received>();
            const bool last = !event || (**event).kind == lcomp::PreviewEvent::Kind::PeerGone;
            if (!event) {
                item->error = QString::fromStdString(event.error());
            } else {
                item->event = std::move(**event);
                if (item->event.kind == lcomp::PreviewEvent::Kind::Frame)
                    item->image = image_from(item->event.frame);
            }
            QMetaObject::invokeMethod(this, [this, id, item] {
                if (link_ && link_->id == id) on_received(*item);
            }, Qt::QueuedConnection);
            if (last) return;
        }
    });
}

void Preview::stop_reader() {
    if (link_ && link_->reader.joinable()) {
        link_->reader.request_stop();
        link_->reader.join();
    }
}

void Preview::try_send() {
    if (!link_ || !link_->alive || !link_->channel || !link_->have_pending || link_->in_flight)
        return;
    link_->incoming.clear();
    link_->failed = false;
    const auto sent = link_->refresh_only
                          ? link_->channel->refresh(preview_edge_)
                          : link_->channel->send_project(link_->pending, preview_edge_);
    if (!sent) {
        if (lcomp::send_would_block(sent.error())) {
            link_->write->setEnabled(true);
            return;
        }
        lose(QString::fromStdString(sent.error()));
        return;
    }
    if (link_->write != nullptr)
        link_->write->setEnabled(false);
    link_->refresh_pending = false;
    link_->in_flight = true;
    link_->dirty = false;
    link_->sent.clear();
    link_->sent.reserve(link_->pending.canvases.size());
    for (const ldoc::Canvas& canvas : link_->pending.canvases)
        link_->sent.push_back(QString::fromStdString(canvas.id.to_string()));
}

void Preview::on_received(Received& item) {
    if (!link_ || !link_->channel)
        return;
    if (!item.error.isEmpty()) {
        lose(item.error);
        return;
    }
    {
        lcomp::PreviewEvent& got = item.event;
        switch (got.kind) {
        case lcomp::PreviewEvent::Kind::Projector: {
            const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(got.projector));
            if (!document.isObject()) {
                lose(QStringLiteral("Invalid projector status."));
                return;
            }
            const auto status = document.object();
            projectors_ = status.value("windows").toArray().toVariantList();
            QStringList errors;
            const auto error = status.value("error").toString();
            if (!error.isEmpty())
                errors.append(error);
            bool active = false;
            for (const auto& entry : projectors_) {
                const auto row = entry.toMap();
                active = active || row.value("active").toBool();
                if (!row.value("error").toString().isEmpty())
                    errors.append(row.value("error").toString());
            }
            const auto text = errors.join(QLatin1Char('\n'));
            if (!text.isEmpty() && text != projector_error_)
                report(text);
            projector_error_ = text;
            if (active && !projector_poll_->isActive())
                projector_poll_->start();
            else if (!active && link_->projector_requests.empty())
                projector_poll_->stop();
            emit projectorsChanged();
            break;
        }
        case lcomp::PreviewEvent::Kind::Capture:
            capture_stats_ = std::move(got.capture);
            emit changed();
            break;
        case lcomp::PreviewEvent::Kind::Pcm:
            lose(QStringLiteral("Unexpected PCM on the UI channel."));
            return;
        case lcomp::PreviewEvent::Kind::Audio:
            audio_levels_.clear();
            for (const auto& level : got.audio)
                audio_levels_[QString::fromStdString(level.node.to_string())] =
                    {{"available", level.available}, {"left", level.left}, {"right", level.right}};
            emit changed();
            break;
        case lcomp::PreviewEvent::Kind::Hello:
            break;
        case lcomp::PreviewEvent::Kind::Record: {
            // Said by the renderer, not assumed here: the encoder opens over there and may
            // refuse, and a UI that latched its own button would claim a recording nobody has.
            if (!got.record.error.empty())
                report(QString::fromStdString(got.record.error));
            const bool state_changed = recording_ != got.record.recording ||
                streaming_ != got.record.streaming || replay_ != got.record.replay ||
                replay_saving_ != got.record.replay_saving || !got.record.clip_path.empty() ||
                (recording_pending_ && got.record.control_reply);
            if (got.record.frames < base_output_frames_ ||
                got.record.missed < base_output_missed_ || got.record.encode_lag < base_output_lag_)
                base_output_frames_ = base_output_missed_ = base_output_lag_ = 0;
            output_stats_available_ = got.record.running || got.record.frames > 0;
            output_frames_ = got.record.frames;
            output_missed_ = got.record.missed;
            output_lag_ = got.record.encode_lag;
            if (got.record.control_reply) recording_pending_ = false;
            const bool was_recording = recording_, was_streaming = streaming_;
            const bool was_replay_saving = replay_saving_;
            recording_ = got.record.recording;
            streaming_ = got.record.streaming;
            replay_ = got.record.replay;
            replay_saving_ = got.record.replay_saving;
            replay_bytes_ = got.record.replay_bytes;
            replay_duration_ms_ = got.record.replay_duration_ms;
            if (!got.record.clip_path.empty()) {
                const auto path = QString::fromStdString(got.record.clip_path);
                if (got.record.error.empty()) {
                    replay_path_ = path;
                    report(QStringLiteral("Replay saved to %1").arg(path));
                }
                emit replaySaved(path, QString::fromStdString(got.record.error));
            } else if (was_replay_saving && !replay_saving_ && !replay_) {
                emit replaySaved({}, got.record.error.empty()
                    ? QStringLiteral("The encoder stopped while saving replay.")
                    : QString::fromStdString(got.record.error));
            }
            if (recording_ && !was_recording)
                report(QStringLiteral("Recording to %1").arg(recording_path_));
            if (streaming_ && !was_streaming)
                report(QStringLiteral("Live: connected to the RTMP server."));
            if (!recording_ && was_recording && got.record.error.empty())
                report(QStringLiteral("Recording saved to %1").arg(recording_path_));
            if (!streaming_ && was_streaming && got.record.error.empty())
                report(QStringLiteral("Stream stopped."));
            emit changed();
            if (!state_changed && !replay_) break;
            emit recordingChanged();
            QTimer::singleShot(0, this, &Preview::syncReplay);
            setLive(program_live_, latest_.canvases.empty() ? ldoc::Fps{}
                                                          : latest_.canvases.front().fps);
            break;
        }
        case lcomp::PreviewEvent::Kind::Frame: {
            Held held;
            held.status = got.frame.status;
            held.width = got.frame.width;
            held.height = got.frame.height;
            held.gaps = got.frame.gaps;
            held.render_ns = got.frame.render_ns;
            held.rendered_frames = got.frame.rendered_frames;
            held.render_lag_frames = got.frame.render_lag_frames;
            held.fps_num = got.frame.fps_num;
            held.fps_den = got.frame.fps_den;
            held.image = std::move(item.image);
            link_->incoming.insert_or_assign(QString::fromStdString(got.frame.canvas.to_string()),
                                             std::move(held));
            break;
        }
        case lcomp::PreviewEvent::Kind::Fault:
            link_->failed = true;
            link_->incoming.clear();
            report(QString::fromStdString(got.fault));
            break;
        case lcomp::PreviewEvent::Kind::Ack: {
            link_->in_flight = false;
            const bool failed = link_->failed;
            link_->failed = false;
            if (!failed && !link_->incoming.empty()) {
                {
                    std::lock_guard lock(state_->mu);
                    const auto now = std::chrono::steady_clock::now();
                    std::erase_if(state_->recent, [&](const auto& entry) {
                        return !link_->incoming.contains(entry.first);
                    });
                    for (const auto& [key, held] : link_->incoming) {
                        const auto old = state_->frames.find(key);
                        const auto frames =
                            old == state_->frames.end() ? 0 : old->second.rendered_frames;
                        const auto ns = old == state_->frames.end() ? 0 : old->second.render_ns;
                        auto& samples = state_->recent[key];
                        if (held.rendered_frames < frames || held.render_ns < ns)
                            samples.clear();
                        else if (held.rendered_frames > frames)
                            samples.push_back(
                                {now, held.render_ns - ns, held.rendered_frames - frames});
                        while (!samples.empty() &&
                               samples.front().at <= now - std::chrono::milliseconds(500))
                            samples.pop_front();
                    }
                    state_->frames.clear();
                    for (auto& [key, held] : link_->incoming)
                        state_->frames.emplace(key, std::move(held));
                    ++state_->generation;
                }
                link_->incoming.clear();
                emit changed();
            } else {
                link_->incoming.clear();
            }
            if (!link_->dirty)
                flushProjectors();
            if (link_->dirty)
                try_send();
            else if (link_->record_request) {
                const auto request = std::exchange(link_->record_request, std::nullopt);
                if (const auto sent = link_->channel->send_record(*request); !sent) {
                    recording_pending_ = false;
                    emit recordingChanged();
                    report(QString::fromStdString(sent.error()));
                }
            } else if (!recording_pending_) {
                syncReplay();
                if (link_->refresh_pending && !recording_pending_) {
                    link_->refresh_only = true;
                    try_send();
                }
            } else if (link_->refresh_pending) {
                link_->refresh_only = true;
                try_send();
            }
            break;
        }
        case lcomp::PreviewEvent::Kind::PeerGone:
            lose(QStringLiteral("The render process is gone."));
            return;
        }
    }
}

QVariantMap Preview::audioLevel(const QString& nodeId) const {
    const auto found = audio_levels_.find(nodeId);
    return rendererAttached() && found != audio_levels_.end() ? found->second : QVariantMap{};
}

void Preview::on_writable() {
    if (link_ && link_->write != nullptr)
        link_->write->setEnabled(false);
    try_send();
}

void Preview::startRecording(const QString& encodeId, const QString& path) {
    if (recording_ || recording_pending_)
        return;
    if (path.isEmpty()) {
        report(QStringLiteral("A recording needs somewhere to write."));
        return;
    }
    streaming_mode_ = false;
    recording_path_ = path;
    const auto id = ldoc::Id::parse(encodeId.toStdString());
    const auto* encode = id ? latest_.encode(*id) : nullptr;
    recording_encode_ = id.value_or(ldoc::Id::nil());
    const auto* canvas = encode ? latest_.canvas(encode->canvas) : nullptr;
    recording_rate_ = canvas ? canvas->fps : ldoc::Fps{};
    send_record(encodeId, path, true);
}

void Preview::startStreaming(const QString& outputId) {
    if (streaming_ || recording_pending_)
        return;
    const auto id = ldoc::Id::parse(outputId.toStdString());
    const auto found =
        std::ranges::find_if(latest_.outputs, [&](const auto& o) { return id && o.id == *id; });
    if (found == latest_.outputs.end() || !found->enabled) {
        report(QStringLiteral("Choose an armed RTMP destination in Encode."));
        return;
    }
    const auto output = *found;
    const auto* video = latest_.encode(output.encode);
    const auto* canvas = video ? latest_.canvas(video->canvas) : nullptr;
    const auto* audio = output.audio_encode ? latest_.audio_encode(*output.audio_encode) : nullptr;
    const QString base = QString::fromStdString(output.destination.url);
    const QUrl url(base, QUrl::StrictMode);
    if (!canvas || !audio || !latest_.bus(audio->bus) || video->encoder.codec != "h264" ||
        audio->codec != "aac" ||
        (output.destination.kind != ldoc::DestinationKind::Rtmp &&
         output.destination.kind != ldoc::DestinationKind::Rtmps) ||
        !url.isValid() || url.scheme().toStdString() != ldoc::to_string(output.destination.kind) ||
        url.host().isEmpty() || url.path().isEmpty() || url.hasFragment() ||
        !url.userInfo().isEmpty() ||
        std::ranges::any_of(base, [](QChar c) { return c.unicode() <= 32; })) {
        report(
            QStringLiteral("RTMP needs a valid server URL, H.264 video and connected AAC audio."));
        return;
    }
    streaming_mode_ = true;
    streaming_output_ = output.id; // Stream keys never become UI properties or notices.
    recording_rate_ = canvas->fps;
    const auto start = [this, output, base](const QString& key) {
        QString address = base;
        if (!key.isEmpty()) {
            const auto query = address.indexOf('?');
            const auto suffix = query < 0 ? QString{} : address.mid(query + 1);
            if (query >= 0)
                address.truncate(query);
            if (!address.endsWith('/'))
                address += '/';
            address += key;
            if (!suffix.isEmpty())
                address += (key.contains('?') ? '&' : '?') + suffix;
        }
        send_record(QString::fromStdString(output.encode.to_string()), address, true, output.id);
    };
    if (output.destination.credential_ref.empty()) {
        start({}); // Local ingest servers may accept an anonymous publish URL.
        return;
    }
    const QString helper = QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
    if (helper.isEmpty()) {
        report(QStringLiteral("Install secret-tool to read the saved stream key."));
        return;
    }
    recording_pending_ = true;
    emit recordingChanged();
    auto* process = new QProcess(this);
    key_lookup_ = process;
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            key_lookup_ = nullptr;
            recording_pending_ = false;
            emit recordingChanged();
            report(QStringLiteral("Could not open the stream key store."));
            process->deleteLater();
        }
    });
    const auto project = latest_.id;
    connect(
        process, &QProcess::finished, this,
        [this, process, timeout, start, project, output](int code, QProcess::ExitStatus status) {
            timeout->stop();
            key_lookup_ = nullptr;
            recording_pending_ = false;
            QByteArray secret = process->readAllStandardOutput();
            if (secret.endsWith('\n'))
                secret.chop(1);
            const auto current = std::ranges::find(latest_.outputs, output.id, &ldoc::Output::id);
            if (status != QProcess::NormalExit || code != 0 || secret.isEmpty() ||
                secret.size() > 16384 ||
                std::ranges::any_of(secret, [](unsigned char c) { return c <= 32 || c == 127; })) {
                report(QStringLiteral("Could not read a valid stream key. Unlock the keyring or "
                                      "save the key again."));
            } else if (latest_.id != project || current == latest_.outputs.end() ||
                       *current != output || !link_ || !link_->alive) {
                report(QStringLiteral("Stream settings changed. Press Go Live again."));
            } else {
                start(QString::fromUtf8(secret));
            }
            secret.fill('\0');
            emit recordingChanged();
            process->deleteLater();
        });
    process->start(helper, {QStringLiteral("lookup"), QStringLiteral("application"),
                            QStringLiteral("munkicast"), QStringLiteral("credential"),
                            QString::fromStdString(output.destination.credential_ref)});
    timeout->start(10000);
}

void Preview::stopStreaming() {
    if (streaming_ && !recording_pending_) {
        streaming_mode_ = true;
        send_record({}, {}, false, streaming_output_);
    }
}

void Preview::stopRecording(const QString& encodeId) {
    if (recording_pending_) return;
    streaming_mode_ = false;
    send_record(recording_encode_.is_nil() ? encodeId
        : QString::fromStdString(recording_encode_.to_string()), {}, false);
}

void Preview::syncReplay() {
    if (!rendererAttached() || recording_pending_) return;
    const auto output = std::find_if(latest_.outputs.begin(), latest_.outputs.end(),
        [](const auto& o) { return o.destination.kind == ldoc::DestinationKind::Replay; });
    const bool enabled = output != latest_.outputs.end() && output->enabled;
    if (!enabled && replay_ && !replay_saving_) stopReplay();
}

void Preview::startReplay() {
    if (replay_ || recording_pending_) return;
    const auto output = std::find_if(latest_.outputs.begin(), latest_.outputs.end(),
        [](const auto& o) { return o.destination.kind == ldoc::DestinationKind::Replay && o.enabled; });
    const auto* encode = output == latest_.outputs.end() ? nullptr : latest_.encode(output->encode);
    const auto* canvas = encode ? latest_.canvas(encode->canvas) : nullptr;
    if (!canvas) { report(QStringLiteral("Enable and connect the Replay destination first.")); return; }
    streaming_mode_ = false;
    recording_rate_ = canvas->fps;
    send_record(QString::fromStdString(encode->id.to_string()), {}, true, output->id,
                lcomp::RecordOperation::ReplayStart);
}

void Preview::stopReplay() {
    if (!replay_ || recording_pending_ || replay_saving_) return;
    streaming_mode_ = false;
    send_record({}, {}, false, {}, lcomp::RecordOperation::ReplayStop);
}

void Preview::saveReplay(const QString& path) {
    if (!replay_ || replay_saving_ || recording_pending_ || path.isEmpty()) {
        const auto error = QStringLiteral("Replay is unavailable, still warming up, or already saving.");
        report(error);
        emit replaySaved(path, error);
        return;
    }
    replay_saving_ = true;
    streaming_mode_ = false;
    send_record({}, path, false, {}, lcomp::RecordOperation::ReplaySave);
    if (!recording_pending_) {
        replay_saving_ = false;
        emit replaySaved(path, QStringLiteral("Could not send the replay save request to the renderer."));
    }
    emit recordingChanged();
}

void Preview::send_record(const QString& encodeId, const QString& path, bool start,
                          const ldoc::Id& output, lcomp::RecordOperation operation) {
    if (!link_ || !link_->channel || !link_->alive) {
        report(QStringLiteral("The renderer is not running, so there is nothing to record."));
        return;
    }
    const auto id = ldoc::Id::parse(encodeId.toStdString());
    if (start && !id) {
        report(QStringLiteral("That is not an encode."));
        return;
    }
    lcomp::RecordRequest request;
    request.start = start;
    // Stop must still work after the recording node or its encoder has been removed.
    request.encode = id.value_or(ldoc::Id::nil());
    request.path = path.toStdString();
    request.output = output;
    request.operation = operation;
    if (recording_pending_)
        return;
    // Start after the renderer has acknowledged the latest encoder and routing settings.
    if (start && (link_->in_flight || link_->dirty)) {
        link_->record_request = request;
        recording_pending_ = true;
        emit recordingChanged();
        return;
    }
    if (const auto sent = link_->channel->send_record(request); !sent) {
        report(QString::fromStdString(sent.error()));
    } else {
        recording_pending_ = true;
        emit recordingChanged();
    }
}

void Preview::lose(const QString& why) {
    capture_stats_.clear();
    // Already torn down. Hello can fail before `alive` is set, and that path still has a child
    // to reap, so "not alive" on its own is not "already gone".
    if (!link_)
        return;
    if (!link_->alive && link_->child <= 0 && !link_->channel)
        return;
    if (key_lookup_) {
        key_lookup_->disconnect(this);
        key_lookup_->kill();
        key_lookup_->deleteLater();
        key_lookup_ = nullptr;
    }
    link_->record_request.reset();
    link_->projector_requests.clear();
    projector_poll_->stop();
    projectors_.clear();
    emit projectorsChanged();
    link_->alive = false;
    link_->in_flight = false;
    link_->have_pending = false;
    link_->dirty = false;
    // A renderer that died took the file with it. Saying otherwise would leave a Record
    // button latched over a recording that stopped some time ago.
    if (recording_ || streaming_ || replay_ || recording_pending_) {
        if (replay_saving_) emit replaySaved({}, QStringLiteral("The renderer disconnected while saving replay."));
        replay_ = replay_saving_ = false;
        replay_bytes_ = replay_duration_ms_ = 0;
        recording_ = false;
        streaming_ = false;
        recording_pending_ = false;
        emit recordingChanged();
    }
    if (link_->write != nullptr)
        link_->write->setEnabled(false);
    stop_reader();
    if (link_->channel) {
        link_->channel->close();
        link_->channel.reset();
    }
    {
        std::lock_guard lock(state_->mu);
        if (!state_->frames.empty()) {
            state_->frames.clear();
            ++state_->generation;
        }
    }
    reap(false);
    emit changed();
    report(why);
}

void Preview::restart() {
    stop_reader();
    if (link_ && link_->channel) {
        link_->channel->quit();
        link_->channel->close();
        link_->channel.reset();
    }
    lose(QStringLiteral("Render process restarted for new preferences."));
    reap(true);
    link_ = std::make_unique<Link>();
    start();
    publish(latest_);
}

void Preview::reap(bool patient) {
    if (!link_ || link_->child <= 0)
        return;
    const pid_t child = link_->child;
    link_->child = -1;
    const auto wait_for = [child](int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            int status = 0;
            const pid_t got = ::waitpid(child, &status, WNOHANG);
            if (got != 0)
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    if (!wait_for(patient ? 500 : 50)) {
        ::kill(child, SIGTERM);
        if (!wait_for(200)) {
            ::kill(child, SIGKILL);
            int status = 0;
            ::waitpid(child, &status, 0);
        }
    }
}

} // namespace munkicast::ui

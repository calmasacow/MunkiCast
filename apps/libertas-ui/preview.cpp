// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preview.hpp"

#include "assetcache.hpp"

#include "lcomp/countdown.hpp"
#include "lcomp/preview.hpp"

#include <QDateTime>
#include <QImage>
#include <QQuickImageProvider>
#include <QSocketNotifier>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>

namespace libertas::ui {
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
    QSocketNotifier* read = nullptr;
    QSocketNotifier* write = nullptr;
    bool alive = false;
    bool in_flight = false;
    bool dirty = false;
    bool have_pending = false;
    bool refresh_only = false;
    bool refresh_pending = false;
    std::optional<lcomp::RecordRequest> record_request;
    /// Set when this commit was a fault, so its ack does not publish a picture of the last one.
    bool failed = false;
    ldoc::Project pending;
    std::vector<QString> sent;
    std::map<QString, Held> incoming;
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
      link_(std::make_unique<Link>()) {}

Preview::~Preview() {
    if (!link_)
        return;
    if (link_->read != nullptr)
        link_->read->setEnabled(false);
    if (link_->write != nullptr)
        link_->write->setEnabled(false);
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

void Preview::resetStats() {
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
    live = live || recording_ || !filter_target_.isEmpty();
    if (recording_ && recording_rate_.valid() && (!rate.valid() || recording_rate_.hz() > rate.hz()))
        rate = recording_rate_;
    if (!live) {
        if (link_) link_->refresh_pending = false;
        if (live_ != nullptr)
            live_->stop();
        return;
    }
    if (live_ == nullptr) {
        live_ = new QTimer(this);
        live_->setTimerType(Qt::PreciseTimer);
        QObject::connect(live_, &QTimer::timeout, this, [this] { tick(); });
    }
    // Rounded to whole milliseconds because QTimer has nothing finer, which is a rounding of
    // when a *preview* is asked to redraw and not of the rate itself: the canvas keeps the exact
    // rational, and the encode path that cares about drift reads that rather than this timer.
    const double hz = rate.valid() ? rate.hz() : 60.0;
    const int interval = hz > 0.0 ? std::max(1, static_cast<int>(1000.0 / hz + 0.5)) : 16;
    if (!live_->isActive() || live_->interval() != interval)
        live_->start(interval);
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
    preview_sizes_[owner] = std::clamp(edge, 1, lcomp::kMaxPreviewEdge);
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

void Preview::publish(const ldoc::Project& project) {
    latest_ = project;
    if (!link_ || !link_->alive)
        return;
    link_->pending = project;
    // The one place a prepared path is substituted. Done on the copy that goes to the renderer
    // and nowhere else, so the saved project keeps the operator's own paths, the two worker
    // processes need no new path logic -- they receive the prepared path as if it had been
    // typed in -- and turning caching off is a flag rather than a migration.
    if (const auto* cache = AssetCache::instance(); cache && cache->enabled())
        for (ldoc::Source& source : link_->pending.sources)
            substitute_source_paths(source, *cache);
    // A countdown's text is deliberately *not* written here. The source worker computes it on
    // its own clock (libertas-source/main.cpp, draw_text), and writing it onto this copy as
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
    link_->read = new QSocketNotifier(link_->channel->poll_fd(), QSocketNotifier::Read, this);
    link_->write = new QSocketNotifier(link_->channel->poll_fd(), QSocketNotifier::Write, this);
    link_->write->setEnabled(false);
    QObject::connect(link_->read, &QSocketNotifier::activated, this, [this] { on_readable(); });
    QObject::connect(link_->write, &QSocketNotifier::activated, this, [this] { on_writable(); });
    // A frame can already be queued by the time the notifier is created. The notifier is
    // level-triggered, and this still drains whatever arrived between hello and the first poll.
    QTimer::singleShot(0, this, [this] { on_readable(); });
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

void Preview::on_readable() {
    if (!link_ || !link_->channel)
        return;
    for (;;) {
        auto event = link_->channel->poll();
        if (!event) {
            lose(QString::fromStdString(event.error()));
            return;
        }
        if (!event->has_value())
            return;
        lcomp::PreviewEvent& got = **event;
        switch (got.kind) {
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
            recording_pending_ = false;
            const bool was_recording = recording_;
            recording_ = got.record.running;
            if (recording_ && !was_recording)
                report(QStringLiteral("Recording to %1").arg(recording_path_));
            if (!recording_ && was_recording && got.record.error.empty())
                report(QStringLiteral("Recording saved to %1").arg(recording_path_));
            emit recordingChanged();
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
            held.image = image_from(got.frame);
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
            if (link_->dirty)
                try_send();
            else if (link_->record_request) {
                const auto request = std::exchange(link_->record_request, std::nullopt);
                if (const auto sent = link_->channel->send_record(*request); !sent) {
                    recording_pending_ = false;
                    emit recordingChanged();
                    report(QString::fromStdString(sent.error()));
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
    recording_path_ = path;
    const auto id = ldoc::Id::parse(encodeId.toStdString());
    const auto* encode = id ? latest_.encode(*id) : nullptr;
    const auto* canvas = encode ? latest_.canvas(encode->canvas) : nullptr;
    recording_rate_ = canvas ? canvas->fps : ldoc::Fps{};
    send_record(encodeId, path, true);
}

void Preview::stopRecording(const QString& encodeId) {
    send_record(encodeId, {}, false);
}

void Preview::send_record(const QString& encodeId, const QString& path, bool start) {
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
    // Already torn down. Hello can fail before `alive` is set, and that path still has a child
    // to reap, so "not alive" on its own is not "already gone".
    if (!link_)
        return;
    if (!link_->alive && link_->child <= 0 && !link_->channel)
        return;
    link_->alive = false;
    link_->in_flight = false;
    link_->have_pending = false;
    link_->dirty = false;
    // A renderer that died took the file with it. Saying otherwise would leave a Record
    // button latched over a recording that stopped some time ago.
    if (recording_ || recording_pending_) {
        recording_ = false;
        recording_pending_ = false;
        emit recordingChanged();
    }
    if (link_->read != nullptr)
        link_->read->setEnabled(false);
    if (link_->write != nullptr)
        link_->write->setEnabled(false);
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

} // namespace libertas::ui

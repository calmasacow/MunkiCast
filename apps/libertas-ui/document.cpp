// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "document.hpp"

#include "assetcache.hpp"

#include "lcomp/composite.hpp"
#include <lcomp/countdown.hpp>
#include "ldoc/command.hpp"

#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QFontDatabase>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QStandardPaths>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <lcapture/audio.hpp>
#include <lcapture/screen.hpp>
#include <lcapture/video.hpp>
#include <lcomp/filters.hpp>
#include <lcore/log.hpp>
#include <set>
#include <span>

namespace libertas::ui {
namespace {

std::filesystem::path g_project_dir;

QString qstr(const std::string& s) { return QString::fromStdString(s); }

std::string to_std(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string{b.constData(), static_cast<std::size_t>(b.size())};
}

QString id_str(const ldoc::Id& id) { return QString::fromStdString(id.to_string()); }

std::optional<ldoc::Id> parse_id(const QString& s) {
    return ldoc::Id::parse(to_std(s));
}

/// A node's kind as a string for QML. Since the format bump this is simply the format's own
/// word: `to_string` says "source" where it used to say "layer" (ADR-0028).
QString kind_str(ldoc::NodeKind k) {
    const auto s = ldoc::to_string(k);
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString node_display_name(const ldoc::Project& project, const ldoc::Node& node) {
    if (const auto* placed = std::get_if<ldoc::SourceData>(&node.data))
        if (placed->source)
            if (const auto* source = project.source(*placed->source))
                return qstr(source->name);
    return qstr(node.name);
}

} // namespace

ldoc::Project Document::studioProject(std::string name) {
    auto p = ldoc::make_default_project(std::move(name));
    auto encode = Document::defaultEncode(p.canvases.front());
    encode.name = "Main Output";
    ldoc::Output destination;
    destination.id = ldoc::Id::generate();
    destination.name = "Main";
    destination.encode = encode.id;
    p.encodes.push_back(std::move(encode));
    p.outputs.push_back(std::move(destination));
    return p;
}

void Document::configure(std::filesystem::path project_dir) {
    g_project_dir = std::move(project_dir);
}

Document* Document::create(QQmlEngine*, QJSEngine*) { return new Document; }

Document::Document(QObject* parent) : QObject(parent) {
    if (g_project_dir.empty()) {
        session_ = lsession::LocalSession::in_memory(studioProject("Untitled"));
    } else {
        std::error_code ec;
        auto opened = std::filesystem::exists(g_project_dir / "project.json", ec)
                          ? lsession::LocalSession::open(g_project_dir)
                          : lsession::LocalSession::create(
                                studioProject(
                                    g_project_dir.stem().string().empty()
                                        ? "Untitled"
                                        : g_project_dir.stem().string()),
                                g_project_dir);
        if (opened) {
            session_ = std::move(*opened);
        } else {
            // Falling back to an unsaved document rather than refusing to start: the operator
            // can still work, and a shell that will not open because of one project is a shell
            // they cannot use to fix it.
            session_ = lsession::LocalSession::in_memory(studioProject("Untitled"));
            QMetaObject::invokeMethod(
                this,
                [this, why = qstr(opened.error())] {
                    emit notice(QStringLiteral("Could not open the project: %1").arg(why));
                },
                Qt::QueuedConnection);
        }
    }

    session_->set_notice_handler([this](ldoc::Severity, const std::string& text) {
        emit notice(qstr(text));
    });
    session_->subscribe([this](const lsession::Change&) { rebuild(); });

    // Saving is debounced rather than on every command: a rename is one keystroke at a time, and
    // an fsync per keystroke is both slow and pointless when the journal already has it durably.
    autosave_.setSingleShot(true);
    autosave_.setInterval(2000);
    connect(&autosave_, &QTimer::timeout, this, [this] {
        if (session_->dirty() && !session_->path().empty())
            save();
    });

    countdown_actions_.setInterval(100);
    connect(&countdown_actions_, &QTimer::timeout, this, [this] {
        QStringList ended;
        for (const auto& source : session_->project().sources)
            if (source.type == "source.text" &&
                lcomp::timer_flag(source.settings, "countdown.enabled") &&
                lcomp::timer_flag(source.settings, "countdown.running") &&
                lcomp::countdown(source.settings, QDateTime::currentMSecsSinceEpoch()).ended)
                ended << id_str(source.id);
        for (const auto& id : ended) controlCountdown(id, "finish");
    });
    countdown_actions_.start();

    // Which scene each canvas starts on is settled by rebuild(), which does it per canvas and
    // has to anyway -- a canvas can gain its first scene at any time.
    rebuild();
    initCollections();
}

Document::~Document() = default;

// ---- reading --------------------------------------------------------------------------------

QString Document::projectName() const { return qstr(session_->project().name); }

QString Document::projectPath() const {
    return session_->path().empty() ? QStringLiteral("(unsaved)")
                                    : qstr(session_->path().string());
}

bool Document::dirty() const { return session_->dirty(); }
int Document::revision() const { return static_cast<int>(session_->revision()); }
bool Document::canUndo() const { return session_->can_undo(); }
bool Document::canRedo() const { return session_->can_redo(); }
QString Document::undoDescription() const { return qstr(session_->undo_description()); }
QString Document::redoDescription() const { return qstr(session_->redo_description()); }

QStringList Document::problems() const {
    QStringList out;
    for (const ldoc::Problem& p : session_->problems())
        out << QStringLiteral("%1: %2")
                   .arg(p.severity == ldoc::Severity::Error ? QStringLiteral("error")
                                                            : QStringLiteral("warning"),
                        qstr(p.message));
    return out;
}

QString Document::monitoringOutput() const {
    const auto& extra = session_->project().settings.extra;
    const auto found = extra.find("audio.monitor.output");
    if (found != extra.end()) {
        if (const auto* value = std::get_if<std::string>(&found->second);
            value && !value->empty() && value->size() <= 1024 &&
            value->find('\0') == std::string::npos &&
            (*value == "none" || *value == "default" ||
             (value->starts_with("sink:") && value->size() > 5)))
            return qstr(*value);
    }
    return QStringLiteral("none");
}

QVariantMap Document::monitoringOutputChoices() const {
    QStringList choices{QStringLiteral("none"), QStringLiteral("default")};
    QVariantMap labels{{"none", tr("None")}, {"default", tr("System default")}};
    std::string error;
    for (const auto& device : libertas::capture::list_audio_sinks(error)) {
        const auto id = QStringLiteral("sink:") + qstr(device.id);
        choices.append(id);
        labels[id] = qstr(device.name);
    }
    const auto current = monitoringOutput();
    if (!choices.contains(current)) {
        choices.append(current);
        labels[current] = tr("Unavailable (%1)").arg(current);
    }
    return {{"choices", choices}, {"choiceLabels", labels}, {"error", qstr(error)}};
}

void Document::setMonitoringOutput(const QString& output) {
    if (output == monitoringOutput())
        return;
    const auto available = monitoringOutputChoices().value("choices").toStringList();
    if (!available.contains(output) || output.isEmpty() || output.size() > 1024 ||
        output.contains(QChar::Null))
        return;
    auto settings = session_->project().settings;
    settings.extra["audio.monitor.output"] = to_std(output);
    run(ldoc::SetProjectSettings{std::move(settings)});
}

QStringList Document::blendModes() const {
    QStringList out;
    for (const auto i :
         {ldoc::BlendMode::Normal, ldoc::BlendMode::LinearDodge, ldoc::BlendMode::Subtract,
          ldoc::BlendMode::Screen, ldoc::BlendMode::Multiply, ldoc::BlendMode::Lighten,
          ldoc::BlendMode::Darken})
        out << QString::fromUtf8(ldoc::to_string(static_cast<ldoc::BlendMode>(i)).data(),
                                 static_cast<qsizetype>(
                                     ldoc::to_string(static_cast<ldoc::BlendMode>(i)).size()));
    return out;
}

QString Document::canvas_key(const QString& canvasId) const {
    // Empty means the first canvas. The layout's plain "scenes"/"sources" panel ids carry no
    // canvas, and Main is the one canvas guaranteed to exist and to stay at index 0, so it is
    // the one that can be named by omission.
    if (!canvasId.isEmpty())
        return canvasId;
    const auto& canvases = session_->project().canvases;
    return canvases.empty() ? QString{} : id_str(canvases.front().id);
}

QAbstractItemModel* Document::scenesFor(const QString& canvasId) {
    const QString key = canvas_key(canvasId);
    auto it = scenes_by_canvas_.find(key);
    if (it == scenes_by_canvas_.end()) {
        // Parented to this, so the QML engine treats it as C++-owned and a binding that
        // outlives its panel does not take the model with it.
        it = scenes_by_canvas_.emplace(key, new SceneModel(this)).first;
    }
    return it->second;
}

QAbstractItemModel* Document::nodesFor(const QString& canvasId) {
    const QString key = canvas_key(canvasId);
    auto it = nodes_by_canvas_.find(key);
    if (it == nodes_by_canvas_.end())
        it = nodes_by_canvas_.emplace(key, new NodeModel(this)).first;
    return it->second;
}

QString Document::selectedSceneOn(const QString& canvasId) const {
    const auto it = selected_scene_.find(canvas_key(canvasId));
    return it == selected_scene_.end() ? QString{} : it->second;
}

void Document::selectSceneOn(const QString& canvasId, const QString& sceneId) {
    const QString key = canvas_key(canvasId);
    const bool same_canvas = current_canvas_ == key ||
                             (current_canvas_.isEmpty() && key == canvas_key(QString{}));
    if (selectedSceneOn(key) == sceneId && same_canvas)
        return;
    selected_scene_[key] = sceneId;
    selected_node_.clear();
    // The canvas the shell is working on follows the click. Picking a row in "Scenes:
    // Vertical" is how an operator says "Vertical now" -- requiring a second click in the
    // canvas list first would be a step that exists only because of how this is stored.
    if (!same_canvas) {
        current_canvas_ = key;
        emit canvasChanged();
    }
    rebuild();
}

QString Document::canvasNameOf(const QString& canvasId) const {
    const auto parsed = parse_id(canvas_key(canvasId));
    const ldoc::Canvas* c = parsed ? session_->project().canvas(*parsed) : nullptr;
    return c ? qstr(c->name) : QString{};
}

QSize Document::canvasSizeOf(const QString& canvasId) const {
    const auto parsed = parse_id(canvas_key(canvasId));
    const ldoc::Canvas* c = parsed ? session_->project().canvas(*parsed) : nullptr;
    return c ? QSize(c->width, c->height) : QSize{};
}

double Document::canvasFpsOf(const QString& canvasId) const {
    const auto parsed = parse_id(canvas_key(canvasId));
    const ldoc::Canvas* c = parsed ? session_->project().canvas(*parsed) : nullptr;
    if (!c || c->fps.den == 0)
        return 0.0;
    return static_cast<double>(c->fps.num) / static_cast<double>(c->fps.den);
}

namespace {

const ldoc::Scene* program_scene(const ldoc::Project& project, const ldoc::Id& canvas);
std::optional<QVariantMap> placed_map(const ldoc::Project& project, const ldoc::Canvas& canvas,
                                      const ldoc::Node& node);

QTransform group_space(const ldoc::Node& group, const ldoc::Canvas& canvas) {
    const auto& data = std::get<ldoc::GroupData>(group.data);
    const auto size = data.size.value_or(
        ldoc::Vec2{static_cast<double>(canvas.width), static_cast<double>(canvas.height)});
    lcomp::GapReason why = lcomp::GapReason::NoSize;
    const auto box = lcomp::drawn_frame(group, size.x, size.y, true, why);
    const auto width = size.x - group.layout.crop.left - group.layout.crop.right;
    const auto height = size.y - group.layout.crop.top - group.layout.crop.bottom;
    if (!box || width <= 0 || height <= 0)
        return {};
    const auto map = [&](double x, double y) {
        x = (x - group.layout.crop.left) / width;
        y = (y - group.layout.crop.top) / height;
        if (group.transform.scale.x < 0)
            x = 1 - x;
        if (group.transform.scale.y < 0)
            y = 1 - y;
        double u = x, v = y;
        switch (lcomp::quarter_turn(group.transform.rotation_deg).value_or(0)) {
        case 1:
            u = 1 - y;
            v = x;
            break;
        case 2:
            u = 1 - x;
            v = 1 - y;
            break;
        case 3:
            u = y;
            v = 1 - x;
            break;
        }
        return QPointF(box->x + u * box->width, box->y + v * box->height);
    };
    const auto origin = map(0, 0), x = map(1, 0) - origin, y = map(0, 1) - origin;
    return QTransform(x.x(), x.y(), y.x(), y.y(), origin.x(), origin.y());
}

QTransform parent_space(const ldoc::Scene& scene, const ldoc::Id& id, const ldoc::Canvas& canvas) {
    QTransform result;
    const auto walk = [&](const auto& self, const std::vector<ldoc::Node>& nodes, const QTransform& parent) -> bool {
        for (const auto& node : nodes) {
            if (node.id == id) { result = parent; return true; }
            if (const auto* children = node.children())
                if (self(self, *children, group_space(node, canvas) * parent)) return true;
        }
        return false;
    };
    walk(walk, scene.roots, {});
    return result;
}

const ldoc::Canvas* canvas_of(const ldoc::Project& project, const ldoc::Id& id) {
    for (const ldoc::Canvas& canvas : project.canvases) {
        if (canvas.id == id)
            return &canvas;
    }
    return nullptr;
}

} // namespace

QVariantMap Document::placedRect(const QString& canvasId, const QString& nodeId) const {
    const ldoc::Project& project = session_->project();
    const auto canvas_id = parse_id(canvas_key(canvasId));
    const auto node_id = parse_id(nodeId);
    if (!canvas_id || !node_id)
        return {};
    const ldoc::Canvas* canvas = canvas_of(project, *canvas_id);
    const ldoc::Scene* scene = canvas ? program_scene(project, canvas->id) : nullptr;
    if (!canvas || !scene)
        return {};
    const ldoc::Node* node = ldoc::find_node(scene->roots, *node_id);
    if (!node)
        return {};
    return placed_rect(*scene, *canvas, *node);
}

QVariantMap Document::placed_rect(const ldoc::Scene& scene, const ldoc::Canvas& canvas,
                                 const ldoc::Node& node) const {
    auto result = placed_map(session_->project(), canvas, node).value_or(QVariantMap{});
    if (!result.isEmpty()) {
        const auto box = parent_space(scene, node.id, canvas).mapRect(QRectF(
            result["x"].toDouble(), result["y"].toDouble(),
            result["width"].toDouble(), result["height"].toDouble()));
        result["x"] = box.x(); result["y"] = box.y();
        result["width"] = box.width(); result["height"] = box.height();
    }
    return result;
}

QVariantList Document::placedNodes(const QString& canvasId) const {
    QVariantList out;
    const ldoc::Project& project = session_->project();
    const auto canvas_id = parse_id(canvas_key(canvasId));
    if (!canvas_id)
        return out;
    const ldoc::Canvas* canvas = canvas_of(project, *canvas_id);
    const ldoc::Scene* scene = canvas ? program_scene(project, canvas->id) : nullptr;
    if (!canvas || !scene)
        return out;

    const auto walk = [&](const auto& self, const ldoc::Node& node, double parent_opacity) -> void {
        if (!node.visible)
            return;
        const double opacity = parent_opacity * std::clamp(node.blend.opacity, 0.0, 1.0);
        if (opacity <= 0.0)
            return;
        if (const auto* kids = node.children()) {
            const auto before = out.size();
            for (const ldoc::Node& child : *kids)
                self(self, child, opacity);
            // A group goes in *after* its children, so an unlocked child still wins the click.
            // What it catches is the rest: a click that landed on a locked child, which would
            // otherwise fall through to whatever is behind. Locking the contents and leaving the
            // group unlocked is how a set of objects is dragged as one unit.
            if (node.locked || out.size() == before)
                return;
            QRectF hit;
            for (auto i = before; i < out.size(); ++i) {
                const QVariantMap& child = out[static_cast<int>(i)].toMap();
                const QRectF box{child["x"].toDouble(), child["y"].toDouble(),
                                 child["width"].toDouble(), child["height"].toDouble()};
                hit = hit.isNull() ? box : hit.united(box);
            }
            auto map = placedRect(canvasId, id_str(node.id));
            if (map.isEmpty() || hit.isNull())
                return;
            // Hit-tested against where the contents actually are, but dragged by the group's own
            // placed rect: that is the geometry `setDrawnRect` solves against, and handing it the
            // contents' union instead would read as a resize and distort the group.
            map["hitX"] = hit.x();
            map["hitY"] = hit.y();
            map["hitWidth"] = hit.width();
            map["hitHeight"] = hit.height();
            map["isGroup"] = true;
            out.push_back(map);
            return;
        }
        const auto map = placedRect(canvasId, id_str(node.id));
        if (!map.isEmpty()) out.push_back(map);
    };
    for (const ldoc::Node& node : scene->roots)
        walk(walk, node, 1.0);
    return out;
}

void Document::focusPlacedNode(const QString& canvasId, const QString& nodeId) {
    const ldoc::Project& project = session_->project();
    const auto canvas_id = parse_id(canvas_key(canvasId));
    const auto node_id = parse_id(nodeId);
    if (!canvas_id || !node_id)
        return;
    const ldoc::Scene* scene = program_scene(project, *canvas_id);
    if (!scene || !ldoc::find_node(scene->roots, *node_id))
        return;
    const QString scene_id = id_str(scene->id);
    if (selectedSceneOn(canvasId) != scene_id || current_canvas_ != canvas_key(canvasId))
        selectSceneOn(canvasId, scene_id);
    selectNodeOn(canvasId, nodeId);
}

QStringList Document::canvasIds() const {
    QStringList out;
    for (const ldoc::Canvas& c : session_->project().canvases)
        out << id_str(c.id);
    return out;
}

QStringList Document::canvasNames() const {
    QStringList out;
    for (const ldoc::Canvas& c : session_->project().canvases)
        out << qstr(c.name);
    return out;
}

const ldoc::Scene* Document::current_scene() const {
    const auto id = parse_id(selectedSceneOn(current_canvas_));
    return id ? session_->project().scene(*id) : nullptr;
}

const ldoc::Scene* Document::scene_of_canvas(const QString& canvasId) const {
    const auto id = parse_id(selectedSceneOn(canvasId));
    return id ? session_->project().scene(*id) : nullptr;
}

const ldoc::Node* Document::current_node() const {
    const ldoc::Scene* s = current_scene();
    const auto id = parse_id(selected_node_);
    if (!s || !id)
        return nullptr;
    return ldoc::find_node(s->roots, *id);
}

const ldoc::Canvas* Document::current_canvas() const {
    const auto& canvases = session_->project().canvases;
    if (canvases.empty())
        return nullptr;
    // Resolved rather than stored: the selection is an id the operator picked, and the canvas
    // behind it can be removed by an undo or by another panel. Falling back to the first one
    // means the shell is never working on a canvas that is not there.
    if (const auto wanted = parse_id(current_canvas_)) {
        for (const ldoc::Canvas& c : canvases) {
            if (c.id == *wanted)
                return &c;
        }
    }
    return &canvases.front();
}

QString Document::currentCanvasName() const {
    const ldoc::Canvas* c = current_canvas();
    return c ? qstr(c->name) : QString{};
}

bool Document::currentCanvasIsMain() const {
    const auto& canvases = session_->project().canvases;
    const ldoc::Canvas* c = current_canvas();
    return !canvases.empty() && c && c->id == canvases.front().id;
}

int Document::currentCanvasSceneCount() const {
    const ldoc::Canvas* c = current_canvas();
    if (!c)
        return 0;
    int n = 0;
    for (const ldoc::Scene& s : session_->project().scenes) {
        if (s.canvas == c->id)
            ++n;
    }
    return n;
}

void Document::selectCanvas(const QString& raw) {
    // Normalised, so selectCanvas("") -- what a panel bound to the first canvas passes --
    // means that canvas rather than "no canvas".
    const QString id = canvas_key(raw);
    if (current_canvas_ == id)
        return;
    current_canvas_ = id;
    // Nothing to strand: each canvas keeps its own place in its own scene list, so switching
    // canvas and switching back lands where the operator left off. Only the node selection is
    // global, and it belongs to a scene that is no longer being shown.
    selected_node_.clear();
    emit canvasChanged();
    rebuild();
}

namespace {

/// Keep a generated name in step with the settings it describes.
///
/// An encode's name is a description -- "1080p60 8 Mbps" -- so leaving it alone when the
/// bitrate changes leaves a name that lies, and the name is the handle: it is what the outputs
/// rows print when they say two destinations share an encode. But a name the operator typed is
/// theirs and must survive.
///
/// Telling the two apart needs no extra field. If the current name is still exactly what the
/// generator would have produced for the settings as they were, nobody has renamed it.
void retitle_if_auto(const ldoc::Project& p, const ldoc::Encode& before, ldoc::Encode& after) {
    if (before.name == ldoc::default_encode_name(p, before))
        after.name = ldoc::default_encode_name(p, after);
}

std::optional<double> number_setting(const ldoc::ParamMap& settings, const char* key) {
    const auto it = settings.find(key);
    if (it == settings.end())
        return std::nullopt;
    if (const auto* i = std::get_if<std::int64_t>(&it->second))
        return static_cast<double>(*i);
    if (const auto* n = std::get_if<double>(&it->second))
        return *n;
    return std::nullopt;
}

/// Pixel size of an image file, from its header. The compositor decodes the same file to paint;
/// this only needs the dimensions the handles are drawn at.
std::optional<QSize> image_pixels(const std::string& path) {
    if (path.empty())
        return std::nullopt;
    QImageReader reader{QString::fromStdString(path)};
    const QSize size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        return std::nullopt;
    return size;
}

bool effect_enabled(const ldoc::Node& node) {
    for (const ldoc::Effect& effect : node.effects) {
        if (effect.enabled)
            return true;
    }
    return false;
}

const ldoc::Scene* program_scene(const ldoc::Project& project, const ldoc::Id& canvas) {
    for (const ldoc::Deck& deck : project.decks) {
        if (deck.canvas == canvas && deck.program_scene)
            return project.scene(*deck.program_scene);
    }
    return nullptr;
}

/// Types whose picture changes without the document changing.
///
/// Stated as a list of the live ones rather than of the still ones so an unknown type -- a
/// project from a later build (ADR-0004) -- answers "not live". The cost of being wrong that way
/// is a preview that does not move until something is edited; the other way it is a timer
/// recompositing a still picture for ever.
bool is_live_source(std::string_view type) {
    return type == "source.screen" || type == "source.window" || type == "source.game" || type == "source.v4l2" ||
           type == "source.browser" || type == "source.media" || type == "source.text" ||
           type == "source.slideshow" || type == "source.playlist" ||
           type == "source.audio-input" || type == "source.audio-output" || type == "source.blackmagic";
}

/// Defined with the other screen helpers, further down beside the settings schema that also
/// needs the monitor list.
bool screen_size(const ldoc::Source& src, double& width, double& height);
bool video_size(const ldoc::Source& src, double& width, double& height);
bool window_size(const ldoc::Source& src, double& width, double& height);

/// Natural size for a kind the compositor actually paints. Anything else has no rectangle.
bool unfiltered_natural_size(const ldoc::Project& project, const ldoc::Canvas& canvas,
                             const ldoc::Node& node, double& width, double& height) {
    if (const auto* group = std::get_if<ldoc::GroupData>(&node.data)) {
        width = group->size ? group->size->x : canvas.width;
        height = group->size ? group->size->y : canvas.height;
        return width > 0 && height > 0;
    }
    if (const auto* source = std::get_if<ldoc::SourceData>(&node.data)) {
        if (!source->source)
            return false;
        const ldoc::Source* src = project.source(*source->source);
        if (!src)
            return false;
        const auto resolved = ldoc::resolve_source(project, *src);
        if (!resolved)
            return false;
        src = &*resolved;
        if (src->type == "source.color") {
            const auto w = number_setting(src->settings, "width");
            const auto h = number_setting(src->settings, "height");
            if (!w || !h || !(*w > 0.0) || !(*h > 0.0))
                return false;
            width = *w;
            height = *h;
            return true;
        }
        if (src->type == "source.image") {
            const auto it = src->settings.find("file");
            const auto* path =
                it == src->settings.end() ? nullptr : std::get_if<std::string>(&it->second);
            if (!path)
                return false;
            const std::optional<QSize> size = image_pixels(*path);
            if (!size)
                return false;
            width = size->width();
            height = size->height();
            return true;
        }
        if (src->type == "source.screen")
            return screen_size(*src, width, height);
        if (src->type == "source.v4l2")
            return video_size(*src, width, height);
        if (src->type == "source.window" || src->type == "source.game")
            return window_size(*src, width, height);
        if (src->type == "source.text" || src->type == "source.media" ||
            src->type == "source.slideshow" || src->type == "source.playlist" || src->type == "source.browser" || src->type == "source.blackmagic") {
            width = number_setting(src->settings, "width").value_or(1920);
            height = number_setting(src->settings, "height").value_or(1080);
            return width > 0 && height > 0 && width <= 4096 && height <= 4096;
        }
        return false;
    }
    if (const auto* pre = std::get_if<ldoc::PrecompData>(&node.data)) {
        if (pre->size.x > 0.0 && pre->size.y > 0.0) {
            width = pre->size.x;
            height = pre->size.y;
            return true;
        }
        if (canvas.width > 0 && canvas.height > 0) {
            width = canvas.width;
            height = canvas.height;
            return true;
        }
    }
    return false;
}

bool natural_size(const ldoc::Project& project, const ldoc::Canvas& canvas, const ldoc::Node& node,
                  double& width, double& height) {
    if (!unfiltered_natural_size(project, canvas, node, width, height))
        return false;
    std::vector<ldoc::Effect> effects;
    if (const auto* data = std::get_if<ldoc::SourceData>(&node.data); data && data->source)
        if (const auto* source = project.source(*data->source))
            if (const auto resolved = ldoc::resolve_source(project, *source))
                effects = resolved->effects;
    effects.insert(effects.end(), node.effects.begin(), node.effects.end());
    const auto size = lcomp::filter_size(effects, width, height);
    if (!size)
        return false;
    width = size->x;
    height = size->y;
    return true;
}

bool drawable(const ldoc::Node& node) {
    if (std::holds_alternative<ldoc::GroupData>(node.data)) return true;
    if (std::holds_alternative<ldoc::PrecompData>(node.data))
        return true;
    const auto* source = std::get_if<ldoc::SourceData>(&node.data);
    if (!source || !source->source)
        return false;
    return true; // natural_size refuses kinds the picture does not draw
}

QVariantMap frame_map(const lcomp::DrawnFrame& frame, const ldoc::Node& node) {
    QVariantMap m;
    m.insert(QStringLiteral("id"), id_str(node.id));
    m.insert(QStringLiteral("x"), frame.x);
    m.insert(QStringLiteral("y"), frame.y);
    m.insert(QStringLiteral("width"), frame.width);
    m.insert(QStringLiteral("height"), frame.height);
    m.insert(QStringLiteral("baseWidth"), frame.base_width);
    m.insert(QStringLiteral("baseHeight"), frame.base_height);
    m.insert(QStringLiteral("relCx"), frame.rel_cx);
    m.insert(QStringLiteral("relCy"), frame.rel_cy);
    m.insert(QStringLiteral("relDx"), frame.rel_dx);
    m.insert(QStringLiteral("relDy"), frame.rel_dy);
    m.insert(QStringLiteral("posX"), node.transform.position.x);
    m.insert(QStringLiteral("posY"), node.transform.position.y);
    m.insert(QStringLiteral("scaleX"), node.transform.scale.x);
    m.insert(QStringLiteral("scaleY"), node.transform.scale.y);
    m.insert(QStringLiteral("locked"), node.locked);
    return m;
}

std::optional<QVariantMap> placed_map(const ldoc::Project& project, const ldoc::Canvas& canvas,
                                      const ldoc::Node& node) {
    if (!node.visible || node.blend.opacity <= 0.0)
        return std::nullopt;
    if (!lcomp::blend_supported(node.blend.mode) || node.blend.clip_to_below ||
        effect_enabled(node) || !drawable(node))
        return std::nullopt;
    double natural_w = 0.0;
    double natural_h = 0.0;
    const bool have = natural_size(project, canvas, node, natural_w, natural_h);
    // An image with no readable file is a gap, even if a box is set. A colour can still be
    // placed from its box alone, which is what the compositor does.
    if (const auto* source = std::get_if<ldoc::SourceData>(&node.data)) {
        const ldoc::Source* src = source->source ? project.source(*source->source) : nullptr;
        // A rectangle on the canvas is a question about size, not about type. Anything with a
        // natural size can be selected, dragged, resized and given the transform menu; a colour
        // is the exception that can be placed from its box alone, which is what the compositor
        // does with it. Listing the drawable types here instead was what kept a screen source
        // from being selectable at all even while its picture was on the canvas -- the overlay
        // and the compositor disagreed about what was on screen.
        if (src && src->type != "source.color" && !have)
            return std::nullopt;
    }
    lcomp::GapReason why = lcomp::GapReason::NoSize;
    const std::optional<lcomp::DrawnFrame> frame =
        lcomp::drawn_frame(node, natural_w, natural_h, have, why);
    if (!frame || !(frame->width > 0.0) || !(frame->height > 0.0) || !(frame->base_width > 0.0) ||
        !(frame->base_height > 0.0))
        return std::nullopt;
    return frame_map(*frame, node);
}

} // namespace

QVariantMap Document::nodeProperties() const {
    QVariantMap m;
    const ldoc::Node* n = current_node();
    if (!n)
        return m;
    m["id"] = id_str(n->id);
    m["name"] = node_display_name(session_->project(), *n);
    m["kind"] = kind_str(n->kind());
    m["visible"] = n->visible;
    m["locked"] = n->locked;
    m["positionX"] = n->transform.position.x;
    m["positionY"] = n->transform.position.y;
    m["scaleX"] = n->transform.scale.x;
    m["scaleY"] = n->transform.scale.y;
    m["rotation"] = n->transform.rotation_deg;
    m["opacity"] = n->blend.opacity;
    m["blendMode"] = QString::fromUtf8(
        ldoc::to_string(n->blend.mode).data(),
        static_cast<qsizetype>(ldoc::to_string(n->blend.mode).size()));
    const auto fit = ldoc::to_string(n->layout.fit);
    m["fit"] = QString::fromUtf8(fit.data(), static_cast<qsizetype>(fit.size()));
    m["boxWidth"] = n->layout.size.x;
    m["boxHeight"] = n->layout.size.y;
    m["cropLeft"] = n->layout.crop.left;
    m["cropTop"] = n->layout.crop.top;
    m["cropRight"] = n->layout.crop.right;
    m["cropBottom"] = n->layout.crop.bottom;
    m["alignX"] = n->layout.alignment.x;
    m["alignY"] = n->layout.alignment.y;
    m["effectCount"] = static_cast<int>(n->effects.size());

    // Audio, for the mixer's selected-node strip. A source node with no bus is unrouted, which is a
    // different thing from muted -- so both are reported rather than collapsed into one flag.
    if (const auto* placed = std::get_if<ldoc::SourceData>(&n->data)) {
        // Asked of the source's *type*, not assumed. Said true for everything until now, which
        // is why a colour card offered to be routed to a bus.
        const ldoc::Source* src =
            placed->source ? session_->project().source(*placed->source) : nullptr;
        m["hasAudio"] = src && ldoc::has_audio(*src);
        // What the settings dialog is opened against. Empty when the node is bound to nothing,
        // which is what disables the gear rather than opening an empty panel.
        m["sourceId"] = placed->source ? id_str(*placed->source) : QString{};
        m["audioBus"] = placed->audio.bus ? id_str(*placed->audio.bus) : QString{};
        m["audioGain"] = placed->audio.gain_db;
        m["audioMute"] = placed->audio.mute;
        m["audioSendCount"] = static_cast<int>(placed->audio.sends.size());
        QString bus_name;
        if (placed->audio.bus) {
            const ldoc::Project& p = session_->project();
            bus_name = QStringLiteral("(missing bus)");
            for (const ldoc::Bus& b : p.buses) {
                if (b.id == *placed->audio.bus)
                    bus_name = qstr(b.name);
            }
        }
        m["audioBusName"] = bus_name;
    } else {
        m["hasAudio"] = false;
        m["sourceId"] = QString{};
    }
    return m;
}

// ---- rebuilding the models -------------------------------------------------------------------

void Document::refreshRows() { rebuild(); }

void Document::rebuild() {
    const ldoc::Project& p = session_->project();

    // Repair the selection *before* building anything from it.
    //
    // A selected thing can be deleted by a command or brought back by an undo, so selection is
    // validated on every change rather than trusted. Doing it afterwards left the Sources list
    // built from a scene that had just been removed -- empty, while a different scene was
    // selected -- and nothing triggered another rebuild, so the panel stayed empty until the
    // operator clicked something.
    // Two directions, not one. Losing the selected scene has to move the selection, and so does
    // *gaining* a scene when there is none selected -- which happens when an undo brings back
    // everything that was deleted. Without the second case the panels stay on "no scene
    // selected" with a full scene list beside them, and nothing but a click gets out of it.
    for (const ldoc::Canvas& c : p.canvases) {
        const QString key = id_str(c.id);
        const ldoc::Scene* held = scene_of_canvas(key);
        if (held && held->canvas == c.id)
            continue;
        // On startup and collection changes, selection follows the saved Program, not list
        // order. Keep a valid operator selection during normal edits (including right-click).
        QString first;
        const auto* active = program_scene(p, c.id);
        if (active && active->canvas == c.id) {
            first = id_str(active->id);
        } else {
            for (const ldoc::Scene& sc : p.scenes) {
                if (sc.canvas == c.id) {
                    first = id_str(sc.id);
                    break;
                }
            }
        }
        selected_scene_[key] = first;
        selected_node_.clear();
    }
    if (!selected_node_.isEmpty() && !current_node())
        selected_node_.clear();
    // The rest of the selection has to be pruned the same way, or a node removed by an undo
    // -- or by the command that just ran -- stays named by something that still acts on it.
    if (!also_selected_.isEmpty()) {
        const ldoc::Scene* sel = current_scene();
        QStringList alive;
        for (const QString& id : also_selected_) {
            const auto parsed = parse_id(id);
            if (sel && parsed && ldoc::find_node(sel->roots, *parsed))
                alive << id;
        }
        also_selected_ = alive;
    }

    // Scenes belong to a canvas (ADR-0027), and each canvas has its own pair of panels --
    // "Scenes: Main", "Scenes: Vertical" -- so each gets its own model rather than one model
    // filtered to whichever canvas is current.
    const ldoc::Id main_canvas = p.canvases.empty() ? ldoc::Id{} : p.canvases.front().id;

    const auto scene_row = [&](const ldoc::Scene& s) {
        SceneRow row;
        row.id = id_str(s.id);
        row.name = qstr(s.name);
        int count = 0;
        ldoc::visit(s.roots, [&count](const ldoc::Node&) { ++count; });
        row.nodeCount = count;
        for (const ldoc::Deck& d : p.decks) {
            row.isProgram = row.isProgram || (d.program_scene && *d.program_scene == s.id);
            row.isPreview = row.isPreview || (d.preview_scene && *d.preview_scene == s.id);
        }
        for (const ldoc::Canvas& c : p.canvases)
            row.isEmergency = row.isEmergency || (c.emergency_scene && *c.emergency_scene == s.id);
        row.isFavourite = s.favourite;
        // How many Main scenes bring this one up. A link always runs from Main, so this is
        // zero on every Main scene by construction, and the panel draws no chain there.
        for (const ldoc::SceneLink& l : p.scene_links) {
            if (l.to == s.id)
                ++row.linkCount;
        }
        row.canLink = !p.canvases.empty() && s.canvas != main_canvas;
        return row;
    };

    std::map<QString, std::vector<SceneRow>> scenes_per_canvas;
    for (const ldoc::Canvas& c : p.canvases)
        scenes_per_canvas[id_str(c.id)]; // every canvas gets a list, including an empty one

    std::vector<SceneRow> favourites;
    for (const ldoc::Scene& s : p.scenes) {
        SceneRow row = scene_row(s);
        // Favourites deliberately span canvases: the panel exists so a live workspace can close
        // the scene list entirely, and the scenes worth reaching in a hurry are not all on one
        // canvas.
        if (row.isFavourite)
            favourites.push_back(row);
        scenes_per_canvas[id_str(s.canvas)].push_back(std::move(row));
    }
    favourites_.set_rows(std::move(favourites));
    for (auto& [canvas, rows] : scenes_per_canvas) {
        auto* model = static_cast<SceneModel*>(scenesFor(canvas));
        model->set_rows(std::move(rows));
    }
    // A canvas that has gone keeps its model, emptied. A panel can outlive its canvas by a
    // frame, and an empty list is a better thing for it to be showing than the last canvas's.
    for (auto& [canvas, model] : scenes_by_canvas_) {
        if (!scenes_per_canvas.contains(canvas))
            model->set_rows({});
    }

    // One node list per canvas: the Sources panel of a canvas shows that canvas's selected
    // scene, so two canvases on screen at once show two different trees.
    const auto node_rows = [&](const ldoc::Scene* s) {
    std::vector<NodeRow> rows;
    if (s) {
        // Flattened depth-first, which is what the Sources panel is: the tree with indentation.
        const std::function<void(const std::vector<ldoc::Node>&, int)> walk =
            [&](const std::vector<ldoc::Node>& nodes, int depth) {
                for (const ldoc::Node& n : nodes) {
                    NodeRow row;
                    row.id = id_str(n.id);
                    row.name = node_display_name(p, n);
                    row.kind = kind_str(n.kind());
                    row.depth = depth;
                    row.visible = n.visible;
                    row.locked = n.locked;
                    row.isGroup = n.is_group();
                    row.expanded = !collapsed_groups_.contains(row.id);
                    row.effectCount = static_cast<int>(n.effects.size());
                    if (const auto* placed = std::get_if<ldoc::SourceData>(&n.data)) {
                        if (placed->source) {
                            // The id is carried even when the source has gone: a row that says
                            // "(missing source)" with no id would look identical to an unbound
                            // one, and they want different things done to them.
                            row.sourceId = id_str(*placed->source);
                            if (const ldoc::Source* src = p.source(*placed->source)) {
                                row.sourceName = qstr(src->name);
                                row.sourceType = qstr(src->type);
                                // The indicator's three answers. Asked of AssetCache, which
                                // already holds off-thread stat results, so building a row
                                // costs no stat on the mount these files live on.
                                row.cacheable = source_type_cacheable(src->type);
                                if (row.cacheable) {
                                    row.cacheUsed = source_uses_cache(*src);
                                    const auto* cache = AssetCache::instance();
                                    row.cacheState =
                                        !row.cacheUsed || !cache
                                            ? QStringLiteral("off")
                                            : cache->worstStateOf(source_asset_paths(*src))
                                                  .value(QStringLiteral("state"))
                                                  .toString();
                                }
                                for (const QVariant& type : sourceTypes()) {
                                    const auto entry = type.toMap();
                                    if (entry.value("id").toString() == row.sourceType) {
                                        row.sourceType = entry.value("title").toString();
                                        break;
                                    }
                                }
                            } else
                                row.sourceName = QStringLiteral("(missing source)");
                        }
                    }
                    rows.push_back(std::move(row));
                    if (const auto* kids = n.children(); kids && !collapsed_groups_.contains(id_str(n.id)))
                        walk(*kids, depth + 1);
                }
            };
        walk(s->roots, 0);
    }
    return rows;
    };

    for (const ldoc::Canvas& c : p.canvases) {
        const QString key = id_str(c.id);
        auto* model = static_cast<NodeModel*>(nodesFor(key));
        model->set_rows(node_rows(scene_of_canvas(key)));
    }
    for (auto& [canvas, model] : nodes_by_canvas_) {
        if (!scenes_per_canvas.contains(canvas))
            model->set_rows({});
    }

    std::vector<SourceRow> sources;
    for (const ldoc::Source& src : p.sources) {
        SourceRow row;
        row.id = id_str(src.id);
        row.name = qstr(src.name);
        row.type = qstr(src.type);
        row.selfContained = ldoc::is_self_contained(src.type);
        for (const ldoc::Scene& s : p.scenes) {
            ldoc::visit(s.roots, [&](const ldoc::Node& n) {
                const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
                if (placed && placed->source && *placed->source == src.id)
                    ++row.useCount;
            });
        }
        sources.push_back(std::move(row));
    }
    sources_.set_rows(std::move(sources));

    // What a build should prepare: every path of every cacheable source whose own checkbox is
    // on. Told to the cache from here rather than read by it, so the cache never has to know
    // what a project is -- and so a source switched off stops being built on the next change.
    if (auto* cache = AssetCache::instance()) {
        QStringList wanted;
        for (const ldoc::Source& src : p.sources)
            if (source_type_cacheable(src.type) && source_uses_cache(src))
                wanted += source_asset_paths(src);
        // The largest canvas, not the selected one. A clip prepared for a 1080p canvas is the
        // wrong clip for the 1440p canvas beside it, and preparing for the larger of the two
        // is the choice that never leaves the compositor upscaling.
        QSize largest;
        for (const ldoc::Canvas& c : p.canvases)
            largest = {std::max(largest.width(), c.width), std::max(largest.height(), c.height)};
        cache->setWanted(std::move(wanted), largest);
    }

    std::vector<BusRow> buses;
    for (const ldoc::Bus& b : p.buses) {
        BusRow row;
        row.id = id_str(b.id);
        row.name = qstr(b.name);
        const auto kind = ldoc::to_string(b.kind);
        row.kind = QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()));
        row.gainDb = b.gain_db;
        row.pan = b.pan;
        row.mute = b.mute;
        row.monitor = b.monitor;
        row.hidden = b.hidden;
        row.levelLocked = b.level_locked;
        row.channels = b.channels;
        row.sendCount = static_cast<int>(b.sends.size());
        // Every scene, not just the selected one: a bus is project-level, and the question the
        // strip answers is whether anything in the show feeds it at all.
        for (const ldoc::Scene& sc : p.scenes) {
            ldoc::visit(sc.roots, [&](const ldoc::Node& n) {
                const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
                if (placed && placed->audio.bus && *placed->audio.bus == b.id)
                    ++row.feedCount;
            });
        }
        if (const ldoc::Node* sel = current_node()) {
            const auto* placed = std::get_if<ldoc::SourceData>(&sel->data);
            row.carriesSelection = placed && placed->audio.bus && *placed->audio.bus == b.id;
        }
        buses.push_back(std::move(row));
    }
    buses_.set_rows(std::move(buses));

    // The source strips, from the scene on screen. Rebuilt from the selected scene rather than
    // the program one: this panel is mostly read while building a scene, and faders that
    // belonged to whatever is on air would be the wrong set at exactly the moment they matter.
    std::vector<MixerStripRow> strips;
    std::set<ldoc::Id> visited_scenes;
    const std::function<void(const ldoc::Scene&)> collect_audio = [&](const ldoc::Scene& scene) {
        // Shared precomps get one set of controls; the same guard also bounds cyclic input.
        if (!visited_scenes.insert(scene.id).second)
            return;
        ldoc::visit(scene.roots, [&](const ldoc::Node& n) {
            if (const auto* precomp = std::get_if<ldoc::PrecompData>(&n.data)) {
                if (const auto* nested = p.scene(precomp->scene))
                    collect_audio(*nested);
                return;
            }
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (!placed)
                return;
            const ldoc::Source* src = placed->source ? p.source(*placed->source) : nullptr;
            // A node bound to nothing has no type to ask, so it gets no strip. Unbound is a
            // state the Sources panel already reports; a silent fader would say it twice and
            // less clearly.
            if (!src || !ldoc::has_audio(*src))
                return;
            MixerStripRow row;
            row.id = id_str(n.id);
            row.nodeId = row.id;
            row.sceneId = id_str(scene.id);
            row.name = node_display_name(p, n);
            if (placed->audio.bus) {
                row.busId = id_str(*placed->audio.bus);
                const ldoc::Bus* b = p.bus(*placed->audio.bus);
                row.busName = b ? qstr(b->name) : QStringLiteral("(missing bus)");
            }
            row.gainDb = placed->audio.gain_db;
            row.mute = placed->audio.mute;
            row.monitor = placed->audio.monitor;
            row.hidden = placed->audio.hidden;
            row.levelLocked = placed->audio.level_locked;
            row.selected = row.nodeId == selected_node_;
            strips.push_back(std::move(row));
        });
    };
    if (const auto* shown = current_scene())
        collect_audio(*shown);
    mixer_strips_.set_rows(std::move(strips));

    // Scene and canvas names, so the deck and output rows can show what is on air rather than
    // an id. Built once rather than searched per row.
    const auto scene_name = [&p](const std::optional<ldoc::Id>& id) -> QString {
        if (!id)
            return {};
        for (const ldoc::Scene& sc : p.scenes) {
            if (sc.id == *id)
                return qstr(sc.name);
        }
        return QStringLiteral("(missing scene)");
    };
    const auto canvas_name = [&p](const ldoc::Id& id) -> QString {
        for (const ldoc::Canvas& c : p.canvases) {
            if (c.id == id)
                return qstr(c.name);
        }
        return QStringLiteral("(missing canvas)");
    };

    std::vector<CanvasRow> canvas_rows;
    for (const ldoc::Canvas& c : p.canvases) {
        CanvasRow row;
        row.id = id_str(c.id);
        row.name = qstr(c.name);
        row.pixelWidth = c.width;
        row.pixelHeight = c.height;
        row.fps = c.fps.den == 0 ? 0.0
                                 : static_cast<double>(c.fps.num) / static_cast<double>(c.fps.den);
        const auto cs = ldoc::to_string(c.color_space);
        row.colorSpace = QString::fromUtf8(cs.data(), static_cast<qsizetype>(cs.size()));
        row.isMain = c.id == p.canvases.front().id;
        row.hasEmergencyScene = c.emergency_scene.has_value();
        for (const ldoc::Scene& sc : p.scenes) {
            if (sc.canvas == c.id)
                ++row.sceneCount;
        }
        canvas_rows.push_back(std::move(row));
    }
    canvases_.set_rows(std::move(canvas_rows));

    // Where a node that has never been dragged is drawn. The migration places what it
    // converts, but a project made after schema 4 starts with every position at the origin,
    // and a pile at the corner is not a graph. Filled in here rather than written into the
    // document on open: a project that edited itself for being looked at would be dirty
    // before the operator touched anything.
    constexpr double kEncodeColumn = 400.0;
    constexpr double kOutputColumn = 760.0;
    constexpr double kRowHeight = 150.0;
    constexpr double kGraphTop = 60.0;
    const auto placed = [](const ldoc::Vec2& at, double column, std::size_t index) {
        return at == ldoc::Vec2{}
                   ? ldoc::Vec2{column, kGraphTop + kRowHeight * static_cast<double>(index)}
                   : at;
    };

    // The encode list, which is the outputs manager's spine: one row per encode with its
    // destinations hanging off it. This is the inspector's "N concurrent encode(s) asked of the
    // GPU" in a shape the operator can act on (ADR-0029).
    std::vector<EncodeRow> encode_rows;
    for (const ldoc::Encode& e : p.encodes) {
        EncodeRow row;
        row.id = id_str(e.id);
        row.name = qstr(e.name);
        row.canvasId = p.canvas(e.canvas) ? id_str(e.canvas) : QString{};
        row.canvasName = canvas_name(e.canvas);
        const ldoc::Canvas* c = p.canvas(e.canvas);
        const double w = e.rescale ? e.rescale->x : (c ? c->width : 0);
        const double h = e.rescale ? e.rescale->y : (c ? c->height : 0);
        row.resolution = QStringLiteral("%1x%2")
                             .arg(static_cast<long long>(w))
                             .arg(static_cast<long long>(h));
        row.codec = qstr(e.encoder.codec);
        row.implementation = qstr(e.encoder.implementation);
        row.bitrateKbps = e.encoder.bitrate_kbps;
        row.preset = qstr(e.encoder.preset);
        for (const ldoc::Output& o : p.outputs) {
            if (o.encode == e.id)
                row.destinations.push_back(qstr(o.name));
        }
        const auto at = placed(e.position, kEncodeColumn, encode_rows.size());
        row.positionX = at.x;
        row.positionY = at.y;
        encode_rows.push_back(std::move(row));
    }
    encodes_.set_rows(std::move(encode_rows));

    // The audio bitstreams, the same shape as the video ones. Several destinations naming one
    // of these share it, exactly as they share a video encode (ADR-0029).
    std::vector<AudioEncodeRow> audio_encode_rows;
    for (const ldoc::AudioEncode& e : p.audio_encodes) {
        AudioEncodeRow row;
        row.id = id_str(e.id);
        row.name = qstr(e.name);
        row.busId = p.bus(e.bus) ? id_str(e.bus) : QString{};
        const ldoc::Bus* b = p.bus(e.bus);
        row.busName = b ? qstr(b->name) : QString{};
        row.codec = qstr(e.codec);
        row.bitrateKbps = e.bitrate_kbps;
        for (const ldoc::Output& o : p.outputs) {
            if (o.audio_encode && *o.audio_encode == e.id)
                row.destinations.push_back(qstr(o.name));
        }
        // Audio encodes continue the middle column below the video ones.
        const auto at = placed(e.position, kEncodeColumn,
                               p.encodes.size() + audio_encode_rows.size());
        row.positionX = at.x;
        row.positionY = at.y;
        audio_encode_rows.push_back(std::move(row));
    }
    audio_encodes_.set_rows(std::move(audio_encode_rows));

    std::vector<DeckRow> decks;
    for (const ldoc::Deck& d : p.decks) {
        DeckRow row;
        row.id = id_str(d.id);
        row.name = qstr(d.name);
        row.canvasName = canvas_name(d.canvas);
        row.programScene = scene_name(d.program_scene);
        row.previewScene = scene_name(d.preview_scene);
        row.programSceneId = d.program_scene ? id_str(*d.program_scene) : QString{};
        row.previewSceneId = d.preview_scene ? id_str(*d.preview_scene) : QString{};
        row.studioMode = d.studio_mode;
        row.transition = qstr(d.transition);
        row.transitionMs = d.transition_ms;
        for (const ldoc::Canvas& c : p.canvases) {
            if (c.id == d.canvas)
                row.hasEmergencyScene = c.emergency_scene.has_value();
        }
        decks.push_back(std::move(row));
    }
    decks_.set_rows(std::move(decks));

    std::vector<OutputRow> outputs;
    for (const ldoc::Output& o : p.outputs) {
        OutputRow row;
        row.id = id_str(o.id);
        row.encodeId = p.encode(o.encode) ? id_str(o.encode) : QString{};
        row.name = qstr(o.name);
        // An output names an encode and the encode names the canvas, so both are one hop
        // away now (ADR-0029). The row shows the encode's settings because that is what this
        // destination actually receives -- and if another output names the same encode, these
        // are the same numbers on both rows, which is true rather than a display bug.
        const ldoc::Encode* enc = p.encode(o.encode);
        row.canvasName = enc ? canvas_name(enc->canvas) : QStringLiteral("(missing encode)");
        row.encodeName = enc ? qstr(enc->name) : QString{};
        // The canvas name says the wrong resolution whenever the encode rescales, and
        // rescaling is the ordinary case: 1080p to Twitch off a 1440p canvas. Show what
        // actually leaves the encoder.
        if (enc) {
            const ldoc::Canvas* c = p.canvas(enc->canvas);
            const double w = enc->rescale ? enc->rescale->x : (c ? c->width : 0);
            const double h = enc->rescale ? enc->rescale->y : (c ? c->height : 0);
            row.resolution = QStringLiteral("%1x%2")
                                 .arg(static_cast<long long>(w))
                                 .arg(static_cast<long long>(h));
        }
        // An encode feeding more than one destination is the whole point of ADR-0029, and
        // two rows carrying the same numbers do not say it -- two separate encodes
        // configured alike look exactly the same. Name the others.
        if (enc) {
            for (const ldoc::Output& other : p.outputs) {
                if (other.id != o.id && other.encode == o.encode)
                    row.sharedWith.push_back(qstr(other.name));
            }
        }
        row.codec = enc ? qstr(enc->encoder.codec) : QString{};
        row.implementation = enc ? qstr(enc->encoder.implementation) : QString{};
        row.bitrateKbps = enc ? enc->encoder.bitrate_kbps : 0;
        const auto kind = ldoc::to_string(o.destination.kind);
        row.destinationKind = QString::fromUtf8(kind.data(),
                                                static_cast<qsizetype>(kind.size()));
        row.url = qstr(o.destination.url);
        // The *name* of a credential. The project format has no field for the credential
        // itself, so there is nothing here to leak (ADR-0022).
        row.credentialRef = qstr(o.destination.credential_ref);
        row.enabled = o.enabled;
        row.misconfigured = o.enabled && o.destination.url.empty() &&
                            o.destination.kind != ldoc::DestinationKind::File &&
                            o.destination.kind != ldoc::DestinationKind::Replay;
        if (o.audio_encode) {
            row.audioEncodeId = id_str(*o.audio_encode);
            const ldoc::AudioEncode* a = p.audio_encode(*o.audio_encode);
            row.audioEncodeName = a ? qstr(a->name) : QString{};
        }
        const auto at = placed(o.position, kOutputColumn, outputs.size());
        row.positionX = at.x;
        row.positionY = at.y;
        outputs.push_back(std::move(row));
    }
    outputs_.set_rows(std::move(outputs));

    emit changed();
    emit selectionChanged();
    if (!session_->path().empty())
        autosave_.start();
}

void Document::run(const ldoc::Command& command) {
    const auto r = session_->run(command);
    if (!r)
        emit notice(qstr(r.error()));
    // No rebuild here: the session's notification does it. Updating from the return value is
    // what ADR-0024 forbids, because a remote session may return before the change has landed.
}

// ---- selection --------------------------------------------------------------------------------

void Document::selectScene(const QString& id) {
    // A scene names its canvas, so a bare "select this scene" can route itself. Keeps every
    // existing caller -- and the favourites panel, which deliberately spans canvases --
    // working without each one having to know which canvas it just reached into.
    const auto parsed = parse_id(id);
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    selectSceneOn(s ? id_str(s->canvas) : current_canvas_, id);
}

void Document::selectNode(const QString& id) {
    // A plain select collapses the selection to one. Every caller that was written before
    // multi-select means "this node and nothing else", and that is still what it does.
    if (selected_node_ == id && also_selected_.isEmpty())
        return;
    selected_node_ = id;
    also_selected_.clear();
    selection_anchor_ = id;
    emit selectionChanged();
}

QStringList Document::selectedNodes() const {
    QStringList out;
    if (selected_node_.isEmpty() && also_selected_.isEmpty())
        return out;
    // In scene order rather than click order: what acts on this -- grouping especially --
    // cares which is topmost, and the operator reads the list top to bottom.
    const ldoc::Scene* s = current_scene();
    if (!s) {
        if (!selected_node_.isEmpty())
            out << selected_node_;
        return out << also_selected_;
    }
    ldoc::visit(s->roots, [&](const ldoc::Node& n) {
        const QString id = id_str(n.id);
        if (id == selected_node_ || also_selected_.contains(id))
            out << id;
    });
    return out;
}

void Document::selectNodeExtended(const QString& canvasId, const QString& nodeId, bool ctrl,
                                  bool shift) {
    const QString key = canvas_key(canvasId);
    if (current_canvas_ != key) {
        current_canvas_ = key;
        emit canvasChanged();
        // A selection does not survive moving to another canvas's panel: the nodes it names
        // are not in the scene being shown.
        selected_node_.clear();
        also_selected_.clear();
        selection_anchor_.clear();
    }

    if (!ctrl && !shift) {
        selectNode(nodeId);
        return;
    }

    if (ctrl) {
        // Toggle. Taking away the node panels are following moves them to whatever is left,
        // rather than leaving them pointed at something no longer selected.
        if (nodeId == selected_node_) {
            selected_node_ = also_selected_.isEmpty() ? QString{} : also_selected_.takeLast();
        } else if (also_selected_.removeAll(nodeId) == 0) {
            if (!selected_node_.isEmpty())
                also_selected_ << selected_node_;
            selected_node_ = nodeId;
        }
        selection_anchor_ = selected_node_;
        emit selectionChanged();
        return;
    }

    // Shift: everything between the anchor and here, in scene order. A flat run through the
    // tree, so a range that starts outside a group and ends inside one takes the nodes in
    // between -- which is what the list on screen shows and therefore what was meant.
    const ldoc::Scene* s = current_scene();
    if (!s || selection_anchor_.isEmpty()) {
        selectNode(nodeId);
        return;
    }
    QStringList order;
    const auto* model = static_cast<NodeModel*>(nodesFor(key));
    for (const auto& row : model->rows())
        order << row.id;
    const qsizetype from = order.indexOf(selection_anchor_);
    const qsizetype to = order.indexOf(nodeId);
    if (from < 0 || to < 0) {
        selectNode(nodeId);
        return;
    }
    also_selected_.clear();
    for (qsizetype i = std::min(from, to); i <= std::max(from, to); ++i) {
        if (order[i] != nodeId)
            also_selected_ << order[i];
    }
    // The clicked node leads, so the panels follow where the pointer last was. The anchor
    // stays put, so dragging the range back and forth keeps measuring from the same end.
    selected_node_ = nodeId;
    emit selectionChanged();
}

// ---- canvases ---------------------------------------------------------------------------------

ldoc::Encode Document::defaultEncode(const ldoc::Canvas& canvas) {
    ldoc::Encode e;
    e.id = ldoc::Id::generate();
    e.canvas = canvas.id;
    e.name = canvas.name + " Output";
    e.encoder.preset = "medium";
    e.encoder.extra = {{"x264.custom_buffer", false}, {"x264.buffer_kbps", std::int64_t{6000}},
        {"x264.profile", std::string{"main"}}, {"x264.tune", std::string{"film"}},
        {"audio.codec", std::string{"aac"}}, {"audio.bitrate_kbps", std::int64_t{160}},
        {"audio.track", std::int64_t{1}}, {"audio.vod_track", std::int64_t{0}}};
    const bool vertical = canvas.height > canvas.width;
    const double scale = std::min({1.0, (vertical ? 1080.0 : 1920.0) / canvas.width,
                                  (vertical ? 1920.0 : 1080.0) / canvas.height});
    e.rescale = ldoc::Vec2{std::max(2.0, std::floor(canvas.width * scale / 2) * 2),
                          std::max(2.0, std::floor(canvas.height * scale / 2) * 2)};
    return e;
}

void Document::addCanvas(const QString& name, bool vertical) {
    const auto& p = session_->project();
    ldoc::Canvas c;
    c.id = ldoc::Id::generate();
    c.name = name.isEmpty() ? std::string{"Canvas"} : to_std(name);
    c.width = vertical ? 1080 : 1920;
    c.height = vertical ? 1920 : 1080;
    c.fps = {60, 1};
    auto e = defaultEncode(c);
    ldoc::Output o;
    o.id = ldoc::Id::generate();
    o.name = c.name;
    o.encode = e.id;
    const auto id = id_str(c.id);
    run(ldoc::make_batch({ldoc::AddCanvas{c, p.canvases.size()},
        ldoc::AddEncode{e, p.encodes.size()}, ldoc::AddOutput{o, p.outputs.size()}}, "Add canvas"));
    selectCanvas(id);
}

void Document::removeCanvas(const QString& id) {
    const auto parsed = parse_id(id);
    if (!parsed) return;
    const auto& p = session_->project();
    std::vector<ldoc::Command> steps;
    for (const auto& deck : p.decks)
        if (deck.canvas == *parsed) steps.push_back(ldoc::RemoveDeck{deck.id});
    for (const auto& e : p.encodes) {
        if (e.canvas != *parsed) continue;
        for (const auto& o : p.outputs)
            if (o.encode == e.id) steps.push_back(ldoc::RemoveOutput{o.id});
        steps.push_back(ldoc::RemoveEncode{e.id});
    }
    steps.push_back(ldoc::RemoveCanvas{*parsed});
    run(ldoc::make_batch(std::move(steps), "Remove canvas"));
    if (current_canvas_ == id && !session_->project().canvas(*parsed)) {
        const auto* fallback = current_canvas();
        selectCanvas(fallback ? id_str(fallback->id) : QString{});
    }
}

void Document::edit_canvas(const QString& id, const std::function<void(ldoc::Canvas&)>& change) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Canvas* existing = session_->project().canvas(*parsed);
    if (!existing)
        return;
    ldoc::Canvas next = *existing;
    change(next);
    if (next == *existing)
        return;
    run(ldoc::SetCanvas{std::move(next)});
}

void Document::renameCanvas(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    edit_canvas(id, [&](ldoc::Canvas& c) { c.name = to_std(name); });
}

void Document::setCanvasSize(const QString& id, int width, int height) {
    if (width <= 0 || height <= 0)
        return;
    edit_canvas(id, [&](ldoc::Canvas& c) {
        c.width = width;
        c.height = height;
    });
}

void Document::setCanvasFps(const QString& id, int num, int den) {
    if (num <= 0 || den <= 0)
        return;
    edit_canvas(id, [&](ldoc::Canvas& c) {
        c.fps.num = num;
        c.fps.den = den;
    });
}

// ---- scene links ------------------------------------------------------------------------------

QVariantList Document::sceneLinkCanvases() const {
    QVariantList out;
    const auto& canvases = session_->project().canvases;
    for (std::size_t i = 1; i < canvases.size(); ++i)
        out.push_back(QVariantMap{{QStringLiteral("id"), id_str(canvases[i].id)},
                                  {QStringLiteral("name"), qstr(canvases[i].name)}});
    return out;
}

int Document::sceneLinkCount(const QString& sceneId) const {
    const auto parsed = parse_id(sceneId);
    if (!parsed)
        return 0;
    int n = 0;
    for (const ldoc::SceneLink& l : session_->project().scene_links) {
        if (l.to == *parsed)
            ++n;
    }
    return n;
}

QVariantList Document::mainScenesForLink(const QString& sceneId) const {
    QVariantList out;
    const ldoc::Project& p = session_->project();
    const auto parsed = parse_id(sceneId);
    if (!parsed || p.canvases.empty())
        return out;
    const ldoc::Scene* target = p.scene(*parsed);
    if (!target || target->canvas == p.canvases.front().id)
        return out; // Main has no link control at all.

    for (const ldoc::Scene& s : p.scenes) {
        if (s.canvas != p.canvases.front().id)
            continue;
        QVariantMap row;
        row[QStringLiteral("id")] = id_str(s.id);
        row[QStringLiteral("name")] = qstr(s.name);
        bool ticked = false;
        QString taken_by;
        for (const ldoc::SceneLink& l : p.scene_links) {
            if (l.from != s.id)
                continue;
            if (l.to == target->id) {
                ticked = true;
                continue;
            }
            // Already cueing something else on *this* canvas. Ticking it here moves the link,
            // and the popup says so before the operator presses Link (ADR-0027 amendment).
            const ldoc::Scene* other = p.scene(l.to);
            if (other && other->canvas == target->canvas)
                taken_by = qstr(other->name);
        }
        row[QStringLiteral("ticked")] = ticked;
        row[QStringLiteral("takenBy")] = taken_by;
        out.push_back(row);
    }
    return out;
}

void Document::commitSceneLinks(const QString& sceneId, const QStringList& mainSceneIds) {
    const ldoc::Project& p = session_->project();
    const auto parsed = parse_id(sceneId);
    if (!parsed || p.canvases.empty())
        return;
    const ldoc::Scene* target = p.scene(*parsed);
    if (!target || target->canvas == p.canvases.front().id)
        return;

    std::vector<ldoc::Id> wanted;
    for (const QString& s : mainSceneIds) {
        const auto id = parse_id(s);
        const auto* source = id ? p.scene(*id) : nullptr;
        if (!source || source->canvas != p.canvases.front().id)
            return;
        if (std::find(wanted.begin(), wanted.end(), *id) == wanted.end())
            wanted.push_back(*id);
    }
    const auto is_wanted = [&](const ldoc::Id& id) {
        return std::find(wanted.begin(), wanted.end(), id) != wanted.end();
    };

    std::vector<ldoc::Command> steps;
    QStringList moved;

    // Removals first, and that ordering is the whole trick. A tick that moves a link needs the
    // old one gone before the new one is added, or the one-cue-per-canvas rule refuses it --
    // and a batch that is refused half way through is a batch that did nothing.
    for (const ldoc::SceneLink& l : p.scene_links) {
        const ldoc::Scene* from = p.scene(l.from);
        const ldoc::Scene* to = p.scene(l.to);
        if (!from || !to)
            continue;
        if (l.to == target->id && !is_wanted(l.from)) {
            steps.push_back(ldoc::RemoveSceneLink{l.id}); // unticked
            continue;
        }
        if (l.to != target->id && to->canvas == target->canvas && is_wanted(l.from)) {
            steps.push_back(ldoc::RemoveSceneLink{l.id}); // moved here from another scene
            moved.push_back(QStringLiteral("%1 (was bringing up %2)")
                                .arg(qstr(from->name), qstr(to->name)));
        }
    }

    std::size_t at = p.scene_links.size() - steps.size();
    for (const ldoc::Id& from : wanted) {
        bool already = false;
        for (const ldoc::SceneLink& l : p.scene_links)
            already = already || (l.from == from && l.to == target->id);
        if (already)
            continue;
        steps.push_back(ldoc::AddSceneLink{ldoc::SceneLink{ldoc::Id::generate(), from, target->id},
                                           at});
        ++at;
    }

    if (steps.empty())
        return;
    // Each link, unlink, or move is one undo step, including all affected relationships.
    const QString target_name = qstr(target->name);
    run(ldoc::make_batch(std::move(steps), "Link scenes"));
    if (!moved.isEmpty())
        emit notice(QStringLiteral("Moved to \"%1\": %2")
                        .arg(target_name, moved.join(QStringLiteral("; "))));
}

// ---- scenes -----------------------------------------------------------------------------------

void Document::activateScene(const QString& id) {
    const auto parsed = parse_id(id);
    const auto& project = session_->project();
    const auto* scene = parsed ? project.scene(*parsed) : nullptr;
    if (!scene) return;
    const auto canvas = scene->canvas;
    const auto* surface = project.canvas(canvas);
    if (!surface) {
        emit notice(QStringLiteral("This scene's canvas is missing."));
        return;
    }
    // Re-clicking Main also reasserts its cues after a target was changed independently.
    const bool needs_cue = std::any_of(project.scene_links.begin(), project.scene_links.end(),
        [&](const auto& link) {
            const auto* target = link.from == *parsed ? project.scene(link.to) : nullptr;
            if (!target) return false;
            const auto deck = std::find_if(project.decks.begin(), project.decks.end(),
                [&](const auto& d) { return d.canvas == target->canvas; });
            return deck == project.decks.end() || deck->program_scene != link.to;
        });
    std::optional<ldoc::Command> command;
    for (const auto& deck : project.decks) {
        if (deck.canvas != canvas) continue;
        if (deck.program_scene != parsed || needs_cue)
            command = ldoc::SetDeckProgram{deck.id, *parsed};
        selectSceneOn(id_str(canvas), id);
        if (command) run(*command);
        return;
    }
    ldoc::Deck deck;
    deck.id = ldoc::Id::generate();
    deck.name = surface->name;
    deck.canvas = canvas;
    deck.program_scene = *parsed;
    const auto index = project.decks.size();
    selectSceneOn(id_str(canvas), id);
    run(ldoc::AddDeck{std::move(deck), index});
}

void Document::addScene(const QString& name) {
    addSceneOn(current_canvas_, name);
}

void Document::selectNodeOn(const QString& canvasId, const QString& nodeId) {
    const QString key = canvas_key(canvasId);
    if (current_canvas_ != key) {
        current_canvas_ = key;
        emit canvasChanged();
    }
    selectNode(nodeId);
}

void Document::addSceneOn(const QString& canvasId, const QString& name) {
    const auto parsed = parse_id(canvas_key(canvasId));
    const ldoc::Canvas* canvas = parsed ? session_->project().canvas(*parsed) : nullptr;
    if (!canvas)
        return;
    ldoc::Scene s;
    s.id = ldoc::Id::generate();
    s.name = name.isEmpty() ? std::string{"Scene"} : to_std(name);
    // The canvas whose list the + was pressed in. Every canvas has its own scenes panel, so
    // there is no guessing left to do here (ADR-0027).
    s.canvas = canvas->id;
    const QString id = id_str(s.id);
    run(ldoc::AddScene{std::move(s), session_->project().scenes.size()});
    selectScene(id);
}

void Document::removeScene(const QString& id) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RemoveScene{*parsed});
}

void Document::renameScene(const QString& id, const QString& name) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RenameScene{*parsed, to_std(name)});
}

void Document::duplicateScene(const QString& id) {
    const auto parsed = parse_id(id);
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    if (!s)
        return;
    ldoc::Scene copy = ldoc::duplicate_scene(*s, s->name + " copy");
    const QString new_id = id_str(copy.id);
    run(ldoc::AddScene{std::move(copy), session_->project().scenes.size()});
    selectScene(new_id);
}

void Document::setSceneFavourite(const QString& id, bool favourite) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::SetSceneFavourite{*parsed, favourite});
}

void Document::moveScene(const QString& id, int delta) {
    const auto parsed = parse_id(id);
    if (!parsed || delta == 0)
        return;
    const auto& scenes = session_->project().scenes;
    const auto it = std::find_if(scenes.begin(), scenes.end(),
                                 [&parsed](const ldoc::Scene& s) { return s.id == *parsed; });
    if (it == scenes.end())
        return;
    const auto from = static_cast<std::ptrdiff_t>(it - scenes.begin());
    const auto to = from + delta;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(scenes.size()))
        return; // already at the end, which is not an error
    run(ldoc::MoveScene{*parsed, static_cast<std::size_t>(to)});
}

QStringList Document::emergencySceneProblems(const QString& id) const {
    QStringList out;
    const auto parsed = parse_id(id);
    if (!parsed)
        return out;
    for (const ldoc::Problem& p : ldoc::check_emergency_scene(session_->project(), *parsed))
        out << qstr(p.message);
    return out;
}

void Document::setEmergencyScene(const QString& id) {
    const auto parsed = parse_id(id);
    // The canvas comes from the scene, not from whichever canvas happens to be current. A
    // scene belongs to exactly one canvas, so "which canvas is this the safety net for" has
    // one answer and it is not a question the operator should be able to get wrong by having
    // the wrong panel focused (ADR-0025, ADR-0027).
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    const ldoc::Canvas* canvas = s ? session_->project().canvas(s->canvas) : nullptr;
    if (!parsed || !canvas)
        return;
    // The command would accept this regardless -- ADR-0023 -- so the check belongs here, in the
    // designation flow, where there is a human to tell.
    for (const ldoc::Problem& p : ldoc::check_emergency_scene(session_->project(), *parsed))
        emit notice(qstr(p.message));
    run(ldoc::SetCanvasEmergencyScene{canvas->id, *parsed});
}

// ---- inputs -------------------------------------------------------------------------------

namespace {

/// sRGB hex, which is what a human types and reads. The conversion to scene-linear happens in
/// `Rgba` and nowhere else (ldoc/types.hpp), so this stays a pure spelling change.
QString hex_of(const ldoc::Rgba& rgba) {
    const auto c = rgba.to_srgb8();
    return QStringLiteral("#%1%2%3")
        .arg(c[0], 2, 16, QLatin1Char('0'))
        .arg(c[1], 2, 16, QLatin1Char('0'))
        .arg(c[2], 2, 16, QLatin1Char('0'));
}

/// The other direction. Returns nothing for anything it cannot read, rather than black: a typo
/// in a hex field silently turning a card black is the kind of thing nobody notices until the
/// show.
std::optional<ldoc::Rgba> hex_to_rgba(const QString& text) {
    QString hex = text.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex = hex.mid(1);
    if (hex.size() != 6 && hex.size() != 8)
        return std::nullopt;
    bool ok = false;
    const uint packed = hex.toUInt(&ok, 16);
    if (!ok)
        return std::nullopt;
    if (hex.size() == 6) {
        return ldoc::Rgba::from_srgb8(static_cast<std::uint8_t>((packed >> 16) & 0xFFu),
                                      static_cast<std::uint8_t>((packed >> 8) & 0xFFu),
                                      static_cast<std::uint8_t>(packed & 0xFFu));
    }
    return ldoc::Rgba::from_srgb8(static_cast<std::uint8_t>((packed >> 24) & 0xFFu),
                                  static_cast<std::uint8_t>((packed >> 16) & 0xFFu),
                                  static_cast<std::uint8_t>((packed >> 8) & 0xFFu),
                                  static_cast<std::uint8_t>(packed & 0xFFu));
}

QString swatch_of(const ldoc::Source& src) {
    const auto it = src.settings.find("color");
    if (it == src.settings.end())
        return {};
    const auto* rgba = std::get_if<ldoc::Rgba>(&it->second);
    return rgba ? hex_of(*rgba) : QString{};
}

/// One row of a source type's settings panel.
///
/// A table rather than a class per type. Ten of these are a description of what a source *is*,
/// and a description is easier to read, review and extend as data than as code -- the render
/// process will eventually be handed exactly these keys, and a table is what can be compared
/// against what it accepts.
struct FieldSpec {
    /// The key in `Source::settings`. Empty for a note.
    const char* key;
    /// What the field is called -- or, for a note, the whole line of prose.
    const char* label;
    /// "text", "path", "bool", "int", "choice", "colour", or "note".
    const char* kind;
    /// The default for text, path, choice and colour.
    const char* text = "";
    bool flag = false;
    std::int64_t number = 0;
    /// A "choice" field's options, separated by '|'.
    const char* choices = "";
    /// Printed after an "int" field: a unit, not a value.
    const char* suffix = "";
};

// A note is honest about what this build cannot do, placed above the field it is about rather
// than collected at the top where nobody reads it.

constexpr FieldSpec kMedia[] = {
    {"localFile", "File", "path", "", false, 0, "", ""},
    {"ignoreAudio", "Ignore Audio", "bool", "", false},
    {"useCache", "Use caching", "bool", "", true},
    // Looping by default is not a preference. `check_emergency_scene` reads this key by name
    // and warns when it is off, and a clip that plays once is a safety net that expires.
    {"loop", "Loop", "bool", "", true},
    {"",
     "A media source in an emergency scene has to loop, or the safety net runs out part "
     "way through the emergency (ADR-0025).",
     "note"},
    {"clearOnEnd", "Show nothing when playback ends", "bool", "", true},
    {"hardwareDecode", "Use hardware decoding when available", "bool", "", true},
    {"restartOnActivate", "Restart playback when source becomes active", "bool", "", true},
    {"closeWhenInactive", "Close file when inactive (slower scene switches)", "bool", "", false},
    {"", "Transparent VP8/VP9 WebM uses software decoding to preserve alpha.", "note"},
    {"speedPercent", "Speed", "int", "", false, 100, "", "%"},
    {"width", "Output width", "int", "", false, 1920, "", "px"},
    {"height", "Output height", "int", "", false, 1080, "", "px"},
    {"", "Inactive media stays loaded by default. Video keeps its aspect ratio inside the output box.",
     "note"},
};

constexpr FieldSpec kSlideshow[] = {
    {"files", "Image paths (one per line)", "multiline"},
    {"useCache", "Use caching", "bool", "", true},
    {"intervalMs", "Slide duration", "int", "", false, 5000, "", "ms"},
    {"loop", "Loop", "bool", "", true},
    {"width", "Output width", "int", "", false, 1920, "", "px"},
    {"height", "Output height", "int", "", false, 1080, "", "px"},
};

constexpr FieldSpec kPlaylist[] = {
    {"files", "Media paths (one per line)", "multiline"},
    {"ignoreAudio", "Ignore Audio", "bool", "", false},
    {"useCache", "Use caching", "bool", "", true},
    {"loop", "Loop", "bool", "", true},
    {"clearOnEnd", "Show nothing when playback ends", "bool", "", true},
    {"hardwareDecode", "Use hardware decoding when available", "bool", "", true},
    {"restartOnActivate", "Restart playback when source becomes active", "bool", "", true},
    {"closeWhenInactive", "Close file when inactive (slower scene switches)", "bool", "", false},
    {"", "Transparent VP8/VP9 WebM uses software decoding to preserve alpha.", "note"},
    {"speedPercent", "Speed", "int", "", false, 100, "", "%"},
    {"width", "Output width", "int", "", false, 1920, "", "px"},
    {"height", "Output height", "int", "", false, 1080, "", "px"},
};

constexpr FieldSpec kImage[] = {
    {"file", "Image file", "path"},
    {"useCache", "Use caching", "bool", "", true},
    {"unloadWhenHidden", "Unload the image when it is not showing", "bool", "", false},
    {"alphaLinear", "Apply alpha in linear space", "bool", "", false},
};

constexpr FieldSpec kText[] = {
    {"text", "Text", "multiline"},
    {"font", "Font", "choice", "Sans"},
    {"size", "Size", "int", "", false, 48, "", "px"},
    {"color", "Colour", "colour", "#ffffff"},
    {"alignment", "Alignment", "choice", "Left", false, 0, "Left|Centre|Right"},
    {"width", "Output width", "int", "", false, 1920, "", "px"},
    {"height", "Output height", "int", "", false, 1080, "", "px"},
};

constexpr FieldSpec kColour[] = {
    {"color", "Colour", "colour", "#3b82f6"},
    {"width", "Width", "int", "", false, 1920, "", "px"},
    {"height", "Height", "int", "", false, 1080, "", "px"},
};

// Which screen, and which window, are chosen by the portal picker at capture time and cannot be
// chosen here: a Wayland client is not told what is on the machine and may not pick for itself
// (ADR-0003). That is a property of the platform, not a gap in this build, so it is stated as
// such rather than left as an empty dropdown.
constexpr FieldSpec kScreen[] = {
    // The note this replaces said a Wayland client cannot choose a screen for itself. That was
    // true when the portal was the only backend; ADR-0011 then measured both and made
    // ext-image-copy-capture-v1 primary, and it names an output directly with no picker.
    {"", "Which monitor to capture. The list is what the compositor says is connected right "
         "now, so unplugging one changes it; a project naming a screen that has gone keeps the "
         "name and reports the source as not drawn rather than quietly capturing another "
         "(ADR-0041).", "note"},
    // Filled at runtime by screen_choices(): the options are monitors, which no compile-time
    // table can know. The stored value stays the bare connector ("DP-1") rather than the label
    // shown here, because that is what the capture path resolves and what survives a restart.
    {"output", "Screen", "choice"},
    {"showCursor", "Show the cursor", "bool", "", true},
    {"pauseWhenHidden", "Stop capturing while the source is not on screen", "bool", "", true},
};

constexpr FieldSpec kWindow[] = {
    {"", "Capture a window through the Wayland compositor. Game Capture uses this same path. "
         "Choose it again after the application closes and reopens.", "note"},
    {"window", "Window", "choice"},
    {"showCursor", "Show the cursor", "bool", "", true},
};

constexpr FieldSpec kCamera[] = {
    {"", "V4L2 video capture. Video reconnect after unplugging is not available yet.", "note"},
    {"deactivateWhenHidden", "Deactivate when not showing", "bool", "", false},
    {"device", "Video device", "choice", ""},
    {"audioDevice", "Audio input", "choice", "default"},
    {"",
     "Default matches this capture card's audio, never the system microphone. None disables audio. "
     "Remove any separate audio source using the same input to avoid doubling the mix.",
     "note"},
    {"videoFormat", "Video format", "choice", "Auto", false, 0, "Auto"},
    {"resolution", "Resolution", "choice", "Leave unchanged", false, 0,
     "Leave unchanged|3840x2160|2560x1440|1920x1080|1280x720|960x540|640x480"},
    {"frameRate", "Frame rate", "choice", "Leave unchanged", false, 0,
     "Leave unchanged|60|59.94|50|30|29.97|25|24"},
    {"colourRange", "Colour range", "choice", "Default", false, 0, "Default|Partial|Full"},
    {"buffering", "Use buffering", "bool", "", true},
    {"",
     "Buffering reads queued device frames in order. Turn it off to discard older queued frames "
     "before conversion.",
     "note"},
    {"autoReset", "Autoreset on timeout", "bool", "", false},
    {"timeoutFrames", "Frames until timeout", "int", "", false, 5, "", "frames"},
};

constexpr FieldSpec kBrowser[] = {
    {"url", "URL", "text", "https://"},
    {"ignoreAudio", "Ignore Audio", "bool", "", false},
    {"", "A browser engine runs as a worker process of its own and is never loaded into ours "
         "(ADR-0004), so a page that wedges costs this source and nothing else.", "note"},
    {"width", "Width", "int", "", false, 1920, "", "px"},
    {"height", "Height", "int", "", false, 1080, "", "px"},
    {"fps", "Frame rate", "int", "", false, 30, "", "fps"},
    {"css", "Custom CSS", "multiline"},
    {"transparentBackground", "Transparent background", "bool", "", true},
    {"", "The page reloads when shown. Unless Ignore Audio is enabled, its audio uses a private "
         "sound-server output and enters the mixer.", "note"},
};

constexpr FieldSpec kClone[] = {
    {"target", "Clone source", "choice"},
    {"audio", "Audio", "bool", "", false},
    {"noFilters", "Bypass input video filters", "bool", "", false},
    {"", "Shares the input capture. Clone filters, transform and audio routing are independent. "
         "Missing inputs render transparently. Audio defaults off to avoid doubling the mix.", "note"},
};

constexpr FieldSpec kAudioInput[] = {
    {"device", "Input device", "choice", "default"},
    {"", "Captures stereo PCM at 48 kHz. Input meters show the signal before fader changes. "
         "Use Monitor to hear it locally; bus gain, mute and sends control the mix.", "note"},
};

constexpr FieldSpec kBlackmagic[] = {
    {"deviceIndex", "Device index", "int", "", false, 0},
    {"mode", "Display mode name", "text", "HD 1080p60"},
    {"width", "Output width", "int", "", false, 1920, "", "px"},
    {"height", "Output height", "int", "", false, 1080, "", "px"},
    {"", "Requires Blackmagic Desktop Video and a capture device. Mode name must match the "
         "device's supported progressive mode. SDR video and stereo audio; hardware validation pending.", "note"},
};

constexpr FieldSpec kAudioOutput[] = {
    {"device", "Output monitor", "choice", "default"},
    {"", "Captures the selected output's monitor at 48 kHz. Input meters show the signal "
         "before fader changes. Monitor plays locally; avoid monitoring the output being captured.", "note"},
};

/// Connector names for every monitor the compositor currently advertises, in its own order.
///
/// The bare connector ("DP-1") rather than a decorated label, because this is both what the
/// dropdown shows and what gets stored: a label with a resolution in it would become wrong the
/// moment the mode changed, and would not resolve on the capture side (ADR-0041).
/// The monitor list, re-read at most every half second.
///
/// `natural_size` below needs a screen's resolution to give the node a rectangle, and it runs for
/// every node every time the overlay is laid out. Opening a Wayland connection at that rate would
/// be absurd, and caching for ever would miss a monitor being plugged in. Half a second is slower
/// than any repaint and faster than anyone can plug a cable and reach for the mouse.
const std::vector<libertas::capture::WlOutputInfo>& screens_now() {
    static std::vector<libertas::capture::WlOutputInfo> cached;
    static QElapsedTimer age;
    if (age.isValid() && age.elapsed() < 500)
        return cached;
    std::string error;
    cached = libertas::capture::list_screens(error);
    if (cached.empty() && !error.empty())
        lc::warn("cannot list screens: {}", error);
    age.start();
    return cached;
}

QStringList screen_connectors() {
    QStringList out;
    for (const libertas::capture::WlOutputInfo& o : screens_now())
        out.append(QString::fromStdString(o.name));
    return out;
}

/// The size of the screen a source names, or of the first one when it names none (ADR-0041).
///
/// Nothing when that connector is not connected: the source then has no rectangle, which is the
/// same answer an image with no readable file gives, and the picture reports it as not drawn
/// rather than inventing a size for a monitor that is gone.
bool screen_size(const ldoc::Source& src, double& width, double& height) {
    const auto it = src.settings.find("output");
    const auto* wanted = it == src.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    const std::vector<libertas::capture::WlOutputInfo>& all = screens_now();
    if (all.empty())
        return false;
    const libertas::capture::WlOutputInfo* chosen = nullptr;
    if (wanted == nullptr || wanted->empty()) {
        chosen = &all.front();
    } else {
        for (const libertas::capture::WlOutputInfo& o : all)
            if (o.name == *wanted)
                chosen = &o;
    }
    if (chosen == nullptr || chosen->width <= 0 || chosen->height <= 0)
        return false;
    width = chosen->width;
    height = chosen->height;
    return true;
}

const std::vector<libertas::capture::VideoDevice>& videos_now() {
    static std::vector<libertas::capture::VideoDevice> cached;
    static QElapsedTimer age;
    if (!age.isValid() || age.elapsed() >= 500) {
        cached = libertas::capture::list_video_devices();
        age.start();
    }
    return cached;
}

const std::vector<libertas::capture::WlWindowInfo>& windows_now() {
    static std::vector<libertas::capture::WlWindowInfo> cached;
    static QElapsedTimer age;
    if (!age.isValid() || age.elapsed() >= 500) {
        std::string error;
        cached = libertas::capture::list_windows(error);
        age.start();
    }
    return cached;
}

bool window_size(const ldoc::Source& source, double& width, double& height) {
    const auto it = source.settings.find("window");
    const auto* id = it == source.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    if (!id || id->empty()) return false;
    struct Size { QElapsedTimer age; int width = 0, height = 0; };
    static std::map<std::string, Size> sizes;
    if (sizes.size() > 128) sizes.clear();
    auto& size = sizes[*id];
    if (!size.age.isValid() || size.age.elapsed() >= 500) {
        std::string error;
        (void)libertas::capture::window_size(*id, size.width, size.height, error);
        size.age.start();
    }
    width = size.width; height = size.height;
    return width > 0 && height > 0;
}

bool video_size(const ldoc::Source& src, double& width, double& height) {
    const auto size = src.settings.find("resolution");
    if (size != src.settings.end()) {
        if (const auto* text = std::get_if<std::string>(&size->second)) {
            const auto parts = QString::fromStdString(*text).split('x');
            if (parts.size() == 2) {
                bool ok_w = false, ok_h = false;
                const int w = parts[0].toInt(&ok_w), h = parts[1].toInt(&ok_h);
                if (ok_w && ok_h && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
                    width = w; height = h; return true;
                }
            }
        }
    }
    const auto it = src.settings.find("device");
    const auto* path = it == src.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    if (path) {
        for (const auto& device : videos_now()) {
            std::error_code ec;
            if (device.path == *path || std::filesystem::equivalent(device.path, *path, ec)) {
                width = device.width; height = device.height;
                return width > 0 && height > 0;
            }
        }
    }
    return false;
}

std::span<const FieldSpec> schema_for(std::string_view type) {
    if (type == "source.clone")        return kClone;
    if (type == "source.media")        return kMedia;
    if (type == "source.slideshow")    return kSlideshow;
    if (type == "source.playlist")     return kPlaylist;
    if (type == "source.image")        return kImage;
    if (type == "source.text")         return kText;
    if (type == "source.color")        return kColour;
    if (type == "source.screen")       return kScreen;
    if (type == "source.window" || type == "source.game") return kWindow;
    if (type == "source.v4l2")         return kCamera;
    if (type == "source.blackmagic")   return kBlackmagic;
    if (type == "source.browser")      return kBrowser;
    if (type == "source.audio-input")  return kAudioInput;
    if (type == "source.audio-output") return kAudioOutput;
    // A type this build does not know -- a project from a later one. No fields rather than a
    // guess at them, and the dialog says so on its face instead of offering an empty grid.
    return {};
}

bool is_note(const FieldSpec& f) { return std::string_view{f.kind} == "note"; }

QVariant default_of(const FieldSpec& f) {
    const std::string_view kind{f.kind};
    if (kind == "bool")
        return f.flag;
    if (kind == "int")
        return QVariant::fromValue(static_cast<qlonglong>(f.number));
    return QString::fromUtf8(f.text);
}

/// A stored value read back as the kind the schema declares, or invalid when it is stored as
/// something else entirely -- a hand-edited project, or a key a later build meant differently.
QVariant stored_as(const ldoc::ParamValue& v, std::string_view kind) {
    if (kind == "bool") {
        const auto* b = std::get_if<bool>(&v);
        return b ? QVariant{*b} : QVariant{};
    }
    if (kind == "int") {
        if (const auto* i = std::get_if<std::int64_t>(&v))
            return QVariant::fromValue(static_cast<qlonglong>(*i));
        if (const auto* d = std::get_if<double>(&v))
            return QVariant::fromValue(static_cast<qlonglong>(std::llround(*d)));
        return {};
    }
    if (kind == "colour") {
        const auto* c = std::get_if<ldoc::Rgba>(&v);
        return c ? QVariant{hex_of(*c)} : QVariant{};
    }
    const auto* s = std::get_if<std::string>(&v);
    return s ? QVariant{qstr(*s)} : QVariant{};
}

/// What a brand-new source of this type is: every field the schema names, written out.
///
/// Not left sparse. The project format stores defaults rather than omitting them (ldoc's
/// `ParamValue`), precisely so that a later build changing its mind about a default cannot
/// silently change what an existing project means. A source created with an empty settings map
/// is the sparse case arriving by the back door.
ldoc::ParamMap default_settings(std::string_view type) {
    ldoc::ParamMap out;
    for (const FieldSpec& f : schema_for(type)) {
        if (is_note(f))
            continue;
        const std::string_view kind{f.kind};
        if (kind == "bool")
            out.emplace(f.key, f.flag);
        else if (kind == "int")
            out.emplace(f.key, f.number);
        else if (kind == "colour")
            out.emplace(f.key, hex_to_rgba(QString::fromLatin1(f.text)).value_or(ldoc::Rgba{}));
        else
            out.emplace(f.key, std::string{f.text});
    }
    if (type == "source.v4l2" && !videos_now().empty())
        out["device"] = videos_now().front().path;
    return out;
}

} // namespace


QVariantList Document::addableKinds() const {
    QVariantList out = sourceTypes();
    // Last, after the source types, because it is the odd one: picking it adds a Precomp node
    // rather than a source (ADR-0034). Listed here at all because this dialog answers "what
    // goes in a scene", and another scene is one of the answers -- where OBS puts it too.
    QVariantMap scene;
    scene["id"] = QStringLiteral("scene");
    scene["title"] = QStringLiteral("Scene");
    scene["icon"] = QString::fromUtf8("\u25F0");
    scene["selfContained"] = false;
    out.append(scene);
    return out;
}

QVariantList Document::sourceTypes() const {
    struct Entry {
        const char* id;
        const char* title;
        /// One glyph, drawn where OBS draws an icon. Chosen from what DejaVu and Noto both
        /// carry, so a missing font does not leave a row of empty boxes.
        const char* icon;
    };
    // Ordered so the ones that are safe in an emergency scene come first, since that is the
    // list an operator is reading when it matters most (ADR-0025).
    static constexpr Entry kTypes[] = {
        {"source.media", "Media File", "\u25B6"},
        {"source.slideshow", "Image Slide Show", "\u25A8"},
        {"source.playlist", "Media Playlist (VLC equivalent)", "\u25B6"},
        {"source.image", "Image", "\u25A8"},
        {"source.clone", "Source Clone", "\u29C9"},
        {"source.text", "Text", "T"},
        {"source.color", "Colour", "\u25A0"},
        {"source.screen", "Screen", "\u25AD"},
        {"source.window", "Window", "\u2750"},
        {"source.game", "Game Capture (Wayland window)", "\u2750"},
        {"source.v4l2", "Video Capture", "\u25C9"},
        {"source.blackmagic", "Blackmagic Device", "\u25C9"},
        {"source.browser", "Browser", "\u25CD"},
        {"source.audio-input", "Audio Input", "\u266A"},
        {"source.audio-output", "Audio Output", "\u266B"},
    };
    QVariantList out;
    for (const Entry& e : kTypes) {
        QVariantMap m;
        m["id"] = QString::fromLatin1(e.id);
        m["title"] = QString::fromLatin1(e.title);
        m["icon"] = QString::fromUtf8(e.icon);
        m["selfContained"] = ldoc::is_self_contained(e.id);
        out.append(m);
    }
    return out;
}

namespace {

/// Where a new source node goes: inside the selected group if one is selected, otherwise at the
/// top.
struct Placement {
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
};

// ponytail: placements carry the shared route; use a source field if unplaced sources need one.
const ldoc::AudioRouting* source_audio(const ldoc::Project& project, const ldoc::Id& source) {
    const ldoc::AudioRouting* result = nullptr;
    for (const auto& scene : project.scenes)
        ldoc::visit(scene.roots, [&](const ldoc::Node& node) {
            const auto* placed = std::get_if<ldoc::SourceData>(&node.data);
            if (!result && placed && placed->source == source)
                result = &placed->audio;
        });
    return result;
}

} // namespace

void Document::addNewSourceToScene(const QString& name, const QString& type, bool visible) {
    const ldoc::Scene* scene = current_scene();
    if (!scene) {
        emit notice(QStringLiteral("There is no scene to add a source to."));
        return;
    }
    if (type.isEmpty())
        return;

    const ldoc::Project& p = session_->project();

    ldoc::Source src;
    src.id = ldoc::Id::generate();
    src.name = name.isEmpty() ? std::string{"Source"} : to_std(name);
    src.type = to_std(type);
    // Fully configured from the moment it exists, from the same table the gear's panel draws
    // from. That is what gives a colour its real swatch for the add dialog to show, and what
    // makes a media source loop -- which the emergency scene needs (ADR-0025) and which the
    // check for one looks for by name.
    src.settings = default_settings(src.type);
    const ldoc::Id source_id = src.id;

    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, src.name);
    node.visible = visible;
    if (src.type == "source.v4l2" && ldoc::has_audio(src))
        for (const auto& bus : p.buses)
            if (bus.kind == ldoc::BusKind::Program) {
                std::get<ldoc::SourceData>(node.data).audio.bus = bus.id;
                break;
            }
    const ldoc::Id node_id = node.id;

    Placement where;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::make_batch({ldoc::AddSource{std::move(src), p.sources.size()},
                          ldoc::AddNode{scene->id, where.parent, where.index, std::move(node)},
                          ldoc::SetNodeSource{scene->id, node_id, source_id}},
                         std::format("Add {}", name.isEmpty() ? std::string{"source"}
                                                              : to_std(name))));
    selectNode(id_str(node_id));
}

void Document::addExistingSourceToScene(const QString& sourceId, bool visible) {
    const ldoc::Scene* scene = current_scene();
    const auto parsed = parse_id(sourceId);
    if (!scene || !parsed)
        return;
    const ldoc::Source* existing = session_->project().source(*parsed);
    if (!existing) {
        emit notice(QStringLiteral("That source is no longer in the project."));
        return;
    }

    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, existing->name);
    node.visible = visible;
    if (const auto* audio = source_audio(session_->project(), existing->id))
        std::get<ldoc::SourceData>(node.data).audio.bus = audio->bus;
    else if (existing->type == "source.v4l2" && ldoc::has_audio(*existing))
        for (const auto& bus : session_->project().buses)
            if (bus.kind == ldoc::BusKind::Program) {
                std::get<ldoc::SourceData>(node.data).audio.bus = bus.id;
                break;
            }
    const ldoc::Id node_id = node.id;

    Placement where;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::make_batch({ldoc::AddNode{scene->id, where.parent, where.index, std::move(node)},
                          ldoc::SetNodeSource{scene->id, node_id, *parsed}},
                         std::format("Add {}", existing->name)));
    selectNode(id_str(node_id));
}


QVariantMap Document::source_tile(const ldoc::Source& src) const {
    const ldoc::Project& p = session_->project();
    QVariantMap m;
    m["id"] = id_str(src.id);
    m["name"] = qstr(src.name);
    m["type"] = qstr(src.type);
    m["selfContained"] = ldoc::is_self_contained(src.type);
    m["swatch"] = swatch_of(src);

    QString icon;
    QString type_title = qstr(src.type);
    for (const QVariant& v : sourceTypes()) {
        const QVariantMap entry = v.toMap();
        if (entry.value("id").toString() == qstr(src.type)) {
            icon = entry.value("icon").toString();
            type_title = entry.value("title").toString();
        }
    }
    m["icon"] = icon;
    m["typeTitle"] = type_title;

    int uses = 0;
    for (const ldoc::Scene& sc : p.scenes) {
        ldoc::visit(sc.roots, [&](const ldoc::Node& n) {
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (placed && placed->source && *placed->source == src.id)
                ++uses;
        });
    }
    m["useCount"] = uses;
    return m;
}

QVariantList Document::sourcesOfType(const QString& type) const {
    QVariantList out;
    for (const ldoc::Source& src : session_->project().sources) {
        if (qstr(src.type) == type)
            out.append(source_tile(src));
    }
    return out;
}

QVariantList Document::recentSources(int limit) const {
    const auto& sources = session_->project().sources;
    QVariantList out;
    // Newest first. Sources are appended, so the tail reversed is the real creation order
    // rather than a guess at one.
    for (auto it = sources.rbegin(); it != sources.rend(); ++it) {
        if (limit > 0 && out.size() >= limit)
            break;
        out.append(source_tile(*it));
    }
    return out;
}

// ---- a source's own settings ------------------------------------------------------------


QVariantMap Document::screenDetails(const QString& connector) const {
    QVariantMap out;
    const std::string wanted = to_std(connector);
    const std::vector<libertas::capture::WlOutputInfo>& all = screens_now();
    if (all.empty())
        return out;
    const libertas::capture::WlOutputInfo* chosen = nullptr;
    // Empty means the first screen, the same rule the capture path follows (ADR-0041), so the
    // dialog shows what would actually be captured rather than a blank.
    if (wanted.empty()) {
        chosen = &all.front();
    } else {
        for (const libertas::capture::WlOutputInfo& o : all)
            if (o.name == wanted)
                chosen = &o;
    }
    if (chosen == nullptr)
        return out;
    out["name"] = QString::fromStdString(chosen->name);
    out["description"] = QString::fromStdString(chosen->description);
    out["width"] = chosen->width;
    out["height"] = chosen->height;
    out["hz"] = chosen->refresh_mhz / 1000.0;
    return out;
}

bool Document::hasLiveSource() const {
    const ldoc::Project& project = session_->project();
    for (const auto& deck : project.decks)
        if (deck.transition.starts_with("transition.stinger:")) return true;
    // Walks every deck's program *and* preview scene, because any canvas being live is enough to
    // keep the preview republishing -- the renderer composites all of them on one commit. The
    // preview scene counts for the same reason the program one does: it is on screen, so a
    // countdown sitting in the scene being built has to tick there too, not only once it is cut
    // to air.
    std::vector<const ldoc::Scene*> watched;
    for (const ldoc::Deck& deck : project.decks)
        for (const auto& which : {deck.program_scene, deck.preview_scene})
            if (which)
                if (const ldoc::Scene* found = project.scene(*which))
                    watched.push_back(found);
    for (const ldoc::Scene* scene : watched) {
        bool live = false;
        std::unordered_set<ldoc::Id> visited{scene->id};
        const auto walk = [&](const auto& self, const ldoc::Node& node) -> void {
            if (live || !node.visible)
                return;
            if (const auto* kids = node.children()) {
                for (const ldoc::Node& child : *kids)
                    self(self, child);
                return;
            }
            if (const auto* pre = std::get_if<ldoc::PrecompData>(&node.data)) {
                if (visited.insert(pre->scene).second)
                    if (const auto* nested = project.scene(pre->scene))
                        for (const auto& child : nested->roots)
                            self(self, child);
                return;
            }
            const auto* source = std::get_if<ldoc::SourceData>(&node.data);
            const ldoc::Source* src =
                source && source->source ? project.source(*source->source) : nullptr;
            const auto resolved = src ? ldoc::resolve_source(project, *src) : std::nullopt;
            if (resolved)
                src = &*resolved;
            if (src && (is_live_source(src->type) ||
                        lcomp::timer_flag(src->settings, "countdown.enabled")))
                live = true;
        };
        for (const ldoc::Node& root : scene->roots)
            walk(walk, root);
        if (live)
            return true;
    }
    return false;
}

QVariantMap Document::videoFormatChoices(const QString& device) const {
    QStringList choices{QStringLiteral("Auto")};
    QVariantMap labels{{QStringLiteral("Auto"), QStringLiteral("Auto")}};
    for (const auto& format : libertas::capture::list_video_formats(to_std(device))) {
        const auto code = QString::fromStdString(format.code);
        choices.append(code);
        labels[code] = QString::fromStdString(format.name) + QStringLiteral(" (%1)").arg(code);
    }
    return {{"choices", choices}, {"choiceLabels", labels}};
}

QVariantList Document::sourceSettingsSchema(const QString& type) const {
    QVariantList out;
    for (const FieldSpec& f : schema_for(to_std(type))) {
        QVariantMap m;
        m["kind"] = QString::fromLatin1(f.kind);
        m["key"] = QString::fromLatin1(f.key);
        m["label"] = QString::fromUtf8(f.label);
        m["value"] = is_note(f) ? QVariant{QString{}} : default_of(f);
        m["suffix"] = QString::fromLatin1(f.suffix);
        if (type == QLatin1String("source.v4l2") && std::string_view{f.key} == "timeoutFrames") {
            m["minimum"] = 2;
            m["maximum"] = 120;
        }
        const QString choices = QString::fromLatin1(f.choices);
        QStringList options = choices.isEmpty() ? QStringList{} : choices.split(QLatin1Char('|'));
        if (type == QLatin1String("source.clone") && std::string_view{f.key} == "target") {
            QVariantMap labels;
            options.append(QString{});
            labels[QString{}] = QStringLiteral("None");
            for (const auto& source : session_->project().sources) {
                options.append(id_str(source.id));
                labels[id_str(source.id)] = qstr(source.name);
            }
            m["choiceLabels"] = labels;
        }
        if (type == QLatin1String("source.text") && std::string_view{f.key} == "font") {
            options = QFontDatabase::families();
            if (!options.contains(QStringLiteral("Sans")))
                options.prepend(QStringLiteral("Sans"));
        }
        // The screen field's options are monitors, which no compile-time table can know. Asked
        // when the dialog is opened rather than cached, because a monitor can be plugged in
        // between one opening and the next and a stale list would offer a screen that is gone.
        if (std::string_view{f.key} == "output" && type == QLatin1String("source.screen")) {
            options.clear();
            for (const QString& name : screen_connectors())
                options.append(name);
            // Nothing connected, or a compositor with no capture protocol. An empty dropdown
            // with the note above it reads as "none to choose", which is the truth; inventing a
            // placeholder row would offer a screen that cannot be captured.
            m["value"] = options.isEmpty() ? QString{} : options.front();
        }
        if (std::string_view{f.key} == "device" && type == QLatin1String("source.v4l2")) {
            options.clear();
            for (const auto& device : videos_now()) options.append(QString::fromStdString(device.path));
            m["value"] = options.isEmpty() ? QString{} : options.front();
        }
        if (std::string_view{f.key} == "window") {
            QVariantMap labels;
            for (const auto& window : windows_now()) {
                const auto id = QString::fromStdString(window.id);
                options.append(id);
                labels[id] = QString::fromStdString(window.title + " [" + window.app_id + "]");
            }
            m["choiceLabels"] = labels;
        }
        if ((std::string_view{f.key} == "device" &&
             (type == QLatin1String("source.audio-input") ||
              type == QLatin1String("source.audio-output"))) ||
            (type == QLatin1String("source.v4l2") && std::string_view{f.key} == "audioDevice")) {
            options = {QStringLiteral("default")};
            QVariantMap labels;
            labels["default"] = QStringLiteral("System default");
            if (type == QLatin1String("source.v4l2")) {
                options.prepend(QStringLiteral("none"));
                labels["none"] = QStringLiteral("None");
                labels["default"] = QStringLiteral("Default (matching capture device)");
            }
            std::string error;
            for (const auto& device : libertas::capture::list_audio_devices(error)) {
                if (device.monitor != (type == QLatin1String("source.audio-output"))) continue;
                const auto id = QString::fromStdString(device.id);
                options.append(id);
                labels[id] = QString::fromStdString(device.name);
            }
            m["choiceLabels"] = labels;
        }
        m["choices"] = options;
        out.append(m);
    }
    return out;
}

QVariantMap Document::sourceDetails(const QString& id) const {
    const auto parsed = parse_id(id);
    const ldoc::Source* src = parsed ? session_->project().source(*parsed) : nullptr;
    if (!src)
        return {};

    QVariantMap m = source_tile(*src);
    QVariantMap settings;
    for (const FieldSpec& f : schema_for(src->type)) {
        if (is_note(f))
            continue;
        QVariant v;
        if (const auto it = src->settings.find(f.key); it != src->settings.end())
            v = stored_as(it->second, f.kind);
        // Absent, or stored as something this field cannot be: either way the default shows,
        // so every field is always editable rather than blank and unexplained.
        settings[QString::fromLatin1(f.key)] =
            v.isValid() ? v
                        : (src->type == "source.v4l2" && std::string_view{f.key} == "audioDevice"
                               ? QVariant{QStringLiteral("none")}
                               : default_of(f));
    }
    m["settings"] = settings;
    m["fieldCount"] = static_cast<int>(settings.size());
    return m;
}

QStringList Document::sourceAssetPaths(const QString& id) const {
    const auto parsed = parse_id(id);
    const ldoc::Source* src = parsed ? session_->project().source(*parsed) : nullptr;
    return src ? source_asset_paths(*src) : QStringList{};
}

void Document::applySourceSettings(const QString& id, const QString& name,
                                   const QVariantMap& values) {
    const auto parsed = parse_id(id);
    const ldoc::Source* src = parsed ? session_->project().source(*parsed) : nullptr;
    if (!src) {
        emit notice(QStringLiteral("That source is no longer in the project."));
        return;
    }

    ldoc::Source copy = *src;
    if (copy.type == "source.v4l2" && values.contains(QStringLiteral("audioDevice"))) {
        const auto device = values.value(QStringLiteral("audioDevice")).toString();
        if (device.isEmpty() || device.size() > 1024 || device.contains(QChar::Null)) {
            emit notice(QStringLiteral("Invalid capture audio input identity."));
            return;
        }
    }
    if (copy.type == "source.v4l2" && values.contains(QStringLiteral("timeoutFrames"))) {
        bool valid = false;
        const auto text = values.value(QStringLiteral("timeoutFrames")).toString();
        const auto frames = text.toLongLong(&valid);
        if (!valid || frames < 2 || frames > 120) {
            emit notice(QStringLiteral("Frames until timeout must be an integer from 2 to 120."));
            return;
        }
    }
    if (!name.trimmed().isEmpty())
        copy.name = to_std(name.trimmed());

    for (const FieldSpec& f : schema_for(copy.type)) {
        if (is_note(f))
            continue;
        const auto it = values.constFind(QString::fromLatin1(f.key));
        if (it == values.constEnd())
            continue;
        const std::string_view kind{f.kind};
        if (kind == "bool") {
            copy.settings.insert_or_assign(std::string{f.key}, it->toBool());
        } else if (kind == "int") {
            copy.settings.insert_or_assign(std::string{f.key},
                                           static_cast<std::int64_t>(it->toLongLong()));
        } else if (kind == "colour") {
            // Refused rather than rounded to something: see `hex_to_rgba`.
            if (const auto rgba = hex_to_rgba(it->toString()))
                copy.settings.insert_or_assign(std::string{f.key}, *rgba);
            else
                emit notice(QStringLiteral("'%1' is not a colour, so %2 was left as it was.")
                                .arg(it->toString(), QString::fromUtf8(f.label)));
        } else {
            copy.settings.insert_or_assign(std::string{f.key}, to_std(it->toString()));
        }
    }

    if (copy.type == "source.clone") {
        auto candidate = session_->project();
        for (auto& source : candidate.sources)
            if (source.id == copy.id)
                source = copy;
        const auto it = copy.settings.find("target");
        const auto* target = it == copy.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
        if (target && !target->empty() && !ldoc::resolve_source(candidate, copy)) {
            emit notice(QStringLiteral("Choose an existing source without a clone cycle."));
            return;
        }
    }

    // Nothing was actually changed -- the dialog was opened and accepted. An undo step for that
    // is a step the operator has to press Ctrl+Z through later to reach a real one.
    if (copy == *src)
        return;
    if ((copy.type == "source.v4l2" || copy.type == "source.clone") &&
        !ldoc::has_audio(*src) && ldoc::has_audio(copy)) {
        std::vector<ldoc::Command> steps;
        for (const auto& bus : session_->project().buses) {
            if (bus.kind != ldoc::BusKind::Program)
                continue;
            for (const auto& scene : session_->project().scenes)
                ldoc::visit(scene.roots, [&](const ldoc::Node& node) {
                    const auto* placed = std::get_if<ldoc::SourceData>(&node.data);
                    if (!placed || placed->source != copy.id || placed->audio.bus)
                        return;
                    auto routing = placed->audio;
                    routing.bus = bus.id;
                    steps.push_back(ldoc::SetNodeAudio{scene.id, node.id, std::move(routing)});
                });
            break;
        }
        steps.push_back(ldoc::SetSource{std::move(copy)});
        run(ldoc::make_batch(std::move(steps), "Enable capture audio"));
        return;
    }
    run(ldoc::SetSource{std::move(copy)});
}

void Document::addSource(const QString& name, const QString& type) {
    if (type.isEmpty())
        return;
    ldoc::Source src;
    src.id = ldoc::Id::generate();
    src.name = name.isEmpty() ? std::string{"Input"} : to_std(name);
    src.type = to_std(type);
    src.settings = default_settings(src.type);
    run(ldoc::AddSource{std::move(src), session_->project().sources.size()});
}

void Document::removeSource(const QString& id) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RemoveSource{*parsed});
}

QStringList Document::scenesUsing(const QString& sourceId) const {
    const auto parsed = parse_id(sourceId);
    if (!parsed)
        return {};
    QStringList out;
    for (const ldoc::Scene& s : session_->project().scenes) {
        bool used = false;
        ldoc::visit(s.roots, [&](const ldoc::Node& n) {
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (placed && placed->source && *placed->source == *parsed)
                used = true;
        });
        // Once per scene, not once per placement: the question the dialog is asking is which
        // scenes change, and "Live, Live, Live" answers it worse than "Live".
        if (used)
            out << qstr(s.name);
    }
    return out;
}

void Document::removeSourceAndUses(const QString& id) {
    const auto parsed = parse_id(id);
    const ldoc::Project& p = session_->project();
    const ldoc::Source* src = parsed ? p.source(*parsed) : nullptr;
    if (!src)
        return;

    std::vector<ldoc::Command> steps;
    for (const ldoc::Scene& s : p.scenes) {
        std::vector<ldoc::Id> doomed;
        ldoc::visit(s.roots, [&](const ldoc::Node& n) {
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (placed && placed->source && *placed->source == *parsed)
                doomed.push_back(n.id);
        });
        for (const ldoc::Id& node : doomed)
            steps.push_back(ldoc::RemoveNode{s.id, node});
    }
    steps.push_back(ldoc::RemoveSource{*parsed});
    run(ldoc::make_batch(std::move(steps),
                         std::format("Delete source {}", src->name)));
}

QVariantList Document::scenesForPrecomp() const {
    const ldoc::Scene* into = current_scene();
    if (!into)
        return {};
    const ldoc::Project& p = session_->project();

    // Would showing `candidate` here eventually come back to the scene being added to? The
    // command refuses a loop; this asks the same question early, so a scene that cannot be
    // added is never offered and then rejected.
    std::unordered_set<ldoc::Id> seen;
    const std::function<bool(const ldoc::Id&)> reaches_into = [&](const ldoc::Id& at) {
        if (at == into->id)
            return true;
        if (!seen.insert(at).second)
            return false;
        const ldoc::Scene* s = p.scene(at);
        if (!s)
            return false;
        bool found = false;
        ldoc::visit(s->roots, [&](const ldoc::Node& n) {
            if (found)
                return;
            if (const auto* pre = std::get_if<ldoc::PrecompData>(&n.data))
                found = reaches_into(pre->scene);
        });
        return found;
    };

    QVariantList out;
    for (const ldoc::Scene& s : p.scenes) {
        if (s.id == into->id)
            continue;
        seen.clear();
        if (reaches_into(s.id))
            continue;
        // Shaped like a source tile, because it is drawn by the same delegate. The fields it
        // would otherwise lack are filled rather than left absent: an undeclared field renders
        // as the literal word "undefined", which the shell has shipped once already.
        QVariantMap m;
        m["id"] = id_str(s.id);
        m["name"] = qstr(s.name);
        const ldoc::Canvas* c = p.canvas(s.canvas);
        m["canvasName"] = c ? qstr(c->name) : QString{};
        m["type"] = QStringLiteral("scene");
        m["typeTitle"] = c ? qstr(c->name) : QStringLiteral("Scene");
        m["icon"] = QString::fromUtf8("\u25F0");
        m["swatch"] = QString{};
        m["selfContained"] = false;
        // How many *other* scenes already show this one, so an overlay in use reads as one.
        int shown_by = 0;
        for (const ldoc::Scene& other : p.scenes) {
            bool uses = false;
            ldoc::visit(other.roots, [&](const ldoc::Node& n) {
                if (const auto* pre = std::get_if<ldoc::PrecompData>(&n.data))
                    uses = uses || pre->scene == s.id;
            });
            if (uses)
                ++shown_by;
        }
        m["useCount"] = shown_by;
        out.append(m);
    }
    return out;
}

void Document::addSceneToScene(const QString& sceneId, bool visible) {
    const ldoc::Scene* into = current_scene();
    const auto parsed = parse_id(sceneId);
    if (!into || !parsed)
        return;
    const ldoc::Scene* shown = session_->project().scene(*parsed);
    if (!shown) {
        emit notice(QStringLiteral("That scene is no longer in the project."));
        return;
    }

    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Precomp, shown->name);
    node.visible = visible;
    ldoc::PrecompData data;
    data.scene = shown->id;
    // size stays (0,0): "the size of the canvas this is composited into", which is what lets
    // an overlay scene authored at one size sit correctly on a canvas of another.
    if (const ldoc::Canvas* c = session_->project().canvas(shown->canvas))
        data.color_space = c->color_space;
    node.data = std::move(data);
    const ldoc::Id node_id = node.id;

    Placement where;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::AddNode{into->id, where.parent, where.index, std::move(node)});
    selectNode(id_str(node_id));
}

namespace {

/// Whether `id` sits inside one of `chosen` other than itself.
///
/// Group, copy and delete all need it: a node already travelling inside a selected parent must
/// not be acted on twice. Dropping it is right rather than refusing the gesture -- selecting a
/// group and something in it plainly means "all of this" (ADR-0037).
bool inside_any(const ldoc::Scene& scene, const QStringList& chosen, const ldoc::Id& id) {
    for (const QString& other : chosen) {
        const auto other_id = ldoc::Id::parse(other.toStdString());
        if (!other_id || *other_id == id)
            continue;
        const ldoc::Node* o = ldoc::find_node(scene.roots, *other_id);
        if (!o)
            continue;
        bool found = false;
        ldoc::visit(*o, [&](const ldoc::Node& n) {
            if (n.id == id && o->id != id)
                found = true;
        });
        if (found)
            return true;
    }
    return false;
}

} // namespace

// ---- the clipboard ---------------------------------------------------------------------------

void Document::copySelectedNode() { copySelectedNodes(); }

void Document::copySelectedNodes() {
    const ldoc::Scene* s = current_scene();
    const QStringList chosen = selectedNodes();
    if (!s || chosen.isEmpty()) {
        emit notice(QStringLiteral("Select a source to copy."));
        return;
    }
    // Descendants of another selected node come along inside their parent, so taking them
    // again would paste them twice -- the same rule grouping uses.
    std::vector<ldoc::Node> taken;
    for (const QString& id : chosen) {
        const auto parsed = parse_id(id);
        const ldoc::Node* n = parsed ? ldoc::find_node(s->roots, *parsed) : nullptr;
        if (!n || inside_any(*s, chosen, *parsed))
            continue;
        taken.push_back(*n);
    }
    if (taken.empty())
        return;
    copied_scene_.reset();
    copied_nodes_ = std::move(taken);
    emit notice(copied_nodes_.size() == 1
                    ? QStringLiteral("Copied %1.").arg(qstr(copied_nodes_.front().name))
                    : QStringLiteral("Copied %1 sources.").arg(copied_nodes_.size()));
}

void Document::copyScene(const QString& sceneId) {
    const auto parsed = parse_id(sceneId);
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    if (!s) {
        emit notice(QStringLiteral("Select a scene to copy."));
        return;
    }
    copied_nodes_.clear();
    copied_scene_ = *s;
    emit notice(QStringLiteral("Copied scene %1.").arg(qstr(s->name)));
}

QString Document::pasteDescription() const {
    if (copied_nodes_.size() == 1)
        return QStringLiteral("Paste %1").arg(qstr(copied_nodes_.front().name));
    if (!copied_nodes_.empty())
        return QStringLiteral("Paste %1 sources").arg(copied_nodes_.size());
    if (copied_scene_)
        return QStringLiteral("Paste scene %1").arg(qstr(copied_scene_->name));
    return {};
}

int Document::pasteWouldMissCanvas() const {
    const ldoc::Canvas* c = current_canvas();
    if (!c)
        return 0;
    // A node sitting entirely past the right or bottom edge, or before the left or top one.
    // Counted rather than listed: the operator asked to be told that something is off, not
    // which things (ADR-0036).
    const auto misses = [c](const ldoc::Node& n) {
        const double w = n.layout.size.x > 0 ? n.layout.size.x : 0.0;
        const double h = n.layout.size.y > 0 ? n.layout.size.y : 0.0;
        return n.transform.position.x >= static_cast<double>(c->width) ||
               n.transform.position.y >= static_cast<double>(c->height) ||
               n.transform.position.x + w <= 0.0 || n.transform.position.y + h <= 0.0;
    };
    int off = 0;
    if (!copied_nodes_.empty()) {
        for (const ldoc::Node& root : copied_nodes_) {
            ldoc::visit(root, [&](const ldoc::Node& n) {
                if (misses(n))
                    ++off;
            });
        }
    } else if (copied_scene_) {
        ldoc::visit(copied_scene_->roots, [&](const ldoc::Node& n) {
            if (misses(n))
                ++off;
        });
    }
    return off;
}

void Document::paste() {
    if (!copied_nodes_.empty()) {
        const ldoc::Scene* into = current_scene();
        if (!into) {
            emit notice(QStringLiteral("There is no scene to paste into."));
            return;
        }
        Placement where;
        if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
            where.parent = sel->id;

        // One batch, so pasting six is one undo. Fresh ids on each, or the paste would
        // collide with what it was copied from -- which AddNode refuses, and rightly.
        std::vector<ldoc::Command> steps;
        QStringList pasted;
        std::size_t at = where.index;
        for (const ldoc::Node& original : copied_nodes_) {
            ldoc::Node node = ldoc::duplicate(original);
            pasted << id_str(node.id);
            steps.push_back(ldoc::AddNode{into->id, where.parent, at, std::move(node)});
            ++at;
        }
        run(steps.size() == 1
                ? steps.front()
                : ldoc::make_batch(std::move(steps),
                                   std::format("Paste {} sources", pasted.size())));
        // Everything pasted is selected, which is what makes "paste then drag" work.
        selected_node_ = pasted.takeLast();
        also_selected_ = pasted;
        selection_anchor_ = selected_node_;
        emit selectionChanged();
        return;
    }

    if (!copied_scene_) {
        emit notice(QStringLiteral("There is nothing to paste."));
        return;
    }

    const ldoc::Canvas* c = current_canvas();
    if (!c) {
        emit notice(QStringLiteral("There is no canvas to paste onto."));
        return;
    }
    // The name travels. Scene names only have to be unique within a canvas, and matching names
    // across canvases are what make a scene link readable: Main's "Live" cues Vertical's
    // "Live" (ADR-0036).
    ldoc::Scene copy = ldoc::duplicate_scene(*copied_scene_, copied_scene_->name);
    copy.canvas = c->id;
    const ldoc::Id scene_id = copy.id;
    run(ldoc::AddScene{std::move(copy), session_->project().scenes.size()});
    selectScene(id_str(scene_id));
}

void Document::renameSource(const QString& id, const QString& name) {
    const auto parsed = parse_id(id);
    const ldoc::Source* src = parsed ? session_->project().source(*parsed) : nullptr;
    if (!src)
        return;
    ldoc::Source copy = *src;
    copy.name = to_std(name);
    run(ldoc::SetSource{std::move(copy)});
}

void Document::bindSelectedNode(const QString& sourceId) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    if (!std::holds_alternative<ldoc::SourceData>(n->data)) {
        emit notice(QStringLiteral("That is not a source, so nothing can be bound to it."));
        return;
    }
    const auto source = sourceId.isEmpty() ? std::nullopt : parse_id(sourceId);
    std::vector<ldoc::Command> steps{ldoc::SetNodeSource{s->id, n->id, source}};
    if (const auto* audio = source ? source_audio(session_->project(), *source) : nullptr) {
        auto routing = std::get<ldoc::SourceData>(n->data).audio;
        if (routing.bus != audio->bus) {
            routing.bus = audio->bus;
            steps.push_back(ldoc::SetNodeAudio{s->id, n->id, std::move(routing)});
        }
    }
    run(ldoc::make_batch(std::move(steps), "Bind source"));
}

// ---- buses ------------------------------------------------------------------------------------

QVariantList Document::busKinds() const {
    struct Entry {
        ldoc::BusKind kind;
        const char* title;
        const char* note;
    };
    static constexpr Entry kKinds[] = {
        {ldoc::BusKind::Submix, "Submix", "A group of inputs with one fader"},
        {ldoc::BusKind::Program, "Program", "What goes to the live outputs"},
        {ldoc::BusKind::Vod, "VOD", "The recording track with the music left off"},
        {ldoc::BusKind::Track, "Track", "A numbered track in the recording file"},
        {ldoc::BusKind::Monitor, "Monitor", "What you hear, which nobody else does"},
        {ldoc::BusKind::Input, "Input", "One device, before anything is done to it"},
    };
    QVariantList out;
    for (const Entry& e : kKinds) {
        const auto id = ldoc::to_string(e.kind);
        QVariantMap m;
        m["id"] = QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        m["title"] = QString::fromLatin1(e.title);
        m["note"] = QString::fromLatin1(e.note);
        out.append(m);
    }
    return out;
}

void Document::addBus(const QString& name, const QString& kind) {
    const auto parsed = ldoc::bus_kind_from_string(to_std(kind));
    if (!parsed) {
        emit notice(QStringLiteral("'%1' is not a kind of bus this build knows.").arg(kind));
        return;
    }
    ldoc::Bus b;
    b.id = ldoc::Id::generate();
    b.name = name.isEmpty() ? std::string{"Bus"} : to_std(name);
    b.kind = *parsed;
    run(ldoc::AddBus{std::move(b), session_->project().buses.size()});
}

void Document::removeBus(const QString& id) {
    const auto parsed = parse_id(id);
    const auto* bus = parsed ? session_->project().bus(*parsed) : nullptr;
    if (bus && bus->kind == ldoc::BusKind::Program) {
        emit notice(QStringLiteral("The Program channel cannot be deleted."));
        return;
    }
    if (parsed)
        run(ldoc::RemoveBus{*parsed});
}

/// Read-modify-write on one bus. The command carries the whole bus rather than a delta, so each
/// of these has to start from the current one -- and there is exactly one place that does it,
/// because a copy of this that forgot a field would silently reset it.
void Document::edit_bus(const QString& id, const std::function<void(ldoc::Bus&)>& change,
                        std::string_view gesture) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Bus* existing = nullptr;
    for (const ldoc::Bus& b : session_->project().buses) {
        if (b.id == *parsed)
            existing = &b;
    }
    if (!existing)
        return;
    ldoc::Bus next = *existing;
    change(next);
    if (next == *existing)
        return; // nothing to put on the undo stack
    if (gesture.empty()) {
        run(ldoc::SetBus{std::move(next)});
        return;
    }
    const auto r = session_->run(ldoc::SetBus{std::move(next)}, gesture);
    if (!r)
        emit notice(qstr(r.error()));
}

void Document::renameBus(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    edit_bus(id, [&](ldoc::Bus& b) { b.name = to_std(name); });
}

void Document::setBusGain(const QString& id, double gainDb, bool dragging) {
    // One gesture per fader, named by the bus, so dragging two of them in turn is two undo
    // steps even if the operator never lets go in between.
    const std::string gesture =
        dragging ? std::format("bus.gain:{}", to_std(id)) : std::string{};
    edit_bus(id, [gainDb](ldoc::Bus& b) { b.gain_db = gainDb; }, gesture);
}

void Document::endGesture() {
    session_->end_gesture();
}

void Document::setBusPan(const QString& id, double pan) {
    edit_bus(id, [pan](ldoc::Bus& b) { b.pan = std::clamp(pan, -1.0, 1.0); });
}

void Document::setBusMute(const QString& id, bool mute) {
    edit_bus(id, [mute](ldoc::Bus& b) { b.mute = mute; });
}

void Document::setBusMonitor(const QString& id, bool monitor) {
    edit_bus(id, [monitor](ldoc::Bus& b) { b.monitor = monitor; });
}

int Document::hiddenStripCount() const {
    int n = 0;
    for (const BusRow& b : buses_.rows()) {
        if (b.hidden)
            ++n;
    }
    for (const MixerStripRow& r : mixer_strips_.rows()) {
        if (r.hidden)
            ++n;
    }
    return n;
}

void Document::setBusHidden(const QString& id, bool hidden) {
    edit_bus(id, [hidden](ldoc::Bus& b) { b.hidden = hidden; });
}

void Document::setBusLevelLocked(const QString& id, bool locked) {
    edit_bus(id, [locked](ldoc::Bus& b) { b.level_locked = locked; });
}

/// Same shape for the selected source node's own routing, for the same reason.
void Document::edit_node_audio(const QString& sceneId, const QString& nodeId,
                               const std::function<void(ldoc::AudioRouting&)>& change,
                               std::string_view gesture) {
    const auto scene_parsed = parse_id(sceneId);
    const auto node_parsed = parse_id(nodeId);
    if (!scene_parsed || !node_parsed)
        return;
    const ldoc::Scene* s = session_->project().scene(*scene_parsed);
    if (!s)
        return;
    const ldoc::Node* n = ldoc::find_node(s->roots, *node_parsed);
    if (!n)
        return;
    const auto* placed = std::get_if<ldoc::SourceData>(&n->data);
    if (!placed) {
        emit notice(QStringLiteral("Only a source has audio of its own."));
        return;
    }
    ldoc::AudioRouting next = placed->audio;
    change(next);
    if (next == placed->audio)
        return;
    if (gesture.empty()) {
        run(ldoc::SetNodeAudio{s->id, n->id, std::move(next)});
        return;
    }
    const auto r = session_->run(ldoc::SetNodeAudio{s->id, n->id, std::move(next)}, gesture);
    if (!r)
        emit notice(qstr(r.error()));
}

void Document::edit_selected_audio(const std::function<void(ldoc::AudioRouting&)>& change) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    edit_node_audio(id_str(s->id), id_str(n->id), change);
}

void Document::setNodeGain(const QString& sceneId, const QString& nodeId, double gainDb,
                           bool dragging) {
    // One gesture per strip, as the bus faders do, so dragging two in turn is two undo steps
    // even if the operator never lets go in between. Closed by endGesture().
    const std::string gesture =
        dragging ? std::format("node.gain:{}", to_std(nodeId)) : std::string{};
    edit_node_audio(
        sceneId, nodeId, [gainDb](ldoc::AudioRouting& a) { a.gain_db = gainDb; }, gesture);
}

void Document::setNodeMute(const QString& sceneId, const QString& nodeId, bool mute) {
    edit_node_audio(sceneId, nodeId, [mute](ldoc::AudioRouting& a) { a.mute = mute; });
}

void Document::setNodeMonitor(const QString& sceneId, const QString& nodeId, bool monitor) {
    edit_node_audio(sceneId, nodeId, [monitor](ldoc::AudioRouting& a) { a.monitor = monitor; });
}

void Document::setNodeHidden(const QString& sceneId, const QString& nodeId, bool hidden) {
    edit_node_audio(sceneId, nodeId, [hidden](ldoc::AudioRouting& a) { a.hidden = hidden; });
}

void Document::setNodeLevelLocked(const QString& sceneId, const QString& nodeId, bool locked) {
    edit_node_audio(sceneId, nodeId,
                    [locked](ldoc::AudioRouting& a) { a.level_locked = locked; });
}

void Document::routeNode(const QString& sceneId, const QString& nodeId, const QString& busId) {
    const auto scene_id = parse_id(sceneId);
    const auto node_id = parse_id(nodeId);
    const auto bus = busId.isEmpty() ? std::nullopt : parse_id(busId);
    const auto& project = session_->project();
    if (!busId.isEmpty() && (!bus || !project.bus(*bus))) {
        emit notice(QStringLiteral("Choose an existing audio bus."));
        return;
    }
    const auto* scene = scene_id ? project.scene(*scene_id) : nullptr;
    const auto* node = scene && node_id ? ldoc::find_node(scene->roots, *node_id) : nullptr;
    if (!node)
        return;
    const auto* selected = std::get_if<ldoc::SourceData>(&node->data);
    if (!selected) {
        emit notice(QStringLiteral("Only a source has audio of its own."));
        return;
    }
    std::vector<ldoc::Command> steps;
    for (const auto& owner : project.scenes)
        ldoc::visit(owner.roots, [&](const ldoc::Node& placed_node) {
            const auto* placed = std::get_if<ldoc::SourceData>(&placed_node.data);
            if (!placed || (selected->source ? placed->source != selected->source
                                             : placed_node.id != node->id) ||
                placed->audio.bus == bus)
                return;
            auto routing = placed->audio;
            routing.bus = bus;
            steps.push_back(ldoc::SetNodeAudio{owner.id, placed_node.id, std::move(routing)});
        });
    if (!steps.empty())
        run(ldoc::make_batch(std::move(steps), "Route source in all scenes"));
}

void Document::routeSelectedNode(const QString& busId) {
    const auto* scene = current_scene();
    const auto* node = current_node();
    if (scene && node)
        routeNode(id_str(scene->id), id_str(node->id), busId);
}

void Document::setSelectedNodeGain(double gainDb) {
    edit_selected_audio([gainDb](ldoc::AudioRouting& a) { a.gain_db = gainDb; });
}

void Document::setSelectedNodeMute(bool mute) {
    edit_selected_audio([mute](ldoc::AudioRouting& a) { a.mute = mute; });
}

// ---- decks and outputs ------------------------------------------------------------------------

QVariantList Document::transitions() const {
    struct Entry {
        const char* id;
        const char* title;
    };
    static constexpr Entry kTransitions[] = {
        {"transition.cut", "Cut"},
        {"transition.fade", "Fade"},
        {"transition.fade-to-black", "Fade to Black"},
        {"transition.slide", "Slide"},
    };
    QVariantList out;
    for (const Entry& e : kTransitions) {
        QVariantMap m;
        m["id"] = QString::fromLatin1(e.id);
        m["title"] = QString::fromLatin1(e.title);
        // A cut needs nothing from a renderer; the rest do, and the panel marks them so an
        // operator is not told a fade works when nothing can draw one yet.
        m["implemented"] = std::string_view{e.id} == "transition.cut";
        out.append(m);
    }
    for (const auto& source : session_->project().sources) {
        if (!lcomp::timer_flag(source.settings, "transitionStinger")) continue;
        out.push_back(QVariantMap{{"id", QStringLiteral("transition.stinger:") + id_str(source.id)},
                                  {"title", qstr(source.name)}, {"implemented", true}});
    }
    return out;
}

QVariantList Document::destinationKinds() const {
    static constexpr ldoc::DestinationKind kKinds[] = {
        ldoc::DestinationKind::Rtmps, ldoc::DestinationKind::Rtmp,
        ldoc::DestinationKind::Srt,   ldoc::DestinationKind::Whip,
    };
    QVariantList out;
    for (const ldoc::DestinationKind k : kKinds) {
        const auto id = ldoc::to_string(k);
        QVariantMap m;
        m["id"] = QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        m["title"] = m["id"].toString().toUpper();
        out.append(m);
    }
    return out;
}

QVariantList Document::codecs() const {
    // Deliberately not a capability list. What a machine can actually encode is `libertas probe`'s
    // question and is answered at run time per device; this is only what the document may say.
    struct Entry {
        const char* codec;
        const char* impl;
        const char* title;
    };
    static constexpr Entry kCodecs[] = {
        {"h264", "nvenc", "H.264 (NVENC)"},    {"h264", "qsv", "H.264 (QuickSync)"},
        {"h264", "vaapi", "H.264 (VAAPI)"},    {"h264", "x264", "H.264 (x264)"},
        {"hevc", "nvenc", "HEVC (NVENC)"},     {"hevc", "qsv", "HEVC (QuickSync)"},
        {"av1", "nvenc", "AV1 (NVENC)"},       {"av1", "qsv", "AV1 (QuickSync)"},
        {"av1", "svt-av1", "AV1 (SVT)"},
    };
    QVariantList out;
    for (const Entry& e : kCodecs) {
        QVariantMap m;
        m["codec"] = QString::fromLatin1(e.codec);
        m["implementation"] = QString::fromLatin1(e.impl);
        m["title"] = QString::fromLatin1(e.title);
        out.append(m);
    }
    return out;
}

QVariantMap Document::stingerSettings(const QString& deckId) const {
    const auto id = parse_id(deckId);
    for (const auto& deck : session_->project().decks) {
        if (!id || deck.id != *id || !deck.transition.starts_with("transition.stinger:")) continue;
        const auto source_id = ldoc::Id::parse(deck.transition.substr(19));
        const auto* source = source_id ? session_->project().source(*source_id) : nullptr;
        if (!source) return {};
        return {{"sourceId", id_str(source->id)}, {"file", qstr(lcomp::timer_text(source->settings, "localFile"))},
                {"cutMs", QVariant::fromValue(lcomp::timer_number(source->settings, "stingerCutMs", 500))},
                {"durationMs", QVariant::fromValue(lcomp::timer_number(source->settings, "stingerDurationMs", 1000))}};
    }
    return {};
}

void Document::setStingerTiming(const QString& deckId, int cutMs, int durationMs) {
    const auto details = stingerSettings(deckId);
    const auto id = parse_id(details.value("sourceId").toString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source) return;
    if (cutMs < 0 || durationMs < 1 || durationMs > 600000 || cutMs > durationMs) {
        emit notice(QStringLiteral("Stinger cut point must be within its duration.")); return;
    }
    auto copy = *source;
    copy.settings["stingerCutMs"] = static_cast<std::int64_t>(cutMs);
    copy.settings["stingerDurationMs"] = static_cast<std::int64_t>(durationMs);
    run(ldoc::SetSource{std::move(copy)});
}

bool Document::addStinger(const QString& deckId, const QString& name, const QString& path,
                           int cutMs, int durationMs) {
    const auto id = parse_id(deckId);
    const auto& project = session_->project();
    const auto deck = std::ranges::find(project.decks, id.value_or(ldoc::Id{}), &ldoc::Deck::id);
    const QFileInfo file(path);
    if (!id || deck == project.decks.end() || name.trimmed().isEmpty() || name.size() > 128 ||
        !file.isAbsolute() || !file.isFile() || !file.isReadable() ||
        durationMs <= 0 || durationMs > 600000 || cutMs < 0 || cutMs > durationMs) {
        emit notice(QStringLiteral("Choose a readable media file and a cut point within its duration."));
        return false;
    }
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = to_std(name.trimmed());
    source.type = "source.media";
    source.settings["localFile"] = file.absoluteFilePath().toStdString();
    source.settings["loop"] = false;
    source.settings["clearOnEnd"] = true;
    source.settings["restartOnActivate"] = true;
    source.settings["closeWhenInactive"] = false;
    source.settings["transitionStinger"] = true;
    source.settings["stingerCutMs"] = static_cast<std::int64_t>(cutMs);
    source.settings["stingerDurationMs"] = static_cast<std::int64_t>(durationMs);
    if (const auto* canvas = project.canvas(deck->canvas)) {
        source.settings["width"] = static_cast<std::int64_t>(canvas->width);
        source.settings["height"] = static_cast<std::int64_t>(canvas->height);
    }
    auto next = *deck;
    next.transition = "transition.stinger:" + source.id.to_string();
    next.transition_ms = durationMs;
    run(ldoc::make_batch({ldoc::AddSource{source, project.sources.size()}, ldoc::SetDeck{next}},
                         "Add stinger transition"));
    return true;
}

void Document::edit_deck(const QString& id, const std::function<void(ldoc::Deck&)>& change) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Deck* existing = nullptr;
    for (const ldoc::Deck& d : session_->project().decks) {
        if (d.id == *parsed)
            existing = &d;
    }
    if (!existing)
        return;
    ldoc::Deck next = *existing;
    change(next);
    if (next == *existing)
        return;
    run(ldoc::SetDeck{std::move(next)});
}

void Document::setDeckProgram(const QString& deckId, const QString& sceneId) {
    const auto deck = parse_id(deckId);
    if (!deck)
        return;
    run(ldoc::SetDeckProgram{*deck, sceneId.isEmpty() ? std::nullopt : parse_id(sceneId)});
}

void Document::setDeckPreview(const QString& deckId, const QString& sceneId) {
    const auto deck = parse_id(deckId);
    if (!deck)
        return;
    run(ldoc::SetDeckPreview{*deck, sceneId.isEmpty() ? std::nullopt : parse_id(sceneId)});
}

void Document::takeDeck(const QString& deckId) {
    const auto parsed = parse_id(deckId);
    if (!parsed)
        return;
    for (const ldoc::Deck& d : session_->project().decks) {
        if (d.id != *parsed)
            continue;
        if (!d.preview_scene) {
            emit notice(QStringLiteral("Nothing is staged on preview."));
            return;
        }
        if (d.program_scene && *d.program_scene == *d.preview_scene)
            return; // already on air; a take would be a no-op on the undo stack
        run(ldoc::SetDeckProgram{d.id, d.preview_scene});
        return;
    }
}

void Document::setDeckStudioMode(const QString& deckId, bool studio) {
    edit_deck(deckId, [studio](ldoc::Deck& d) { d.studio_mode = studio; });
}

void Document::setDeckTransition(const QString& deckId, const QString& transition,
                                 int durationMs) {
    edit_deck(deckId, [&](ldoc::Deck& d) {
        if (!transition.isEmpty())
            d.transition = to_std(transition);
        if (durationMs >= 0)
            d.transition_ms = durationMs;
    });
}

int Document::encodeCount() const {
    return static_cast<int>(session_->project().encodes.size());
}

void Document::addOutput(const QString& name) {
    const ldoc::Project& p = session_->project();
    if (p.canvases.empty()) {
        emit notice(QStringLiteral("An output needs a canvas to encode."));
        return;
    }
    // Its own encode, not a share of an existing one. Sharing is the point of ADR-0029, but it
    // has to be asked for: a new output that silently joined another's bitstream would make
    // changing its bitrate change someone else's stream, which is the surprise this model
    // exists to make visible rather than to hide.
    auto e = defaultEncode(*current_canvas());
    e.name = name.isEmpty() ? "Output" : to_std(name);

    ldoc::Output o;
    o.id = ldoc::Id::generate();
    o.name = name.isEmpty() ? std::string{"Output"} : to_std(name);
    o.encode = e.id;
    // Disabled, with nowhere to send to. An output that arrived enabled and pointed at a URL
    // would be one keystroke from going somewhere the operator did not choose.
    o.enabled = false;

    // One undo step: nobody who presses Ctrl+Z after adding an output wants the encode left
    // behind.
    run(ldoc::make_batch({ldoc::AddEncode{std::move(e), p.encodes.size()},
                          ldoc::AddOutput{std::move(o), p.outputs.size()}},
                         "Add output"));
}

void Document::removeOutput(const QString& id) {
    const auto parsed = parse_id(id);
    if (!parsed) return;
    for (const auto& o : session_->project().outputs)
        if (o.id == *parsed && (o.destination.kind == ldoc::DestinationKind::File ||
                               o.destination.kind == ldoc::DestinationKind::Replay))
            return;
    run(ldoc::RemoveOutput{*parsed});
}

void Document::initializeEncodeGraph() {
    const auto& p = session_->project();
    const bool initialized = p.settings.extra.contains("encode.graph.initialized");

    std::vector<ldoc::Command> steps;
    // Arrange once; always repair missing permanent outputs from older versions.
    const auto place = [](auto nodes, double x, double top) {
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].position == ldoc::Vec2{})
                nodes[i].position = {x, top + 170.0 * static_cast<double>(i)};
        return nodes;
    };
    const auto encodes = initialized ? p.encodes : place(p.encodes, 400.0, 60.0);
    const auto origin = encodes.empty() ? ldoc::Vec2{400.0, 60.0} : encodes.front().position;
    for (const auto& e : encodes)
        if (e != *p.encode(e.id))
            steps.push_back(ldoc::SetEncode{e});
    auto audio = initialized ? p.audio_encodes : place(p.audio_encodes, 400.0,
                       60.0 + 170.0 * static_cast<double>(encodes.size()));
    for (std::size_t i = 0; i < audio.size(); ++i)
        if (audio[i] != p.audio_encodes[i])
            steps.push_back(ldoc::SetAudioEncode{audio[i]});
    if (!initialized && audio.empty()) {
        const auto bus = std::find_if(p.buses.begin(), p.buses.end(), [](const auto& b) {
            return b.kind == ldoc::BusKind::Program;
        });
        if (bus != p.buses.end()) {
            ldoc::AudioEncode a;
            a.id = ldoc::Id::generate();
            a.name = "Program AAC";
            a.bus = bus->id;
            a.position = {400.0, 60.0 + 170.0 * static_cast<double>(encodes.size())};
            audio.push_back(a);
            steps.push_back(ldoc::AddAudioEncode{a, p.audio_encodes.size()});
        }
    }
    auto outputs = initialized ? p.outputs : place(p.outputs, origin.x + 360.0, 60.0);
    for (std::size_t i = 0; i < outputs.size(); ++i)
        if (outputs[i] != p.outputs[i])
            steps.push_back(ldoc::SetOutput{outputs[i]});
    double next_y = origin.y;
    for (const auto& o : outputs)
        next_y = std::max(next_y, o.position.y + 170.0);
    for (const auto kind : {ldoc::DestinationKind::File, ldoc::DestinationKind::Replay}) {
        if (std::ranges::any_of(outputs, [kind](const auto& o) {
                return o.destination.kind == kind;
            }))
            continue;
        ldoc::Output o;
        o.id = ldoc::Id::generate();
        o.name = kind == ldoc::DestinationKind::File ? "Recording" : "Backtrack";
        if (!encodes.empty())
            o.encode = encodes.front().id;
        if (!audio.empty())
            o.audio_encode = audio.front().id;
        o.destination.kind = kind;
        o.position = {origin.x + 360.0, next_y};
        next_y += 170.0;
        steps.push_back(ldoc::AddOutput{o, outputs.size()});
        outputs.push_back(o);
    }
    if (!initialized) {
        auto settings = p.settings;
        settings.extra["encode.graph.initialized"] = true;
        steps.push_back(ldoc::SetProjectSettings{std::move(settings)});
    }
    if (!steps.empty())
        run(ldoc::make_batch(std::move(steps), "Initialize Encode graph"));
}

QString Document::recordingEncodeId() const {
    const auto& p = session_->project();
    for (const auto& o : p.outputs) {
        if (o.destination.kind != ldoc::DestinationKind::File)
            continue;
        const auto* video = p.encode(o.encode);
        const auto* audio = o.audio_encode ? p.audio_encode(*o.audio_encode) : nullptr;
        if (video && p.canvas(video->canvas) && audio && p.bus(audio->bus))
            return id_str(o.encode);
        return {};
    }
    return {};
}

void Document::disconnectGraphPort(const QString& kind, const QString& id, const QString& port) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const auto& p = session_->project();
    std::vector<ldoc::Command> steps;
    const bool outgoing = port == QLatin1String("output");
    for (auto e : p.encodes) {
        if ((kind == "encode" && port == "input" && e.id == *parsed) ||
            (kind == "canvas" && outgoing && e.canvas == *parsed)) {
            if (!e.canvas.is_nil()) {
                e.canvas = ldoc::Id::nil();
                steps.push_back(ldoc::SetEncode{std::move(e)});
            }
        }
    }
    for (auto e : p.audio_encodes) {
        if ((kind == "audio-encode" && port == "input" && e.id == *parsed) ||
            (kind == "bus" && outgoing && e.bus == *parsed)) {
            if (!e.bus.is_nil()) {
                e.bus = ldoc::Id::nil();
                steps.push_back(ldoc::SetAudioEncode{std::move(e)});
            }
        }
    }
    for (auto o : p.outputs) {
        bool changed = false;
        if ((kind == "output" && port == "video" && o.id == *parsed) ||
            (kind == "encode" && outgoing && o.encode == *parsed)) {
            changed = !o.encode.is_nil();
            o.encode = ldoc::Id::nil();
        }
        if ((kind == "output" && port == "audio" && o.id == *parsed) ||
            (kind == "audio-encode" && outgoing && o.audio_encode == *parsed)) {
            changed = o.audio_encode.has_value();
            o.audio_encode.reset();
        }
        if (changed)
            steps.push_back(ldoc::SetOutput{std::move(o)});
    }
    if (!steps.empty())
        run(ldoc::make_batch(std::move(steps), "Disconnect graph port"));
}

bool Document::connectGraphPorts(const QString& sourceKind, const QString& sourceId,
                                 const QString& targetKind, const QString& targetId,
                                 const QString& targetPort) {
    const auto source = parse_id(sourceId);
    const auto target = parse_id(targetId);
    if (!source || !target)
        return false;
    const auto& p = session_->project();
    std::optional<ldoc::Command> command;
    if (sourceKind == "canvas" && targetKind == "encode" && targetPort == "input") {
        const auto* existing = p.encode(*target);
        if (!existing || !p.canvas(*source)) return false;
        if (existing->canvas == *source) return true;
        auto next = *existing;
        next.canvas = *source;
        retitle_if_auto(p, *existing, next);
        command = ldoc::SetEncode{std::move(next)};
    } else if (sourceKind == "bus" && targetKind == "audio-encode" && targetPort == "input") {
        const auto* existing = p.audio_encode(*target);
        const auto* bus = p.bus(*source);
        if (!existing || !bus ||
            (bus->kind != ldoc::BusKind::Program && bus->kind != ldoc::BusKind::Vod &&
             bus->kind != ldoc::BusKind::Track && bus->kind != ldoc::BusKind::Submix))
            return false;
        if (existing->bus == *source) return true;
        auto next = *existing;
        next.bus = *source;
        command = ldoc::SetAudioEncode{std::move(next)};
    } else if (targetKind == "output") {
        const auto existing = std::ranges::find(p.outputs, *target, &ldoc::Output::id);
        if (existing == p.outputs.end()) return false;
        auto next = *existing;
        if (sourceKind == "encode" && targetPort == "video" && p.encode(*source))
            next.encode = *source;
        else if (sourceKind == "audio-encode" && targetPort == "audio" && p.audio_encode(*source))
            next.audio_encode = *source;
        else
            return false;
        if (next == *existing) return true;
        // Replacing a wire keeps both encoders; this edits only the destination's input.
        command = ldoc::SetOutput{std::move(next)};
    }
    if (!command)
        return false;
    const auto result = session_->run(*command);
    if (!result)
        emit notice(qstr(result.error()));
    return result.has_value();
}

void Document::removeEncode(const QString& id) {
    const auto parsed = parse_id(id);
    if (!parsed || !session_->project().encode(*parsed)) return;
    std::vector<ldoc::Command> steps;
    for (auto o : session_->project().outputs)
        if (o.encode == *parsed) {
            o.encode = {};
            steps.push_back(ldoc::SetOutput{std::move(o)});
        }
    steps.push_back(ldoc::RemoveEncode{*parsed});
    run(ldoc::make_batch(std::move(steps), "Remove video encode"));
}

void Document::edit_output(const QString& id, const std::function<void(ldoc::Output&)>& change) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Output* existing = nullptr;
    for (const ldoc::Output& o : session_->project().outputs) {
        if (o.id == *parsed)
            existing = &o;
    }
    if (!existing)
        return;
    ldoc::Output next = *existing;
    change(next);
    if (next == *existing)
        return;
    run(ldoc::SetOutput{std::move(next)});
}

void Document::renameOutput(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    edit_output(id, [&](ldoc::Output& o) { o.name = to_std(name); });
}

void Document::setOutputEnabled(const QString& id, bool enabled) {
    edit_output(id, [enabled](ldoc::Output& o) { o.enabled = enabled; });
}

void Document::addEncode() {
    const ldoc::Project& p = session_->project();
    const ldoc::Canvas* c = current_canvas();
    if (!c) {
        emit notice(QStringLiteral("An encode needs a canvas to encode."));
        return;
    }
    auto e = defaultEncode(*c);
    ldoc::Output o;
    o.id = ldoc::Id::generate();
    o.name = "Destination";
    o.encode = e.id;
    run(ldoc::make_batch({ldoc::AddEncode{e, p.encodes.size()},
                         ldoc::AddOutput{o, p.outputs.size()}}, "Add output group"));
}

void Document::setEncodeCanvas(const QString& id, const QString& canvasId) {
    const auto parsed = parse_id(id);
    const auto canvas = parse_id(canvas_key(canvasId));
    if (!parsed || !canvas)
        return;
    const ldoc::Encode* existing = session_->project().encode(*parsed);
    if (!existing)
        return;
    if (!session_->project().canvas(*canvas)) {
        emit notice(QStringLiteral("That canvas is no longer in the project."));
        return;
    }
    ldoc::Encode next = *existing;
    next.canvas = *canvas;
    // The generated name carries the resolution and the canvas decides it, so a name nobody
    // typed has to follow the move -- the same rule setEncodeRescale works by.
    retitle_if_auto(session_->project(), *existing, next);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
}

void Document::renameEncode(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Encode* existing = session_->project().encode(*parsed);
    if (!existing)
        return;
    ldoc::Encode next = *existing;
    next.name = to_std(name);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
}

void Document::setEncodeRescale(const QString& id, int width, int height, const QString& filter) {
    if (!filter.isEmpty() && !QStringList{"bilinear", "bicubic", "lanczos"}.contains(filter)) {
        emit notice(QStringLiteral("Unknown rescale filter."));
        return;
    }
    if ((width != 0 || height != 0) &&
        (width < 2 || height < 2 || width > 16384 || height > 16384 || width % 2 || height % 2)) {
        emit notice(QStringLiteral("Output dimensions must be even numbers from 2 to 16384."));
        return;
    }
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Encode* existing = session_->project().encode(*parsed);
    if (!existing)
        return;
    ldoc::Encode next = *existing;
    if (!filter.isEmpty())
        next.encoder.extra["rescale_filter"] = to_std(filter);
    // Zero means "no rescale": send the canvas at its own size. A rescale equal to the canvas
    // is not the same thing stored -- it would survive a canvas resize and quietly pin the
    // encode to the old size.
    if (width <= 0 || height <= 0)
        next.rescale.reset();
    else
        next.rescale = ldoc::Vec2{static_cast<double>(width), static_cast<double>(height)};
    retitle_if_auto(session_->project(), *existing, next);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
}

void Document::setOutputEncode(const QString& output_id, const QString& encode_id) {
    const auto out_parsed = parse_id(output_id);
    const auto enc_parsed = parse_id(encode_id);
    if (!out_parsed || !enc_parsed)
        return;
    const ldoc::Project& p = session_->project();
    const ldoc::Output* out = nullptr;
    for (const ldoc::Output& o : p.outputs) {
        if (o.id == *out_parsed)
            out = &o;
    }
    const ldoc::Encode* onto = p.encode(*enc_parsed);
    if (!out || !onto || out->encode == *enc_parsed)
        return;

    ldoc::Output next = *out;
    next.encode = *enc_parsed;
    run(ldoc::SetOutput{std::move(next)});
}

void Document::addOutputOnEncode(const QString& encode_id, const QString& name) {
    const auto parsed = parse_id(encode_id);
    if (!parsed)
        return;
    const ldoc::Project& p = session_->project();
    if (!p.encode(*parsed)) {
        emit notice(QStringLiteral("That encode is gone."));
        return;
    }
    ldoc::Output o;
    o.id = ldoc::Id::generate();
    o.name = name.isEmpty() ? std::string{"Output"} : to_std(name);
    o.encode = *parsed;
    // Disabled and pointed nowhere, exactly as addOutput makes one. Sharing an encode does not
    // make a destination any less of a thing that could go live by accident.
    o.enabled = false;
    run(ldoc::AddOutput{std::move(o), p.outputs.size()});
}

void Document::edit_encode(const QString& id, const std::function<void(ldoc::Encode&)>& change) {
    const auto parsed = parse_id(id);
    if (!parsed) return;
    const auto& p = session_->project();
    const auto* existing = p.encode(*parsed);
    if (!existing) return;
    ldoc::Encode next = *existing;
    change(next);
    if (next.encoder.implementation == "nvenc") {
        if (next.encoder.preset.empty()) next.encoder.preset = "p5";
        auto& profile = next.encoder.extra["nvenc.profile"];
        const auto* text = std::get_if<std::string>(&profile);
        if (!text || text->empty() || next.encoder.codec != existing->encoder.codec)
            profile = std::string{next.encoder.codec == "h264" ? "high" : "main"};
    }
    retitle_if_auto(p, *existing, next);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
}

void Document::edit_audio_encode(const QString& id,
                                 const std::function<void(ldoc::AudioEncode&)>& change,
                                 std::string_view gesture) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const auto* existing = session_->project().audio_encode(*parsed);
    if (!existing)
        return;
    ldoc::AudioEncode next = *existing;
    change(next);
    if (next == *existing)
        return; // nothing to put on the undo stack
    if (gesture.empty()) {
        run(ldoc::SetAudioEncode{std::move(next)});
        return;
    }
    const auto r = session_->run(ldoc::SetAudioEncode{std::move(next)}, gesture);
    if (!r)
        emit notice(qstr(r.error()));
}

void Document::addAudioEncode() {
    const ldoc::Project& p = session_->project();
    // The program bus, because that is what a new audio encode almost always wants and the
    // alternative -- refusing until one is picked -- makes the button lie about what it does.
    const ldoc::Bus* bus = nullptr;
    for (const ldoc::Bus& b : p.buses) {
        if (b.kind == ldoc::BusKind::Program)
            bus = &b;
    }
    if (!bus) {
        for (const ldoc::Bus& b : p.buses) {
            if (b.kind == ldoc::BusKind::Vod || b.kind == ldoc::BusKind::Track) {
                bus = &b;
                break;
            }
        }
    }
    if (!bus) {
        emit notice(QStringLiteral("An audio encode needs a bus to encode."));
        return;
    }
    ldoc::AudioEncode e;
    e.id = ldoc::Id::generate();
    e.bus = bus->id;
    e.codec = "aac";
    e.bitrate_kbps = 160;
    // Named for what it is, never for a destination -- the same rule as an encode.
    e.name = std::format("AAC {}", e.bitrate_kbps);
    run(ldoc::AddAudioEncode{std::move(e), p.audio_encodes.size()});
}

void Document::removeAudioEncode(const QString& id) {
    const auto parsed = parse_id(id);
    if (!parsed || !session_->project().audio_encode(*parsed))
        return;
    // The command refuses while an output still names it, so clear those first rather than
    // handing the operator a refusal they would have to undo by hand.
    std::vector<ldoc::Command> steps;
    for (const ldoc::Output& o : session_->project().outputs) {
        if (o.audio_encode && *o.audio_encode == *parsed) {
            ldoc::Output next = o;
            next.audio_encode.reset();
            steps.push_back(ldoc::SetOutput{std::move(next)});
        }
    }
    steps.push_back(ldoc::RemoveAudioEncode{*parsed});
    run(ldoc::make_batch(std::move(steps), "Remove audio encode"));
}

void Document::renameAudioEncode(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    edit_audio_encode(id, [&](ldoc::AudioEncode& e) { e.name = to_std(name); });
}

void Document::setAudioEncodeBus(const QString& id, const QString& busId) {
    const auto parsed = parse_id(busId);
    if (!parsed)
        return;
    edit_audio_encode(id, [&](ldoc::AudioEncode& e) { e.bus = *parsed; });
}

void Document::setAudioEncodeCodec(const QString& id, const QString& codec) {
    if (codec != QLatin1String("aac") && codec != QLatin1String("opus"))
        return;
    edit_audio_encode(id, [&](ldoc::AudioEncode& e) { e.codec = to_std(codec); });
}

void Document::setAudioEncodeBitrate(const QString& id, int kbps) {
    if (kbps <= 0)
        return;
    edit_audio_encode(id, [kbps](ldoc::AudioEncode& e) { e.bitrate_kbps = kbps; });
}

QVariantMap Document::audioEncodeSettings(const QString& id) const {
    const auto parsed = parse_id(id);
    if (!parsed)
        return {};
    const auto* e = session_->project().audio_encode(*parsed);
    if (!e)
        return {};
    const ldoc::Bus* b = session_->project().bus(e->bus);
    return {{"id", id_str(e->id)},
            {"name", qstr(e->name)},
            {"busId", b ? id_str(e->bus) : QString{}},
            {"busName", b ? qstr(b->name) : QString{}},
            {"codec", qstr(e->codec)},
            {"bitrateKbps", e->bitrate_kbps}};
}

QVariantList Document::audioEncodeChoices() const {
    QVariantList choices;
    for (const ldoc::AudioEncode& e : session_->project().audio_encodes)
        choices.append(QVariantMap{{"id", id_str(e.id)}, {"name", qstr(e.name)}});
    return choices;
}

QVariantList Document::audioBusChoices() const {
    QVariantList choices;
    // Program, VOD and the numbered tracks: the buses that are a destination for a mix rather
    // than a place one source arrives. An input bus is not something to encode.
    for (const ldoc::Bus& b : session_->project().buses) {
        if (b.kind == ldoc::BusKind::Program || b.kind == ldoc::BusKind::Vod ||
            b.kind == ldoc::BusKind::Track || b.kind == ldoc::BusKind::Submix)
            choices.append(QVariantMap{{"id", id_str(b.id)}, {"name", qstr(b.name)}});
    }
    return choices;
}

void Document::setOutputAudioEncode(const QString& outputId, const QString& audioEncodeId) {
    // An empty id means no audio, which is a destination that goes out silent. validate()
    // warns rather than refusing, because a silent file capture is a thing someone can mean.
    std::optional<ldoc::Id> wanted;
    if (!audioEncodeId.isEmpty()) {
        const auto parsed = parse_id(audioEncodeId);
        if (!parsed || !session_->project().audio_encode(*parsed))
            return;
        wanted = *parsed;
    }
    edit_output(outputId, [&](ldoc::Output& o) { o.audio_encode = wanted; });
}

QString Document::outputAudioEncode(const QString& outputId) const {
    const auto parsed = parse_id(outputId);
    if (!parsed)
        return {};
    for (const ldoc::Output& o : session_->project().outputs) {
        if (o.id == *parsed)
            return o.audio_encode ? id_str(*o.audio_encode) : QString{};
    }
    return {};
}

void Document::setRecordingDirectory(const QString& outputId, const QString& path) {
    const auto id = parse_id(outputId);
    const auto& outputs = session_->project().outputs;
    const auto output = std::find_if(outputs.begin(), outputs.end(),
        [&](const auto& item) { return id && item.id == *id; });
    if (output == outputs.end() || output->destination.kind != ldoc::DestinationKind::File)
        return;
    QString directory = path.trimmed();
    if (directory == "~" || directory.startsWith("~/"))
        directory.replace(0, 1, QDir::homePath());
    if (!directory.isEmpty()) {
        if (directory.contains(QChar::Null) || !QDir::isAbsolutePath(directory) ||
            !QFileInfo(directory).isDir()) {
            emit notice(QStringLiteral("Choose an existing absolute recording directory."));
            return;
        }
        directory = QDir::cleanPath(directory);
    }
    setOutputDestination(outputId, QStringLiteral("file"), directory);
}

QString Document::recordingDirectory() const {
    for (const auto& output : session_->project().outputs)
        if (output.destination.kind == ldoc::DestinationKind::File &&
            !output.destination.url.empty())
            return qstr(output.destination.url);
    return session_->path().empty() ? QDir::homePath()
                                   : qstr(session_->path().parent_path().string());
}

QString Document::recordingPathFor(const QString& encodeId) const {
    const auto parsed = parse_id(encodeId);
    if (!parsed)
        return {};
    const ldoc::Encode* e = session_->project().encode(*parsed);
    if (!e)
        return {};
    const auto base = std::filesystem::path{recordingDirectory().toStdString()};
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"));
    QString name = qstr(e->name).simplified();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    if (name.isEmpty())
        name = QStringLiteral("recording");
    // ponytail: MKV is the recording container; expose a format choice when another is needed.
    return qstr((base / QStringLiteral("%1-%2-%3.mkv")
                           .arg(name, stamp, id_str(ldoc::Id::generate())).toStdString()).string());
}

QString Document::encodeCanvasOf(const QString& encodeId) const {
    const auto parsed = parse_id(encodeId);
    if (!parsed)
        return {};
    const ldoc::Encode* e = session_->project().encode(*parsed);
    return e && session_->project().canvas(e->canvas) ? id_str(e->canvas) : QString{};
}

QVariantList Document::outputsWithAudio() const {
    QVariantList out;
    for (const ldoc::Output& o : session_->project().outputs) {
        if (o.audio_encode)
            out.append(QVariantMap{{"id", id_str(o.id)},
                                   {"audioEncode", id_str(*o.audio_encode)}});
    }
    return out;
}

void Document::setGraphPosition(const QString& kind, const QString& id, double x, double y,
                                bool dragging) {
    // One gesture per node, named by it, so dragging two in turn is two undo steps even if the
    // operator never lets go in between -- the same handling a fader gets.
    const std::string gesture =
        dragging ? std::format("graph.move:{}", to_std(id)) : std::string{};
    const ldoc::Vec2 at{x, y};
    if (kind == QLatin1String("encode")) {
        const auto parsed = parse_id(id);
        if (!parsed)
            return;
        const auto* existing = session_->project().encode(*parsed);
        if (!existing || existing->position == at)
            return;
        ldoc::Encode next = *existing;
        next.position = at;
        if (gesture.empty()) {
            run(ldoc::SetEncode{std::move(next)});
            return;
        }
        const auto r = session_->run(ldoc::SetEncode{std::move(next)}, gesture);
        if (!r)
            emit notice(qstr(r.error()));
        return;
    }
    if (kind == QLatin1String("audio-encode")) {
        edit_audio_encode(id, [at](ldoc::AudioEncode& e) { e.position = at; }, gesture);
        return;
    }
    if (kind != QLatin1String("output"))
        return;
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Output* existing = nullptr;
    for (const ldoc::Output& o : session_->project().outputs) {
        if (o.id == *parsed)
            existing = &o;
    }
    if (!existing || existing->position == at)
        return;
    ldoc::Output next = *existing;
    next.position = at;
    if (gesture.empty()) {
        run(ldoc::SetOutput{std::move(next)});
        return;
    }
    const auto r = session_->run(ldoc::SetOutput{std::move(next)}, gesture);
    if (!r)
        emit notice(qstr(r.error()));
}

void Document::setOutputBitrate(const QString& id, int kbps) {
    const auto encode = outputEncoderSettings(id).value("id").toString();
    setEncodeBitrate(encode, kbps);
}

void Document::setEncodeBitrate(const QString& id, int kbps) {
    edit_encode(id, [kbps](ldoc::Encode& e) { e.encoder.bitrate_kbps = kbps; });
}

void Document::setOutputEncoder(const QString& id, const QString& codec, const QString& implementation) {
    const auto encode = outputEncoderSettings(id).value("id").toString();
    setEncodeEncoder(encode, codec, implementation);
}

void Document::setEncodeEncoder(const QString& id, const QString& codec,
                                const QString& implementation) {
    bool supported = false;
    for (const auto& entry : codecs()) {
        const auto choice = entry.toMap();
        supported = supported || (choice.value("codec").toString() == codec &&
                                  choice.value("implementation").toString() == implementation);
    }
    if (!supported) {
        emit notice(QStringLiteral("Unknown video encoder configuration."));
        return;
    }
    edit_encode(id, [&](ldoc::Encode& e) {
        if (e.encoder.implementation != to_std(implementation)) {
            e.encoder.preset.clear();
            if ((e.encoder.rate_control == "crf" && implementation != "x264") ||
                (e.encoder.rate_control == "cqp" && implementation != "nvenc"))
                e.encoder.rate_control = "cbr";
        }
        e.encoder.codec = to_std(codec);
        e.encoder.implementation = to_std(implementation);
        if (implementation == "nvenc") {
            auto& profile = e.encoder.extra["nvenc.profile"];
            const auto* text = std::get_if<std::string>(&profile);
            const auto allowed = codec == "h264" ? QStringList{"baseline", "main", "high"}
                               : codec == "hevc" ? QStringList{"main", "main10"} : QStringList{"main"};
            if (!text || !allowed.contains(qstr(*text)))
                profile = to_std(codec == "h264" ? QStringLiteral("high") : QStringLiteral("main"));
        }
    });
}

void Document::setOutputDestination(const QString& id, const QString& kind, const QString& url) {
    const auto parsed_kind = ldoc::destination_kind_from_string(to_std(kind));
    if (!kind.isEmpty() && !parsed_kind) {
        emit notice(QStringLiteral("'%1' is not a kind of destination this build knows.")
                        .arg(kind));
        return;
    }
    edit_output(id, [&](ldoc::Output& o) {
        const auto permanent = [](ldoc::DestinationKind k) {
            return k == ldoc::DestinationKind::File || k == ldoc::DestinationKind::Replay;
        };
        if (parsed_kind && *parsed_kind != o.destination.kind &&
            (permanent(o.destination.kind) || permanent(*parsed_kind)))
            return;
        if (parsed_kind)
            o.destination.kind = *parsed_kind;
        o.destination.url = to_std(url);
    });
}

QVariantList Document::encodeChoices() const {
    QVariantList choices;
    for (const auto& e : session_->project().encodes) {
        QVariantList destinations;
        for (const auto& o : session_->project().outputs) {
            if (o.encode == e.id)
                destinations.append(QVariantMap{{"id", id_str(o.id)}, {"name", qstr(o.name)}});
        }
        choices.append(QVariantMap{{"id", id_str(e.id)}, {"name", qstr(e.name)},
                                   {"destinations", destinations}});
    }
    return choices;
}

QVariantMap Document::outputEncoderSettings(const QString& id) const {
    const auto parsed = parse_id(id);
    if (!parsed) return {};
    for (const auto& o : session_->project().outputs) {
        if (o.id == *parsed) {
            auto settings = encodeSettings(id_str(o.encode));
            settings["outputName"] = qstr(o.name);
            const auto kind = ldoc::to_string(o.destination.kind);
            settings["destinationKind"] = QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()));
            settings["url"] = qstr(o.destination.url);
            settings["credentialRef"] = qstr(o.destination.credential_ref);
            return settings;
        }
    }
    return {};
}

QVariantMap Document::encodeSettings(const QString& id) const {
    const auto parsed = parse_id(id);
    if (!parsed) return {};
    if (const auto* e = session_->project().encode(*parsed)) {
        QVariantMap extra;
        for (const auto& [key, value] : e->encoder.extra) {
            if (const auto* text = std::get_if<std::string>(&value)) extra[qstr(key)] = qstr(*text);
            else if (const auto* flag = std::get_if<bool>(&value)) extra[qstr(key)] = *flag;
            else if (const auto* number = std::get_if<std::int64_t>(&value))
                extra[qstr(key)] = QVariant::fromValue(static_cast<qlonglong>(*number));
        }
        QString preset = qstr(e->encoder.preset);
        if (e->encoder.implementation == "nvenc") {
            if (preset.isEmpty()) preset = QStringLiteral("p5");
            if (extra.value("nvenc.profile").toString().isEmpty())
                extra["nvenc.profile"] = e->encoder.codec == "h264" ? "high" : "main";
        }
        return {{"id", id_str(e->id)}, {"name", qstr(e->name)},
                {"rateControl", qstr(e->encoder.rate_control)},
                {"preset", preset}, {"extra", extra},
                {"width", e->rescale ? e->rescale->x : 0},
                {"height", e->rescale ? e->rescale->y : 0},
                {"codec", qstr(e->encoder.codec)},
                {"implementation", qstr(e->encoder.implementation)},
                {"bitrate", e->encoder.bitrate_kbps},
                {"keyframeMs", e->encoder.keyframe_interval_ms}};
    }
    return {};
}

void Document::setOutputEncodingOptions(const QString& id, const QString& rateControl, const QString& preset, int keyframeMs) {
    const auto encode = outputEncoderSettings(id).value("id").toString();
    setEncodeOptions(encode, rateControl, preset, keyframeMs);
}

void Document::setEncodeOptions(const QString& id, const QString& rateControl,
                                        const QString& preset, int keyframeMs) {
    if ((rateControl != "cbr" && rateControl != "vbr" && rateControl != "cqp" &&
         rateControl != "crf") || keyframeMs < 0 || keyframeMs > 600000 ||
        preset.size() > 128 || preset.contains(QChar::Null)) {
        emit notice(QStringLiteral("Invalid encoder settings."));
        return;
    }
    edit_encode(id, [&](ldoc::Encode& e) {
        e.encoder.rate_control = to_std(rateControl);
        e.encoder.preset = to_std(preset.trimmed());
        e.encoder.keyframe_interval_ms = keyframeMs;
    });
}

void Document::makeOutputEncodeIndependent(const QString& id) {
    const auto parsed = parse_id(id);
    if (!parsed) return;
    const auto& p = session_->project();
    for (const auto& o : p.outputs) {
        if (o.id != *parsed) continue;
        const auto* existing = p.encode(o.encode);
        if (!existing) return;
        const auto users = std::count_if(p.outputs.begin(), p.outputs.end(),
            [&](const auto& other) { return other.encode == o.encode; });
        if (users < 2) return;
        auto copy = *existing;
        copy.id = ldoc::Id::generate();
        copy.name += " (independent)";
        auto output = o;
        output.encode = copy.id;
        run(ldoc::make_batch({ldoc::AddEncode{copy, p.encodes.size()},
                             ldoc::SetOutput{output}}, "Make encoder independent"));
        return;
    }
}

void Document::setOutputEncoderExtra(const QString& id, const QString& key, const QVariant& value) {
    const auto encode = outputEncoderSettings(id).value("id").toString();
    setEncodeExtra(encode, key, value);
}

std::optional<ldoc::ParamValue> Document::encoderExtraValue(
    const QString& key, const QVariant& value) {
    const QMap<QString, QStringList> enums{
        {"nvenc.tuning", {"hq", "ll", "ull", "lossless"}},
        {"nvenc.multipass", {"disabled", "quarter", "full"}},
        {"nvenc.profile", {"", "baseline", "main", "high", "main10"}},
        {"x264.profile", {"", "baseline", "main", "high"}},
        {"x264.tune", {"", "film", "animation", "grain", "stillimage", "psnr", "ssim", "fastdecode", "zerolatency"}},
        {"audio.codec", {"aac", "opus"}},
        {"rescale_filter", {"bilinear", "bicubic", "lanczos"}}
    };
    const QMap<QString, QPair<int, int>> numbers{
        {"nvenc.bframes", {0, 4}}, {"x264.buffer_kbps", {0, 1000000}},
        {"quality", {0, 63}}, {"gpu", {-1, 255}}, {"audio.track", {1, 6}},
        {"audio.vod_track", {0, 6}}, {"audio.bitrate_kbps", {32, 512}}
    };
    const QStringList flags{"nvenc.lookahead", "nvenc.adaptive_quantization", "x264.custom_buffer"};
    ldoc::ParamValue stored;
    bool valid = false;
    if (enums.contains(key)) {
        valid = value.metaType().id() == QMetaType::QString && enums[key].contains(value.toString());
        stored = to_std(value.toString());
    } else if (numbers.contains(key)) {
        bool ok = false;
        const double number = value.toDouble(&ok);
        const auto bounds = numbers[key];
        valid = ok && std::isfinite(number) && std::floor(number) == number &&
                number >= bounds.first && number <= bounds.second;
        stored = valid ? static_cast<std::int64_t>(number) : std::int64_t{0};
    } else if (flags.contains(key)) {
        valid = value.metaType().id() == QMetaType::Bool;
        stored = value.toBool();
    } else if (key == "nvenc.options" || key == "x264.options") {
        const auto text = value.toString();
        valid = value.metaType().id() == QMetaType::QString && text.size() <= 4096 &&
                !text.contains(QChar::Null) && !text.contains('\n') && !text.contains('\r');
        stored = to_std(text);
    }
    return valid ? std::optional<ldoc::ParamValue>{stored} : std::nullopt;
}

void Document::setEncodeExtra(const QString& id, const QString& key, const QVariant& value) {
    const auto stored = encoderExtraValue(key, value);
    if (!stored) {
        emit notice(QStringLiteral("Invalid encoder option."));
        return;
    }
    edit_encode(id, [&](ldoc::Encode& e) { e.encoder.extra[to_std(key)] = *stored; });
}

void Document::saveOutputKey(const QString& id, const QString& key) {
    const auto parsed = parse_id(id);
    if (!parsed || key.isEmpty() || key.size() > 16384 || key.contains(QChar::Null)) {
        emit notice(QStringLiteral("Enter a nonempty stream key (at most 16384 characters)."));
        return;
    }
    QString ref;
    bool found = false;
    for (const auto& o : session_->project().outputs) {
        if (o.id == *parsed) {
            ref = qstr(o.destination.credential_ref);
            found = true;
            break;
        }
    }
    if (!found)
        return;
    if (ref.isEmpty())
        ref = QStringLiteral("libertas-output-%1").arg(id_str(ldoc::Id::generate()));
    const auto helper = QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
    if (helper.isEmpty()) {
        emit notice(QStringLiteral("Install secret-tool and unlock a Secret Service keyring to save keys."));
        return;
    }
    auto* process = new QProcess(this);
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    const auto projectId = session_->project().id;
    connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            emit notice(QStringLiteral("Could not start the stream key store."));
            process->deleteLater();
        }
    });
    connect(process, &QProcess::finished, this,
            [this, process, timeout, id, ref, projectId](int code, QProcess::ExitStatus status) {
        timeout->stop();
        if (status == QProcess::NormalExit && code == 0) {
            if (session_->project().id == projectId)
                setOutputCredentialRef(id, ref);
            emit notice(QStringLiteral("Stream key saved in the system keyring."));
        } else {
            emit notice(QStringLiteral("Stream key was not saved. Check that the Secret Service keyring is unlocked."));
        }
        process->deleteLater();
    });
    process->start(helper, {QStringLiteral("store"), QStringLiteral("--label=Libertas Studio stream key"),
                            QStringLiteral("application"), QStringLiteral("libertas-studio"),
                            QStringLiteral("credential"), ref});
    process->write(key.toUtf8());
    process->closeWriteChannel();
    timeout->start(30000);
}

void Document::setOutputCredentialRef(const QString& id, const QString& ref) {
    edit_output(id, [&](ldoc::Output& o) { o.destination.credential_ref = to_std(ref); });
}

// ---- the nodes of the current scene -----------------------------------------------------------

namespace {

/// Where a new node goes: inside the selected group if one is selected, otherwise at the top of
/// the scene. Top, because `roots[0]` is the front-most node and a new thing should be visible.
struct Destination {
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
};

} // namespace

void Document::addSourceNode(const QString& name) {
    const ldoc::Scene* s = current_scene();
    if (!s)
        return;
    ldoc::Node n = ldoc::make_node(ldoc::NodeKind::Source,
                                   name.isEmpty() ? std::string{"Source"} : to_std(name));
    const QString id = id_str(n.id);

    Destination dest;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        dest.parent = sel->id;
    run(ldoc::AddNode{s->id, dest.parent, dest.index, std::move(n)});
    selectNode(id);
}

void Document::addGroup(const QString& name) {
    const ldoc::Scene* s = current_scene();
    if (!s)
        return;
    ldoc::Node n = ldoc::make_node(ldoc::NodeKind::Group,
                                   name.isEmpty() ? std::string{"Group"} : to_std(name));
    const QString id = id_str(n.id);
    run(ldoc::AddNode{s->id, std::nullopt, 0, std::move(n)});
    selectNode(id);
}

void Document::groupSelectedNodes() {
    const ldoc::Scene* s = current_scene();
    const QStringList chosen = selectedNodes();
    if (!s || chosen.isEmpty()) {
        emit notice(QStringLiteral("Select the sources to group first."));
        return;
    }

    // Where the group goes: the place of the topmost selected node, so the grouped items keep
    // their draw order relative to everything else (ADR-0037). selectedNodes() is already in
    // scene order, so the first entry is that node.
    struct At {
        std::optional<ldoc::Id> parent;
        std::size_t index = 0;
        bool found = false;
    };
    const auto place_of = [&](const ldoc::Id& wanted) {
        At at;
        const std::function<void(const std::vector<ldoc::Node>&, std::optional<ldoc::Id>)> walk =
            [&](const std::vector<ldoc::Node>& nodes, std::optional<ldoc::Id> parent) {
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i].id == wanted) {
                        at = At{parent, i, true};
                        return;
                    }
                    if (const auto* g = std::get_if<ldoc::GroupData>(&nodes[i].data))
                        walk(g->children, nodes[i].id);
                }
            };
        walk(s->roots, std::nullopt);
        return at;
    };

    const auto first = parse_id(chosen.front());
    if (!first)
        return;
    const At top = place_of(*first);
    if (!top.found)
        return;

    // A node cannot be moved inside a group that is itself being moved inside the new one, and
    // a group that contains another selected node would take it along twice. Dropping the
    // descendants is right rather than refusing: selecting a group and something inside it
    // plainly means "group these", and the inner one is already coming.
    QStringList moving;
    for (const QString& id : chosen) {
        const auto parsed = parse_id(id);
        if (parsed && !inside_any(*s, chosen, *parsed))
            moving << id;
    }
    if (moving.isEmpty())
        return;

    ldoc::Node group = ldoc::make_node(ldoc::NodeKind::Group, "Group");
    const ldoc::Id group_id = group.id;

    std::vector<ldoc::Command> steps;
    steps.push_back(ldoc::AddNode{s->id, top.parent, top.index, std::move(group)});
    // In order, each to the end of the group, so the list inside reads the way it read outside.
    std::size_t at = 0;
    for (const QString& id : moving) {
        const auto parsed = parse_id(id);
        if (!parsed)
            continue;
        steps.push_back(ldoc::MoveNode{s->id, *parsed, group_id, at});
        ++at;
    }
    run(ldoc::make_batch(std::move(steps), "Group sources"));
    selectNode(id_str(group_id));
}

QString Document::parentOfNode(const QString& nodeId) const {
    const ldoc::Scene* s = current_scene();
    const auto wanted = parse_id(nodeId);
    if (!s || !wanted)
        return {};
    QString found;
    const std::function<void(const std::vector<ldoc::Node>&, const QString&)> walk =
        [&](const std::vector<ldoc::Node>& nodes, const QString& parent) {
            for (const ldoc::Node& n : nodes) {
                if (n.id == *wanted) {
                    found = parent;
                    return;
                }
                if (const auto* g = std::get_if<ldoc::GroupData>(&n.data))
                    walk(g->children, id_str(n.id));
            }
        };
    walk(s->roots, QString{});
    return found;
}

void Document::ungroupNode(const QString& nodeId) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(nodeId);
    const ldoc::Node* group = (s && parsed) ? ldoc::find_node(s->roots, *parsed) : nullptr;
    if (!group)
        return;
    const auto* data = std::get_if<ldoc::GroupData>(&group->data);
    if (!data) {
        emit notice(QStringLiteral("Only a group can be taken apart."));
        return;
    }
    if (data->size && !data->children.empty()) {
        emit notice(QStringLiteral("This group has saved surface bounds; ungrouping could change "
                                   "clipping and blending. Move its sources out individually."));
        return;
    }
    if (const auto* canvas = current_canvas(); canvas &&
        (!group_space(*group, *canvas).isIdentity() || group->blend.opacity != 1.0)) {
        emit notice(QStringLiteral("Reset the group's transform and opacity before ungrouping; "
                                   "ungrouping now would change its children's picture."));
        return;
    }

    // Where the group sits now, so its children land in its place rather than at the top.
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
    {
        const std::function<bool(const std::vector<ldoc::Node>&, std::optional<ldoc::Id>)> walk =
            [&](const std::vector<ldoc::Node>& nodes, std::optional<ldoc::Id> within) {
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i].id == *parsed) {
                        parent = within;
                        index = i;
                        return true;
                    }
                    if (const auto* g = std::get_if<ldoc::GroupData>(&nodes[i].data)) {
                        if (walk(g->children, nodes[i].id))
                            return true;
                    }
                }
                return false;
            };
        walk(s->roots, std::nullopt);
    }

    // An empty group is just removed. Moving nothing and then deleting it is the same
    // outcome with an extra step in the undo description.
    std::vector<ldoc::Command> steps;
    std::size_t at = index;
    for (const ldoc::Node& child : data->children) {
        steps.push_back(ldoc::MoveNode{s->id, child.id, parent, at});
        ++at;
    }
    steps.push_back(ldoc::RemoveNode{s->id, *parsed});
    // The children's ids, read before the command runs -- `group` points into the project and
    // the batch rewrites it.
    QStringList freed;
    for (const ldoc::Node& child : data->children)
        freed << id_str(child.id);

    run(ldoc::make_batch(std::move(steps),
                         std::format("Ungroup {}", group->name)));

    // Selected after, not before: the rebuild the session triggers prunes anything that is not
    // in the scene, and before the move the children were still inside the group.
    selected_node_.clear();
    also_selected_.clear();
    if (!freed.isEmpty()) {
        selected_node_ = freed.takeLast();
        also_selected_ = freed;
        selection_anchor_ = selected_node_;
    }
    emit selectionChanged();
}

void Document::selectAllNodes() {
    const ldoc::Scene* s = current_scene();
    if (!s || s->roots.empty())
        return;
    QStringList all;
    for (const ldoc::Node& n : s->roots)
        all << id_str(n.id);
    // The last one leads, so the Properties panel points at the bottom of the list rather
    // than at nothing -- and so a following ctrl-click has an anchor to measure from.
    selected_node_ = all.takeLast();
    also_selected_ = all;
    selection_anchor_ = selected_node_;
    emit selectionChanged();
}

namespace {

/// Where a node sits: its parent, and its index among that parent's children.
struct Seat {
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
    bool found = false;
};

Seat seat_of(const ldoc::Scene& scene, const ldoc::Id& wanted) {
    Seat out;
    const std::function<bool(const std::vector<ldoc::Node>&, std::optional<ldoc::Id>)> walk =
        [&](const std::vector<ldoc::Node>& nodes, std::optional<ldoc::Id> parent) {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].id == wanted) {
                    out = Seat{parent, i, true};
                    return true;
                }
                if (const auto* g = std::get_if<ldoc::GroupData>(&nodes[i].data)) {
                    if (walk(g->children, nodes[i].id))
                        return true;
                }
            }
            return false;
        };
    walk(scene.roots, std::nullopt);
    return out;
}

/// Whether `target` is `root` or sits anywhere inside it. Dropping a group into its own
/// contents is the one move that cannot be allowed: the tree would stop being a tree.
bool within(const ldoc::Scene& scene, const ldoc::Id& root, const ldoc::Id& target) {
    const ldoc::Node* n = ldoc::find_node(scene.roots, root);
    if (!n)
        return false;
    bool found = false;
    ldoc::visit(*n, [&](const ldoc::Node& each) {
        if (each.id == target)
            found = true;
    });
    return found;
}

} // namespace

void Document::moveSelectedIntoGroup(const QString& groupId) {
    const ldoc::Scene* s = current_scene();
    const QStringList chosen = selectedNodes();
    if (!s || chosen.isEmpty())
        return;

    std::optional<ldoc::Id> into;
    if (!groupId.isEmpty()) {
        const auto parsed = parse_id(groupId);
        const ldoc::Node* g = parsed ? ldoc::find_node(s->roots, *parsed) : nullptr;
        if (!g || !std::holds_alternative<ldoc::GroupData>(g->data)) {
            emit notice(QStringLiteral("That is not a group."));
            return;
        }
        into = *parsed;
    }

    // Where they land: the end of the group, in the order they are in now, so the list reads
    // the way it read before the move.
    std::size_t at = 0;
    if (into) {
        const ldoc::Node* g = ldoc::find_node(s->roots, *into);
        const auto* data = g ? std::get_if<ldoc::GroupData>(&g->data) : nullptr;
        at = data ? data->children.size() : 0;
    } else {
        at = s->roots.size();
    }

    std::vector<ldoc::Command> steps;
    for (const QString& id : chosen) {
        const auto parsed = parse_id(id);
        if (!parsed || inside_any(*s, chosen, *parsed))
            continue;
        // A node already in the right place is not a move, and MoveNode would make an undo
        // step out of it.
        if (parentOfNode(id) == groupId)
            continue;
        steps.push_back(ldoc::MoveNode{s->id, *parsed, into, at});
        ++at;
    }
    if (steps.empty())
        return;
    run(steps.size() == 1
            ? steps.front()
            : ldoc::make_batch(std::move(steps),
                               std::format("Move {} sources", steps.size())));
}

void Document::toggleGroupExpanded(const QString& canvasId, const QString& nodeId) {
    const auto* scene = scene_of_canvas(canvas_key(canvasId));
    const auto id = parse_id(nodeId);
    const auto* node = scene && id ? ldoc::find_node(scene->roots, *id) : nullptr;
    if (!node || !node->is_group())
        return;
    if (collapsed_groups_.removeAll(nodeId) == 0)
        collapsed_groups_ << nodeId;
    selectNodeOn(canvasId, nodeId);
    rebuild();
}

QVariantMap Document::nodeAtRow(int row) const {
    const auto it = nodes_by_canvas_.find(current_canvas_);
    if (it == nodes_by_canvas_.end() || !it->second)
        return {};
    const std::vector<NodeRow>& rows = it->second->rows();
    if (row < 0 || static_cast<std::size_t>(row) >= rows.size())
        return {};
    QVariantMap m;
    m["id"] = rows[static_cast<std::size_t>(row)].id;
    m["isGroup"] = rows[static_cast<std::size_t>(row)].isGroup;
    return m;
}

int Document::rowOfNode(const QString& nodeId) const {
    const auto it = nodes_by_canvas_.find(current_canvas_);
    if (it == nodes_by_canvas_.end() || !it->second)
        return -1;
    const std::vector<NodeRow>& rows = it->second->rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].id == nodeId)
            return static_cast<int>(i);
    }
    return -1;
}

bool Document::canDropAt(const QString& targetId, const QString& zone) const {
    const ldoc::Scene* s = current_scene();
    const auto target = parse_id(targetId);
    if (!s || !target)
        return false;
    const ldoc::Node* t = ldoc::find_node(s->roots, *target);
    if (!t)
        return false;
    if (zone == QLatin1String("into") && !std::holds_alternative<ldoc::GroupData>(t->data))
        return false;

    const QStringList chosen = selectedNodes();
    if (chosen.isEmpty())
        return false;
    for (const QString& id : chosen) {
        const auto parsed = parse_id(id);
        if (!parsed)
            continue;
        // Into itself, or into something it contains: the move has no meaning and the tree
        // would stop being one.
        if (within(*s, *parsed, *target))
            return false;
    }
    return true;
}

void Document::dropSelectedAt(const QString& targetId, const QString& zone) {
    const ldoc::Scene* s = current_scene();
    const auto target = parse_id(targetId);
    if (!s || !target || !canDropAt(targetId, zone))
        return;

    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
    if (zone == QLatin1String("into")) {
        const ldoc::Node* t = ldoc::find_node(s->roots, *target);
        const auto* g = t ? std::get_if<ldoc::GroupData>(&t->data) : nullptr;
        parent = *target;
        // The top of the group, which is the front of the draw order -- a layers panel drops
        // what you dragged above what was already there.
        index = 0;
        static_cast<void>(g);
    } else {
        const Seat at = seat_of(*s, *target);
        if (!at.found)
            return;
        parent = at.parent;
        index = zone == QLatin1String("below") ? at.index + 1 : at.index;
    }

    // Moved in order, each after the last, so a multi-row drag keeps its arrangement.
    const QStringList chosen = selectedNodes();
    std::vector<ldoc::Command> steps;
    std::size_t at = index;
    for (const QString& id : chosen) {
        const auto parsed = parse_id(id);
        if (!parsed || inside_any(*s, chosen, *parsed))
            continue;
        // Moving a node down within its own parent: every node before it shifts up by one as
        // it leaves, so the index it was given counts one too many.
        const Seat from = seat_of(*s, *parsed);
        std::size_t landing = at;
        if (from.found && from.parent == parent && from.index < at && landing > 0)
            --landing;
        steps.push_back(ldoc::MoveNode{s->id, *parsed, parent, landing});
        ++at;
    }
    if (steps.empty())
        return;
    run(steps.size() == 1
            ? steps.front()
            : ldoc::make_batch(std::move(steps),
                               std::format("Move {} sources", steps.size())));
}

void Document::removeSelectedNodes() {
    const ldoc::Scene* s = current_scene();
    const QStringList chosen = selectedNodes();
    if (!s || chosen.isEmpty())
        return;
    std::vector<ldoc::Command> steps;
    for (const QString& id : chosen) {
        const auto parsed = parse_id(id);
        if (!parsed || inside_any(*s, chosen, *parsed))
            continue;
        steps.push_back(ldoc::RemoveNode{s->id, *parsed});
    }
    if (steps.empty())
        return;
    run(steps.size() == 1
            ? steps.front()
            : ldoc::make_batch(std::move(steps),
                               std::format("Remove {} sources", steps.size())));
}

void Document::removeNode(const QString& id) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::RemoveNode{s->id, *parsed});
}

void Document::renameNode(const QString& id, const QString& name) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::RenameNode{s->id, *parsed, to_std(name)});
}

void Document::setNodeVisible(const QString& id, bool visible) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::SetNodeVisible{s->id, *parsed, visible});
}

void Document::setNodeLocked(const QString& id, bool locked) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::SetNodeLocked{s->id, *parsed, locked});
}

void Document::moveNode(const QString& id, int delta) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (!s || !parsed || delta == 0)
        return;

    // Find the node's siblings so the move stays within its own level: the Sources panel's up and
    // down move a thing within its group, not out of it.
    const std::function<bool(const std::vector<ldoc::Node>&, std::optional<ldoc::Id>)> search =
        [&](const std::vector<ldoc::Node>& nodes,
            std::optional<ldoc::Id> parent) -> bool {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].id == *parsed) {
                const auto target = static_cast<std::ptrdiff_t>(i) + delta;
                if (target < 0 || target >= static_cast<std::ptrdiff_t>(nodes.size()))
                    return true; // already at the end; not an error, just nothing to do
                run(ldoc::MoveNode{s->id, *parsed, parent,
                                   static_cast<std::size_t>(target)});
                return true;
            }
            if (const auto* kids = nodes[i].children()) {
                if (search(*kids, nodes[i].id))
                    return true;
            }
        }
        return false;
    };
    search(s->roots, std::nullopt);
}

// ---- properties -------------------------------------------------------------------------------

void Document::setNodePosition(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.position = ldoc::Vec2{x, y};
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setNodeFrame(double x, double y, double scaleX, double scaleY, bool dragging) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n || n->locked)
        return;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(scaleX) || !std::isfinite(scaleY))
        return;
    ldoc::Transform t = n->transform;
    t.position = ldoc::Vec2{x, y};
    t.scale = ldoc::Vec2{scaleX, scaleY};
    if (t == n->transform)
        return;
    const ldoc::SetNodeTransform command{s->id, n->id, t};
    if (!dragging) {
        run(command);
        return;
    }
    // One name for the whole drag, position and scale together, so the inverse kept is the
    // transform from before the press. Closed by endGesture().
    const std::string gesture = std::format("node.frame:{}", n->id.to_string());
    const auto applied = session_->run(command, gesture);
    if (!applied)
        emit notice(qstr(applied.error()));
}

bool Document::prepare_transform(const ldoc::Scene*& scene, const ldoc::Node*& node) {
    scene = current_scene();
    node = current_node();
    if (!scene || !node)
        return false;
    if (node->locked) {
        emit notice(QStringLiteral("That source is locked."));
        return false;
    }
    return true;
}

void Document::setDrawnRect(double x, double y, double width, double height, bool dragging) {
    const ldoc::Scene* scene = current_scene();
    const ldoc::Node* node = current_node();
    const ldoc::Canvas* canvas = current_canvas();
    if (!scene || !node || !canvas || node->locked)
        return;
    if (!std::isfinite(x) || !std::isfinite(y) || !(width > 0.0) || !(height > 0.0))
        return;
    bool invertible = false;
    const auto inverse = parent_space(*scene, node->id, *canvas).inverted(&invertible);
    if (!invertible) return;
    const auto local = inverse.mapRect(QRectF(x, y, width, height));
    x = local.x(); y = local.y(); width = local.width(); height = local.height();
    double natural_w = 0.0;
    double natural_h = 0.0;
    const bool have = natural_size(session_->project(), *canvas, *node, natural_w, natural_h);
    const auto solved =
        lcomp::solve_drawn_rect(*node, natural_w, natural_h, have, x, y, width, height);
    if (!solved)
        return;
    ldoc::Transform t = node->transform;
    t.position = ldoc::Vec2{solved->x, solved->y};
    t.scale = ldoc::Vec2{solved->scale_x, solved->scale_y};
    if (t == node->transform)
        return;
    const ldoc::SetNodeTransform command{scene->id, node->id, t};
    if (!dragging) {
        run(command);
        return;
    }
    const std::string gesture = std::format("node.frame:{}", node->id.to_string());
    const auto applied = session_->run(command, gesture);
    if (!applied)
        emit notice(qstr(applied.error()));
}

void Document::resetTransform() {
    const ldoc::Scene* scene = nullptr;
    const ldoc::Node* node = nullptr;
    if (!prepare_transform(scene, node))
        return;
    ldoc::Transform t = node->transform;
    t.position = {};
    t.rotation_deg = 0.0;
    t.scale = {1.0, 1.0};
    t.skew_deg = {};
    if (t == node->transform)
        return;
    run(ldoc::SetNodeTransform{scene->id, node->id, t});
}

void Document::rotateBy(double degrees) {
    const ldoc::Scene* scene = nullptr;
    const ldoc::Node* node = nullptr;
    if (!prepare_transform(scene, node) || !std::isfinite(degrees))
        return;
    ldoc::Transform t = node->transform;
    double turned = std::fmod(t.rotation_deg + degrees, 360.0);
    if (turned < 0.0)
        turned += 360.0;
    t.rotation_deg = turned;
    if (t == node->transform)
        return;
    run(ldoc::SetNodeTransform{scene->id, node->id, t});
}

void Document::flipHorizontal() {
    const ldoc::Scene* scene = nullptr;
    const ldoc::Node* node = nullptr;
    if (!prepare_transform(scene, node))
        return;
    ldoc::Transform t = node->transform;
    t.scale.x = t.scale.x == 0.0 ? -1.0 : -t.scale.x;
    if (t == node->transform)
        return;
    run(ldoc::SetNodeTransform{scene->id, node->id, t});
}

void Document::flipVertical() {
    const ldoc::Scene* scene = nullptr;
    const ldoc::Node* node = nullptr;
    if (!prepare_transform(scene, node))
        return;
    ldoc::Transform t = node->transform;
    t.scale.y = t.scale.y == 0.0 ? -1.0 : -t.scale.y;
    if (t == node->transform)
        return;
    run(ldoc::SetNodeTransform{scene->id, node->id, t});
}

void Document::fitToCanvas(bool stretch) {
    const ldoc::Scene* scene = nullptr;
    const ldoc::Node* node = nullptr;
    if (!prepare_transform(scene, node))
        return;
    const ldoc::Canvas* canvas = current_canvas();
    if (!canvas || canvas->width <= 0 || canvas->height <= 0)
        return;
    const QVariantMap rect = placed_rect(*scene, *canvas, *node);
    const double w = rect.value(QStringLiteral("width")).toDouble();
    const double h = rect.value(QStringLiteral("height")).toDouble();
    if (!(w > 0.0) || !(h > 0.0))
        return;
    const double cw = static_cast<double>(canvas->width);
    const double ch = static_cast<double>(canvas->height);
    if (stretch) {
        setDrawnRect(0.0, 0.0, cw, ch, false);
        return;
    }
    const double s = std::min(cw / w, ch / h);
    const double nw = w * s;
    const double nh = h * s;
    setDrawnRect((cw - nw) / 2.0, (ch - nh) / 2.0, nw, nh, false);
}

void Document::centerOnCanvas(bool horizontal, bool vertical) {
    const ldoc::Scene* scene = nullptr;
    const ldoc::Node* node = nullptr;
    if (!prepare_transform(scene, node))
        return;
    const ldoc::Canvas* canvas = current_canvas();
    if (!canvas || canvas->width <= 0 || canvas->height <= 0)
        return;
    const QVariantMap rect = placed_rect(*scene, *canvas, *node);
    const double w = rect.value(QStringLiteral("width")).toDouble();
    const double h = rect.value(QStringLiteral("height")).toDouble();
    if (!(w > 0.0) || !(h > 0.0))
        return;
    double nx = rect.value(QStringLiteral("x")).toDouble();
    double ny = rect.value(QStringLiteral("y")).toDouble();
    if (horizontal)
        nx = (static_cast<double>(canvas->width) - w) / 2.0;
    if (vertical)
        ny = (static_cast<double>(canvas->height) - h) / 2.0;
    setDrawnRect(nx, ny, w, h, false);
}

void Document::copyTransform() {
    const ldoc::Node* node = current_node();
    if (!node) {
        emit notice(QStringLiteral("Select a source to copy the transform from."));
        return;
    }
    copied_transform_ = node->transform;
    emit notice(QStringLiteral("Copied the transform."));
}

void Document::pasteTransform() {
    if (!copied_transform_) {
        emit notice(QStringLiteral("There is no transform to paste."));
        return;
    }
    const ldoc::Scene* scene = nullptr;
    const ldoc::Node* node = nullptr;
    if (!prepare_transform(scene, node))
        return;
    if (node->transform == *copied_transform_)
        return;
    run(ldoc::SetNodeTransform{scene->id, node->id, *copied_transform_});
}

bool Document::hasCopiedTransform() const { return copied_transform_.has_value(); }

void Document::moveSelectedToEnd(bool front) {
    const ldoc::Scene* scene = current_scene();
    const QString id = selectedNode();
    const auto parsed = parse_id(id);
    if (!scene || !parsed)
        return;
    const std::function<bool(const std::vector<ldoc::Node>&)> search =
        [&](const std::vector<ldoc::Node>& nodes) -> bool {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].id == *parsed) {
                const int delta = front ? -static_cast<int>(i)
                                        : static_cast<int>(nodes.size() - 1 - i);
                moveNode(id, delta);
                return true;
            }
            if (const auto* kids = nodes[i].children()) {
                if (search(*kids))
                    return true;
            }
        }
        return false;
    };
    search(scene->roots);
}

void Document::requestRename() {
    if (selected_node_.isEmpty())
        return;
    emit renameRequested(selected_node_);
}

void Document::setNodeScale(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.scale = ldoc::Vec2{x, y};
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setNodeRotation(double degrees) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.rotation_deg = degrees;
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setNodeOpacity(double opacity) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Blend b = n->blend;
    b.opacity = std::clamp(opacity, 0.0, 1.0);
    run(ldoc::SetNodeBlend{s->id, n->id, b});
}

QStringList Document::fitModes() const {
    QStringList out;
    for (int i = 0; i < 6; ++i) {
        const auto name = ldoc::to_string(static_cast<ldoc::FitMode>(i));
        out << QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
    }
    return out;
}

void Document::setNodeFit(const QString& fit) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    const auto parsed = ldoc::fit_mode_from_string(to_std(fit));
    if (!s || !n || !parsed)
        return;
    ldoc::Layout l = n->layout;
    l.fit = *parsed;
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeBoxSize(double w, double h) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.size = ldoc::Vec2{std::max(0.0, w), std::max(0.0, h)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeCrop(double left, double top, double right, double bottom) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.crop = ldoc::CropRect{std::max(0.0, left), std::max(0.0, top), std::max(0.0, right),
                            std::max(0.0, bottom)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeAlignment(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.alignment = ldoc::Vec2{std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeBlendMode(const QString& mode) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    const auto parsed = ldoc::blend_mode_from_string(to_std(mode));
    if (!s || !n || !parsed)
        return;
    ldoc::Blend b = n->blend;
    b.mode = *parsed;
    run(ldoc::SetNodeBlend{s->id, n->id, b});
}

// ---- history and persistence -------------------------------------------------------------------

const ldoc::Project& Document::projectForBindings() const { return session_->project(); }

void Document::runCommand(const ldoc::Command& command) { run(command); }

void Document::undo() {
    if (const auto r = session_->undo(); !r)
        emit notice(qstr(r.error()));
}

void Document::redo() {
    if (const auto r = session_->redo(); !r)
        emit notice(qstr(r.error()));
}

void Document::save() {
    if (session_->path().empty()) {
        saveCurrentCollection();
        return;
    }
    if (const auto r = session_->save(); !r)
        emit notice(QStringLiteral("Could not save: %1").arg(qstr(r.error())));
    else
        rememberCollection();
}

} // namespace libertas::ui

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphics.hpp"

#include "document.hpp"

#include <QDir>
#include <QFile>
#include <QTextStream>

#include <ldoc/json.hpp>
#include <ldoc/project_io.hpp>

#include <algorithm>

namespace munkicast::ui {
namespace {

QString to_qt(const ldoc::Id& id) {
    return id.is_nil() ? QString{} : QString::fromStdString(id.to_string());
}

ldoc::Id from_qt(const QString& text) {
    const auto parsed = lc::Ulid::parse(text.toStdString());
    return parsed ? *parsed : ldoc::Id{};
}

QString hex(std::uint32_t rgb) {
    return QStringLiteral("#%1").arg(rgb & 0xFFFFFF, 6, 16, QLatin1Char('0'));
}

/// The channels a layer has, by the names the UI uses.
const std::pair<const char*, ldoc::ChannelId> kChannels[] = {
    {"x", ldoc::ChannelId::X},
    {"y", ldoc::ChannelId::Y},
    {"width", ldoc::ChannelId::Width},
    {"height", ldoc::ChannelId::Height},
    {"rotation", ldoc::ChannelId::Rotation},
    {"opacity", ldoc::ChannelId::Opacity},
};

const ldoc::Channel* channel_of(const ldoc::Layer& l, ldoc::ChannelId which) {
    switch (which) {
    case ldoc::ChannelId::X:
        return &l.x;
    case ldoc::ChannelId::Y:
        return &l.y;
    case ldoc::ChannelId::Width:
        return &l.width;
    case ldoc::ChannelId::Height:
        return &l.height;
    case ldoc::ChannelId::Rotation:
        return &l.rotation;
    case ldoc::ChannelId::Opacity:
        return &l.opacity;
    }
    return nullptr;
}

QVariantMap channel_map(const ldoc::Channel& c) {
    QVariantList keys;
    for (const ldoc::Keyframe& k : c.keys) {
        keys.append(QVariantMap{{QStringLiteral("at"), static_cast<qlonglong>(k.at)},
                                {QStringLiteral("value"), k.value},
                                {QStringLiteral("ease"), k.ease}});
    }
    return QVariantMap{{QStringLiteral("constant"), c.constant},
                       {QStringLiteral("animated"), c.animated()},
                       {QStringLiteral("keys"), keys}};
}

} // namespace

Graphics* Graphics::create(QQmlEngine* engine, QJSEngine*) {
    auto* g = new Graphics;
    if (engine) {
        g->document_ = engine->singletonInstance<Document*>("MunkiCast", "Document");
        if (g->document_) {
            connect(g->document_, &Document::projectOpened, g, [g](const QString& dir) {
                g->setProject(dir.isEmpty() ? std::filesystem::path{}
                                            : std::filesystem::path{dir.toStdString()});
            });
            // A QML singleton is created the first time something asks for it, which may be
            // long after the project was opened. Seeding it here is what stops it starting
            // with no project in a session that already has one.
            const QString open = g->document_->projectDirectory();
            if (!open.isEmpty())
                g->setProject(std::filesystem::path{open.toStdString()});
        }
    }
    return g;
}

Graphics::Graphics(QObject* parent) : QObject(parent) {}

ldoc::GraphicHistory* Graphics::history() {
    return open_.is_nil() ? nullptr : bench_.graphic(open_);
}

const ldoc::GraphicHistory* Graphics::history() const {
    return open_.is_nil() ? nullptr : bench_.graphic(open_);
}

const ldoc::Layer* Graphics::selected() const {
    const ldoc::GraphicHistory* h = history();
    return h && !selected_layer_.is_nil() ? h->document().layer(selected_layer_) : nullptr;
}

QVariantMap Graphics::graphic() const {
    const ldoc::GraphicHistory* h = history();
    if (!h)
        return {};
    const ldoc::Graphic& g = h->document();

    QVariantList layers;
    for (const ldoc::Layer& l : g.layers) {
        QVariantMap channels;
        for (const auto& [name, which] : kChannels)
            channels.insert(QLatin1String(name), channel_map(*channel_of(l, which)));
        layers.append(QVariantMap{
            {QStringLiteral("id"), to_qt(l.id)},
            {QStringLiteral("name"), QString::fromStdString(l.name)},
            {QStringLiteral("kind"), QString::fromStdString(std::string{to_string(l.kind)})},
            {QStringLiteral("visible"), l.visible},
            {QStringLiteral("locked"), l.locked},
            {QStringLiteral("channels"), channels},
        });
    }

    return QVariantMap{
        {QStringLiteral("id"), to_qt(g.id)},
        {QStringLiteral("name"), QString::fromStdString(g.name)},
        {QStringLiteral("fps"), g.fps.hz()},
        {QStringLiteral("width"), g.width},
        {QStringLiteral("height"), g.height},
        {QStringLiteral("duration"), static_cast<qlonglong>(g.duration)},
        {QStringLiteral("playhead"), static_cast<qlonglong>(g.playhead)},
        {QStringLiteral("layers"), layers},
    };
}

QVariantList Graphics::frame() const {
    const ldoc::GraphicHistory* h = history();
    if (!h)
        return {};
    const ldoc::Graphic& g = h->document();
    const ldoc::Frame at = g.playhead;

    QVariantList out;
    for (const ldoc::Layer& l : g.layers) {
        // Every channel evaluated at the playhead, which is the whole of what the stage needs
        // to draw. Doing it here rather than in QML keeps one interpolator in the project
        // instead of a second one that could disagree with the exported page.
        out.append(QVariantMap{
            {QStringLiteral("id"), to_qt(l.id)},
            {QStringLiteral("name"), QString::fromStdString(l.name)},
            {QStringLiteral("kind"), QString::fromStdString(std::string{to_string(l.kind)})},
            {QStringLiteral("visible"), l.visible},
            {QStringLiteral("locked"), l.locked},
            {QStringLiteral("x"), l.x.value_at(at)},
            {QStringLiteral("y"), l.y.value_at(at)},
            {QStringLiteral("width"), l.width.value_at(at)},
            {QStringLiteral("height"), l.height.value_at(at)},
            {QStringLiteral("rotation"), l.rotation.value_at(at)},
            {QStringLiteral("opacity"), std::clamp(l.opacity.value_at(at), 0.0, 1.0)},
            {QStringLiteral("color"), hex(l.color)},
            {QStringLiteral("text"), QString::fromStdString(l.text)},
            {QStringLiteral("dataField"), QString::fromStdString(l.data_field)},
            {QStringLiteral("fontSize"), l.font_size},
            {QStringLiteral("source"), QString::fromStdString(l.source)},
            {QStringLiteral("selected"), l.id == selected_layer_},
        });
    }
    return out;
}

QStringList Graphics::dataFields() const {
    const ldoc::GraphicHistory* h = history();
    if (!h)
        return {};
    QStringList fields;
    for (const std::string& field : h->document().data_fields())
        fields << QString::fromStdString(field);
    return fields;
}

QVariantList Graphics::graphics() const {
    QVariantList rows;
    if (!document_)
        return rows;
    for (const ldoc::SubdocRef& ref : document_->projectForBindings().graphics) {
        rows.append(QVariantMap{{QStringLiteral("id"), to_qt(ref.id)},
                                {QStringLiteral("name"), QString::fromStdString(ref.name)},
                                {QStringLiteral("open"), ref.id == open_}});
    }
    return rows;
}

QString Graphics::selectedLayer() const {
    return to_qt(selected_layer_);
}

QVariantMap Graphics::selectedLayerDetails() const {
    const ldoc::Layer* l = selected();
    if (!l)
        return {};
    QVariantMap channels;
    for (const auto& [name, which] : kChannels)
        channels.insert(QLatin1String(name), channel_map(*channel_of(*l, which)));
    return QVariantMap{
        {QStringLiteral("id"), to_qt(l->id)},
        {QStringLiteral("name"), QString::fromStdString(l->name)},
        {QStringLiteral("kind"), QString::fromStdString(std::string{to_string(l->kind)})},
        {QStringLiteral("visible"), l->visible},
        {QStringLiteral("locked"), l->locked},
        {QStringLiteral("color"), hex(l->color)},
        {QStringLiteral("text"), QString::fromStdString(l->text)},
        {QStringLiteral("dataField"), QString::fromStdString(l->data_field)},
        {QStringLiteral("fontSize"), l->font_size},
        {QStringLiteral("channels"), channels},
    };
}

bool Graphics::canUndo() const {
    const ldoc::GraphicHistory* h = history();
    return h && h->can_undo();
}

bool Graphics::canRedo() const {
    const ldoc::GraphicHistory* h = history();
    return h && h->can_redo();
}

bool Graphics::dirty() const {
    const ldoc::GraphicHistory* h = history();
    return h && h->dirty();
}

void Graphics::setProject(const std::filesystem::path& dir) {
    bench_.clear();
    open_ = {};
    selected_layer_ = {};
    dir_ = dir;
    emit changed();
    emit listChanged();
    emit selectionChanged();
}

QString Graphics::createGraphic(const QString& name) {
    if (dir_.empty()) {
        emit notice(tr("Open a project before making a graphic."));
        return {};
    }

    ldoc::Graphic g;
    g.id = ldoc::Id::generate();
    g.name = name.isEmpty() ? tr("Graphic").toStdString() : name.toStdString();

    if (const auto r = ldoc::save_graphic(dir_, g, 0); !r) {
        emit notice(tr("The graphic could not be written: %1")
                        .arg(QString::fromStdString(r.error())));
        return {};
    }

    ldoc::SubdocRef ref;
    ref.id = g.id;
    ref.name = g.name;
    if (document_)
        document_->runCommand(
            ldoc::AddGraphicRef{ref, document_->projectForBindings().graphics.size()});

    const ldoc::Id id = g.id;
    bench_.open_graphic(std::move(g));
    open_ = id;
    selected_layer_ = {};
    emit changed();
    emit listChanged();
    emit selectionChanged();
    return to_qt(id);
}

void Graphics::openGraphic(const QString& id) {
    const ldoc::Id wanted = from_qt(id);
    if (wanted.is_nil())
        return;
    if (!bench_.graphic(wanted)) {
        auto loaded = ldoc::load_graphic(dir_, wanted);
        if (!loaded) {
            emit notice(tr("That graphic could not be opened: %1")
                            .arg(QString::fromStdString(loaded.error())));
            return;
        }
        bench_.open_graphic(std::move(*loaded));
    }
    open_ = wanted;
    selected_layer_ = {};
    emit changed();
    emit listChanged();
    emit selectionChanged();
}

void Graphics::save() {
    ldoc::GraphicHistory* h = history();
    if (!h || dir_.empty())
        return;
    if (const auto r = ldoc::save_graphic(dir_, h->document(), 0); !r) {
        emit notice(tr("The graphic could not be saved: %1")
                        .arg(QString::fromStdString(r.error())));
        return;
    }
    h->note_saved();
    emit changed();
}

void Graphics::run(const ldoc::GraphicCommand& command, const QString& gesture) {
    ldoc::GraphicHistory* h = history();
    if (!h)
        return;
    const auto applied = gesture.isEmpty() ? h->run(command)
                                           : h->run(command, gesture.toStdString());
    if (!applied) {
        emit notice(QString::fromStdString(applied.error()));
        return;
    }
    emit changed();
    emit selectionChanged();
}

void Graphics::undo() {
    ldoc::GraphicHistory* h = history();
    if (!h)
        return;
    if (const auto r = h->undo(); !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    emit changed();
    emit selectionChanged();
}

void Graphics::redo() {
    ldoc::GraphicHistory* h = history();
    if (!h)
        return;
    if (const auto r = h->redo(); !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    emit changed();
    emit selectionChanged();
}

void Graphics::endGesture() {
    if (ldoc::GraphicHistory* h = history())
        h->end_gesture();
}

void Graphics::setPlayhead(int frame) {
    ldoc::GraphicHistory* h = history();
    if (!h)
        return;
    // Not a command, for the same reason the timeline's playhead is not one.
    const_cast<ldoc::Graphic&>(h->document()).playhead =
        std::clamp<ldoc::Frame>(frame, 0, h->document().duration);
    emit changed();
}

void Graphics::addLayer(const QString& kind) {
    const ldoc::GraphicHistory* h = history();
    if (!h)
        return;
    const ldoc::Graphic& g = h->document();

    ldoc::Layer l;
    l.id = ldoc::Id::generate();
    l.kind = kind == QLatin1String("text")   ? ldoc::LayerKind::Text
             : kind == QLatin1String("image") ? ldoc::LayerKind::Image
                                              : ldoc::LayerKind::Rect;
    l.name = QStringLiteral("%1 %2")
                 .arg(kind.isEmpty() ? QStringLiteral("Layer") : kind)
                 .arg(g.layers.size() + 1)
                 .toStdString();

    // Something visible, placed in the lower third where a broadcast graphic usually lands.
    // A layer added at 0x0 looks like nothing happened.
    l.x.constant = g.width * 0.08;
    l.y.constant = g.height * 0.72;
    l.width.constant = g.width * 0.40;
    l.height.constant = l.kind == ldoc::LayerKind::Text ? 64.0 : g.height * 0.10;
    l.opacity.constant = 1.0;
    if (l.kind == ldoc::LayerKind::Text) {
        l.text = tr("Text").toStdString();
        l.color = 0xFFFFFF;
    } else {
        l.color = 0x1D4E89;
    }

    // An image layer with no file would be refused by `apply`, so it is not offered until
    // there is a picture to give it.
    if (l.kind == ldoc::LayerKind::Image) {
        emit notice(tr("Add an image layer by dropping a picture on the stage."));
        return;
    }
    run(ldoc::AddLayer{std::move(l), g.layers.size()});
}

void Graphics::removeSelectedLayer() {
    if (selected_layer_.is_nil())
        return;
    run(ldoc::RemoveLayer{selected_layer_});
    selected_layer_ = {};
    emit selectionChanged();
}

void Graphics::selectLayer(const QString& id) {
    selected_layer_ = from_qt(id);
    emit selectionChanged();
    emit changed();
}

void Graphics::moveSelectedLayer(double x, double y) {
    const ldoc::Layer* l = selected();
    if (!l)
        return;
    const ldoc::GraphicHistory* h = history();
    const ldoc::Frame at = h->document().playhead;

    // Dragging a layer that has keyframes moves the key at the playhead rather than the
    // constant, because otherwise the drag would appear to do nothing: the curve would go on
    // overriding the value being changed.
    auto set = [&](ldoc::ChannelId which, const ldoc::Channel& current, double value) {
        ldoc::Channel next = current;
        if (next.keys.empty()) {
            next.constant = value;
        } else {
            const auto key = std::ranges::find(next.keys, at, &ldoc::Keyframe::at);
            if (key != next.keys.end()) {
                key->value = value;
            } else {
                const auto after = std::ranges::upper_bound(next.keys, at, {},
                                                            &ldoc::Keyframe::at);
                next.keys.insert(after, ldoc::Keyframe{at, value, true});
            }
        }
        return ldoc::SetChannel{selected_layer_, which, std::move(next)};
    };

    run(ldoc::make_graphic_batch({set(ldoc::ChannelId::X, l->x, x),
                            set(ldoc::ChannelId::Y, l->y, y)},
                           tr("Move '%1'").arg(QString::fromStdString(l->name)).toStdString()),
        QStringLiteral("layer.move:%1").arg(to_qt(selected_layer_)));
}

void Graphics::resizeSelectedLayer(double width, double height) {
    const ldoc::Layer* l = selected();
    if (!l)
        return;
    ldoc::Channel w = l->width;
    ldoc::Channel hgt = l->height;
    w.constant = std::max(1.0, width);
    hgt.constant = std::max(1.0, height);
    run(ldoc::make_graphic_batch(
            {ldoc::SetChannel{selected_layer_, ldoc::ChannelId::Width, std::move(w)},
             ldoc::SetChannel{selected_layer_, ldoc::ChannelId::Height, std::move(hgt)}},
            tr("Resize '%1'").arg(QString::fromStdString(l->name)).toStdString()),
        QStringLiteral("layer.resize:%1").arg(to_qt(selected_layer_)));
}

void Graphics::setLayerProperty(const QString& key, const QVariant& value) {
    const ldoc::Layer* l = selected();
    if (!l)
        return;
    // SetLayer carries the whole value, so every field has to be filled from the current one or
    // the ones not being edited would be reset.
    ldoc::SetLayer edit{l->id,    l->name,       l->visible,     l->locked,
                        l->color, l->text,       l->data_field,  l->font_size};

    if (key == QLatin1String("name"))
        edit.name = value.toString().toStdString();
    else if (key == QLatin1String("text"))
        edit.text = value.toString().toStdString();
    else if (key == QLatin1String("dataField"))
        edit.data_field = value.toString().toStdString();
    else if (key == QLatin1String("fontSize"))
        edit.font_size = value.toDouble();
    else if (key == QLatin1String("visible"))
        edit.visible = value.toBool();
    else if (key == QLatin1String("locked"))
        edit.locked = value.toBool();
    else if (key == QLatin1String("color"))
        edit.color = value.toString().mid(1).toUInt(nullptr, 16) & 0xFFFFFF;
    else
        return;

    run(edit);
}

bool Graphics::hasKeyframe(const QString& channel) const {
    const ldoc::Layer* l = selected();
    const ldoc::GraphicHistory* h = history();
    if (!l || !h)
        return false;
    for (const auto& [name, which] : kChannels) {
        if (channel != QLatin1String(name))
            continue;
        const ldoc::Channel* c = channel_of(*l, which);
        return std::ranges::any_of(c->keys, [at = h->document().playhead](
                                                const ldoc::Keyframe& k) { return k.at == at; });
    }
    return false;
}

void Graphics::setKeyframe(const QString& channel, bool on) {
    const ldoc::Layer* l = selected();
    ldoc::GraphicHistory* h = history();
    if (!l || !h)
        return;
    const ldoc::Frame at = h->document().playhead;

    for (const auto& [name, which] : kChannels) {
        if (channel != QLatin1String(name))
            continue;
        const ldoc::Channel* current = channel_of(*l, which);
        ldoc::Channel next = *current;
        const auto existing = std::ranges::find(next.keys, at, &ldoc::Keyframe::at);

        if (on) {
            if (existing != next.keys.end())
                return; // already keyed here
            // The value it has at this frame, so adding a key never moves anything: the curve
            // through it is the same curve until the operator drags it.
            const double value = current->value_at(at);
            const auto after = std::ranges::upper_bound(next.keys, at, {}, &ldoc::Keyframe::at);
            next.keys.insert(after, ldoc::Keyframe{at, value, true});
        } else {
            if (existing == next.keys.end())
                return;
            // Removing the last key leaves the constant at the value the layer had, rather
            // than snapping back to whatever it was before it was ever animated.
            const double value = current->value_at(at);
            next.keys.erase(existing);
            if (next.keys.empty())
                next.constant = value;
        }
        run(ldoc::SetChannel{selected_layer_, which, std::move(next)});
        return;
    }
}

QString Graphics::exportHtml() {
    const ldoc::GraphicHistory* h = history();
    if (!h || dir_.empty())
        return {};
    const ldoc::Graphic& g = h->document();

    const QString folder = QString::fromStdString((dir_ / "Graphics").string());
    QDir().mkpath(folder);
    const QString path = QDir(folder).filePath(
        QString::fromStdString(g.id.to_string()) + QStringLiteral(".html"));

    // The document, as JSON, inside a page that plays it.
    //
    // The whole graphic is embedded rather than fetched, so the page is one file: a browser
    // source points at it and nothing else has to be served. The player below is the same
    // interpolation `Channel::value_at` does, which is why the preview on the stage and the
    // picture on air agree.
    const QString json = QString::fromStdString(ldoc::graphic_to_json(g, 0));

    QString page;
    QTextStream out(&page);
    out << "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    out << "<title>" << QString::fromStdString(g.name).toHtmlEscaped() << "</title>\n";
    out << "<style>\n";
    out << "  html,body{margin:0;padding:0;background:transparent;overflow:hidden}\n";
    out << "  #stage{position:relative;width:" << g.width << "px;height:" << g.height
        << "px;transform-origin:top left}\n";
    out << "  .layer{position:absolute;will-change:transform,opacity}\n";
    out << "  .text{white-space:pre;font-family:sans-serif;line-height:1}\n";
    out << "  img.layer{object-fit:contain}\n";
    out << "</style>\n</head>\n<body>\n<div id=\"stage\"></div>\n<script>\n";
    out << "const DOC = " << json << ";\n";
    out << R"JS(
// The show's data, set by the playout system. `window.setFields({name: "..."})` replaces the
// authored placeholder text of any layer bound to that field -- which is what makes this a
// template rather than a finished picture.
let FIELDS = {};
window.setFields = function (fields) {
  FIELDS = Object.assign({}, FIELDS, fields || {});
  draw(playhead);
};

const stage = document.getElementById('stage');
const fps = DOC.fps && DOC.fps.den ? DOC.fps.num / DOC.fps.den : 30;

// Scaled to the window, so one page plays at whatever size the browser source is given.
function fit() {
  const sx = window.innerWidth / DOC.width;
  const sy = window.innerHeight / DOC.height;
  const s = Math.min(sx, sy);
  stage.style.transform = 'scale(' + s + ')';
  stage.style.left = ((window.innerWidth - DOC.width * s) / 2) + 'px';
  stage.style.top = ((window.innerHeight - DOC.height * s) / 2) + 'px';
}
window.addEventListener('resize', fit);

// The same curve the editor draws: held at the ends, smoothstep between eased keys.
function valueAt(channel, at) {
  const keys = channel.keys || [];
  if (keys.length === 0) return channel.constant || 0;
  if (at <= keys[0].at) return keys[0].value;
  if (at >= keys[keys.length - 1].at) return keys[keys.length - 1].value;
  let i = 0;
  while (i < keys.length - 1 && keys[i + 1].at <= at) i++;
  const a = keys[i], b = keys[i + 1];
  const span = b.at - a.at;
  if (span <= 0) return b.value;
  let t = (at - a.at) / span;
  if (a.ease) t = t * t * (3 - 2 * t);
  return a.value + (b.value - a.value) * t;
}

const nodes = (DOC.layers || []).map(function (layer) {
  const el = layer.kind === 'image' ? document.createElement('img')
                                    : document.createElement('div');
  el.className = 'layer' + (layer.kind === 'text' ? ' text' : '');
  if (layer.kind === 'image') el.src = layer.source;
  stage.appendChild(el);
  return { layer: layer, el: el };
});

function draw(frame) {
  for (const node of nodes) {
    const l = node.layer, el = node.el;
    const w = valueAt(l.width, frame), h = valueAt(l.height, frame);
    el.style.left = valueAt(l.x, frame) + 'px';
    el.style.top = valueAt(l.y, frame) + 'px';
    el.style.width = w + 'px';
    el.style.height = h + 'px';
    el.style.opacity = l.visible === false ? 0 : Math.max(0, Math.min(1, valueAt(l.opacity, frame)));
    el.style.transform = 'rotate(' + valueAt(l.rotation, frame) + 'deg)';
    const colour = '#' + ('000000' + (l.color || 0).toString(16)).slice(-6);
    if (l.kind === 'text') {
      el.style.color = colour;
      el.style.fontSize = (l.font_size || 48) + 'px';
      // The bound field when the show has supplied one, the authored text otherwise -- so the
      // page previews with real-looking content and plays with the real thing.
      const bound = l.data_field && Object.prototype.hasOwnProperty.call(FIELDS, l.data_field);
      el.textContent = bound ? FIELDS[l.data_field] : (l.text || '');
    } else if (l.kind === 'rect') {
      el.style.background = colour;
    }
  }
}

// Playback. `window.play()` from the start, `window.stop()` to hold the last frame; a browser
// source that just loads the page plays it once, which is what a lower third does.
let playhead = 0, startedAt = 0, running = false;
function tick(now) {
  if (!running) return;
  playhead = Math.min(DOC.duration, Math.round((now - startedAt) / 1000 * fps));
  draw(playhead);
  if (playhead < DOC.duration) requestAnimationFrame(tick);
  else running = false;
}
window.play = function () {
  running = true;
  startedAt = performance.now();
  requestAnimationFrame(tick);
};
window.stop = function () { running = false; };
window.seek = function (frame) { playhead = frame; draw(frame); };

fit();
draw(0);
window.play();
)JS";
    out << "\n</script>\n</body>\n</html>\n";
    out.flush();

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit notice(tr("The page could not be written to %1.").arg(path));
        return {};
    }
    file.write(page.toUtf8());
    file.close();
    emit notice(tr("Exported %1").arg(path));
    return path;
}

} // namespace munkicast::ui

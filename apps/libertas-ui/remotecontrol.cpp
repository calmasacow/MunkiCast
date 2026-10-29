// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "remotecontrol.hpp"

#include "document.hpp"
#include "lcomp/filters.hpp"
#include "ldoc/json.hpp"
#include "preview.hpp"
#include "shortcuts.hpp"

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMap>
#include <QMetaMethod>
#include <QSaveFile>
#include <QScreen>
#include <QStorageInfo>
#include <QSysInfo>
#include <QUrl>
#include <QWindow>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sys/resource.h>

namespace libertas::ui {
namespace {
QString text(const std::string& value) {
    return QString::fromStdString(value);
}

QString id(const ldoc::Id& value) {
    return text(value.to_string());
}

QJsonObject object(const std::string& json) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
}

RemoteResult ok(const QJsonObject& data = {}) {
    return {100, {}, data};
}

RemoteResult fail(int code, const QString& why) {
    return {code, why, {}};
}

QJsonObject protocol() {
    static const auto data = [] {
        QFile file(":/remote/data/remote-protocol.json");
        if (!file.open(QIODevice::ReadOnly))
            return QJsonObject{};
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return data;
}

bool matches(const QJsonObject& data, const QString& prefix, const ldoc::Id& key,
             const std::string& name) {
    // An explicitly supplied identity is authoritative, including an invalid UUID.
    return data.contains(prefix + "Uuid") ? data[prefix + "Uuid"].toString() == id(key)
                                          : data[prefix + "Name"].toString() == text(name);
}

QJsonObject params(const ldoc::ParamMap& values) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.settings = values;
    const auto json = object(ldoc::command_to_json(ldoc::SetSource{source}));
    return json["value"].toObject()["settings"].toObject();
}

std::expected<ldoc::ParamMap, QString> readParams(const QJsonObject& values) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    auto json = object(ldoc::command_to_json(ldoc::SetSource{source}));
    auto value = json["value"].toObject();
    value["settings"] = values;
    json["value"] = value;
    auto command = ldoc::command_from_json(QJsonDocument(json).toJson().toStdString());
    if (!command)
        return std::unexpected(text(command.error()));
    return std::get<ldoc::SetSource>(*command).value.settings;
}

QString blendName(ldoc::BlendMode mode) {
    if (mode == ldoc::BlendMode::LinearDodge)
        return "OBS_BLEND_ADDITIVE";
    return "OBS_BLEND_" + QString::fromUtf8(ldoc::to_string(mode)).toUpper().replace('-', '_');
}

QString wireKind(const QString& kind, bool obs) {
    static const QMap<QString, QString> aliases{
        {"source.media", "ffmpeg_source"},
        {"source.playlist", "vlc_source"},
        {"source.image", "image_source"},
        {"source.slideshow", "slideshow"},
        {"source.text", "text_ft2_source_v2"},
        {"source.color", "color_source_v3"},
        {"source.browser", "browser_source"},
        {"source.v4l2", "v4l2_input"},
        {"source.blackmagic", "decklink-input"},
        {"source.audio-input", "pulse_input_capture"},
        {"source.audio-output", "pulse_output_capture"},
        {"source.screen", "pipewire-desktop-capture-source"},
        {"source.window", "pipewire-window-capture-source"}};
    return obs ? aliases.value(kind, kind) : kind;
}

QString nativeKind(const QString& kind) {
    const QStringList kinds{"source.media",       "source.playlist",     "source.image",
                            "source.slideshow",   "source.text",         "source.color",
                            "source.browser",     "source.v4l2",         "source.blackmagic",
                            "source.audio-input", "source.audio-output", "source.screen",
                            "source.window"};
    for (const auto& key : kinds)
        if (wireKind(key, true) == kind)
            return key;
    return kind;
}

QJsonObject wireSettings(QJsonObject values, const QString& kind, bool outgoing) {
    QMap<QString, QString> keys;
    if (kind == "source.media" || kind == "source.playlist")
        keys = {{"localFile", "local_file"},
                {"loop", "looping"},
                {"clearOnEnd", "clear_on_media_end"},
                {"hardwareDecode", "hw_decode"},
                {"restartOnActivate", "restart_on_activate"},
                {"closeWhenInactive", "close_when_inactive"},
                {"speedPercent", "speed_percent"}};
    else if (kind == "source.browser")
        keys = {{"shutdownWhenHidden", "shutdown"}};
    else if (kind == "source.image")
        keys = {{"unloadWhenHidden", "unload"}};
    else if (kind == "source.audio-input" || kind == "source.audio-output")
        keys = {{"device", "device_id"}};
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const auto from = outgoing ? it.key() : it.value(), to = outgoing ? it.value() : it.key();
        if (values.contains(from)) {
            values[to] = values[from];
            values.remove(from);
        }
    }
    if (kind == "source.text") {
        if (outgoing) {
            if (values.contains("font") || values.contains("size")) {
                values["font"] = QJsonObject{{"face", values["font"]},
                                             {"size", values["size"]},
                                             {"style", "Regular"},
                                             {"flags", 0}};
                values.remove("size");
            }
        } else if (values["font"].isObject()) {
            const auto font = values["font"].toObject();
            if (font.contains("size"))
                values["size"] = font["size"];
            values["font"] = font["face"];
        }
    }
    if ((kind == "source.color" || kind == "source.text") && values.contains("color")) {
        if (outgoing && values["color"].isString()) {
            const QColor color(values["color"].toString());
            if (color.isValid())
                values["color"] = static_cast<double>(static_cast<quint32>(color.alpha()) << 24 |
                                                      static_cast<quint32>(color.blue()) << 16 |
                                                      static_cast<quint32>(color.green()) << 8 |
                                                      static_cast<quint32>(color.red()));
        } else if (!outgoing && values["color"].isDouble()) {
            const double number = values["color"].toDouble();
            if (number >= 0 && number <= 4294967295.0) {
                const auto color = static_cast<quint32>(number);
                values["color"] =
                    QColor(static_cast<int>(color & 255), static_cast<int>((color >> 8) & 255),
                           static_cast<int>((color >> 16) & 255),
                           static_cast<int>((color >> 24) & 255))
                        .name(QColor::HexArgb);
            }
        }
    }
    return values;
}

bool callable(const QMetaMethod& method) {
    if (method.methodType() != QMetaMethod::Method || method.access() != QMetaMethod::Public ||
        method.parameterCount() > 10)
        return false;
    const auto supported = [](QMetaType t) {
        return t == QMetaType::fromType<QString>() || t == QMetaType::fromType<bool>() ||
               t == QMetaType::fromType<int>() || t == QMetaType::fromType<double>() ||
               t == QMetaType::fromType<QStringList>() || t == QMetaType::fromType<QVariant>() ||
               t == QMetaType::fromType<QVariantList>() || t == QMetaType::fromType<QVariantMap>();
    };
    for (int i = 0; i < method.parameterCount(); ++i)
        if (!supported(method.parameterMetaType(i)))
            return false;
    return method.returnMetaType().id() == QMetaType::Void || supported(method.returnMetaType());
}
} // namespace

RemoteControl::RemoteControl(Document& document, Preview& preview, bool obs, Shortcuts* shortcuts)
    : document_(document), preview_(preview), obs_(obs), shortcuts_(shortcuts) {}

int RemoteControl::itemId(const ldoc::Id& key) {
    auto [it, added] = item_ids_.try_emplace(key, next_item_id_);
    if (added)
        ++next_item_id_;
    return it->second;
}

const ldoc::Scene* RemoteControl::scene(const QJsonObject& data, const QString& prefix) const {
    for (const auto& s : document_.projectForBindings().scenes)
        if (matches(data, prefix, s.id, s.name) &&
            (!data.contains("canvasUuid") || id(s.canvas) == data["canvasUuid"].toString()))
            return &s;
    return nullptr;
}

const ldoc::Source* RemoteControl::source(const QJsonObject& data, const QString& prefix) const {
    for (const auto& s : document_.projectForBindings().sources)
        if (matches(data, prefix, s.id, s.name))
            return &s;
    return nullptr;
}

RemoteResult RemoteControl::execute(const ldoc::Command& command) {
    const auto result = document_.remoteCommand(command);
    return result ? ok() : fail(402, text(result.error()));
}

QString RemoteControl::transitionName(const QString& key) const {
    for (const auto& v : document_.transitions()) {
        const auto t = v.toMap();
        if (t["id"].toString() == key)
            return t["title"].toString();
    }
    return key;
}

QJsonObject RemoteControl::project() const {
    const auto& p = document_.projectForBindings();
    auto json = object(ldoc::project_to_json(p));
    QJsonArray scenes;
    for (const auto& s : p.scenes)
        scenes.append(object(ldoc::scene_to_json(s)));
    json["sceneDocuments"] = scenes;
    json["assets"] = object(ldoc::assets_to_json(p.assets))["assets"];
    return json;
}

QJsonObject RemoteControl::state() {
    return {{"revision", document_.revision()},
            {"projectName", document_.projectName()},
            {"mainDeck", QJsonObject::fromVariantMap(document_.mainDeck())},
            {"streaming", preview_.streaming()},
            {"recording", preview_.recording()},
            {"replayActive", preview_.replayActive()},
            {"outputPending", preview_.outputPending()},
            {"replaySaving", preview_.replaySaving()},
            {"lastReplayPath", preview_.lastReplayPath()},
            {"recordingPath", preview_.recordingPath()},
            {"renderStats", QJsonObject::fromVariantMap(preview_.renderStats({}))},
            {"outputStats", QJsonObject::fromVariantMap(preview_.outputStats())}};
}

QJsonArray RemoteControl::methods() const {
    QJsonArray result;
    const auto* meta = document_.metaObject();
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const auto m = meta->method(i);
        if (!callable(m))
            continue;
        QJsonArray args;
        for (int n = 0; n < m.parameterCount(); ++n)
            args.append(QJsonObject{{"name", QString::fromUtf8(m.parameterNames()[n])},
                                    {"type", QString::fromUtf8(m.parameterTypes()[n])}});
        result.append(QJsonObject{{"method", QString::fromUtf8(m.methodSignature())},
                                  {"arguments", args},
                                  {"returnType", QString::fromUtf8(m.typeName())}});
    }
    return result;
}

RemoteResult RemoteControl::invoke(const QJsonObject& data) {
    const auto signature = data["method"].toString().toUtf8();
    const auto* meta = document_.metaObject();
    const int index = meta->indexOfMethod(signature.constData());
    if (index < meta->methodOffset())
        return fail(600, "Unknown document method signature.");
    const auto m = meta->method(index);
    if (!callable(m))
        return fail(602, "This method cannot be invoked remotely.");
    if (!data["arguments"].isArray())
        return fail(400, "arguments must be an array.");
    const auto values = data["arguments"].toArray();
    if (values.size() != m.parameterCount())
        return fail(402, "Wrong argument count.");
    QVariant storage[10];
    QGenericArgument args[10];
    for (int i = 0; i < values.size(); ++i) {
        const auto t = m.parameterMetaType(i);
        const auto v = values[i];
        if ((t.id() == QMetaType::Bool && !v.isBool()) ||
            (t.id() == QMetaType::QString && !v.isString()) ||
            ((t.id() == QMetaType::Double || t.id() == QMetaType::Int) && !v.isDouble()) ||
            (t.id() == QMetaType::Int && (v.toDouble() != std::trunc(v.toDouble()) ||
                                          v.toDouble() < std::numeric_limits<int>::min() ||
                                          v.toDouble() > std::numeric_limits<int>::max())) ||
            (t.id() == QMetaType::QVariantMap && !v.isObject()) ||
            ((t.id() == QMetaType::QStringList || t.id() == QMetaType::QVariantList) &&
             !v.isArray()))
            return fail(401, "Argument type mismatch.");
        if (t.id() == QMetaType::QStringList)
            for (const auto entry : v.toArray())
                if (!entry.isString())
                    return fail(401, "String-list arguments require strings.");
        storage[i] = v.toVariant();
        if (t.id() != QMetaType::QVariant && !storage[i].convert(t))
            return fail(401, "Argument cannot be converted.");
        args[i] = QGenericArgument(
            t.name(), t.id() == QMetaType::QVariant ? &storage[i] : storage[i].constData());
    }
    QVariant result(m.returnMetaType());
    QGenericReturnArgument ret;
    if (m.returnMetaType().id() != QMetaType::Void)
        ret = QGenericReturnArgument(
            m.typeName(), m.returnMetaType().id() == QMetaType::QVariant ? &result : result.data());
    QString error;
    const auto connection = QObject::connect(&document_, &Document::notice, &document_,
                                             [&](const QString& why) { error = why; });
    const bool invoked = m.invoke(&document_, Qt::DirectConnection, ret, args[0], args[1], args[2],
                                  args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
    QObject::disconnect(connection);
    if (!invoked)
        return fail(205, "Document invocation failed.");
    if (!error.isEmpty())
        return fail(402, error);
    return ok({{"result", QJsonValue::fromVariant(result)}, {"revision", document_.revision()}});
}

QJsonArray RemoteControl::availableRequests() const {
    QJsonArray names;
    const auto requests = protocol();
    for (auto it = requests.begin(); it != requests.end(); ++it)
        if (it.value().toObject()["supported"].toBool() &&
            (!obs_ || !it.key().startsWith("Libertas")))
            names.append(it.key());
    return names;
}

QJsonObject RemoteControl::item(const ldoc::Scene& s, const ldoc::Node& node, int index) {
    const auto& p = document_.projectForBindings();
    const auto* binding = std::get_if<ldoc::SourceData>(&node.data);
    const auto* src = binding && binding->source ? p.source(*binding->source) : nullptr;
    const auto* pre = std::get_if<ldoc::PrecompData>(&node.data);
    const auto* nested = pre ? p.scene(pre->scene) : nullptr;
    const auto* canvas = p.canvas(s.canvas);
    const auto size = document_.placedRect(id(s.canvas), id(node.id));
    return {
        {"sceneItemId", itemId(node.id)},
        {"sceneItemUuid", id(node.id)},
        {"sceneItemIndex", index},
        {"sourceName", src      ? text(src->name)
                       : nested ? text(nested->name)
                                : text(node.name)},
        {"sourceUuid", src      ? id(src->id)
                       : nested ? id(nested->id)
                                : id(node.id)},
        {"sourceType",
         node.is_group() || nested ? "OBS_SOURCE_TYPE_SCENE" : "OBS_SOURCE_TYPE_INPUT"},
        {"inputKind", src ? wireKind(text(src->type), obs_) : QJsonValue(QJsonValue::Null)},
        {"isGroup", node.is_group()},
        {"sceneItemEnabled", node.visible},
        {"sceneItemLocked", node.locked},
        {"sceneItemBlendMode", blendName(node.blend.mode)},
        {"sceneItemTransform",
         QJsonObject{
             {"positionX", node.transform.position.x},
             {"positionY", node.transform.position.y},
             {"rotation", node.transform.rotation_deg},
             {"scaleX", node.transform.scale.x},
             {"scaleY", node.transform.scale.y},
             {"alignment", 5},
             {"boundsType", "OBS_BOUNDS_NONE"},
             {"boundsAlignment", 0},
             {"boundsWidth", node.layout.size.x},
             {"boundsHeight", node.layout.size.y},
             {"cropLeft", node.layout.crop.left},
             {"cropTop", node.layout.crop.top},
             {"cropRight", node.layout.crop.right},
             {"cropBottom", node.layout.crop.bottom},
             {"sourceWidth", size.value("naturalWidth", canvas ? canvas->width : 0).toDouble()},
             {"sourceHeight", size.value("naturalHeight", canvas ? canvas->height : 0).toDouble()},
             {"width", size.value("width").toDouble()},
             {"height", size.value("height").toDouble()}}}};
}

RemoteResult RemoteControl::request(const QString& type, const QJsonObject& data) {
    const auto schema = protocol()[type].toObject();
    if (schema.isEmpty() || !schema["supported"].toBool() || (obs_ && type.startsWith("Libertas")))
        return fail(204, schema["reason"].toString(
                             "Unknown or unavailable request. See docs/remote-control.md."));
    for (const auto v : schema["requestFields"].toArray()) {
        const auto field = v.toObject();
        const auto name = field["valueName"].toString();
        if (!data.contains(name)) {
            if (!field["valueOptional"].toBool())
                return fail(300, "Missing field: " + name);
            continue;
        }
        const auto value = data[name];
        const auto t = field["valueType"].toString();
        if ((t == "String" && !value.isString()) || (t == "Number" && !value.isDouble()) ||
            (t == "Boolean" && !value.isBool()) || (t == "Object" && !value.isObject()) ||
            (t.startsWith("Array") && !value.isArray()))
            return fail(401, "Wrong type: " + name);
    }
    const auto& p = document_.projectForBindings();
    const auto deckMap = document_.mainDeck();
    const auto deckKey = ldoc::Id::parse(deckMap["id"].toString().toStdString());
    const auto deckIt =
        std::ranges::find(p.decks, deckKey.value_or(ldoc::Id::nil()), &ldoc::Deck::id);
    const auto* deck = deckIt == p.decks.end() ? nullptr : &*deckIt;
    const auto* canvas = p.canvases.empty() ? nullptr : &p.canvases.front();
    if (type == "LibertasGetScreenshot" || type == "GetSourceScreenshot" ||
        type == "SaveSourceScreenshot") {
        QString canvasKey = data["canvasId"].toString();
        if (!type.startsWith("Libertas")) {
            const auto* sc = scene(data, "source");
            if (!sc || std::ranges::none_of(
                           p.decks, [&](const auto& d) { return d.program_scene == sc->id; }))
                return fail(602, "Only scenes currently on program can be captured. Arbitrary "
                                 "source rendering is unavailable.");
            canvasKey = id(sc->canvas);
        } else if (!canvasKey.isEmpty()) {
            const auto key = ldoc::Id::parse(canvasKey.toStdString());
            if (!key || !p.canvas(*key))
                return fail(600, "Canvas not found.");
        }
        auto image = preview_.snapshot(canvasKey);
        if (image.isNull())
            return fail(604, "No rendered frame available.");
        const auto format = data["imageFormat"].toString("png").toLower();
        if (format != "png" && format != "jpg" && format != "jpeg")
            return fail(402, "Use png or jpeg.");
        const double width = data["imageWidth"].toDouble(image.width());
        const double height = data["imageHeight"].toDouble(image.height());
        if (width < 1 || height < 1 || width > 4096 || height > 4096 ||
            width != std::trunc(width) || height != std::trunc(height))
            return fail(402, "Screenshot dimensions must be integers from 1 to 4096.");
        image = image.scaled(static_cast<int>(width), static_cast<int>(height), Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        const double quality = data["imageCompressionQuality"].toDouble(-1);
        if (quality < -1 || quality > 100 || quality != std::trunc(quality))
            return fail(402, "Image quality must be -1 or an integer from 0 to 100.");
        if (!image.save(&buffer, format.toLatin1().constData(), static_cast<int>(quality)))
            return fail(604, "Image encoding failed.");
        if (type == "SaveSourceScreenshot") {
            const auto path = data["imageFilePath"].toString();
            if (!QFileInfo(path).isAbsolute())
                return fail(402, "Screenshot file path must be absolute.");
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
                !file.commit())
                return fail(604, "Screenshot could not be saved.");
            return ok();
        }
        const auto encoded = QString::fromLatin1(bytes.toBase64());
        return ok({{"imageData", type.startsWith("Libertas")
                                     ? encoded
                                     : "data:image/" +
                                           (format == "png" ? QString("png") : QString("jpeg")) +
                                           ";base64," + encoded},
                   {"mimeType", format == "png" ? "image/png" : "image/jpeg"},
                   {"width", image.width()},
                   {"height", image.height()}});
    }
    if (type == "LibertasGetCommandTemplates") {
        QJsonArray templates;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            const auto append = [&]<std::size_t N>() {
                using T = std::variant_alternative_t<N, ldoc::Command>;
                if constexpr (std::is_same_v<T, ldoc::Batch>)
                    templates.append(
                        object(ldoc::command_to_json(ldoc::make_batch({}, "Remote batch"))));
                else
                    templates.append(object(ldoc::command_to_json(T{})));
            };
            (append.template operator()<I>(), ...);
        }(std::make_index_sequence<std::variant_size_v<ldoc::Command>>{});
        return ok({{"templates", templates}});
    }
    if (type == "LibertasGetState")
        return ok(state());
    if (type == "LibertasGetProject")
        return ok({{"project", project()}});
    if (type == "LibertasGetMethods")
        return ok({{"methods", methods()}});
    if (type == "LibertasInvoke")
        return invoke(data);
    if (type == "LibertasExecuteCommand") {
        auto command = ldoc::command_from_json(
            QJsonDocument(data["command"].toObject()).toJson().toStdString());
        if (!command)
            return fail(402, text(command.error()));
        auto result = execute(*command);
        result.data["revision"] = document_.revision();
        return result;
    }
    if (type == "LibertasGetCapabilities")
        return ok({{"protocolVersion", 1},
                   {"mode", obs_ ? "obs" : "libertas"},
                   {"requests", protocol()},
                   {"methods", methods()}});
    if (type == "GetVersion")
        return ok({{"obsVersion", "32.0.0"},
                   {"obsWebSocketVersion", "5.6.3"},
                   {"rpcVersion", 1},
                   {"platform", QSysInfo::kernelType()},
                   {"platformDescription",
                    "Libertas Studio (OBS compatibility); " + QSysInfo::prettyProductName()},
                   {"availableRequests", availableRequests()},
                   {"supportedImageFormats", QJsonArray{"png", "jpeg"}}});
    if (type == "GetSceneList" || type == "GetCanvasList" || type == "GetGroupList") {
        QJsonArray scenes, canvases, groups;
        for (const auto& c : p.canvases)
            canvases.append(QJsonObject{{"canvasUuid", id(c.id)},
                                        {"canvasName", text(c.name)},
                                        {"canvasWidth", c.width},
                                        {"canvasHeight", c.height}});
        for (const auto& s : p.scenes) {
            if (data.contains("canvasUuid") && id(s.canvas) != data["canvasUuid"].toString())
                continue;
            scenes.append(QJsonObject{{"sceneName", text(s.name)},
                                      {"sceneUuid", id(s.id)},
                                      {"sceneIndex", scenes.size()},
                                      {"canvasUuid", id(s.canvas)}});
            ldoc::visit(s.roots, [&](const ldoc::Node& n) {
                if (n.is_group())
                    groups.append(text(n.name));
            });
        }
        const auto* program = deck && deck->program_scene ? p.scene(*deck->program_scene) : nullptr;
        const auto* preview = deck && deck->preview_scene ? p.scene(*deck->preview_scene) : nullptr;
        return ok({{"scenes", scenes},
                   {"canvases", canvases},
                   {"groups", groups},
                   {"currentProgramSceneName", program ? text(program->name) : QString{}},
                   {"currentProgramSceneUuid", program ? id(program->id) : QString{}},
                   {"currentPreviewSceneName", preview ? text(preview->name) : QString{}},
                   {"currentPreviewSceneUuid", preview ? id(preview->id) : QString{}}});
    }
    if (type == "GetCurrentProgramScene" || type == "GetCurrentPreviewScene") {
        if (!deck)
            return fail(600, "No Main deck.");
        if (type == "GetCurrentPreviewScene" && !deck->studio_mode)
            return fail(506, "Studio Mode is disabled.");
        const auto key =
            type == "GetCurrentProgramScene" ? deck->program_scene : deck->preview_scene;
        const auto* s = key ? p.scene(*key) : nullptr;
        if (!s)
            return fail(600, "No scene on this deck.");
        return ok({{"sceneName", text(s->name)}, {"sceneUuid", id(s->id)}});
    }
    if (type == "SetCurrentProgramScene" || type == "SetCurrentPreviewScene") {
        const auto* s = scene(data);
        if (!s)
            return fail(600, "Scene not found.");
        const auto it = std::ranges::find(p.decks, s->canvas, &ldoc::Deck::canvas);
        if (it == p.decks.end())
            return fail(600, "Scene has no deck.");
        if (type == "SetCurrentPreviewScene") {
            if (!it->studio_mode)
                return fail(506, "Studio Mode is disabled.");
            return execute(ldoc::SetDeckPreview{it->id, s->id});
        }
        // The command bus applies linked canvas cues, including during Studio Mode.
        return execute(ldoc::SetDeckProgram{it->id, s->id});
    }
    if (type == "CreateScene") {
        if (!canvas)
            return fail(600, "No canvas.");
        if (data["sceneName"].toString().trimmed().isEmpty())
            return fail(402, "Scene name is empty.");
        if (scene(data))
            return fail(601, "Scene already exists.");
        ldoc::Scene value;
        value.id = ldoc::Id::generate();
        value.canvas = canvas->id;
        if (data.contains("canvasUuid")) {
            auto key = ldoc::Id::parse(data["canvasUuid"].toString().toStdString());
            if (!key || !p.canvas(*key))
                return fail(600, "Canvas not found.");
            value.canvas = *key;
        }
        value.name = data["sceneName"].toString().toStdString();
        auto result = execute(ldoc::AddScene{value, p.scenes.size()});
        result.data = {{"sceneUuid", id(value.id)}};
        return result;
    }
    if (type == "RemoveScene" || type == "SetSceneName") {
        const auto* s = scene(data);
        if (!s)
            return fail(600, "Scene not found.");
        if (type == "RemoveScene")
            return execute(ldoc::RemoveScene{s->id});
        if (data["newSceneName"].toString().trimmed().isEmpty())
            return fail(402, "Scene name is empty.");
        if (scene({{"sceneName", data["newSceneName"]}}))
            return fail(601, "Scene name already exists.");
        return execute(ldoc::RenameScene{s->id, data["newSceneName"].toString().toStdString()});
    }
    if (type == "GetStudioModeEnabled")
        return ok({{"studioModeEnabled", deck && deck->studio_mode}});
    if (type == "SetStudioModeEnabled" || type == "TriggerStudioModeTransition" ||
        type == "SetCurrentSceneTransition" || type == "SetCurrentSceneTransitionDuration" ||
        type == "SetTBarPosition") {
        if (!deck)
            return fail(600, "No Main deck.");
        if (type == "SetStudioModeEnabled") {
            auto value = *deck;
            value.studio_mode = data["studioModeEnabled"].toBool();
            if (value.studio_mode && !value.preview_scene)
                value.preview_scene = value.program_scene;
            return execute(ldoc::SetDeck{value});
        }
        if (type == "TriggerStudioModeTransition") {
            if (!deck->studio_mode)
                return fail(506, "Studio Mode is disabled.");
            if (!deck->preview_scene)
                return fail(600, "No preview scene.");
            document_.takeDeck(id(deck->id));
            return ok();
        }
        if (type == "SetTBarPosition") {
            if (!deck->studio_mode)
                return fail(506, "Studio Mode is disabled.");
            const auto pos = data["position"].toDouble();
            if (pos < 0 || pos > 1)
                return fail(402, "position must be 0–1.");
            tbar_ = pos;
            preview_.setManualTransition(pos);
            if (data["release"].toBool(true)) {
                if (pos == 1)
                    document_.takeDeck(id(deck->id));
                preview_.setManualTransition(-1);
                tbar_ = 0;
            }
            return ok();
        }
        auto value = *deck;
        if (type == "SetCurrentSceneTransitionDuration") {
            const double duration = data["transitionDuration"].toDouble();
            if (duration < 50 || duration > 20000 || duration != std::trunc(duration))
                return fail(402, "Duration must be 50–20000 ms.");
            value.transition_ms = static_cast<int>(duration);
        } else {
            bool found = false;
            for (const auto& v : document_.transitions()) {
                const auto t = v.toMap();
                if (t["title"].toString() == data["transitionName"].toString() &&
                    t["implemented"].toBool()) {
                    value.transition = t["id"].toString().toStdString();
                    found = true;
                    break;
                }
            }
            if (!found)
                return fail(600, "Implemented transition not found.");
        }
        return execute(ldoc::SetDeck{value});
    }
    if (type == "SetCurrentSceneTransitionSettings") {
        if (!deck)
            return fail(600, "No Main deck.");
        const auto values = data["transitionSettings"].toObject();
        if (!deck->transition.starts_with("transition.stinger:"))
            return values.isEmpty() ? ok()
                                    : fail(602, "This transition has no configurable settings.");
        const auto details = document_.stingerSettings(id(deck->id));
        const auto key = ldoc::Id::parse(details["sourceId"].toString().toStdString());
        const auto* input = key ? p.source(*key) : nullptr;
        if (!input)
            return fail(600, "Stinger media not found.");
        auto value = *input;
        auto cut = details["cutMs"].toDouble(), duration = details["durationMs"].toDouble();
        for (auto field = values.begin(); field != values.end(); ++field) {
            if (field.key() == "path" && field.value().isString()) {
                const QFileInfo file(field.value().toString());
                if (!file.isFile() || !file.isReadable())
                    return fail(402, "Stinger path must name a readable media file.");
                value.settings["localFile"] = file.absoluteFilePath().toStdString();
            } else if (field.key() == "transition_point" && field.value().isDouble())
                cut = field.value().toDouble();
            else if (field.key() == "duration" && field.value().isDouble())
                duration = field.value().toDouble();
            else if (field.key() == "tp_type" && field.value().toInt(-1) == 0) {
            } else
                return fail(602, "Stinger settings support path, transition_point (milliseconds), "
                                 "duration, tp_type=0.");
        }
        if (cut < 0 || duration < 1 || duration > 600000 || cut > duration ||
            cut != std::trunc(cut) || duration != std::trunc(duration))
            return fail(402, "Stinger cut must be within a duration of 1–600000 ms.");
        value.settings["stingerCutMs"] = static_cast<std::int64_t>(cut);
        value.settings["stingerDurationMs"] = static_cast<std::int64_t>(duration);
        return execute(ldoc::SetSource{value});
    }
    if (type == "GetSceneTransitionList" || type == "GetCurrentSceneTransition" ||
        type == "GetTransitionKindList" || type == "GetCurrentSceneTransitionCursor") {
        const auto transitionKind = [&](const QString& key) {
            if (!obs_)
                return key;
            if (key == "transition.cut")
                return QString("cut_transition");
            if (key == "transition.fade")
                return QString("fade_transition");
            if (key == "transition.fade-to-black")
                return QString("fade_to_color_transition");
            if (key.startsWith("transition.stinger:"))
                return QString("obs_stinger_transition");
            return key;
        };
        QJsonObject settings;
        const bool stinger = deck && deck->transition.starts_with("transition.stinger:");
        if (stinger) {
            const auto values = document_.stingerSettings(id(deck->id));
            settings = {{"path", values["file"].toString()},
                        {"transition_point", values["cutMs"].toDouble()},
                        {"duration", values["durationMs"].toDouble()},
                        {"tp_type", 0}};
        }
        QJsonArray transitions, kinds;
        for (const auto& v : document_.transitions()) {
            const auto t = v.toMap();
            if (!t["implemented"].toBool())
                continue;
            const auto key = t["id"].toString();
            if (!kinds.contains(transitionKind(key)))
                kinds.append(transitionKind(key));
            transitions.append(
                QJsonObject{{"transitionName", t["title"].toString()},
                            {"transitionKind", transitionKind(key)},
                            {"transitionFixed",
                             key == "transition.cut" || key.startsWith("transition.stinger:")},
                            {"transitionConfigurable", key.startsWith("transition.stinger:")}});
        }
        return ok({{"transitions", transitions},
                   {"transitionKinds", kinds},
                   {"currentSceneTransitionName",
                    deck ? transitionName(text(deck->transition)) : QString{}},
                   {"currentSceneTransitionUuid", deck ? text(deck->transition) : QString{}},
                   {"transitionName", deck ? transitionName(text(deck->transition)) : QString{}},
                   {"transitionKind", deck ? transitionKind(text(deck->transition)) : QString{}},
                   {"transitionFixed", stinger || (deck && deck->transition == "transition.cut")},
                   {"transitionConfigurable", stinger},
                   {"transitionDuration", deck ? deck->transition_ms : 0},
                   {"transitionSettings", settings},
                   {"transitionCursor", tbar_}});
    }
    if (type == "GetInputList") {
        QJsonArray inputs;
        for (const auto& s : p.sources) {
            if (data.contains("inputKind") &&
                nativeKind(data["inputKind"].toString()) != text(s.type))
                continue;
            inputs.append(QJsonObject{{"inputName", text(s.name)},
                                      {"inputUuid", id(s.id)},
                                      {"inputKind", wireKind(text(s.type), obs_)},
                                      {"unversionedInputKind", wireKind(text(s.type), obs_)}});
        }
        return ok({{"inputs", inputs}});
    }
    if (type == "GetInputKindList") {
        QJsonArray kinds;
        for (const auto& v : document_.sourceTypes())
            kinds.append(wireKind(v.toMap()["id"].toString(), obs_));
        return ok({{"inputKinds", kinds}});
    }
    if (type == "GetInputDefaultSettings") {
        const auto kind = nativeKind(data["inputKind"].toString());
        bool found = false;
        for (const auto& v : document_.sourceTypes())
            if (v.toMap()["id"].toString() == kind)
                found = true;
        if (!found)
            return fail(600, "Input kind not found. Use GetInputKindList.");
        QJsonObject defaults;
        for (const auto& v : document_.sourceSettingsSchema(kind)) {
            const auto row = v.toMap();
            const auto key = row["key"].toString();
            if (!key.isEmpty())
                defaults[key] = QJsonValue::fromVariant(row["value"]);
        }
        return ok({{"defaultInputSettings", obs_ ? wireSettings(defaults, kind, true) : defaults}});
    }
    if (type == "GetInputPropertiesListPropertyItems" || type == "OpenInputFiltersDialog" ||
        type == "OpenInputPropertiesDialog") {
        const auto* input = source(data);
        if (!input)
            return fail(600, "Input not found.");
        if (type == "OpenInputFiltersDialog") {
            document_.showFilters(id(input->id));
            return ok();
        }
        if (type == "OpenInputPropertiesDialog") {
            for (auto* window : QGuiApplication::topLevelWindows())
                if (auto* dialog = window->findChild<QObject*>("sourceSettingsDialog")) {
                    if (QMetaObject::invokeMethod(dialog, "open", Q_ARG(QVariant, id(input->id))))
                        return ok();
                }
            return fail(602, "Open a workspace containing the Sources panel first.");
        }
        auto property = data["propertyName"].toString();
        if (obs_)
            property =
                wireSettings({{property, QJsonValue(QJsonValue::Null)}}, text(input->type), false)
                    .keys()
                    .value(0);
        for (const auto& value : document_.sourceSettingsSchema(text(input->type))) {
            const auto row = value.toMap();
            if (row["key"].toString() != property)
                continue;
            if (row["kind"].toString() != "choice")
                return fail(602, "Property is not a list.");
            QJsonArray items;
            const auto labels = row["choiceLabels"].toMap();
            for (const auto& choice : row["choices"].toStringList())
                items.append(QJsonObject{{"itemName", labels.value(choice, choice).toString()},
                                         {"itemValue", choice},
                                         {"itemEnabled", true}});
            return ok({{"propertyItems", items}});
        }
        return fail(600, "Input property not found.");
    }
    if (type == "GetSpecialInputs")
        return ok({{"desktop1", QJsonValue(QJsonValue::Null)},
                   {"desktop2", QJsonValue(QJsonValue::Null)},
                   {"mic1", QJsonValue(QJsonValue::Null)},
                   {"mic2", QJsonValue(QJsonValue::Null)},
                   {"mic3", QJsonValue(QJsonValue::Null)},
                   {"mic4", QJsonValue(QJsonValue::Null)}});
    if (type == "CreateInput") {
        const auto* s = scene(data);
        if (!s)
            return fail(600, "Scene not found.");
        const auto name = data["inputName"].toString();
        if (name.trimmed().isEmpty() || source(data))
            return fail(601, "Input name empty or already in use.");
        const auto defaults =
            request("GetInputDefaultSettings", {{"inputKind", data["inputKind"]}});
        if (defaults.code != 100)
            return defaults;
        auto settings = defaults.data["defaultInputSettings"].toObject();
        const auto kind = nativeKind(data["inputKind"].toString());
        const auto supplied = data["inputSettings"].toObject();
        for (auto i = supplied.begin(); i != supplied.end(); ++i)
            settings[i.key()] = i.value();
        auto values = readParams(obs_ ? wireSettings(settings, kind, false) : settings);
        if (!values)
            return fail(402, values.error());
        ldoc::Source input{
            ldoc::Id::generate(), name.toStdString(), kind.toStdString(), *values, {}, {}};
        auto node = ldoc::make_node(ldoc::NodeKind::Source, input.name);
        std::get<ldoc::SourceData>(node.data).source = input.id;
        node.visible = data["sceneItemEnabled"].toBool(true);
        auto result = execute(ldoc::make_batch(
            {ldoc::AddSource{input, p.sources.size()}, ldoc::AddNode{s->id, {}, 0, node}},
            "Create remote input"));
        result.data = {{"inputUuid", id(input.id)}, {"sceneItemId", itemId(node.id)}};
        return result;
    }
    if (type == "RemoveInput" || type == "SetInputName" || type == "GetInputSettings" ||
        type == "SetInputSettings") {
        const auto* s = source(data);
        if (!s)
            return fail(600, "Input not found.");
        if (type == "GetInputSettings")
            return ok(
                {{"inputKind", wireKind(text(s->type), obs_)},
                 {"inputSettings", obs_ ? wireSettings(params(s->settings), text(s->type), true)
                                        : params(s->settings)}});
        if (type == "RemoveInput") {
            std::vector<ldoc::Command> commands;
            for (const auto& sc : p.scenes)
                ldoc::visit(sc.roots, [&](const ldoc::Node& n) {
                    const auto* binding = std::get_if<ldoc::SourceData>(&n.data);
                    if (binding && binding->source == s->id)
                        commands.push_back(ldoc::RemoveNode{sc.id, n.id});
                });
            commands.push_back(ldoc::RemoveSource{s->id});
            return execute(ldoc::make_batch(std::move(commands), "Remove remote input"));
        }
        auto value = *s;
        if (type == "SetInputName") {
            const auto name = data["newInputName"].toString();
            if (name.trimmed().isEmpty() || source({{"inputName", name}}))
                return fail(601, "Input name empty or already in use.");
            value.name = name.toStdString();
        } else {
            auto settings = readParams(
                obs_ ? wireSettings(data["inputSettings"].toObject(), text(s->type), false)
                     : data["inputSettings"].toObject());
            if (!settings)
                return fail(402, settings.error());
            if (!data["overlay"].toBool(true))
                value.settings.clear();
            for (const auto& [key, v] : *settings)
                value.settings[key] = v;
        }
        return execute(ldoc::SetSource{value});
    }
    if (type == "GetInputMute" || type == "SetInputMute" || type == "ToggleInputMute" ||
        type == "GetInputVolume" || type == "SetInputVolume" ||
        type == "GetInputAudioMonitorType" || type == "SetInputAudioMonitorType" ||
        type == "GetInputAudioTracks" || type == "SetInputAudioTracks") {
        const auto* input = source(data);
        if (!input)
            return fail(600, "Input not found.");
        if (!ldoc::has_audio(*input))
            return fail(602, "Input does not produce audio.");
        std::vector<std::pair<ldoc::Id, const ldoc::Node*>> nodes;
        for (const auto& s : p.scenes)
            ldoc::visit(s.roots, [&](const ldoc::Node& node) {
                const auto* binding = std::get_if<ldoc::SourceData>(&node.data);
                if (binding && binding->source == input->id)
                    nodes.emplace_back(s.id, &node);
            });
        if (nodes.empty())
            return fail(604, "Input has no placements with audio routing.");
        const auto first = std::get<ldoc::SourceData>(nodes.front().second->data).audio;
        if (type == "GetInputMute")
            return ok({{"inputMuted", first.mute}});
        if (type == "GetInputVolume")
            return ok({{"inputVolumeDb", first.gain_db},
                       {"inputVolumeMul", std::pow(10., first.gain_db / 20.)}});
        if (type == "GetInputAudioMonitorType")
            return ok({{"monitorType", first.monitor ? "OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT"
                                                     : "OBS_MONITORING_TYPE_NONE"}});
        std::vector<ldoc::Id> tracks;
        for (const auto& bus : p.buses)
            if (bus.kind == ldoc::BusKind::Track && tracks.size() < 6)
                tracks.push_back(bus.id);
        if (type == "GetInputAudioTracks") {
            QJsonObject enabled;
            for (int i = 0; i < 6; ++i) {
                const auto key = i < static_cast<int>(tracks.size())
                                     ? std::optional(tracks[static_cast<std::size_t>(i)])
                                     : std::nullopt;
                enabled[QString::number(i + 1)] =
                    key && (first.bus == key ||
                            std::ranges::any_of(first.sends,
                                                [&](const auto& send) { return send.to == *key; }));
            }
            return ok({{"inputAudioTracks", enabled}});
        }
        if (type == "SetInputAudioTracks") {
            const auto values = data["inputAudioTracks"].toObject();
            for (auto it = values.begin(); it != values.end(); ++it) {
                const int track = it.key().toInt();
                if (track < 1 || track > 6 || it.key() != QString::number(track) ||
                    !it.value().isBool())
                    return fail(402, "Track keys must be 1–6 with boolean values.");
                if (it.value().toBool() && track > static_cast<int>(tracks.size()))
                    return fail(600, "Requested recording track bus does not exist.");
            }
        }
        double gain = first.gain_db;
        if (type == "SetInputVolume") {
            if (data.contains("inputVolumeDb") == data.contains("inputVolumeMul"))
                return fail(400, "Supply inputVolumeDb or inputVolumeMul.");
            const auto mul = data["inputVolumeMul"].toDouble();
            if (data.contains("inputVolumeMul") && (mul < 0 || mul > 20))
                return fail(402, "Multiplier must be 0–20.");
            gain = data.contains("inputVolumeDb") ? data["inputVolumeDb"].toDouble()
                   : mul == 0                     ? -100
                                                  : 20 * std::log10(mul);
            if (gain < -100 || gain > 26)
                return fail(402, "Gain must be -100–26 dB.");
        }
        if (type == "SetInputAudioMonitorType" &&
            data["monitorType"] != "OBS_MONITORING_TYPE_NONE" &&
            data["monitorType"] != "OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT")
            return fail(602, "Libertas supports no monitoring or monitor and output; monitor-only "
                             "is unavailable.");
        std::vector<ldoc::Command> commands;
        for (const auto& [s, n] : nodes) {
            auto audio = std::get<ldoc::SourceData>(n->data).audio;
            if (type == "SetInputAudioTracks") {
                const auto values = data["inputAudioTracks"].toObject();
                for (std::size_t i = 0; i < tracks.size(); ++i) {
                    const auto name = QString::number(i + 1);
                    if (!values.contains(name))
                        continue;
                    const auto key = tracks[i];
                    std::erase_if(audio.sends, [&](const auto& send) { return send.to == key; });
                    if (!values[name].toBool()) {
                        if (audio.bus == key)
                            audio.bus.reset();
                    } else if (audio.bus != key)
                        audio.sends.push_back(ldoc::Send{key, 0, false});
                }
            } else if (type == "SetInputVolume") {
                if (audio.level_locked)
                    return fail(604, "An input placement has its fader locked.");
                audio.gain_db = gain;
            } else if (type == "SetInputAudioMonitorType")
                audio.monitor = data["monitorType"] == "OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT";
            else
                audio.mute = type == "ToggleInputMute" ? !first.mute : data["inputMuted"].toBool();
            commands.push_back(ldoc::SetNodeAudio{s, n->id, audio});
        }
        auto result = execute(ldoc::make_batch(std::move(commands), "Change remote input audio"));
        if (type == "ToggleInputMute")
            result.data["inputMuted"] = !first.mute;
        return result;
    }
    if (type.contains("SceneItem")) {
        const auto* s = scene(data);
        const std::vector<ldoc::Node>* roots = s ? &s->roots : nullptr;
        std::optional<ldoc::Id> parent;
        if (!s) {
            for (const auto& sc : p.scenes)
                ldoc::visit(sc.roots, [&](const ldoc::Node& n) {
                    if (n.is_group() && matches(data, "scene", n.id, n.name)) {
                        s = &sc;
                        roots = n.children();
                        parent = n.id;
                    }
                });
        }
        if (!s || !roots)
            return fail(600, "Scene or group not found.");
        if (type == "GetSceneItemList" || type == "GetGroupSceneItemList") {
            QJsonArray items;
            for (int i = static_cast<int>(roots->size()) - 1; i >= 0; --i)
                items.append(item(*s, (*roots)[static_cast<std::size_t>(i)],
                                  static_cast<int>(roots->size()) - 1 - i));
            return ok({{"sceneItems", items}});
        }
        if (type == "CreateSceneItem") {
            ldoc::Node node;
            if (const auto* src = source(data, "source")) {
                node = ldoc::make_node(ldoc::NodeKind::Source, src->name);
                std::get<ldoc::SourceData>(node.data).source = src->id;
            } else if (const auto* nested = scene(data, "source")) {
                node = ldoc::make_node(ldoc::NodeKind::Precomp, nested->name);
                std::get<ldoc::PrecompData>(node.data).scene = nested->id;
            } else
                return fail(600, "Source not found.");
            node.visible = data["sceneItemEnabled"].toBool(true);
            auto result = execute(ldoc::AddNode{s->id, parent, 0, node});
            result.data = {{"sceneItemId", itemId(node.id)}};
            return result;
        }
        const ldoc::Node* node = nullptr;
        std::size_t index = 0;
        if (type == "GetSceneItemId") {
            std::vector<const ldoc::Node*> found;
            for (const auto& n : *roots) {
                const auto row = item(*s, n, 0);
                if (data.contains("sourceUuid") ? row["sourceUuid"] == data["sourceUuid"]
                                                : row["sourceName"] == data["sourceName"])
                    found.push_back(&n);
            }
            const int offset = data["searchOffset"].toInt();
            if (offset < 0 || offset >= static_cast<int>(found.size()))
                return fail(600, "Scene item not found.");
            return ok({{"sceneItemId", itemId(found[static_cast<std::size_t>(offset)]->id)}});
        }
        const auto numericId = data["sceneItemId"].toDouble(-1);
        for (std::size_t i = 0; i < roots->size(); ++i)
            if (itemId((*roots)[i].id) == numericId) {
                node = &(*roots)[i];
                index = i;
                break;
            }
        if (!node)
            return fail(600, "Scene item not found.");
        if (type == "GetSceneItemSource") {
            const auto row = item(*s, *node, 0);
            return ok({{"sourceName", row["sourceName"]}, {"sourceUuid", row["sourceUuid"]}});
        }
        if (type == "GetSceneItemEnabled")
            return ok({{"sceneItemEnabled", node->visible}});
        if (type == "GetSceneItemLocked")
            return ok({{"sceneItemLocked", node->locked}});
        if (type == "GetSceneItemIndex")
            return ok({{"sceneItemIndex", static_cast<int>(roots->size() - 1 - index)}});
        if (type == "GetSceneItemTransform")
            return ok({{"sceneItemTransform", item(*s, *node, 0)["sceneItemTransform"]}});
        if (type == "GetSceneItemBlendMode")
            return ok({{"sceneItemBlendMode", blendName(node->blend.mode)}});
        if (type == "SetSceneItemEnabled")
            return execute(
                ldoc::SetNodeVisible{s->id, node->id, data["sceneItemEnabled"].toBool()});
        if (type == "SetSceneItemLocked")
            return execute(ldoc::SetNodeLocked{s->id, node->id, data["sceneItemLocked"].toBool()});
        if (type == "RemoveSceneItem")
            return execute(ldoc::RemoveNode{s->id, node->id});
        if (type == "SetSceneItemIndex") {
            const auto pos = data["sceneItemIndex"].toDouble(-1);
            if (pos < 0 || pos >= static_cast<double>(roots->size()) || pos != std::trunc(pos))
                return fail(402, "Index out of range.");
            return execute(ldoc::MoveNode{s->id, node->id, parent,
                                          roots->size() - 1 - static_cast<std::size_t>(pos)});
        }
        if (type == "DuplicateSceneItem") {
            const auto* dest =
                data.contains("destinationSceneName") || data.contains("destinationSceneUuid")
                    ? scene(data, "destinationScene")
                    : s;
            if (!dest)
                return fail(600, "Destination scene not found.");
            const auto duplicate = ldoc::duplicate(*node);
            auto result = execute(ldoc::AddNode{dest->id, {}, 0, duplicate});
            result.data = {{"sceneItemId", itemId(duplicate.id)}};
            return result;
        }
        if (node->locked)
            return fail(604, "Scene item is locked.");
        if (type == "SetSceneItemBlendMode") {
            auto blend = node->blend;
            bool found = false;
            for (int i = 0; i <= static_cast<int>(ldoc::BlendMode::Luminosity); ++i) {
                const auto mode = static_cast<ldoc::BlendMode>(i);
                if (blendName(mode) == data["sceneItemBlendMode"].toString()) {
                    blend.mode = mode;
                    found = true;
                    break;
                }
            }
            if (!found)
                return fail(402, "Unknown blend mode.");
            return execute(ldoc::SetNodeBlend{s->id, node->id, blend});
        }
        if (type == "SetSceneItemTransform") {
            auto transform = node->transform;
            auto layout = node->layout;
            const auto values = data["sceneItemTransform"].toObject();
            const QStringList fields{"positionX",  "positionY",      "rotation",     "scaleX",
                                     "scaleY",     "cropLeft",       "cropTop",      "cropRight",
                                     "cropBottom", "boundsWidth",    "boundsHeight", "boundsType",
                                     "alignment",  "boundsAlignment"};
            for (auto it = values.begin(); it != values.end(); ++it) {
                if (!fields.contains(it.key()))
                    return fail(402, "Unsupported transform field: " + it.key());
                if (it.key() == "boundsType") {
                    if (it.value() != "OBS_BOUNDS_NONE")
                        return fail(602,
                                    "OBS bounds fitting requires native Libertas layout controls.");
                } else if (!it.value().isDouble())
                    return fail(401, "Transform fields must be numbers.");
            }
            if ((values.contains("alignment") && values["alignment"].toInt() != 5) ||
                (values.contains("boundsAlignment") && values["boundsAlignment"].toInt() != 0))
                return fail(602, "Only top-left OBS alignment is supported. Use native node layout "
                                 "for other anchors.");
            const auto assign = [&](const char* key, double& dest) {
                if (values.contains(key))
                    dest = values[key].toDouble();
            };
            assign("positionX", transform.position.x);
            assign("positionY", transform.position.y);
            assign("rotation", transform.rotation_deg);
            assign("scaleX", transform.scale.x);
            assign("scaleY", transform.scale.y);
            assign("cropLeft", layout.crop.left);
            assign("cropTop", layout.crop.top);
            assign("cropRight", layout.crop.right);
            assign("cropBottom", layout.crop.bottom);
            assign("boundsWidth", layout.size.x);
            assign("boundsHeight", layout.size.y);
            if (layout.crop.left < 0 || layout.crop.right < 0 || layout.crop.top < 0 ||
                layout.crop.bottom < 0 || layout.size.x < 0 || layout.size.y < 0)
                return fail(402, "Crop and bounds must be nonnegative.");
            return execute(ldoc::make_batch({ldoc::SetNodeTransform{s->id, node->id, transform},
                                             ldoc::SetNodeLayout{s->id, node->id, layout}},
                                            "Transform remote scene item"));
        }
    }
    if (type == "GetSourceFilterKindList" || type == "GetSourceFilterDefaultSettings") {
        QJsonArray kinds;
        for (const auto& info : lcomp::filter_catalog()) {
            kinds.append(obs_ && !info.obs.empty() ? text(info.obs) : text(info.type));
            if (data["filterKind"].toString() == text(info.type) ||
                data["filterKind"].toString() == text(info.obs)) {
                ldoc::ParamMap defaults;
                for (const auto& f : info.fields)
                    defaults[f.key] = f.value;
                if (type == "GetSourceFilterDefaultSettings")
                    return ok({{"defaultFilterSettings", params(defaults)}});
            }
        }
        return type == "GetSourceFilterKindList" ? ok({{"sourceFilterKinds", kinds}})
                                                 : fail(600, "Filter kind not found.");
    }
    if (type.contains("SourceFilter")) {
        const auto* input = source(data, "source");
        const auto* sc = scene(data, "source");
        const auto owner = input ? input->id : sc ? sc->id : ldoc::Id::nil();
        const auto* stack = p.effect_stack(owner);
        if (!stack)
            return fail(600, "Filter owner not found.");
        QJsonArray filters;
        const auto filterName = [](const ldoc::Effect& e) {
            const auto* info = lcomp::filter_info(e.type);
            return text(e.name.empty() ? info ? info->name : e.type : e.name);
        };
        std::size_t index = stack->size();
        for (std::size_t i = 0; i < stack->size(); ++i) {
            const auto& e = (*stack)[i];
            if (filterName(e) == data["filterName"].toString())
                index = i;
            filters.append(
                QJsonObject{{"filterName", filterName(e)},
                            {"filterKind", obs_ && lcomp::filter_info(e.type) &&
                                                   !lcomp::filter_info(e.type)->obs.empty()
                                               ? text(lcomp::filter_info(e.type)->obs)
                                               : text(e.type)},
                            {"filterIndex", static_cast<int>(i)},
                            {"filterEnabled", e.enabled},
                            {"filterSettings", params(e.params)}});
        }
        if (type == "GetSourceFilterList")
            return ok({{"filters", filters}});
        if (type == "CreateSourceFilter") {
            if (index != stack->size())
                return fail(601, "Filter name already exists.");
            const auto name = data["filterName"].toString();
            if (name.trimmed().isEmpty())
                return fail(402, "Filter name empty.");
            const lcomp::FilterInfo* info = nullptr;
            for (const auto& entry : lcomp::filter_catalog())
                if (data["filterKind"].toString() == text(entry.type) ||
                    data["filterKind"].toString() == text(entry.obs))
                    info = &entry;
            if (!info)
                return fail(600, "Filter kind not found.");
            ldoc::Effect effect;
            effect.id = ldoc::Id::generate();
            effect.name = name.toStdString();
            effect.type = info->type;
            for (const auto& f : info->fields)
                effect.params[f.key] = f.value;
            const auto supplied = readParams(data["filterSettings"].toObject());
            if (!supplied)
                return fail(402, supplied.error());
            for (const auto& [key, v] : *supplied)
                effect.params[key] = v;
            const auto valid = lcomp::validate_filter(effect);
            if (!valid)
                return fail(402, text(valid.error()));
            auto effects = *stack;
            effects.push_back(effect);
            return execute(ldoc::SetEffects{owner, effects});
        }
        if (index == stack->size())
            return fail(600, "Filter not found.");
        if (type == "GetSourceFilter")
            return ok(filters[static_cast<int>(index)].toObject());
        auto effects = *stack;
        if (type == "RemoveSourceFilter")
            effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(index));
        else if (type == "SetSourceFilterEnabled")
            effects[index].enabled = data["filterEnabled"].toBool();
        else if (type == "SetSourceFilterName") {
            const auto name = data["newFilterName"].toString();
            if (name.trimmed().isEmpty() ||
                std::ranges::any_of(effects, [&](const auto& e) { return filterName(e) == name; }))
                return fail(601, "Filter name empty or already in use.");
            effects[index].name = name.toStdString();
        } else if (type == "SetSourceFilterIndex") {
            const auto pos = data["filterIndex"].toDouble(-1);
            if (pos < 0 || pos >= static_cast<double>(effects.size()) || pos != std::trunc(pos))
                return fail(402, "Filter index out of range.");
            auto effect = effects[index];
            effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(index));
            effects.insert(effects.begin() + static_cast<int>(pos), effect);
        } else if (type == "SetSourceFilterSettings") {
            auto settings = readParams(data["filterSettings"].toObject());
            if (!settings)
                return fail(402, settings.error());
            if (!data["overlay"].toBool(true))
                effects[index].params.clear();
            for (const auto& [key, v] : *settings)
                effects[index].params[key] = v;
            const auto valid = lcomp::validate_filter(effects[index]);
            if (!valid)
                return fail(402, text(valid.error()));
        } else
            return fail(204, "Unsupported filter request.");
        return execute(ldoc::SetEffects{owner, effects});
    }
    if (type == "GetVideoSettings") {
        if (!canvas)
            return fail(600, "No canvas.");
        return ok({{"fpsNumerator", canvas->fps.num},
                   {"fpsDenominator", canvas->fps.den},
                   {"baseWidth", canvas->width},
                   {"baseHeight", canvas->height},
                   {"outputWidth", canvas->width},
                   {"outputHeight", canvas->height}});
    }
    if (type == "SetVideoSettings") {
        if (!canvas)
            return fail(600, "No canvas.");
        if (preview_.recording() || preview_.streaming() || preview_.replayActive() ||
            preview_.outputPending())
            return fail(500, "Stop outputs before changing video settings.");
        auto value = *canvas;
        for (auto it = data.begin(); it != data.end(); ++it)
            if (it.value().toDouble() != std::trunc(it.value().toDouble()) ||
                it.value().toDouble() < 1 || it.value().toDouble() > 65536)
                return fail(402, "Video settings must be positive integers up to 65536.");
        if (data.contains("baseWidth"))
            value.width = data["baseWidth"].toInt();
        if (data.contains("baseHeight"))
            value.height = data["baseHeight"].toInt();
        if ((data.contains("outputWidth") && data["outputWidth"].toInt() != value.width) ||
            (data.contains("outputHeight") && data["outputHeight"].toInt() != value.height))
            return fail(602,
                        "Use Libertas encode rescale settings for separate output dimensions.");
        if (data.contains("fpsNumerator"))
            value.fps.num = data["fpsNumerator"].toInt();
        if (data.contains("fpsDenominator"))
            value.fps.den = data["fpsDenominator"].toInt();
        return execute(ldoc::SetCanvas{value});
    }
    if (type == "GetRecordDirectory")
        return ok({{"recordDirectory", document_.recordingDirectory()}});
    if (type == "SetRecordDirectory") {
        if (preview_.recording() || preview_.outputPending())
            return fail(500, "Stop recording first.");
        const auto path = data["recordDirectory"].toString();
        if (path.isEmpty() || !QFileInfo(path).isAbsolute() || !QFileInfo(path).isDir())
            return fail(402, "Recording directory must be an existing absolute directory.");
        for (const auto& o : p.outputs)
            if (o.destination.kind == ldoc::DestinationKind::File) {
                auto value = o;
                value.destination.url = path.toStdString();
                return execute(ldoc::SetOutput{value});
            }
        return fail(600, "No recording destination.");
    }
    if (type == "GetStreamServiceSettings" || type == "SetStreamServiceSettings") {
        const ldoc::Output* output = nullptr;
        for (const auto& o : p.outputs)
            if (o.destination.kind == ldoc::DestinationKind::Rtmp ||
                o.destination.kind == ldoc::DestinationKind::Rtmps) {
                output = &o;
                if (o.enabled)
                    break;
            }
        if (!output)
            return fail(600, "No RTMP streaming destination.");
        if (type == "GetStreamServiceSettings")
            return ok({{"streamServiceType", "rtmp_custom"},
                       {"streamServiceSettings",
                        QJsonObject{{"server", text(output->destination.url)},
                                    {"service", text(output->destination.service)}}}});
        if (preview_.streaming() || preview_.outputPending())
            return fail(500, "Stop streaming first.");
        if (data["streamServiceType"] != "rtmp_custom")
            return fail(602, "Use rtmp_custom with a server URL.");
        const auto settings = data["streamServiceSettings"].toObject();
        // Credentials remain in the existing secret-store path, never in the document.
        if (settings.contains("key"))
            return fail(602, "Configure the stream key in Libertas Encode; remote credential "
                             "writes are unavailable.");
        const QUrl url(settings["server"].toString(), QUrl::StrictMode);
        if (!url.isValid() || (url.scheme() != "rtmp" && url.scheme() != "rtmps") ||
            url.host().isEmpty() || url.path().isEmpty() || !url.userInfo().isEmpty() ||
            url.hasFragment())
            return fail(402, "Invalid RTMP server URL.");
        auto value = *output;
        value.destination.url = url.toString().toStdString();
        value.destination.kind =
            url.scheme() == "rtmps" ? ldoc::DestinationKind::Rtmps : ldoc::DestinationKind::Rtmp;
        return execute(ldoc::SetOutput{value});
    }
    if (type == "GetStats") {
        const auto render = preview_.renderStats({}), output = preview_.outputStats();
        const QStorageInfo storage(document_.recordingDirectory());
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const auto cpu =
            static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
            static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.;
        const auto now = std::chrono::steady_clock::now();
        static auto previousAt = now;
        static double previousCpu = cpu;
        const double elapsed = std::chrono::duration<double>(now - previousAt).count();
        const double percent = elapsed > 0 ? std::max(0., (cpu - previousCpu) / elapsed * 100.) : 0;
        previousAt = now;
        previousCpu = cpu;
        double memory = 0;
        QFile status("/proc/self/status");
        if (status.open(QIODevice::ReadOnly)) {
            for (const auto& line : status.readAll().split('\n'))
                if (line.startsWith("VmRSS:"))
                    memory = line.mid(6).simplified().split(' ').value(0).toDouble() / 1024.;
        }
        return ok(
            {{"cpuUsage", percent},
             {"memoryUsage", memory},
             {"availableDiskSpace", static_cast<double>(storage.bytesAvailable()) / 1048576.0},
             {"activeFps", render["fps"].toDouble()},
             {"averageFrameRenderTime", render["renderMs"].toDouble()},
             {"renderSkippedFrames", render["missed"].toDouble()},
             {"renderTotalFrames", render["frames"].toDouble()},
             {"outputSkippedFrames", output["missed"].toDouble()},
             {"outputTotalFrames", output["frames"].toDouble()},
             {"webSocketSessionIncomingMessages", 0},
             {"webSocketSessionOutgoingMessages", 0}});
    }
    if (type == "SetOutputSettings") {
        if (preview_.recording() || preview_.streaming() || preview_.replayActive() ||
            preview_.outputPending())
            return fail(500, "Stop outputs before changing output settings.");
        const auto it = std::ranges::find_if(p.outputs, [&](const auto& o) {
            return text(o.name) == data["outputName"].toString();
        });
        if (it == p.outputs.end())
            return fail(600, "Output not found.");
        auto value = *it;
        const auto settings = data["outputSettings"].toObject();
        for (auto field = settings.begin(); field != settings.end(); ++field) {
            if (field.key() == "enabled" && field.value().isBool())
                value.enabled = field.value().toBool();
            else if (field.key() == "url" && field.value().isString())
                value.destination.url = field.value().toString().toStdString();
            else
                return fail(402,
                            "Output settings support url (string) and enabled (boolean) only.");
        }
        return execute(ldoc::SetOutput{value});
    }
    if (type == "GetOutputList" || type == "GetOutputSettings") {
        QJsonArray outputs;
        for (const auto& o : p.outputs) {
            const auto kind = o.destination.kind;
            const bool active = kind == ldoc::DestinationKind::Replay ? preview_.replayActive()
                                : kind == ldoc::DestinationKind::File ? preview_.recording()
                                                                      : preview_.streaming();
            outputs.append(
                QJsonObject{{"outputName", text(o.name)},
                            {"outputKind", QString::fromUtf8(ldoc::to_string(kind))},
                            {"outputWidth", canvas ? canvas->width : 0},
                            {"outputHeight", canvas ? canvas->height : 0},
                            {"outputActive", active},
                            {"outputFlags",
                             QJsonObject{{"audio", true},
                                         {"video", true},
                                         {"encoded", true},
                                         {"multiTrack", false},
                                         {"service", kind != ldoc::DestinationKind::File &&
                                                         kind != ldoc::DestinationKind::Replay}}}});
            if (type == "GetOutputSettings" && data["outputName"].toString() == text(o.name))
                return ok({{"outputSettings", QJsonObject{{"url", text(o.destination.url)},
                                                          {"enabled", o.enabled}}}});
        }
        return type == "GetOutputSettings" ? fail(600, "Output not found.")
                                           : ok({{"outputs", outputs}});
    }
    if (type == "GetMonitorList") {
        QJsonArray monitors;
        for (auto* screen : QGuiApplication::screens())
            monitors.append(QJsonObject{{"monitorName", screen->name()},
                                        {"monitorIndex", monitors.size()},
                                        {"monitorWidth", screen->size().width()},
                                        {"monitorHeight", screen->size().height()},
                                        {"monitorPositionX", screen->geometry().x()},
                                        {"monitorPositionY", screen->geometry().y()}});
        return ok({{"monitors", monitors}});
    }
    if (type == "GetSceneCollectionList") {
        QJsonArray names;
        for (const auto& v : document_.collections())
            names.append(v.toMap()["name"].toString());
        return ok(
            {{"sceneCollections", names}, {"currentSceneCollectionName", document_.projectName()}});
    }
    if (type == "SetCurrentSceneCollection" || type == "CreateSceneCollection") {
        if (preview_.streaming() || preview_.recording() || preview_.replayActive() ||
            preview_.outputPending())
            return fail(500, "Stop outputs before changing collections.");
        const auto name = data["sceneCollectionName"].toString();
        if (name.trimmed().isEmpty())
            return fail(402, "Collection name empty.");
        if (type == "CreateSceneCollection")
            return document_.createCollection(name) ? ok()
                                                    : fail(604, document_.collectionReport());
        for (const auto& v : document_.collections()) {
            const auto row = v.toMap();
            if (row["name"].toString() == name)
                return document_.openCollection(row["path"].toString())
                           ? ok()
                           : fail(604, document_.collectionReport());
        }
        return fail(600, "Collection not found.");
    }
    if (type == "GetProfileList")
        return ok({{"profiles", QJsonArray{"Libertas Studio"}},
                   {"currentProfileName", "Libertas Studio"}});
    if (type == "TriggerHotkeyByKeySequence") {
        if (!shortcuts_)
            return fail(602, "Keyboard controls unavailable.");
        const auto keyName = data["keyId"].toString();
        if (!keyName.startsWith("OBS_KEY_"))
            return fail(402, "Use an OBS_KEY_* key ID.");
        const auto suffix = keyName.mid(8);
        int key = 0;
        if (suffix.size() == 1 && suffix[0].isLetterOrNumber())
            key = suffix[0].toUpper().unicode();
        else if (suffix.startsWith('F')) {
            bool valid = false;
            const int n = suffix.mid(1).toInt(&valid);
            if (valid && n >= 1 && n <= 35)
                key = Qt::Key_F1 + n - 1;
        } else {
            const QMap<QString, int> keys{
                {"RETURN", Qt::Key_Return}, {"ENTER", Qt::Key_Enter},
                {"SPACE", Qt::Key_Space},   {"TAB", Qt::Key_Tab},
                {"ESCAPE", Qt::Key_Escape}, {"BACKSPACE", Qt::Key_Backspace},
                {"DELETE", Qt::Key_Delete}, {"INSERT", Qt::Key_Insert},
                {"HOME", Qt::Key_Home},     {"END", Qt::Key_End},
                {"LEFT", Qt::Key_Left},     {"RIGHT", Qt::Key_Right},
                {"UP", Qt::Key_Up},         {"DOWN", Qt::Key_Down},
                {"PAGEUP", Qt::Key_PageUp}, {"PAGEDOWN", Qt::Key_PageDown}};
            key = keys.value(suffix);
        }
        if (!key)
            return fail(602, "This key ID has no Libertas shortcut mapping.");
        int modifiers = 0;
        const auto values = data["keyModifiers"].toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!it.value().isBool())
                return fail(401, "Key modifiers must be boolean.");
            const QMap<QString, int> flags{{"shift", Qt::ShiftModifier},
                                           {"control", Qt::ControlModifier},
                                           {"alt", Qt::AltModifier},
                                           {"command", Qt::MetaModifier}};
            if (!flags.contains(it.key()))
                return fail(402, "Unknown key modifier.");
            if (it.value().toBool())
                modifiers |= flags[it.key()];
        }
        const auto trigger = shortcuts_->formatKey(key, modifiers);
        const auto* model = shortcuts_->bindingsModel();
        QString ref;
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto index = model->index(row, 0);
            if (!model->data(index, BindingModel::AvailableRole).toBool() ||
                model->data(index, BindingModel::TriggerRole).toString() != trigger)
                continue;
            if (!ref.isEmpty())
                return fail(604, "Shortcut is ambiguous; trigger an action by name instead.");
            ref = model->data(index, BindingModel::RefRole).toString();
        }
        if (ref.isEmpty())
            return fail(600, "No available action bound to this key sequence.");
        return request("TriggerHotkeyByName", {{"hotkeyName", ref}});
    }
    if (type == "GetHotkeyList" || type == "TriggerHotkeyByName") {
        if (!shortcuts_)
            return fail(602, "Keyboard controls unavailable.");
        const auto* model = shortcuts_->bindingsModel();
        QJsonArray hotkeys;
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto key = model->index(row, 0);
            if (!model->data(key, BindingModel::AvailableRole).toBool())
                continue;
            const auto ref = model->data(key, BindingModel::RefRole).toString();
            hotkeys.append(ref);
            if (type == "TriggerHotkeyByName" && data["hotkeyName"].toString() == ref) {
                QString error;
                const auto connection = QObject::connect(shortcuts_, &Shortcuts::notice, shortcuts_,
                                                         [&](const QString& why) { error = why; });
                shortcuts_->perform(ref);
                QObject::disconnect(connection);
                return error.isEmpty() ? ok() : fail(604, error);
            }
        }
        return type == "GetHotkeyList" ? ok({{"hotkeys", hotkeys}})
                                       : fail(600, "Hotkey not found.");
    }
    if (type == "GetSourceActive") {
        const auto* src = source(data, "source");
        const auto* sc = scene(data, "source");
        if (!src && !sc)
            return fail(600, "Source not found.");
        bool active = false, showing = false;
        for (const auto& d : p.decks)
            for (const auto& s : p.scenes) {
                if (s.canvas != d.canvas)
                    continue;
                bool used = sc && sc->id == s.id;
                const auto walk = [&](const auto& self,
                                      const std::vector<ldoc::Node>& nodes) -> void {
                    for (const auto& n : nodes) {
                        if (!n.visible)
                            continue;
                        if (const auto* binding = std::get_if<ldoc::SourceData>(&n.data);
                            binding && src && binding->source == src->id)
                            used = true;
                        if (const auto* children = n.children())
                            self(self, *children);
                    }
                };
                walk(walk, s.roots);
                if (used && d.program_scene == s.id)
                    active = true;
                if (used && d.preview_scene == s.id && d.studio_mode)
                    showing = true;
            }
        return ok({{"videoActive", active}, {"videoShowing", active || showing}});
    }
    // Output controls and asynchronous completion are owned by the WebSocket transport.
    return fail(204, "Request requires transport dispatch or is unavailable.");
}
} // namespace libertas::ui

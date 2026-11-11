// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "document.hpp"
#include "ldoc/command.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <expected>

namespace munkicast::ui {
namespace {
QString libraryPath;
const QString horizontal = QStringLiteral("Default 16:9");
const QString vertical = QStringLiteral("Default 9:16");

std::expected<QJsonObject, QString> readLibrary() {
    QFile file(libraryPath);
    if (!file.exists()) return QJsonObject{};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return std::unexpected(QStringLiteral("Cannot read encoder preset library."));
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject() ||
        json.object().value("version").toInt() != 1 ||
        !json.object().value("presets").isObject())
        return std::unexpected(QStringLiteral("Encoder preset library is invalid; it was not changed."));
    return json.object().value("presets").toObject();
}

bool writeLibrary(const QJsonObject& presets) {
    if (libraryPath.isEmpty() || !QDir().mkpath(QFileInfo(libraryPath).absolutePath())) return false;
    QSaveFile file(libraryPath);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"presets", presets}}).toJson();
    if (bytes.size() > 1024 * 1024) return false;
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QVariantMap builtIn(const QString& name) {
    return {{"codec", "h264"}, {"implementation", "x264"}, {"bitrate", 6000},
        {"rateControl", "cbr"}, {"preset", "medium"}, {"keyframeMs", 2000},
        {"width", name == vertical ? 1080 : 1920}, {"height", name == vertical ? 1920 : 1080},
        {"extra", QVariantMap{{"x264.custom_buffer", false}, {"x264.buffer_kbps", 6000},
            {"x264.profile", "main"}, {"x264.tune", "film"}, {"audio.codec", "aac"},
            {"audio.bitrate_kbps", 160}, {"audio.track", 1}, {"audio.vod_track", 0}}}};
}
} // namespace

void Document::configurePresets(std::filesystem::path path) {
    libraryPath = QString::fromStdString(path.string());
}

QStringList Document::encoderPresetNames() const {
    QStringList names{horizontal, vertical};
    const auto library = readLibrary();
    if (library) {
        for (const auto& name : library->keys())
            if (name != horizontal && name != vertical) names.append(name);
    }
    return names;
}

bool Document::saveEncoderPreset(const QString& encodeId, const QString& name, bool overwrite) {
    const auto clean = name.trimmed();
    if (clean.isEmpty() || clean.size() > 128 || clean.contains(QChar::Null) ||
        clean == horizontal || clean == vertical) {
        emit notice(QStringLiteral("Choose a unique preset name; built-in defaults cannot be replaced."));
        return false;
    }
    auto settings = encodeSettings(encodeId);
    if (settings.isEmpty()) return false;
    settings.remove("id");
    settings.remove("name");
    auto library = readLibrary();
    if (!library) { emit notice(library.error()); return false; }
    if (library->contains(clean) && !overwrite) {
        emit notice(QStringLiteral("Preset already exists. Use Replace selected to overwrite it."));
        return false;
    }
    library->insert(clean, QJsonObject::fromVariantMap(settings));
    if (!writeLibrary(*library)) {
        emit notice(QStringLiteral("Could not save encoder presets; existing library is intact."));
        return false;
    }
    emit encoderPresetsChanged();
    return true;
}

bool Document::removeEncoderPreset(const QString& name) {
    if (name == horizontal || name == vertical) return false;
    auto library = readLibrary();
    if (!library) { emit notice(library.error()); return false; }
    if (!library->contains(name)) return false;
    library->remove(name);
    if (!writeLibrary(*library)) {
        emit notice(QStringLiteral("Could not remove encoder preset."));
        return false;
    }
    emit encoderPresetsChanged();
    return true;
}

bool Document::applyEncoderPreset(const QString& encodeId, const QString& name) {
    const auto parsed = ldoc::Id::parse(encodeId.toStdString());
    const auto* existing = parsed ? session_->project().encode(*parsed) : nullptr;
    if (!existing) return false;
    QVariantMap settings;
    if (name == horizontal || name == vertical) settings = builtIn(name);
    else {
        const auto library = readLibrary();
        if (!library) { emit notice(library.error()); return false; }
        settings = library->value(name).toObject().toVariantMap();
    }
    const auto invalid = [this] {
        emit notice(QStringLiteral("Encoder preset has invalid or unsupported settings."));
        return false;
    };
    const auto integer = [&](const char* key, int low, int high) {
        bool ok = false;
        const double value = settings.value(key).toDouble(&ok);
        return ok && std::isfinite(value) && value == std::floor(value) && value >= low && value <= high;
    };
    bool encoderKnown = false;
    for (const auto& c : codecs()) {
        const auto item = c.toMap();
        encoderKnown |= item.value("codec") == settings.value("codec") &&
                        item.value("implementation") == settings.value("implementation");
    }
    const auto rate = settings.value("rateControl").toString();
    if (!encoderKnown || !integer("bitrate", 1, 1000000) || !integer("keyframeMs", 0, 600000) ||
        !integer("width", 0, 16384) || !integer("height", 0, 16384) ||
        !QStringList{"cbr", "vbr", "crf", "cqp"}.contains(rate) ||
        settings.value("preset").metaType().id() != QMetaType::QString ||
        settings.value("preset").toString().size() > 128 ||
        settings.value("preset").toString().contains(QChar::Null) ||
        settings.value("extra").metaType().id() != QMetaType::QVariantMap) return invalid();
    const int width = settings.value("width").toInt();
    const int height = settings.value("height").toInt();
    if ((width == 0) != (height == 0) || width % 2 || height % 2) return invalid();
    ldoc::Encode next = *existing;
    next.encoder.codec = settings.value("codec").toString().toStdString();
    next.encoder.implementation = settings.value("implementation").toString().toStdString();
    next.encoder.bitrate_kbps = settings.value("bitrate").toInt();
    next.encoder.keyframe_interval_ms = settings.value("keyframeMs").toInt();
    next.encoder.rate_control = rate.toStdString();
    next.encoder.preset = settings.value("preset").toString().toStdString();
    next.encoder.extra.clear();
    const auto extra = settings.value("extra").toMap();
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        const auto value = encoderExtraValue(it.key(), it.value());
        if (!value) return invalid();
        next.encoder.extra[it.key().toStdString()] = *value;
    }
    next.rescale.reset();
    if (width > 0) {
        const auto* canvas = session_->project().canvas(next.canvas);
        if (!canvas) return invalid();
        const auto scale = std::min({1.0, double(width) / canvas->width, double(height) / canvas->height});
        next.rescale = ldoc::Vec2{std::max(2.0, std::floor(canvas->width * scale / 2) * 2),
                                std::max(2.0, std::floor(canvas->height * scale / 2) * 2)};
    }
    run(ldoc::SetEncode{std::move(next)});
    return true;
}
} // namespace munkicast::ui

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cmath>
#include <lcomp/countdown.hpp>
#include <set>

namespace munkicast::ui {
namespace {

/// The plugin's own id shape: eight hex characters, short enough to read off a dock header and
/// to type into a websocket request.
QString new_timer_id() {
    const auto hash = QCryptographicHash::hash(QUuid::createUuid().toByteArray(),
                                               QCryptographicHash::Md5);
    return QString::fromLatin1(hash.toHex().left(8));
}

/// Timers are shown in this order, so the dock's up/down arrows have something to move.
///
/// Carried as a per-source number rather than by reordering `project.sources`: the source list
/// is the sources panel's order and is not the countdown dock's to rearrange, and there is no
/// command for moving a source anyway.
std::int64_t countdown_order(const ldoc::Source& source) {
    return lcomp::timer_number(source.settings, "countdown.order");
}

/// sRGB hex to the document's linear colour. `#rrggbb` or `#rrggbbaa`; nothing else is accepted,
/// because a typo quietly turning a caption black is the kind of thing nobody notices until the
/// show (the same rule, for the same reason, as the properties panel's colour field).
std::optional<ldoc::Rgba> countdown_hex(const QString& text) {
    QString hex = text.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex = hex.mid(1);
    if (hex.size() != 6 && hex.size() != 8)
        return std::nullopt;
    bool ok = false;
    const uint packed = hex.toUInt(&ok, 16);
    if (!ok)
        return std::nullopt;
    const auto byte = [](uint value) { return static_cast<std::uint8_t>(value & 0xFFu); };
    if (hex.size() == 6)
        return ldoc::Rgba::from_srgb8(byte(packed >> 16), byte(packed >> 8), byte(packed));
    return ldoc::Rgba::from_srgb8(byte(packed >> 24), byte(packed >> 16), byte(packed >> 8),
                                  byte(packed));
}

QString countdown_hex_of(const ldoc::Rgba& rgba) {
    const auto c = rgba.to_srgb8();
    return QStringLiteral("#%1%2%3")
        .arg(c[0], 2, 16, QLatin1Char('0'))
        .arg(c[1], 2, 16, QLatin1Char('0'))
        .arg(c[2], 2, 16, QLatin1Char('0'));
}

/// The four period fields, clamped to the plugin's spin-box ranges.
std::optional<std::int64_t> countdown_period_limit(const std::string& key) {
    if (key == "periodDays")
        return lcomp::kCountdownMaxDays;
    if (key == "periodHours")
        return 23;
    if (key == "periodMinutes" || key == "periodSeconds")
        return 59;
    return std::nullopt;
}

constexpr bool countdown_is_flag(std::string_view key) {
    return key == "showDays" || key == "showHours" || key == "showMinutes" ||
           key == "showSeconds" || key == "leadingZero" || key == "showEndMessage" ||
           key == "showEndScene" || key == "countUp" || key == "steady" ||
           key == "useFormattedOutput" || key == "useTextColour" ||
           key == "startOnStreamStart" || key == "resetOnStreamStart";
}

} // namespace

QVariantList Document::countdownTextSources() const {
    QVariantList out;
    for (const auto& source : session_->project().sources)
        if (source.type == "source.text")
            out.push_back(QVariantMap{{"id", QString::fromStdString(source.id.to_string())},
                                      {"name", QString::fromStdString(source.name)},
                                      {"bound", lcomp::timer_flag(source.settings,
                                                                  "countdown.enabled")}});
    return out;
}

QVariantList Document::countdownScenes() const {
    QVariantList out;
    for (const auto& scene : session_->project().scenes)
        out.push_back(QVariantMap{{"id", QString::fromStdString(scene.id.to_string())},
                                  {"name", QString::fromStdString(scene.name)}});
    return out;
}

QVariantList Document::countdowns() const {
    std::vector<const ldoc::Source*> bound;
    for (const auto& source : session_->project().sources)
        if (source.type == "source.text" &&
            lcomp::timer_flag(source.settings, "countdown.enabled"))
            bound.push_back(&source);
    std::stable_sort(bound.begin(), bound.end(),
                     [](const ldoc::Source* a, const ldoc::Source* b) {
                         return countdown_order(*a) < countdown_order(*b);
                     });
    QVariantList out;
    for (const ldoc::Source* source : bound) {
        QVariantMap row{{"id", QString::fromStdString(source->id.to_string())},
                        {"name", QString::fromStdString(source->name)}};
        for (const auto& [key, value] : source->settings) {
            if (!key.starts_with("countdown."))
                continue;
            const auto field = QString::fromStdString(key.substr(10));
            std::visit(
                [&](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        row[field] = QString::fromStdString(item);
                    else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t>)
                        row[field] = QVariant::fromValue(item);
                    else if constexpr (std::is_same_v<T, ldoc::Rgba>)
                        row[field] = countdown_hex_of(item);
                },
                value);
        }
        // Derived rather than stored, so a project written before these existed still answers
        // what the dock asks it. The panel reads only these.
        row["periodMs"] = QVariant::fromValue(lcomp::countdown_period_ms(source->settings));
        row["type"] = QString::fromStdString(lcomp::countdown_type(source->settings));
        row["countUp"] = lcomp::countdown_counts_up(source->settings);
        out.push_back(row);
    }
    return out;
}

QString Document::addCountdown(const QString& sourceId) {
    // Binds an existing text source and never makes one. The plugin works this way, and the
    // reason is the same here: creating the source is a separate act with its own defaults, and
    // doing it from this dock put an empty text element into the scene before the timer that was
    // about to fill it had been configured.
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source || source->type != "source.text") {
        emit notice(QStringLiteral("Pick a text source to bind the timer to."));
        return {};
    }
    if (lcomp::timer_flag(source->settings, "countdown.enabled")) {
        emit notice(QStringLiteral("That text source already has a timer."));
        return {};
    }
    std::int64_t last = 0;
    for (const auto& other : session_->project().sources)
        if (lcomp::timer_flag(other.settings, "countdown.enabled"))
            last = std::max(last, countdown_order(other) + 1);
    auto copy = *source;
    copy.settings["countdown.enabled"] = true;
    copy.settings["countdown.running"] = false;
    copy.settings["countdown.order"] = last;
    copy.settings.try_emplace("countdown.timerId", new_timer_id().toStdString());
    // Fifteen minutes, the plugin's own default, written as the four fields the dock edits.
    copy.settings.try_emplace("countdown.periodDays", std::int64_t{0});
    copy.settings.try_emplace("countdown.periodHours", std::int64_t{0});
    copy.settings.try_emplace("countdown.periodMinutes", std::int64_t{15});
    copy.settings.try_emplace("countdown.periodSeconds", std::int64_t{0});
    copy.settings.try_emplace("countdown.type", std::string("period"));
    copy.settings.try_emplace("countdown.format", std::string("%time%"));
    run(ldoc::SetSource{std::move(copy)});
    return sourceId;
}

void Document::configureCountdown(const QString& sourceId, const QVariantMap& settings) {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source || source->type != "source.text")
        return;
    auto copy = *source;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const auto key = it.key().toStdString();
        const auto full = "countdown." + key;
        if (const auto limit = countdown_period_limit(key)) {
            bool ok = false;
            const double value = it.value().toDouble(&ok);
            if (!ok || !std::isfinite(value) || value < 0 || value > double(*limit)) {
                emit notice(QStringLiteral("Countdown time is outside its supported range."));
                return;
            }
            copy.settings[full] = static_cast<std::int64_t>(value);
            // The two spellings must never disagree, so writing a field rewrites the duration
            // the older format and the renderer's fallback both still read.
            copy.settings["countdown.durationMs"] = lcomp::countdown_period_ms(copy.settings);
        } else if (key == "durationMs" || key == "targetAtMs") {
            bool ok = false;
            const double value = it.value().toDouble(&ok);
            const double max = key == "durationMs" ? double(lcomp::kCountdownLimit)
                                                   : double(lcomp::kCountdownEpochLimit);
            if (!ok || !std::isfinite(value) || value < 0 || value > max) {
                emit notice(QStringLiteral("Countdown time is outside its supported range."));
                return;
            }
            copy.settings[full] = static_cast<std::int64_t>(value);
            if (key == "durationMs") {
                const auto total = static_cast<std::int64_t>(value) / 1000;
                copy.settings["countdown.periodDays"] = total / 86400;
                copy.settings["countdown.periodHours"] = total % 86400 / 3600;
                copy.settings["countdown.periodMinutes"] = total % 3600 / 60;
                copy.settings["countdown.periodSeconds"] = total % 60;
            }
        } else if (key == "timerId") {
            const auto value = it.value().toString().trimmed();
            if (value.isEmpty() || value.size() > 64) {
                emit notice(QStringLiteral("A timer needs an ID of 1 to 64 characters."));
                return;
            }
            for (const auto& other : session_->project().sources)
                if (other.id != copy.id &&
                    lcomp::timer_flag(other.settings, "countdown.enabled") &&
                    QString::fromStdString(lcomp::timer_text(other.settings,
                                                             "countdown.timerId")) == value) {
                    emit notice(QStringLiteral("Another timer already uses the ID %1.").arg(value));
                    return;
                }
            copy.settings[full] = value.toStdString();
        } else if (key == "mainTextColour") {
            const auto colour = countdown_hex(it.value().toString());
            if (!colour) {
                emit notice(QStringLiteral("Enter a colour as #rrggbb."));
                return;
            }
            copy.settings[full] = *colour;
        } else if (key == "colourRules") {
            const auto value = it.value().toString();
            if (value.size() > 4096) {
                emit notice(QStringLiteral("That is more colour rules than a timer supports."));
                return;
            }
            copy.settings[full] = value.toStdString();
        } else if (key == "type") {
            const auto value = it.value().toString();
            if (value != "period" && value != "datetime") {
                emit notice(QStringLiteral("Invalid countdown setting."));
                return;
            }
            copy.settings[full] = value.toStdString();
            // Kept in step so an older build, which reads only `mode`, opens the same timer.
            copy.settings["countdown.mode"] =
                value == "datetime" ? std::string("target")
                : lcomp::countdown_counts_up(copy.settings) ? std::string("elapsed")
                                                            : std::string("remaining");
        } else if (key == "mode" || key == "format" || key == "endMessage" || key == "endScene") {
            const auto value = it.value().toString();
            if (value.size() > 1024 || (key == "mode" && value != "remaining" &&
                                        value != "elapsed" && value != "target")) {
                emit notice(QStringLiteral("Invalid countdown setting."));
                return;
            }
            if (key == "endScene" && !value.isEmpty()) {
                const auto scene = ldoc::Id::parse(value.toStdString());
                if (!scene || !session_->project().scene(*scene)) {
                    emit notice(QStringLiteral("Countdown end scene no longer exists."));
                    return;
                }
            }
            copy.settings[full] = value.toStdString();
        } else if (countdown_is_flag(key)) {
            copy.settings[full] = it.value().toBool();
            if (key == "countUp")
                copy.settings["countdown.mode"] =
                    lcomp::countdown_type(copy.settings) == "datetime" ? std::string("target")
                    : it.value().toBool() ? std::string("elapsed")
                                          : std::string("remaining");
        } else {
            emit notice(QStringLiteral("Unknown countdown setting: %1").arg(it.key()));
            return;
        }
    }
    run(ldoc::SetSource{std::move(copy)});
}

QVariantMap Document::countdownStatus(const QString& sourceId) const {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source)
        return {};
    const auto value = lcomp::countdown(source->settings, QDateTime::currentMSecsSinceEpoch());
    QVariantMap out{{"text", QString::fromStdString(value.text)},
                    {"running", lcomp::timer_flag(source->settings, "countdown.running")},
                    {"ended", value.ended},
                    {"shownMs", QVariant::fromValue(value.shown)},
                    // The dock's big readout is always the full DD:HH:MM:SS, whatever the text
                    // source is formatted to show.
                    {"display", QStringLiteral("%1:%2:%3:%4")
                                    .arg(value.shown / 86400000, 2, 10, QLatin1Char('0'))
                                    .arg(value.shown / 3600000 % 24, 2, 10, QLatin1Char('0'))
                                    .arg(value.shown / 60000 % 60, 2, 10, QLatin1Char('0'))
                                    .arg(value.shown / 1000 % 60, 2, 10, QLatin1Char('0'))}};
    if (value.has_colour)
        out["colour"] = countdown_hex_of(value.colour);
    return out;
}

void Document::controlCountdown(const QString& sourceId, const QString& action) {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source || !lcomp::timer_flag(source->settings, "countdown.enabled"))
        return;
    if (action != "start" && action != "pause" && action != "reset" && action != "finish")
        return;
    auto copy = *source;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto current = lcomp::countdown(copy.settings, now);
    if (action == "start" && lcomp::timer_flag(copy.settings, "countdown.running"))
        return;
    // Starting a finished countdown restarts it rather than sitting at zero, which is what the
    // plugin's play button does with a timer that has run out.
    const bool restart = action == "start" && current.ended;
    copy.settings["countdown.elapsedMs"] =
        action == "reset" || restart ? std::int64_t{0} : current.elapsed;
    copy.settings["countdown.targetRemainingMs"] =
        action == "reset"
            ? std::max<std::int64_t>(
                  0, lcomp::timer_number(copy.settings, "countdown.targetAtMs", now) - now)
            : current.remaining;
    copy.settings["countdown.startedAtMs"] = static_cast<std::int64_t>(now);
    copy.settings["countdown.running"] = action == "start";
    const auto target = lcomp::timer_text(copy.settings, "countdown.endScene");
    const bool switches = lcomp::timer_flag(copy.settings, "countdown.showEndScene", !target.empty());
    run(ldoc::SetSource{std::move(copy)});
    if (action == "finish" && switches && !target.empty())
        activateScene(QString::fromStdString(target));
}

void Document::moveCountdown(const QString& sourceId, bool up) {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    if (!id)
        return;
    std::vector<const ldoc::Source*> bound;
    for (const auto& source : session_->project().sources)
        if (source.type == "source.text" &&
            lcomp::timer_flag(source.settings, "countdown.enabled"))
            bound.push_back(&source);
    std::stable_sort(bound.begin(), bound.end(),
                     [](const ldoc::Source* a, const ldoc::Source* b) {
                         return countdown_order(*a) < countdown_order(*b);
                     });
    const auto at = std::find_if(bound.begin(), bound.end(),
                                 [&](const ldoc::Source* s) { return s->id == *id; });
    if (at == bound.end())
        return;
    const auto other = up ? at - 1 : at + 1;
    if ((up && at == bound.begin()) || (!up && other == bound.end()))
        return;
    // Both ends are rewritten, and as one batch: a swap that could half-apply would leave two
    // timers claiming the same place in the list.
    auto first = **at;
    auto second = **other;
    std::swap(first.settings["countdown.order"], second.settings["countdown.order"]);
    run(ldoc::make_batch({ldoc::SetSource{std::move(first)}, ldoc::SetSource{std::move(second)}},
                         "Reorder countdown timers"));
}

void Document::controlAllCountdowns(const QString& action) {
    QStringList ids;
    for (const auto& source : session_->project().sources)
        if (source.type == "source.text" &&
            lcomp::timer_flag(source.settings, "countdown.enabled"))
            ids << QString::fromStdString(source.id.to_string());
    for (const auto& id : ids)
        controlCountdown(id, action);
}

void Document::removeCountdown(const QString& sourceId) {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source)
        return;
    auto copy = *source;
    copy.settings["countdown.enabled"] = false;
    copy.settings["countdown.running"] = false;
    run(ldoc::SetSource{std::move(copy)});
}

bool Document::importCountdowns(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        emit notice(QStringLiteral("Cannot read countdown configuration."));
        return false;
    }
    QJsonParseError error;
    const auto data = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !data.isObject() ||
        !data.object()["timer_widgets"].isArray()) {
        emit notice(QStringLiteral("Invalid OBS countdown configuration."));
        return false;
    }
    if (data.object()["timer_widgets"].toArray().size() > 128) {
        emit notice(QStringLiteral("Countdown configuration has too many timers."));
        return false;
    }
    std::vector<ldoc::Command> commands;
    QStringList warnings;
    std::set<ldoc::Id> bound;
    std::set<QString> used_ids;
    std::int64_t order = 0;
    for (const auto& other : session_->project().sources)
        if (lcomp::timer_flag(other.settings, "countdown.enabled")) {
            order = std::max(order, countdown_order(other) + 1);
            used_ids.insert(
                QString::fromStdString(lcomp::timer_text(other.settings, "countdown.timerId")));
        }
    for (const auto& entry : data.object()["timer_widgets"].toArray()) {
        if (!entry.isObject()) {
            warnings << "Timer entry must be an object";
            continue;
        }
        const auto item = entry.toObject();
        bool valid = item["selectedSource"].isString();
        for (const char* field : {"periodDays", "periodHours", "periodMinutes", "periodSeconds",
                                  "selectedCountdownType"})
            if (item.contains(field) && (!item[field].isDouble() || item[field].toDouble() < 0))
                valid = false;
        for (const char* field :
             {"shouldCountUp", "showDays", "showHours", "showMinutes", "showSeconds",
              "showLeadingZero", "showEndMessage", "showEndScene", "useFormattedOutput",
              "useTextColour", "smoothenPeriodTimer", "startOnStreamStart",
              "resetTimerOnStreamStart"})
            if (item.contains(field) && !item[field].isBool())
                valid = false;
        for (const char* field :
             {"endMessage", "outputStringFormat", "selectedScene", "dateTime", "timerId"})
            if (item.contains(field) &&
                (!item[field].isString() || item[field].toString().size() > 1024))
                valid = false;
        if (!valid || item["selectedCountdownType"].toInt() > 1) {
            warnings << "Invalid or unsupported timer configuration";
            continue;
        }
        const auto name = item["selectedSource"].toString();
        const ldoc::Source* found = nullptr;
        int matches = 0;
        for (const auto& source : session_->project().sources)
            if (source.type == "source.text" && QString::fromStdString(source.name) == name) {
                found = &source;
                ++matches;
            }
        if (matches != 1 || !found || !bound.insert(found->id).second) {
            warnings << QStringLiteral("Missing, ambiguous or repeated text source: %1").arg(name);
            continue;
        }
        auto copy = *found;
        auto& settings = copy.settings;
        const double days = item["periodDays"].toDouble();
        const double hours = item["periodHours"].toDouble();
        const double minutes = item["periodMinutes"].toDouble();
        const double seconds = days * 86400 + hours * 3600 + minutes * 60 +
                               item["periodSeconds"].toDouble();
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 366.0 * 86400 ||
            days > double(lcomp::kCountdownMaxDays) || hours > 23 || minutes > 59 ||
            item["periodSeconds"].toDouble() > 59) {
            warnings << QStringLiteral("Invalid duration: %1").arg(name);
            continue;
        }
        const bool target = item["selectedCountdownType"].toInt() == 1;
        settings["countdown.obsOriginal"] =
            QJsonDocument(item).toJson(QJsonDocument::Compact).toStdString();
        settings["countdown.enabled"] = true;
        settings["countdown.running"] = false;
        settings["countdown.elapsedMs"] = std::int64_t{0};
        settings["countdown.order"] = order++;
        // The timer's own ID is carried across where it is free, because it is what a websocket
        // request and the dock header name it by. A collision takes a fresh one rather than
        // quietly giving two timers the same name.
        auto timer_id = item["timerId"].toString().trimmed();
        if (timer_id.isEmpty() || timer_id.size() > 64 || used_ids.contains(timer_id)) {
            if (!timer_id.isEmpty())
                warnings << QStringLiteral("Timer ID already in use, a new one was made: %1")
                                .arg(timer_id);
            timer_id = new_timer_id();
        }
        used_ids.insert(timer_id);
        settings["countdown.timerId"] = timer_id.toStdString();
        settings["countdown.periodDays"] = static_cast<std::int64_t>(days);
        settings["countdown.periodHours"] = static_cast<std::int64_t>(hours);
        settings["countdown.periodMinutes"] = static_cast<std::int64_t>(minutes);
        settings["countdown.periodSeconds"] =
            static_cast<std::int64_t>(item["periodSeconds"].toDouble());
        settings["countdown.durationMs"] = static_cast<std::int64_t>(seconds * 1000);
        settings["countdown.type"] = std::string(target ? "datetime" : "period");
        settings["countdown.countUp"] = item["shouldCountUp"].toBool();
        settings["countdown.mode"] = std::string(target                           ? "target"
                                                 : item["shouldCountUp"].toBool() ? "elapsed"
                                                                                  : "remaining");
        settings["countdown.useFormattedOutput"] = item["useFormattedOutput"].toBool();
        settings["countdown.format"] = item["useFormattedOutput"].toBool()
                                           ? item["outputStringFormat"].toString().toStdString()
                                           : std::string("%time%");
        settings["countdown.endMessage"] = item["endMessage"].toString().toStdString();
        settings["countdown.steady"] = item["smoothenPeriodTimer"].toBool();
        settings["countdown.startOnStreamStart"] = item["startOnStreamStart"].toBool();
        settings["countdown.resetOnStreamStart"] = item["resetTimerOnStreamStart"].toBool();
        settings["countdown.showEndScene"] = item["showEndScene"].toBool();
        for (const auto& [native, obs] : {std::pair{"showDays", "showDays"},
                                          {"showHours", "showHours"},
                                          {"showMinutes", "showMinutes"},
                                          {"showSeconds", "showSeconds"},
                                          {"leadingZero", "showLeadingZero"},
                                          {"showEndMessage", "showEndMessage"}})
            settings[std::string("countdown.") + native] = item[obs].toBool();
        if (target) {
            const auto time = QDateTime::fromString(item["dateTime"].toString(), Qt::ISODate);
            if (!time.isValid()) {
                warnings << "Invalid target time: " + name;
                continue;
            }
            settings["countdown.targetAtMs"] = static_cast<std::int64_t>(time.toMSecsSinceEpoch());
        }
        settings.erase("countdown.endScene");
        if (item["showEndScene"].toBool()) {
            const auto sceneName = item["selectedScene"].toString();
            int count = 0;
            for (const auto& scene : session_->project().scenes)
                if (QString::fromStdString(scene.name) == sceneName) {
                    settings["countdown.endScene"] = scene.id.to_string();
                    ++count;
                }
            if (count != 1) {
                warnings << "Missing/ambiguous end scene: " + sceneName;
                settings.erase("countdown.endScene");
                settings["countdown.showEndScene"] = false;
            }
        }
        // Colours come across as the flat form this build stores them in. A rule whose numbers
        // are missing is dropped rather than guessed at, and said so below.
        settings["countdown.useTextColour"] = item["useTextColour"].toBool();
        if (const auto main = item["mainTextColour"].toObject(); !main.isEmpty())
            settings["countdown.mainTextColour"] = ldoc::Rgba::from_srgb8(
                static_cast<std::uint8_t>(std::clamp(main["r"].toInt(), 0, 255)),
                static_cast<std::uint8_t>(std::clamp(main["g"].toInt(), 0, 255)),
                static_cast<std::uint8_t>(std::clamp(main["b"].toInt(), 0, 255)),
                static_cast<std::uint8_t>(std::clamp(main["a"].toInt(255), 0, 255)));
        QStringList rules;
        int dropped = 0;
        for (const auto& rule : item["colourRules"].toArray()) {
            const auto object = rule.toObject();
            const auto bound_ms = [&](const char* key) {
                const auto d = object[key].toObject();
                return ((std::int64_t(d["days"].toInt()) * 24 + d["hours"].toInt()) * 60 +
                        d["minutes"].toInt()) *
                           60 * 1000 +
                       std::int64_t(d["seconds"].toInt()) * 1000;
            };
            if (!object.contains("minTime") || !object.contains("maxTime")) {
                ++dropped;
                continue;
            }
            rules << QStringLiteral("%1,%2,%3,%4,%5,%6")
                         .arg(bound_ms("minTime"))
                         .arg(bound_ms("maxTime"))
                         .arg(std::clamp(object["r"].toInt(), 0, 255))
                         .arg(std::clamp(object["g"].toInt(), 0, 255))
                         .arg(std::clamp(object["b"].toInt(), 0, 255))
                         .arg(std::clamp(object["a"].toInt(255), 0, 255));
        }
        if (dropped > 0)
            warnings << QStringLiteral("%1 colour rule(s) without times were dropped: %2")
                            .arg(dropped)
                            .arg(name);
        settings["countdown.colourRules"] = rules.join(QLatin1Char(';')).toStdString();
        if (item["startOnStreamStart"].toBool())
            warnings << "Start on stream start is saved but nothing starts a broadcast yet: " +
                            name;
        commands.push_back(ldoc::SetSource{std::move(copy)});
    }
    if (!commands.empty())
        run(ldoc::make_batch(std::move(commands), "Import OBS countdown timers"));
    emit notice(QStringLiteral(
                    "Countdown import finished. Timers start paused. Hotkeys are not imported.%1")
                    .arg(warnings.isEmpty() ? QString{} : "\n" + warnings.join('\n')));
    return warnings.isEmpty();
}
} // namespace munkicast::ui

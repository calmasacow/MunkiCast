// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.hpp"

#include <algorithm>
#include <lcomp/filters.hpp>
#include <lcomp/vst.hpp>

namespace munkicast::ui {

namespace {
QVariant variant(const ldoc::ParamValue& v) {
    return std::visit(
        [](const auto& x) -> QVariant {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>)
                return QString::fromStdString(x);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return QVariant::fromValue(static_cast<qlonglong>(x));
            else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, bool>)
                return x;
            else
                return {};
        },
        v);
}

QVariantList fields(const lcomp::FilterInfo& info, const ldoc::ParamMap& params = {}) {
    QVariantList result;
    for (const auto& f : info.fields) {
        QStringList choices;
        for (const auto& choice : f.choices)
            choices.push_back(QString::fromStdString(choice));
        const auto found = params.find(f.key);
        result.push_back(
            QVariantMap{{"key", QString::fromStdString(f.key)},
                        {"label", QString::fromStdString(f.label)},
                        {"kind", QString::fromStdString(f.kind)},
                        {"value", variant(found == params.end() ? f.value : found->second)},
                        {"defaultValue", variant(f.value)},
                        {"minimum", f.minimum},
                        {"maximum", f.maximum},
                        {"choices", choices},
                        {"suffix", QString{}}});
    }
    return result;
}
} // namespace

QVariantMap Document::filterDetails(const QString& target) const {
    const auto parsed = ldoc::Id::parse(target.toStdString());
    if (!parsed)
        return {};
    const auto& p = session_->project();
    auto owner = *parsed;
    std::string name;
    for (const auto& scene : p.scenes)
        if (const auto* node = ldoc::find_node(scene.roots, owner)) {
            name = node->name;
            if (const auto* source = std::get_if<ldoc::SourceData>(&node->data);
                source && source->source)
                owner = *source->source;
            else if (const auto* pre = std::get_if<ldoc::PrecompData>(&node->data))
                owner = pre->scene;
            break;
        }
    if (const auto* source = p.source(owner))
        name = source->name;
    else if (const auto* scene = p.scene(owner))
        name = scene->name;
    else if (const auto* bus = p.bus(owner))
        name = bus->name;
    const auto* stack = p.effect_stack(owner);
    if (!stack)
        return {};
    QVariantList effects;
    for (const auto& e : *stack) {
        const auto* info = lcomp::filter_info(e.type);
        QVariantMap values;
        if (info)
            for (const auto& field : info->fields) {
                const auto it = e.params.find(field.key);
                values[QString::fromStdString(field.key)] =
                    variant(it == e.params.end() ? field.value : it->second);
            }
        auto valid = lcomp::validate_filter(e);
        if (valid && (e.type == "filter.mask" || e.type == "filter.lut")) {
            const auto size = lcomp::filter_size(std::span{&e, 1}, 1, 1);
            if (!size)
                valid = std::unexpected(size.error());
        }
        auto schema = info ? fields(*info, e.params) : QVariantList{};
        for (auto& row : schema) {
            auto field = row.toMap();
            const auto key = field.value("key").toString();
            if (key == "key_color" || key == "color_multiply" || key == "color_add" ||
                key == "color") {
                field["kind"] = "colour";
                field["label"] = field.value("label").toString().remove(" (RGB)");
                field["value"] =
                    QString("#%1").arg(field.value("value").toUInt(), 6, 16, QLatin1Char('0'));
                field["defaultValue"] = QString("#%1").arg(field.value("defaultValue").toUInt(), 6,
                                                           16, QLatin1Char('0'));
                values[key] = field["value"];
            } else if (field.value("kind") == "real") {
                field["kind"] = "slider";
                field["decimals"] = 4;
                if (key == "similarity" || key == "smoothness" || key == "spill") {
                    field["displayScale"] = 1000;
                    field["value"] = field.value("value").toDouble() * 1000;
                    field["minimum"] = field.value("minimum").toDouble() * 1000;
                    field["maximum"] = field.value("maximum").toDouble() * 1000;
                    field["decimals"] = 0;
                }
            }
            if (key == "attack_time" || key == "release_time" || key == "hold_time" ||
                key == "delay_ms")
                field["suffix"] = "ms";
            if (key == "db" || key == "threshold" || key == "open_threshold" ||
                key == "close_threshold" || key == "output_gain" || key == "suppress_level" ||
                key == "low" || key == "mid" || key == "high" || key == "knee")
                field["suffix"] = "dB";
            bool visible = key != "chunk_data";
            if (e.type == "filter.crop" && (key == "right" || key == "bottom"))
                visible = lcomp::filter_flag(e, "relative");
            if (e.type == "filter.crop" && (key == "cx" || key == "cy"))
                visible = !lcomp::filter_flag(e, "relative");
            if (e.type == "filter.scroll" && key == "cx")
                visible = lcomp::filter_flag(e, "limit_cx");
            if (e.type == "filter.scroll" && key == "cy")
                visible = lcomp::filter_flag(e, "limit_cy");
            if (e.type == "filter.noise-suppression" && key == "suppress_level")
                visible = lcomp::filter_text(e, "method") == "speex";
            field["visible"] = visible;
            row = field;
        }
        if (e.type == "filter.vst") {
            const auto plugin = lcomp::vst_probe(e);
            if (!plugin.error.empty())
                valid = std::unexpected(plugin.error);
            for (std::size_t index = 0; index < plugin.parameters.size(); ++index) {
                const auto key = "parameter." + std::to_string(index);
                const auto& parameter = plugin.parameters[index];
                const auto it = e.params.find(key);
                const auto current =
                    it == e.params.end() ? QVariant{parameter.value} : variant(it->second);
                schema.push_back(QVariantMap{{"key", QString::fromStdString(key)},
                                             {"label", QString::fromStdString(parameter.name)},
                                             {"kind", "slider"},
                                             {"decimals", 3},
                                             {"value", current},
                                             {"minimum", 0},
                                             {"maximum", 1},
                                             {"suffix", QString::fromStdString(parameter.unit)}});
                values[QString::fromStdString(key)] = current;
            }
            for (auto& row : schema) {
                auto field = row.toMap();
                if (field.value("key") == "program") {
                    field["maximum"] = std::max(0, plugin.programs - 1);
                    row = field;
                }
            }
        }
        for (auto& row : schema) {
            auto field = row.toMap();
            if (field.value("key").toString() == "path") {
                field["kind"] = "path";
                row = field;
            }
            if (field.value("key").toString() != "sidechain")
                continue;
            QStringList choices{"none"};
            QVariantMap labels{{"none", QStringLiteral("None")}};
            for (const auto& source : p.sources)
                if (ldoc::has_audio(source) && source.id != owner) {
                    const auto id = QString::fromStdString(source.id.to_string());
                    choices.push_back(id);
                    labels[id] = QString::fromStdString(source.name);
                }
            const auto saved = field.value("value").toString();
            if (!choices.contains(saved)) {
                choices.push_back(saved);
                labels[saved] = QStringLiteral("Unavailable source");
            }
            field["kind"] = "choice";
            field["label"] = "Sidechain source";
            field["choices"] = choices;
            field["choiceLabels"] = labels;
            row = field;
        }
        effects.push_back(QVariantMap{
            {"id", QString::fromStdString(e.id.to_string())},
            {"name",
             QString::fromStdString(e.name.empty() ? (info ? info->name : e.type) : e.name)},
            {"type", QString::fromStdString(e.type)},
            {"enabled", e.enabled},
            {"audio", info && info->audio},
            {"values", values},
            {"schema", schema},
            {"status", valid ? QString{} : QString::fromStdString(valid.error())}});
    }
    const auto* source = p.source(owner);
    const bool audioOnly =
        p.bus(owner) ||
        (source && (source->type == "source.audio-input" || source->type == "source.audio-output"));
    return {{"audioOnly", audioOnly},
            {"hasAudio", !source || ldoc::has_audio(*source)},
            {"owner", QString::fromStdString(owner.to_string())},
            {"name", QString::fromStdString(name)},
            {"effects", effects}};
}

void Document::showFilters(const QString& target) {
    const auto details = filterDetails(target);
    if (!details.empty())
        emit filtersRequested(details.value("owner").toString());
}

QVariantList Document::filterTypes(const QString& owner) const {
    QVariantList result;
    const auto id = ldoc::Id::parse(owner.toStdString());
    if (!id)
        return result;
    const auto* source = session_->project().source(*id);
    const bool audioOnly =
        session_->project().bus(*id) ||
        (source && (source->type == "source.audio-input" || source->type == "source.audio-output"));
    const bool audio = !source || ldoc::has_audio(*source);
    for (const auto& info : lcomp::filter_catalog()) {
        if ((info.audio && !audio) || (!info.audio && audioOnly))
            continue;
        result.push_back(QVariantMap{{"type", QString::fromStdString(info.type)},
                                     {"name", QString::fromStdString(info.name)},
                                     {"audio", info.audio}});
    }
    return result;
}

void Document::addFilter(const QString& owner, const QString& type) {
    const auto id = ldoc::Id::parse(owner.toStdString());
    const auto* info = lcomp::filter_info(type.toStdString());
    const auto* stack = id ? session_->project().effect_stack(*id) : nullptr;
    if (!stack || !info)
        return;
    const auto available = filterTypes(owner);
    if (std::ranges::none_of(
            available, [&](const auto& v) { return v.toMap().value("type").toString() == type; }))
        return;
    ldoc::Effect e;
    e.id = ldoc::Id::generate();
    e.type = info->type;
    e.name = info->name;
    for (const auto& f : info->fields)
        e.params[f.key] = f.value;
    auto next = *stack;
    next.push_back(std::move(e));
    run(ldoc::SetEffects{*id, std::move(next)});
}

void Document::editFilter(const QString& owner, const QString& effect, const QString& name,
                          bool enabled, const QVariantMap& values) {
    const auto id = ldoc::Id::parse(owner.toStdString()),
               eid = ldoc::Id::parse(effect.toStdString());
    const auto* stack = id ? session_->project().effect_stack(*id) : nullptr;
    if (!stack || !eid || name.size() > 256)
        return;
    auto next = *stack;
    for (auto& e : next)
        if (e.id == *eid) {
            e.name = name.toStdString();
            e.enabled = enabled;
            if (const auto* info = lcomp::filter_info(e.type))
                for (const auto& f : info->fields) {
                    const auto it = values.find(QString::fromStdString(f.key));
                    if (it == values.end())
                        continue;
                    if (f.kind == "bool") {
                        if (it->metaType().id() != QMetaType::Bool)
                            return;
                        e.params[f.key] = it->toBool();
                    } else if (f.kind == "int" || f.kind == "real") {
                        bool ok = false;
                        const bool color = f.key == "key_color" || f.key == "color_multiply" ||
                                           f.key == "color_add" || f.key == "color";
                        const double n = color && it->toString().startsWith('#')
                                             ? it->toString().mid(1).toUInt(&ok, 16)
                                             : it->toDouble(&ok);
                        if (!ok)
                            return;
                        e.params[f.key] = n;
                    } else
                        e.params[f.key] = it->toString().toStdString();
                }
            if (e.type == "filter.vst")
                for (auto it = values.begin(); it != values.end(); ++it)
                    if (it.key().startsWith("parameter.")) {
                        bool ok = false;
                        const double v = it->toDouble(&ok);
                        if (!ok || !std::isfinite(v) || v < 0 || v > 1)
                            return;
                        e.params[it.key().toStdString()] = v;
                    }
            if (enabled)
                if (auto valid = lcomp::validate_filter(e); !valid) {
                    emit notice(QString::fromStdString(valid.error()));
                    return;
                }
            run(ldoc::SetEffects{*id, std::move(next)});
            return;
        }
}

void Document::moveFilter(const QString& owner, const QString& effect, int offset) {
    if (offset != -1 && offset != 1)
        return;
    const auto id = ldoc::Id::parse(owner.toStdString()),
               eid = ldoc::Id::parse(effect.toStdString());
    const auto* stack = id ? session_->project().effect_stack(*id) : nullptr;
    if (!stack || !eid)
        return;
    auto next = *stack;
    for (std::size_t i = 0; i < next.size(); ++i)
        if (next[i].id == *eid) {
            const auto* info = lcomp::filter_info(next[i].type);
            const bool audio = info && info->audio;
            for (auto j = static_cast<std::ptrdiff_t>(i) + offset;
                 j >= 0 && j < static_cast<std::ptrdiff_t>(next.size()); j += offset) {
                const auto index = static_cast<std::size_t>(j);
                const auto* other = lcomp::filter_info(next[index].type);
                if ((other && other->audio) != audio)
                    continue;
                std::swap(next[i], next[index]);
                run(ldoc::SetEffects{*id, std::move(next)});
                return;
            }
            return;
        }
}

void Document::removeFilter(const QString& owner, const QString& effect) {
    const auto id = ldoc::Id::parse(owner.toStdString()),
               eid = ldoc::Id::parse(effect.toStdString());
    const auto* stack = id ? session_->project().effect_stack(*id) : nullptr;
    if (!stack || !eid)
        return;
    auto next = *stack;
    std::erase_if(next, [&](const auto& e) { return e.id == *eid; });
    if (next.size() != stack->size())
        run(ldoc::SetEffects{*id, std::move(next)});
}
} // namespace munkicast::ui

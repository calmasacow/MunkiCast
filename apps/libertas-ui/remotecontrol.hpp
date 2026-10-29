// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ldoc/command.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <map>

namespace libertas::ui {
class Document;
class Preview;
class Shortcuts;
struct RemoteResult {
    int code = 100;
    QString comment;
    QJsonObject data;
};
// Both wire modes use the same validated document commands and renderer controls.
class RemoteControl {
  public:
    RemoteControl(Document& document, Preview& preview, bool obs, Shortcuts* shortcuts = nullptr);
    RemoteResult request(const QString& type, const QJsonObject& data);
    QJsonArray availableRequests() const;
    QJsonObject state();
    QJsonObject project() const;
    int itemId(const ldoc::Id& id);
    QJsonObject item(const ldoc::Scene& scene, const ldoc::Node& node, int index);
    QJsonArray methods() const;
    bool obsMode() const { return obs_; }
  private:
    RemoteResult execute(const ldoc::Command& command);
    RemoteResult invoke(const QJsonObject& data);
    const ldoc::Scene* scene(const QJsonObject& data, const QString& prefix = "scene") const;
    const ldoc::Source* source(const QJsonObject& data, const QString& prefix = "input") const;
    QString transitionName(const QString& id) const;
    Document& document_;
    Preview& preview_;
    bool obs_;
    Shortcuts* shortcuts_;
    std::map<ldoc::Id, int> item_ids_;
    int next_item_id_ = 1;
    double tbar_ = 0;
};
} // namespace libertas::ui

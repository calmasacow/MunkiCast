// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <filesystem>

class QQmlEngine;
class QJSEngine;

namespace munkicast::ui {
class Preview;
class Shortcuts;
class Document;
class WebsocketServer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(int port READ port NOTIFY changed)
    Q_PROPERTY(QString password READ password NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

  public:
    static WebsocketServer* create(QQmlEngine*, QJSEngine*);
    void initialize(Preview& preview, Document& document, const std::filesystem::path& path,
                    bool useEnvironment, Shortcuts* shortcuts = nullptr);
    bool enabled() const { return enabled_; }
    int port() const { return port_; }
    QString password() const { return password_; }
    QString mode() const { return mode_; }
    bool running() const { return !server_.isNull(); }
    QString error() const { return error_; }
    Q_INVOKABLE bool apply(bool enabled, int port, const QString& password);
    Q_INVOKABLE bool applyMode(bool enabled, int port, const QString& password, const QString& mode);

  signals:
    void changed();

  private:
    using QObject::QObject;
    void restart();
    Preview* preview_ = nullptr;
    Document* document_ = nullptr;
    Shortcuts* shortcuts_ = nullptr;
    QPointer<QObject> server_;
    QString path_, password_, error_;
    QString mode_ = "munkicast";
    bool enabled_ = false;
    int port_ = 4455;
};
} // namespace munkicast::ui

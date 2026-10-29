// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMetaMethod>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QtQml/qqmlregistration.h>
#include <filesystem>

class QQmlEngine;
class QJSEngine;
class QTcpSocket;

namespace libertas::ui {
class Document;

class McpServer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(int port READ port NOTIFY changed)
    Q_PROPERTY(QString token READ token NOTIFY changed)
    Q_PROPERTY(QString endpoint READ endpoint NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
  public:
    static McpServer* create(QQmlEngine*, QJSEngine*);
    void initialize(QQmlEngine& engine, Document& document, const std::filesystem::path& path,
                    const QMap<QString, QObject*>& targets);

    bool enabled() const { return enabled_; }

    bool running() const { return listener_.isListening(); }

    int port() const { return port_; }

    QString token() const { return token_; }

    QString endpoint() const;

    QString error() const { return error_; }

    Q_INVOKABLE bool apply(bool enabled, int port, const QString& token);
    Q_INVOKABLE QString newToken() const;
  signals:
    void changed();

  private:
    explicit McpServer(QObject* parent = nullptr);
    void restart();
    void accept();
    void http(QTcpSocket* socket, const QByteArray& request);
    QJsonObject rpc(const QJsonObject& request);
    QJsonArray tools();
    QJsonObject call(const QString& name, const QJsonObject& args);
    QString objectId(QObject* object);
    QObject* object(const QString& id) const;
    QList<QObject*> uiObjects() const;
    QJsonObject properties(QObject* object);
    QJsonValue json(const QVariant& value);
    QJsonValue invoke(QObject* object, const QMetaMethod& method, const QJsonArray& args);
    QMap<QString, QPair<QObject*, QMetaMethod>> nativeTools_;
    QMap<QString, QPointer<QObject>> objects_;
    QHash<QObject*, QString> ids_;
    quint64 serial_ = 0;
    QPointer<QQmlEngine> engine_;
    Document* document_ = nullptr;
    QMap<QString, QObject*> targets_;
    QTcpServer listener_;
    QList<QPointer<QTcpSocket>> sockets_;
    QString path_, token_, error_;
    bool enabled_ = false;
    int port_ = 4466;
    Qt::MouseButtons buttons_ = Qt::NoButton;
};
} // namespace libertas::ui

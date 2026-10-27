// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <optional>

namespace libertas::ui {
std::optional<QString> oauth_callback_code(const QByteArray& request, const QString& state);

class Connectors : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString target READ target NOTIFY changed)
    Q_PROPERTY(QVariantList streams READ streams NOTIFY changed)
  public:
    explicit Connectors(QObject* parent = nullptr);
    static QVariantList catalog();
    static QVariantMap service(const QString& id);

    bool busy() const { return busy_; }

    QString status() const { return status_; }

    QString target() const { return output_; }

    QVariantList streams() const { return streams_; }

    Q_INVOKABLE QString clientId(const QString& provider) const;
    Q_INVOKABLE void setClientId(const QString& provider, const QString& id);

    Q_INVOKABLE void setGoogleSecret(const QString& secret) { googleSecret_ = secret; }

    Q_INVOKABLE void cancel();
    Q_INVOKABLE void chooseStream(const QString& id);
    void keySaved(const QString& output, bool saved);
    void start(const QString& output, const QString& provider, const QString& project);
  signals:
    void changed();
    void authorized(const QString& output, const QString& provider, const QString& project,
                    const QString& url, const QString& key);

  private:
    void finish(const QString& message);
    void request(const QUrl& url, const QUrlQuery& form, std::function<void(QJsonObject, int)> done,
                 bool get = false);
    void twitchPoll();
    void loadAccount(const QString& token);
    void loadYouTubeStreams(const QString& page = {});
    QNetworkAccessManager network_;
    QTcpServer listener_;
    QTimer poll_, deadline_;
    bool busy_ = false;
    quint64 generation_ = 0;
    QString status_, output_, provider_, project_, device_, state_, verifier_, redirect_;
    // ponytail: OAuth only retrieves stream keys; add keyring-backed refresh tokens when
    // account APIs are needed during broadcasts.
    QString googleSecret_, token_;
    QVariantList streams_;
    QMap<QString, QJsonObject> streamData_;
};
} // namespace libertas::ui

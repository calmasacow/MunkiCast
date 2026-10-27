// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "connectors.hpp"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSettings>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QUuid>
#include <memory>

namespace libertas::ui {
std::optional<QString> oauth_callback_code(const QByteArray& request, const QString& state) {
    if (state.isEmpty() || request.size() > 16384 || !request.contains("\r\n\r\n"))
        return {};
    const auto parts = request.first(request.indexOf("\r\n")).split(' ');
    if (parts.size() != 3 || parts[0] != "GET" || parts[2] != "HTTP/1.1")
        return {};
    const QUrl url(QString::fromUtf8(parts[1]), QUrl::StrictMode);
    const QUrlQuery query(url);
    if (!url.isValid() || !url.isRelative() || !url.host().isEmpty() || url.path() != "/callback" ||
        url.hasFragment() || query.queryItemValue("state", QUrl::FullyDecoded) != state)
        return {};
    return query.queryItemValue("code", QUrl::FullyDecoded);
}

QVariantList Connectors::catalog() {
    static const QVariantList entries = [] {
        QVariantList out{
            QVariantMap{{"id", "rtmp"}, {"title", "RTMP"}, {"kind", "rtmp"}},
            QVariantMap{{"id", "srt"}, {"title", "SRT (not available yet)"}, {"kind", "srt"}},
            QVariantMap{{"id", "whip"}, {"title", "WHIP (not available yet)"}, {"kind", "whip"}}};
        QFile file(QStringLiteral(":/connectors/data/obs-services.json"));
        if (!file.open(QIODevice::ReadOnly))
            return out;
        const auto services =
            QJsonDocument::fromJson(file.readAll()).object()["services"].toArray();
        for (const auto& item : services) {
            const auto s = item.toObject();
            if (s["protocol"].toString("RTMP") != "RTMP")
                continue;
            const auto name = s["name"].toString();
            const auto id = name == "Twitch"            ? QStringLiteral("twitch")
                            : name == "YouTube - RTMPS" ? QStringLiteral("youtube")
                                                        : QStringLiteral("obs:") + name;
            QVariantList servers;
            for (const auto& server : s["servers"].toArray()) {
                const auto entry = server.toObject();
                const auto url = entry["url"].toString();
                if (url.startsWith("rtmp://") || url.startsWith("rtmps://"))
                    servers.append(QVariantMap{{"id", url}, {"title", entry["name"].toString()}});
            }
            if (servers.isEmpty())
                continue;
            if (id == "twitch")
                servers.prepend(
                    QVariantMap{{"id", "rtmp://live.twitch.tv/app"}, {"title", "Auto"}});
            out.append(QVariantMap{{"id", id},
                                   {"title", id == "youtube" ? "YouTube" : name},
                                   {"kind", "rtmp"},
                                   {"servers", servers},
                                   {"oauth", id == "twitch" || id == "youtube"},
                                   {"keyLink", s["stream_key_link"].toString()}});
        }
        return out;
    }();
    return entries;
}

QVariantMap Connectors::service(const QString& id) {
    for (const auto& entry : catalog())
        if (entry.toMap()["id"] == id)
            return entry.toMap();
    return {};
}

Connectors::Connectors(QObject* parent) : QObject(parent) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this,
            [this] { finish(tr("Account connection timed out.")); });
    poll_.setSingleShot(true);
    connect(&poll_, &QTimer::timeout, this, &Connectors::twitchPoll);
    connect(&listener_, &QTcpServer::newConnection, this, [this] {
        while (listener_.hasPendingConnections()) {
            auto* socket = listener_.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            const auto generation = generation_;
            QTimer::singleShot(5000, socket, [socket] { socket->disconnectFromHost(); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, generation] {
                buffer->append(socket->readAll());
                if (buffer->size() > 16384) {
                    socket->disconnectFromHost();
                    return;
                }
                if (!buffer->contains("\r\n\r\n"))
                    return;
                const auto code = oauth_callback_code(*buffer, state_);
                const bool valid = generation == generation_ && busy_ && code.has_value();
                socket->write(
                    valid
                        ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: "
                          "close\r\n\r\nReturn to Libertas Studio."
                        : "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid callback.");
                socket->disconnectFromHost();
                if (!valid)
                    return;
                listener_.close();
                state_.clear();
                if (code->isEmpty()) {
                    finish(tr("Account authorization was declined."));
                    return;
                }
                QUrlQuery form;
                form.addQueryItem("client_id", clientId("youtube"));
                const auto secret = googleSecret_.isEmpty()
                                        ? qEnvironmentVariable("LIBERTAS_GOOGLE_CLIENT_SECRET")
                                        : googleSecret_;
                if (!secret.isEmpty())
                    form.addQueryItem("client_secret", secret);
                form.addQueryItem("code", *code);
                form.addQueryItem("code_verifier", verifier_);
                form.addQueryItem("redirect_uri", redirect_);
                form.addQueryItem("grant_type", "authorization_code");
                request(QUrl("https://oauth2.googleapis.com/token"), form,
                        [this](QJsonObject data, int status) {
                            if (status != 200 || data["access_token"].toString().isEmpty())
                                finish(tr("Google token exchange failed. Check the desktop OAuth "
                                          "credentials."));
                            else
                                loadAccount(data["access_token"].toString());
                        });
            });
        }
    });
}

QString Connectors::clientId(const QString& provider) const {
    if (provider != "twitch" && provider != "youtube")
        return {};
    const auto env = qEnvironmentVariable(provider == "twitch" ? "LIBERTAS_TWITCH_CLIENT_ID"
                                                               : "LIBERTAS_GOOGLE_CLIENT_ID");
    return env.isEmpty() ? QSettings("Libertas", "Studio").value("oauth/" + provider).toString()
                         : env;
}

void Connectors::setClientId(const QString& provider, const QString& id) {
    if ((provider != "twitch" && provider != "youtube") || id.size() > 1024)
        return;
    cancel();
    QSettings("Libertas", "Studio").setValue("oauth/" + provider, id.trimmed());
    emit changed();
}

void Connectors::finish(const QString& message) {
    ++generation_;
    for (auto* reply : network_.findChildren<QNetworkReply*>())
        reply->abort();
    busy_ = false;
    poll_.stop();
    deadline_.stop();
    listener_.close();
    device_.clear();
    state_.clear();
    verifier_.clear();
    token_.clear();
    streams_.clear();
    streamData_.clear();
    status_ = message;
    emit changed();
}

void Connectors::cancel() {
    finish({});
}

void Connectors::keySaved(const QString& output, bool saved) {
    if (output != output_ || busy_)
        return;
    status_ = saved
                  ? tr("Connected. Stream key saved in your system keyring.")
                  : tr("Stream key could not be saved. Check that the system keyring is unlocked.");
    emit changed();
}

void Connectors::request(const QUrl& url, const QUrlQuery& form,
                         std::function<void(QJsonObject, int)> done, bool get) {
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    if (get) {
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
        if (provider_ == "twitch")
            req.setRawHeader("Client-Id", clientId("twitch").toUtf8());
    } else
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    QByteArray body;
    for (const auto& item : form.queryItems(QUrl::FullyDecoded)) {
        if (!body.isEmpty())
            body += '&';
        body += QUrl::toPercentEncoding(item.first) + '=' + QUrl::toPercentEncoding(item.second);
    }
    auto* reply = get ? network_.get(req) : network_.post(req, body);
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, done] {
        const auto data = QJsonDocument::fromJson(reply->readAll()).object();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (generation == generation_ && busy_)
            done(data, status);
    });
}

void Connectors::start(const QString& output, const QString& provider, const QString& project) {
    cancel();
    output_ = output;
    provider_ = provider;
    project_ = project;
    if (provider != "twitch" && provider != "youtube")
        return;
    if (clientId(provider).isEmpty()) {
        finish(tr("Enter this app's OAuth client ID first."));
        return;
    }
    busy_ = true;
    status_ = tr("Waiting for account authorization in your browser…");
    deadline_.start(300000);
    emit changed();
    if (provider == "twitch") {
        QUrlQuery form;
        form.addQueryItem("client_id", clientId(provider));
        form.addQueryItem("scopes", "channel:read:stream_key");
        request(
            QUrl("https://id.twitch.tv/oauth2/device"), form, [this](QJsonObject data, int status) {
                device_ = data["device_code"].toString();
                const QUrl url(data["verification_uri"].toString());
                if (status != 200 || device_.isEmpty() || url.scheme() != "https" ||
                    url.host() != "www.twitch.tv") {
                    finish(
                        tr("Twitch device authorization failed. Register a public OAuth client."));
                    return;
                }
                status_ = tr("Enter code %1 in your browser.").arg(data["user_code"].toString());
                emit changed();
                if (!QDesktopServices::openUrl(url)) {
                    finish(tr("Could not open the authorization browser."));
                    return;
                }
                poll_.start(qMax(5, data["interval"].toInt(5)) * 1000);
            });
    } else {
        if (!listener_.listen(QHostAddress::LocalHost, 0)) {
            finish(tr("Could not open the local OAuth callback."));
            return;
        }
        state_ = QUuid::createUuid().toString(QUuid::Id128);
        verifier_ =
            QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
        redirect_ = QString("http://127.0.0.1:%1/callback").arg(listener_.serverPort());
        QUrl url("https://accounts.google.com/o/oauth2/v2/auth");
        QUrlQuery query;
        query.addQueryItem("client_id", clientId(provider));
        query.addQueryItem("redirect_uri", redirect_);
        query.addQueryItem("response_type", "code");
        query.addQueryItem("scope", "https://www.googleapis.com/auth/youtube.readonly");
        query.addQueryItem("state", state_);
        query.addQueryItem("code_challenge_method", "S256");
        query.addQueryItem(
            "code_challenge",
            QString::fromLatin1(
                QCryptographicHash::hash(verifier_.toUtf8(), QCryptographicHash::Sha256)
                    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
        url.setQuery(query);
        if (!QDesktopServices::openUrl(url))
            finish(tr("Could not open the authorization browser."));
    }
}

void Connectors::twitchPoll() {
    QUrlQuery form;
    form.addQueryItem("client_id", clientId("twitch"));
    form.addQueryItem("device_code", device_);
    form.addQueryItem("grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    request(QUrl("https://id.twitch.tv/oauth2/token"), form, [this](QJsonObject data, int status) {
        if (status == 200 && !data["access_token"].toString().isEmpty()) {
            loadAccount(data["access_token"].toString());
            return;
        }
        const auto error = data["message"].toString(data["error"].toString());
        if (error == "authorization_pending")
            poll_.start();
        else if (error == "slow_down")
            poll_.start(poll_.interval() + 5000);
        else
            finish(tr("Twitch authorization failed or expired. Try connecting again."));
    });
}

void Connectors::loadAccount(const QString& token) {
    token_ = token;
    status_ = tr("Loading streaming destinations…");
    emit changed();
    if (provider_ == "youtube") {
        loadYouTubeStreams();
        return;
    }
    request(
        QUrl("https://api.twitch.tv/helix/users"), {},
        [this](QJsonObject data, int status) {
            const auto id = data["data"].toArray().first().toObject()["id"].toString();
            if (status != 200 || id.isEmpty()) {
                finish(tr("Could not read the Twitch account."));
                return;
            }
            QUrl url("https://api.twitch.tv/helix/streams/key");
            QUrlQuery query;
            query.addQueryItem("broadcaster_id", id);
            url.setQuery(query);
            request(
                url, {},
                [this](QJsonObject keyData, int keyStatus) {
                    const auto key =
                        keyData["data"].toArray().first().toObject()["stream_key"].toString();
                    if (keyStatus != 200 || key.isEmpty()) {
                        finish(tr("Could not read the Twitch stream key. Check account permissions "
                                  "and two-factor authentication."));
                        return;
                    }
                    finish(tr("Authorization complete. Saving the stream key to your keyring."));
                    emit authorized(output_, provider_, project_, "rtmp://live.twitch.tv/app", key);
                },
                true);
        },
        true);
}

void Connectors::loadYouTubeStreams(const QString& page) {
    QUrl url("https://www.googleapis.com/youtube/v3/liveStreams");
    QUrlQuery query;
    query.addQueryItem("part", "snippet,cdn");
    query.addQueryItem("mine", "true");
    query.addQueryItem("maxResults", "50");
    if (!page.isEmpty())
        query.addQueryItem("pageToken", page);
    url.setQuery(query);
    request(
        url, {},
        [this](QJsonObject data, int status) {
            if (status != 200) {
                finish(tr("Could not read YouTube streams. Enable YouTube Data API v3 and live "
                          "streaming for the account."));
                return;
            }
            for (const auto& value : data["items"].toArray()) {
                const auto stream = value.toObject();
                const auto id = stream["id"].toString();
                const auto ingestion = stream["cdn"].toObject()["ingestionInfo"].toObject();
                if (id.isEmpty() || ingestion["streamName"].toString().isEmpty())
                    continue;
                streamData_[id] = ingestion;
                streams_.append(QVariantMap{
                    {"id", id}, {"title", stream["snippet"].toObject()["title"].toString()}});
            }
            const auto next = data["nextPageToken"].toString();
            if (!next.isEmpty() && streams_.size() < 500) {
                loadYouTubeStreams(next);
                return;
            }
            token_.clear();
            if (streams_.isEmpty()) {
                finish(tr("No YouTube RTMP streams found. Create a stream in YouTube Studio, then "
                          "reconnect."));
                return;
            }
            status_ = tr("Choose the YouTube stream to connect.");
            emit changed();
        },
        true);
}

void Connectors::chooseStream(const QString& id) {
    if (!busy_ || provider_ != "youtube" || !streamData_.contains(id))
        return;
    const auto data = streamData_.value(id);
    const auto url = data["rtmpsIngestionAddress"].toString(data["ingestionAddress"].toString());
    if (!url.startsWith("rtmp://") && !url.startsWith("rtmps://")) {
        finish(tr("This YouTube stream does not use RTMP ingestion."));
        return;
    }
    finish(tr("Authorization complete. Saving the stream key to your keyring."));
    emit authorized(output_, provider_, project_, url, data["streamName"].toString());
}
} // namespace libertas::ui

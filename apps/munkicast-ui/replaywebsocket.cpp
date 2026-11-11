// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "replaywebsocket.hpp"

#include "document.hpp"
#include "preview.hpp"
#include "remotecontrol.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#undef signals
#include <cstring>
#include <libsoup/soup.h>
#include <msgpack.h>

namespace munkicast::ui {
namespace {
QByteArray hash(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toBase64();
}

bool authenticated(const QByteArray& supplied, const QByteArray& expected) {
    if (supplied.size() != expected.size())
        return false;
    volatile unsigned char difference = 0;
    for (qsizetype i = 0; i < expected.size(); ++i)
        difference = static_cast<unsigned char>(difference | (supplied[i] ^ expected[i]));
    return difference == 0;
}

std::expected<QJsonValue, QString> unpackValue(const msgpack_object& value, int depth = 0) {
    if (depth > 64)
        return std::unexpected("MessagePack nesting exceeds 64.");
    switch (value.type) {
    case MSGPACK_OBJECT_NIL:
        return QJsonValue(QJsonValue::Null);
    case MSGPACK_OBJECT_BOOLEAN:
        return QJsonValue(value.via.boolean);
    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        if (value.via.u64 > 9007199254740991ULL)
            return std::unexpected("Integer exceeds JSON precision.");
        return QJsonValue(static_cast<double>(value.via.u64));
    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        if (value.via.i64 < -9007199254740991LL)
            return std::unexpected("Integer exceeds JSON precision.");
        return QJsonValue(static_cast<double>(value.via.i64));
    case MSGPACK_OBJECT_FLOAT32:
    case MSGPACK_OBJECT_FLOAT64:
        if (!std::isfinite(value.via.f64))
            return std::unexpected("Non-finite number.");
        return QJsonValue(value.via.f64);
    case MSGPACK_OBJECT_STR:
        return QJsonValue(QString::fromUtf8(value.via.str.ptr, value.via.str.size));
    case MSGPACK_OBJECT_ARRAY: {
        QJsonArray result;
        for (uint32_t i = 0; i < value.via.array.size; ++i) {
            auto child = unpackValue(value.via.array.ptr[i], depth + 1);
            if (!child)
                return std::unexpected(child.error());
            result.append(*child);
        }
        return result;
    }
    case MSGPACK_OBJECT_MAP: {
        QJsonObject result;
        for (uint32_t i = 0; i < value.via.map.size; ++i) {
            const auto& entry = value.via.map.ptr[i];
            if (entry.key.type != MSGPACK_OBJECT_STR)
                return std::unexpected("Object keys must be strings.");
            const auto key = QString::fromUtf8(entry.key.via.str.ptr, entry.key.via.str.size);
            if (result.contains(key))
                return std::unexpected("Duplicate object key.");
            auto child = unpackValue(entry.val, depth + 1);
            if (!child)
                return std::unexpected(child.error());
            result[key] = *child;
        }
        return result;
    }
    default:
        return std::unexpected("Binary and extension values are not JSON values.");
    }
}

void packValue(msgpack_packer& packer, const QJsonValue& value) {
    if (value.isBool()) {
        if (value.toBool())
            msgpack_pack_true(&packer);
        else
            msgpack_pack_false(&packer);
    } else if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::trunc(number) == number && number >= -9007199254740991.0 &&
            number <= 9007199254740991.0)
            msgpack_pack_int64(&packer, static_cast<int64_t>(number));
        else
            msgpack_pack_double(&packer, number);
    } else if (value.isString()) {
        const auto bytes = value.toString().toUtf8();
        msgpack_pack_str(&packer, static_cast<std::size_t>(bytes.size()));
        msgpack_pack_str_body(&packer, bytes.constData(), static_cast<std::size_t>(bytes.size()));
    } else if (value.isArray()) {
        const auto array = value.toArray();
        msgpack_pack_array(&packer, static_cast<std::size_t>(array.size()));
        for (const auto child : array)
            packValue(packer, child);
    } else if (value.isObject()) {
        const auto object = value.toObject();
        msgpack_pack_map(&packer, static_cast<std::size_t>(object.size()));
        for (auto it = object.begin(); it != object.end(); ++it) {
            packValue(packer, it.key());
            packValue(packer, it.value());
        }
    } else
        msgpack_pack_nil(&packer);
}

bool binary(SoupWebsocketConnection* connection) {
    const auto* protocol = soup_websocket_connection_get_protocol(connection);
    return protocol && std::strcmp(protocol, "obswebsocket.msgpack") == 0;
}

QString id(const ldoc::Id& value) {
    return QString::fromStdString(value.to_string());
}

QString text(const std::string& value) {
    return QString::fromStdString(value);
}

class ReplayWebsocket : public QObject {
    struct Request {
        QString id, type;
        QJsonObject data;
        bool batch = false;
    };

    struct Batch {
        QString id;
        QJsonArray requests, results;
        int next = 0;
        bool halt = false, failed = false;
    };

    struct Client {
        QByteArray expected;
        bool identified = false;
        int events = 2047;
        quint64 incoming = 0, outgoing = 0, serial = 0;
        std::optional<Request> pending;
        std::optional<Batch> batch;
        QString savePath, family;
        bool wanted = false;
        qint64 deadline = 0;
    };

    SoupServer* server_ = nullptr;
    quint64 nextClient_ = 0;
    std::map<SoupWebsocketConnection*, Client> clients_;
    Preview& preview_;
    Document& document_;
    RemoteControl api_;
    QByteArray password_;
    QString persistentPath_;
    Shortcuts* shortcuts_ = nullptr;
    ldoc::Project previous_;
    bool stream_ = false, record_ = false, replay_ = false;
    std::map<QString, qint64> started_;

    void send(SoupWebsocketConnection* connection, const QJsonObject& data) {
        if (soup_websocket_connection_get_state(connection) != SOUP_WEBSOCKET_STATE_OPEN)
            return;
        if (auto found = clients_.find(connection); found != clients_.end())
            ++found->second.outgoing;
        if (binary(connection)) {
            msgpack_sbuffer buffer;
            msgpack_sbuffer_init(&buffer);
            msgpack_packer packer;
            msgpack_packer_init(&packer, &buffer, msgpack_sbuffer_write);
            packValue(packer, data);
            soup_websocket_connection_send_binary(connection, buffer.data, buffer.size);
            msgpack_sbuffer_destroy(&buffer);
        } else {
            const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
            soup_websocket_connection_send_text(connection, bytes.constData());
        }
    }

    void event(const QString& type, int intent, const QJsonObject& data) {
        for (auto& [connection, client] : clients_)
            if (client.identified && (client.events & intent))
                send(connection, {{"op", 5},
                                  {"d", QJsonObject{{"eventType", type},
                                                    {"eventIntent", intent},
                                                    {"eventData", data}}}});
    }

    void nativeEvent() {
        if (!api_.obsMode())
            event("MunkiCastStateChanged", 512, api_.state());
    }

    void reply(SoupWebsocketConnection* connection, const Request& request, int code,
               const QString& comment = {}, const QJsonObject& data = {}) {
        const QJsonObject result{
            {"requestType", request.type},
            {"requestId", request.id},
            {"requestStatus",
             QJsonObject{{"result", code == 100}, {"code", code}, {"comment", comment}}},
            {"responseData", data}};
        auto found = clients_.find(connection);
        if (request.batch && found != clients_.end() && found->second.batch) {
            auto& batch = *found->second.batch;
            batch.results.append(result);
            batch.failed = code != 100;
            const auto serial = found->second.serial;
            QTimer::singleShot(0, this, [this, connection, serial] {
                const auto it = clients_.find(connection);
                if (it != clients_.end() && it->second.serial == serial)
                    nextBatch(connection);
            });
        } else
            send(connection, {{"op", 7}, {"d", result}});
    }

    void nextBatch(SoupWebsocketConnection* connection) {
        auto found = clients_.find(connection);
        if (found == clients_.end() || !found->second.batch)
            return;
        auto& batch = *found->second.batch;
        if (batch.next >= batch.requests.size() || (batch.halt && batch.failed)) {
            const QJsonObject result{{"requestId", batch.id}, {"results", batch.results}};
            found->second.batch.reset();
            send(connection, {{"op", 9}, {"d", result}});
            return;
        }
        const auto payload = batch.requests[batch.next++].toObject();
        dispatch(connection, {payload["requestId"].toString(), payload["requestType"].toString(),
                              payload["requestData"].toObject(), true});
    }

    bool active(const QString& family) const {
        return family == "Stream"   ? preview_.streaming()
               : family == "Record" ? preview_.recording()
                                    : preview_.replayActive();
    }

    QJsonObject status(const QString& family) const {
        const bool running = active(family);
        const auto ms = running && started_.contains(family)
                            ? QDateTime::currentMSecsSinceEpoch() - started_.at(family)
                            : 0;
        const auto stats = preview_.outputStats();
        const QString timecode = QString("%1:%2:%3.%4")
                                     .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
                                     .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
                                     .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
                                     .arg(ms % 1000, 3, 10, QLatin1Char('0'));
        return {{"outputActive", running},
                {"outputPaused", false},
                {"outputReconnecting", false},
                {"outputTimecode", timecode},
                {"outputDuration", static_cast<double>(ms)},
                {"outputCongestion", 0},
                {"outputBytes",
                 family == "ReplayBuffer" ? static_cast<double>(preview_.replayBytes()) : 0},
                {"outputSkippedFrames", stats["missed"].toDouble()},
                {"outputTotalFrames", stats["frames"].toDouble()}};
    }

    void outputChanged() {
        for (const auto& family : {QString("Stream"), QString("Record"), QString("ReplayBuffer")}) {
            auto& old = family == "Stream" ? stream_ : family == "Record" ? record_ : replay_;
            const bool now = active(family);
            if (old == now)
                continue;
            old = now;
            if (now)
                started_[family] = QDateTime::currentMSecsSinceEpoch();
            event(family + "StateChanged", 64,
                  {{"outputActive", now},
                   {"outputState",
                    now ? "OBS_WEBSOCKET_OUTPUT_STARTED" : "OBS_WEBSOCKET_OUTPUT_STOPPED"},
                   {"outputPath", family == "Record" ? preview_.recordingPath() : QString{}}});
        }
        for (auto& [connection, client] : clients_) {
            if (!client.pending || client.family == "Save" || client.family == "Sleep")
                continue;
            if (preview_.outputPending())
                continue;
            const auto request = *client.pending;
            const bool success = active(client.family) == client.wanted;
            client.pending.reset();
            reply(connection, request, success ? 100 : 604,
                  success ? QString{}
                          : QStringLiteral("Renderer could not change output state. Check the "
                                           "Encode graph and credentials."),
                  {{"outputActive", active(client.family)},
                   {"outputPath", preview_.recordingPath()}});
        }
        nativeEvent();
    }

    void dispatch(SoupWebsocketConnection* connection, Request request) {
        auto found = clients_.find(connection);
        if (found == clients_.end())
            return;
        auto& client = found->second;
        if (request.type.isEmpty() || request.type.size() > 256 ||
            (!request.batch && request.id.isEmpty()) || request.id.size() > 1024) {
            reply(connection, request, 400, "A request ID and type are required.");
            return;
        }
        if (!api_.availableRequests().contains(request.type)) {
            reply(connection, request, 204,
                  "Request is unavailable in this mode. Inspect GetVersion.availableRequests.");
            return;
        }
        auto type = request.type;
        auto data = request.data;
        if (type == "GetPersistentData" || type == "SetPersistentData") {
            if (!data["realm"].isString() || !data["slotName"].isString() ||
                data["slotName"].toString().isEmpty() ||
                data["slotName"].toString().toUtf8().size() > 256) {
                reply(connection, request, 402, "Supply realm and a slotName of 1–256 bytes.");
                return;
            }
            const auto realm = data["realm"].toString(), slot = data["slotName"].toString();
            const bool global = realm == "OBS_WEBSOCKET_DATA_REALM_GLOBAL";
            if (!global && realm != "OBS_WEBSOCKET_DATA_REALM_PROFILE") {
                reply(connection, request, 402, "Unknown persistent data realm.");
                return;
            }
            QJsonObject values;
            if (global) {
                QFile file(persistentPath_);
                if (file.exists()) {
                    QJsonParseError error;
                    if (!file.open(QIODevice::ReadOnly) || file.size() > 1048576) {
                        reply(connection, request, 604, "Persistent data cannot be read.");
                        return;
                    }
                    const auto parsed = QJsonDocument::fromJson(file.readAll(), &error);
                    if (error.error != QJsonParseError::NoError || !parsed.isObject()) {
                        reply(connection, request, 604,
                              "Persistent data is corrupt; file preserved.");
                        return;
                    }
                    values = parsed.object();
                }
            } else {
                const auto& extra = document_.projectForBindings().settings.extra;
                const auto it = extra.find("remote.persistent-data");
                if (it != extra.end()) {
                    const auto* json = std::get_if<std::string>(&it->second);
                    QJsonParseError error;
                    const auto parsed = QJsonDocument::fromJson(
                        json ? QByteArray::fromStdString(*json) : QByteArray{}, &error);
                    if (error.error != QJsonParseError::NoError || !parsed.isObject()) {
                        reply(connection, request, 604,
                              "Collection persistent data is invalid; data preserved.");
                        return;
                    }
                    values = parsed.object();
                }
            }
            if (type == "GetPersistentData") {
                reply(connection, request, 100, {},
                      {{"slotValue",
                        values.contains(slot) ? values[slot] : QJsonValue(QJsonValue::Null)}});
                return;
            }
            if (!data.contains("slotValue")) {
                reply(connection, request, 300, "slotValue is required.");
                return;
            }
            values[slot] = data["slotValue"];
            const auto bytes = QJsonDocument(values).toJson(QJsonDocument::Compact);
            if (bytes.size() > 1048576) {
                reply(connection, request, 402, "Persistent data exceeds 1 MiB.");
                return;
            }
            if (global) {
                QSaveFile file(persistentPath_);
                if (!file.open(QIODevice::WriteOnly) ||
                    !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                    file.write(bytes) != bytes.size() || !file.commit()) {
                    reply(connection, request, 604, "Persistent data could not be saved.");
                    return;
                }
            } else {
                auto settings = document_.projectForBindings().settings;
                settings.extra["remote.persistent-data"] = bytes.toStdString();
                const auto result = document_.remoteCommand(ldoc::SetProjectSettings{settings});
                if (!result) {
                    reply(connection, request, 402, text(result.error()));
                    return;
                }
            }
            reply(connection, request, 100);
            return;
        }
        if (type == "BroadcastCustomEvent") {
            if (!data["eventData"].isObject()) {
                reply(connection, request, 401, "eventData must be an object.");
                return;
            }
            event("CustomEvent", 512, data["eventData"].toObject());
            reply(connection, request, 100);
            return;
        }
        if (type == "Sleep") {
            if (!request.batch || !data["sleepMillis"].isDouble() ||
                data["sleepMillis"].toDouble() < 0 || data["sleepMillis"].toDouble() > 10000 ||
                client.pending) {
                reply(connection, request, 402,
                      "Sleep supports batches with sleepMillis 0–10000 only.");
                return;
            }
            client.pending = request;
            client.family = "Sleep";
            const auto serial = client.serial;
            QTimer::singleShot(data["sleepMillis"].toInt(), this, [this, connection, serial] {
                auto it = clients_.find(connection);
                if (it == clients_.end() || it->second.serial != serial || !it->second.pending ||
                    it->second.family != "Sleep")
                    return;
                const auto req = *it->second.pending;
                it->second.pending.reset();
                reply(connection, req, 100);
            });
            return;
        }
        if (type == "CallVendorRequest") {
            if (data["vendorName"] != "munkicast" || !data["requestType"].isString() ||
                !data["requestData"].isObject()) {
                reply(connection, request, 600, "Vendor request unavailable.");
                return;
            }
            // Native operations stay available to integrations through the standard OBS vendor
            // tunnel.
            RemoteControl native(document_, preview_, false, shortcuts_);
            const auto result =
                native.request(data["requestType"].toString(), data["requestData"].toObject());
            if (result.code != 100)
                reply(connection, request, result.code, result.comment);
            else
                reply(connection, request, 100, {},
                      {{"vendorName", "munkicast"},
                       {"requestType", data["requestType"]},
                       {"responseData", result.data}});
            return;
        }
        if (type == "GetLastReplayBufferReplay") {
            reply(connection, request, preview_.lastReplayPath().isEmpty() ? 604 : 100, {},
                  {{"savedReplayPath", preview_.lastReplayPath()}});
            return;
        }
        if (type == "GetVirtualCamStatus") {
            reply(connection, request, 100, {}, {{"outputActive", false}});
            return;
        }
        QString family;
        if (type.endsWith("Stream") || type == "GetStreamStatus")
            family = "Stream";
        else if (type.endsWith("Record") || type == "GetRecordStatus")
            family = "Record";
        else if (type.endsWith("ReplayBuffer") || type == "GetReplayBufferStatus")
            family = "ReplayBuffer";
        if (type.endsWith("Output") || type == "GetOutputStatus") {
            const auto& p = document_.projectForBindings();
            const auto it = std::ranges::find_if(p.outputs, [&](const auto& o) {
                return text(o.name) == data["outputName"].toString();
            });
            if (it == p.outputs.end()) {
                reply(connection, request, 600, "Output not found.");
                return;
            }
            family = it->destination.kind == ldoc::DestinationKind::Replay ? "ReplayBuffer"
                     : it->destination.kind == ldoc::DestinationKind::File ? "Record"
                                                                           : "Stream";
            if (family == "Stream" &&
                document_.streamingTarget()["outputId"].toString() != id(it->id)) {
                reply(connection, request, 604,
                      "Only the single armed streaming destination can be controlled.");
                return;
            }
            type.replace("Output", family);
        }
        if (!family.isEmpty() && (type.startsWith("Get") || type.startsWith("Start") ||
                                  type.startsWith("Stop") || type.startsWith("Toggle"))) {
            if (type.startsWith("Get")) {
                reply(connection, request, 100, {}, status(family));
                return;
            }
            if (client.pending || preview_.outputPending() || preview_.replaySaving()) {
                reply(connection, request, 604, "An output operation is pending.");
                return;
            }
            const bool wanted =
                type.startsWith("Toggle") ? !active(family) : type.startsWith("Start");
            if (active(family) == wanted) {
                reply(connection, request, wanted ? 500 : 501,
                      wanted ? "Output already active." : "Output not active.");
                return;
            }
            QString encode, path, output;
            if (family == "Record") {
                encode = document_.recordingEncodeId();
                path = document_.recordingPathFor(encode);
                if (encode.isEmpty() || (wanted && path.isEmpty())) {
                    reply(connection, request, 604,
                          "Connect a recording destination and encoder first.");
                    return;
                }
            } else if (family == "Stream" && wanted) {
                const auto target = document_.streamingTarget();
                output = target["outputId"].toString();
                if (output.isEmpty()) {
                    reply(connection, request, 604, target["reason"].toString());
                    return;
                }
            } else if (family == "ReplayBuffer" && wanted) {
                const auto key = document_.replayOutputId();
                if (key.isEmpty() || !document_.outputEncoderSettings(key)["enabled"].toBool()) {
                    reply(connection, request, 604,
                          "Connect and enable Replay Buffer in Backtrack Properties first.");
                    return;
                }
            }
            client.pending = request;
            client.family = family;
            client.wanted = wanted;
            client.deadline = QDateTime::currentMSecsSinceEpoch() + 30000;
            if (family == "Record") {
                if (wanted)
                    preview_.startRecording(encode, path);
                else
                    preview_.stopRecording(encode);
            } else if (family == "Stream") {
                if (wanted)
                    preview_.startStreaming(output);
                else
                    preview_.stopStreaming();
            } else {
                if (wanted)
                    preview_.startReplay();
                else
                    preview_.stopReplay();
            }
            outputChanged();
            return;
        }
        if (type == "SaveReplayBuffer") {
            if (client.pending || preview_.outputPending() || preview_.replaySaving() ||
                !preview_.replayActive() || !preview_.replayDurationMs()) {
                reply(connection, request, 604, "Replay is not ready or is already saving.");
                return;
            }
            const auto path = document_.replayPath();
            if (path.isEmpty()) {
                reply(connection, request, 604, "Connect the Replay encoder first.");
                return;
            }
            client.pending = request;
            client.family = "Save";
            client.savePath = path;
            client.deadline = QDateTime::currentMSecsSinceEpoch() + 60000;
            preview_.saveReplay(path);
            return;
        }
        auto result = api_.request(request.type, request.data);
        if (request.type == "GetStats") {
            result.data["webSocketSessionIncomingMessages"] = static_cast<double>(client.incoming);
            result.data["webSocketSessionOutgoingMessages"] = static_cast<double>(client.outgoing);
        }
        reply(connection, request, result.code, result.comment, result.data);
    }

    void receive(SoupWebsocketConnection* connection, SoupWebsocketDataType type, GBytes* bytes) {
        auto found = clients_.find(connection);
        if (found == clients_.end())
            return;
        auto& client = found->second;
        ++client.incoming;
        gsize size = 0;
        const auto* raw = static_cast<const char*>(g_bytes_get_data(bytes, &size));
        if (size > 1048576 || (binary(connection) ? type != SOUP_WEBSOCKET_DATA_BINARY
                                                  : type != SOUP_WEBSOCKET_DATA_TEXT)) {
            soup_websocket_connection_close(connection, 4002, "Invalid message");
            return;
        }
        QJsonObject message;
        if (binary(connection)) {
            msgpack_unpacked unpacked;
            msgpack_unpacked_init(&unpacked);
            std::size_t offset = 0;
            const auto result = msgpack_unpack_next(&unpacked, raw, size, &offset);
            auto value =
                (result == MSGPACK_UNPACK_SUCCESS && offset == size)
                    ? unpackValue(unpacked.data)
                    : std::expected<QJsonValue, QString>(std::unexpected("Invalid MessagePack"));
            if (value && value->isObject())
                message = value->toObject();
            msgpack_unpacked_destroy(&unpacked);
        } else {
            QJsonParseError parseError;
            const auto json =
                QJsonDocument::fromJson(QByteArray(raw, static_cast<qsizetype>(size)), &parseError);
            if (parseError.error == QJsonParseError::NoError && json.isObject())
                message = json.object();
        }
        if (!message["d"].isObject()) {
            soup_websocket_connection_close(connection, 4002, "Invalid message envelope");
            return;
        }
        const auto payload = message["d"].toObject();
        const int op = message["op"].toInt(-1);
        if (payload.contains("eventSubscriptions") &&
            (!payload["eventSubscriptions"].isDouble() ||
             payload["eventSubscriptions"].toDouble() < 0 ||
             payload["eventSubscriptions"].toDouble() > 2147483647 ||
             payload["eventSubscriptions"].toDouble() !=
                 std::trunc(payload["eventSubscriptions"].toDouble()))) {
            soup_websocket_connection_close(connection, 4002, "Invalid event subscription mask");
            return;
        }
        if (!client.identified) {
            if (op != 1) {
                soup_websocket_connection_close(connection, 4003, "Not identified");
                return;
            }
            if (payload.contains("rpcVersion") &&
                (!payload["rpcVersion"].isDouble() || payload["rpcVersion"].toDouble() != 1)) {
                soup_websocket_connection_close(connection, 4006, "Unsupported RPC version");
                return;
            }
            if (!authenticated(payload["authentication"].toString().toUtf8(), client.expected)) {
                soup_websocket_connection_close(connection, 4009, "Authentication failed");
                return;
            }
            client.expected.clear();
            client.identified = true;
            client.events = payload["eventSubscriptions"].toInt(2047);
            send(connection, {{"op", 2}, {"d", QJsonObject{{"negotiatedRpcVersion", 1}}}});
            return;
        }
        if (op == 1) {
            soup_websocket_connection_close(connection, 4008, "Already identified");
            return;
        }
        if (op == 3) {
            if (payload.contains("eventSubscriptions"))
                client.events = payload["eventSubscriptions"].toInt();
            return;
        }
        if (op == 8) {
            const auto list = payload["requests"].toArray();
            if (client.batch || list.isEmpty() || list.size() > 128 ||
                payload["requestId"].toString().isEmpty() ||
                (payload.contains("executionType") && (!payload["executionType"].isDouble() ||
                                                       payload["executionType"].toDouble() != 0)) ||
                (payload.contains("haltOnFailure") && !payload["haltOnFailure"].isBool())) {
                soup_websocket_connection_close(connection, 4007,
                                                "Use a batch ID, 1–128 requests, SerialRealtime "
                                                "execution, one batch at a time");
                return;
            }
            for (const auto v : list) {
                if (!v.isObject() || !v.toObject()["requestType"].isString() ||
                    (v.toObject().contains("requestData") &&
                     !v.toObject()["requestData"].isObject() &&
                     !v.toObject()["requestData"].isNull())) {
                    soup_websocket_connection_close(connection, 4007, "Invalid batch request");
                    return;
                }
            }
            client.batch = Batch{payload["requestId"].toString(),   list, {}, 0,
                                 payload["haltOnFailure"].toBool(), false};
            nextBatch(connection);
            return;
        }
        if (op != 6) {
            soup_websocket_connection_close(connection, 4005, "Unknown opcode");
            return;
        }
        if (payload.contains("requestData") && !payload["requestData"].isObject() &&
            !payload["requestData"].isNull()) {
            reply(connection,
                  {payload["requestId"].toString(), payload["requestType"].toString(), {}, false},
                  401, "requestData must be an object.");
            return;
        }
        dispatch(connection, {payload["requestId"].toString(), payload["requestType"].toString(),
                              payload["requestData"].toObject(), false});
    }

    void documentChanged() {
        const auto& current = document_.projectForBindings();
        const auto sceneData = [](const ldoc::Scene& s) {
            return QJsonObject{{"sceneName", text(s.name)},
                               {"sceneUuid", id(s.id)},
                               {"canvasUuid", id(s.canvas)},
                               {"isGroup", false}};
        };
        if (previous_.scenes != current.scenes)
            event("SceneListChanged", 4, {});
        for (const auto& old : previous_.scenes)
            if (!current.scene(old.id))
                event("SceneRemoved", 4, sceneData(old));
        for (const auto& s : current.scenes) {
            const auto* old = previous_.scene(s.id);
            if (!old)
                event("SceneCreated", 4, sceneData(s));
            else if (old->name != s.name) {
                auto data = sceneData(s);
                data["oldSceneName"] = text(old->name);
                event("SceneNameChanged", 4, data);
            }
            bool reordered = !old || s.roots != old->roots;
            QJsonArray order;
            const auto walk = [&](const auto& self, const std::vector<ldoc::Node>& roots) -> void {
                for (std::size_t i = 0; i < roots.size(); ++i) {
                    const auto& node = roots[i];
                    const auto* prior = old ? ldoc::find_node(old->roots, node.id) : nullptr;
                    auto data = sceneData(s);
                    data["sceneItemId"] = api_.itemId(node.id);
                    const auto row = api_.item(s, node, static_cast<int>(roots.size() - 1 - i));
                    data["sourceName"] = row["sourceName"];
                    data["sourceUuid"] = row["sourceUuid"];
                    data["sceneItemIndex"] = row["sceneItemIndex"];
                    if (!prior)
                        event("SceneItemCreated", 128, data);
                    if (!prior || prior->visible != node.visible) {
                        auto d = data;
                        d["sceneItemEnabled"] = node.visible;
                        event("SceneItemEnableStateChanged", 128, d);
                    }
                    if (!prior || prior->locked != node.locked) {
                        auto d = data;
                        d["sceneItemLocked"] = node.locked;
                        event("SceneItemLockStateChanged", 128, d);
                    }
                    if (prior &&
                        (prior->transform != node.transform || prior->layout != node.layout)) {
                        auto d = data;
                        d["sceneItemTransform"] = row["sceneItemTransform"];
                        event("SceneItemTransformChanged", 1 << 19, d);
                    }
                    if (!prior || prior->blend.mode != node.blend.mode) {
                        auto d = data;
                        d["sceneItemBlendMode"] = row["sceneItemBlendMode"];
                        event("SceneItemBlendModeChanged", 128, d);
                    }
                    order.append(QJsonObject{{"sceneItemId", api_.itemId(node.id)},
                                             {"sceneItemIndex", row["sceneItemIndex"]}});
                    if (const auto* children = node.children())
                        self(self, *children);
                }
            };
            walk(walk, s.roots);
            if (old)
                ldoc::visit(old->roots, [&](const ldoc::Node& n) {
                    if (!ldoc::find_node(s.roots, n.id)) {
                        auto data = sceneData(s);
                        data["sceneItemId"] = api_.itemId(n.id);
                        event("SceneItemRemoved", 128, data);
                    }
                });
            if (reordered) {
                auto data = sceneData(s);
                data["sceneItems"] = order;
                event("SceneItemListReindexed", 128, data);
            }
        }
        for (const auto& old : previous_.sources)
            if (!current.source(old.id))
                event("InputRemoved", 8,
                      {{"inputName", text(old.name)}, {"inputUuid", id(old.id)}});
        for (const auto& input : current.sources) {
            const auto* old = previous_.source(input.id);
            const QJsonObject identity{{"inputName", text(input.name)},
                                       {"inputUuid", id(input.id)}};
            if (!old) {
                auto data = identity;
                data["inputKind"] = text(input.type);
                data["unversionedInputKind"] = text(input.type);
                data["inputSettings"] =
                    api_.request("GetInputSettings", identity).data["inputSettings"];
                data["defaultInputSettings"] =
                    api_.request("GetInputDefaultSettings", {{"inputKind", text(input.type)}})
                        .data["defaultInputSettings"];
                event("InputCreated", 8, data);
            } else if (old->name != input.name) {
                auto data = identity;
                data["oldInputName"] = text(old->name);
                event("InputNameChanged", 8, data);
            }
            if (old && old->settings != input.settings) {
                auto data = identity;
                data["inputSettings"] =
                    api_.request("GetInputSettings", identity).data["inputSettings"];
                event("InputSettingsChanged", 8, data);
            }
            const auto audio = [&](const ldoc::Project& p) -> std::optional<ldoc::AudioRouting> {
                std::optional<ldoc::AudioRouting> value;
                for (const auto& s : p.scenes)
                    ldoc::visit(s.roots, [&](const ldoc::Node& n) {
                        const auto* binding = std::get_if<ldoc::SourceData>(&n.data);
                        if (!value && binding && binding->source == input.id)
                            value = binding->audio;
                    });
                return value;
            };
            const auto before = audio(previous_), after = audio(current);
            if (after) {
                if (!before || before->mute != after->mute) {
                    auto data = identity;
                    data["inputMuted"] = after->mute;
                    event("InputMuteStateChanged", 8, data);
                }
                if (!before || before->gain_db != after->gain_db) {
                    auto data = identity;
                    data["inputVolumeDb"] = after->gain_db;
                    data["inputVolumeMul"] = std::pow(10., after->gain_db / 20.);
                    event("InputVolumeChanged", 8, data);
                }
                if (!before || before->monitor != after->monitor) {
                    auto data = identity;
                    data["monitorType"] = after->monitor ? "OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT"
                                                         : "OBS_MONITORING_TYPE_NONE";
                    event("InputAudioMonitorTypeChanged", 8, data);
                }
            }
        }
        const auto filters = [&](const ldoc::Id& owner, const std::string& name,
                                 const std::vector<ldoc::Effect>& effects,
                                 const std::vector<ldoc::Effect>* before) {
            const QJsonObject identity{{"sourceName", text(name)}, {"sourceUuid", id(owner)}};
            if (before && *before == effects)
                return;
            QJsonArray order;
            for (std::size_t i = 0; i < effects.size(); ++i) {
                const auto& e = effects[i];
                const ldoc::Effect* prior = nullptr;
                if (before)
                    for (const auto& v : *before)
                        if (v.id == e.id)
                            prior = &v;
                auto data = identity;
                data["filterName"] = text(e.name.empty() ? e.type : e.name);
                data["filterIndex"] = static_cast<int>(i);
                order.append(QJsonObject{{"filterName", data["filterName"]},
                                         {"filterIndex", static_cast<int>(i)}});
                if (!prior) {
                    data["filterKind"] = text(e.type);
                    event("SourceFilterCreated", 32, data);
                } else {
                    if (prior->name != e.name) {
                        auto d = data;
                        d["oldFilterName"] = text(prior->name.empty() ? prior->type : prior->name);
                        event("SourceFilterNameChanged", 32, d);
                    }
                    if (prior->enabled != e.enabled) {
                        auto d = data;
                        d["filterEnabled"] = e.enabled;
                        event("SourceFilterEnableStateChanged", 32, d);
                    }
                }
            }
            if (before)
                for (const auto& e : *before)
                    if (std::ranges::none_of(effects,
                                             [&](const auto& v) { return v.id == e.id; })) {
                        auto data = identity;
                        data["filterName"] = text(e.name.empty() ? e.type : e.name);
                        event("SourceFilterRemoved", 32, data);
                    }
            auto data = identity;
            data["filters"] = order;
            event("SourceFilterListReindexed", 32, data);
        };
        for (const auto& input : current.sources) {
            const auto* old = previous_.source(input.id);
            filters(input.id, input.name, input.effects, old ? &old->effects : nullptr);
        }
        for (const auto& s : current.scenes) {
            const auto* old = previous_.scene(s.id);
            filters(s.id, s.name, s.effects, old ? &old->effects : nullptr);
        }
        for (const auto& deck : current.decks) {
            const auto priorDeck = std::ranges::find(previous_.decks, deck.id, &ldoc::Deck::id);
            const auto* old = priorDeck == previous_.decks.end() ? nullptr : &*priorDeck;
            const auto changedScene = [&](bool program) {
                const auto key = program ? deck.program_scene : deck.preview_scene;
                if (old && key == (program ? old->program_scene : old->preview_scene))
                    return;
                if (!key)
                    return;
                const auto* s = current.scene(*key);
                if (!s)
                    return;
                event(program ? "CurrentProgramSceneChanged" : "CurrentPreviewSceneChanged", 4,
                      sceneData(*s));
            };
            changedScene(true);
            changedScene(false);
            if (!current.canvases.empty() && deck.canvas == current.canvases.front().id) {
                if (!old || old->studio_mode != deck.studio_mode)
                    event("StudioModeStateChanged", 1024,
                          {{"studioModeEnabled", deck.studio_mode}});
                if (!old || old->transition != deck.transition)
                    event("CurrentSceneTransitionChanged", 16,
                          api_.request("GetCurrentSceneTransition", {}).data);
                if (!old || old->transition_ms != deck.transition_ms)
                    event("CurrentSceneTransitionDurationChanged", 16,
                          {{"transitionDuration", deck.transition_ms}});
            }
        }
        if (previous_.name != current.name)
            event("CurrentSceneCollectionChanged", 2,
                  {{"sceneCollectionName", text(current.name)}});
        previous_ = current;
        nativeEvent();
    }

  public:
    bool listening = false;

    ReplayWebsocket(Preview& preview, Document& document, const QByteArray& password, unsigned port,
                    bool obs, Shortcuts* shortcuts, const QString& persistentPath)
        : QObject(&preview), preview_(preview), document_(document),
          api_(document, preview, obs, shortcuts), password_(password),
          persistentPath_(persistentPath), shortcuts_(shortcuts),
          previous_(document.projectForBindings()), stream_(preview.streaming()),
          record_(preview.recording()), replay_(preview.replayActive()) {
        server_ = soup_server_new(nullptr, nullptr);
        char obsProtocol[] = "obswebsocket.json", nativeProtocol[] = "munkicastwebsocket.json",
             msgpackProtocol[] = "obswebsocket.msgpack";
        // Native mode retains the v5 envelope so clients can reuse mature OBS transports.
        char* protocols[] = {obsProtocol, msgpackProtocol, obs ? nullptr : nativeProtocol, nullptr};
        soup_server_add_websocket_handler(
            server_, "/", nullptr, protocols,
            +[](SoupServer*, SoupServerMessage*, const char*, SoupWebsocketConnection* connection,
                gpointer owner_data) {
                auto* self = static_cast<ReplayWebsocket*>(owner_data);
                if (self->clients_.size() >= 8) {
                    soup_websocket_connection_close(connection, 1013, "Connection limit reached");
                    return;
                }
                g_object_ref(connection);
                soup_websocket_connection_set_max_incoming_payload_size(connection, 1048576);
                const auto salt = QUuid::createUuid().toByteArray(QUuid::Id128),
                           challenge = QUuid::createUuid().toByteArray(QUuid::Id128);
                self->clients_[connection].expected =
                    hash(hash(self->password_ + salt) + challenge);
                const auto serial = self->clients_[connection].serial = ++self->nextClient_;
                g_signal_connect(connection, "message",
                                 G_CALLBACK(+[](SoupWebsocketConnection* c, int type, GBytes* bytes,
                                                gpointer data) {
                                     static_cast<ReplayWebsocket*>(data)->receive(
                                         c, static_cast<SoupWebsocketDataType>(type), bytes);
                                 }),
                                 self);
                g_signal_connect(connection, "closed",
                                 G_CALLBACK(+[](SoupWebsocketConnection* c, gpointer data) {
                                     auto* owner = static_cast<ReplayWebsocket*>(data);
                                     owner->clients_.erase(c);
                                     g_object_unref(c);
                                 }),
                                 self);
                self->send(
                    connection,
                    {{"op", 0},
                     {"d",
                      QJsonObject{{"obsWebSocketVersion", "5.6.3"},
                                  {"rpcVersion", 1},
                                  {"authentication",
                                   QJsonObject{{"salt", QString::fromLatin1(salt)},
                                               {"challenge", QString::fromLatin1(challenge)}}}}}});
                QTimer::singleShot(10000, self, [self, connection, serial] {
                    const auto found = self->clients_.find(connection);
                    if (found != self->clients_.end() && found->second.serial == serial &&
                        !found->second.identified)
                        soup_websocket_connection_close(connection, 4009,
                                                        "Authentication timed out");
                });
            },
            this, nullptr);
        GError* listenError = nullptr;
        listening =
            soup_server_listen_all(server_, port, SOUP_SERVER_LISTEN_IPV4_ONLY, &listenError);
        g_clear_error(&listenError);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            for (int i = 0; i < 16 && g_main_context_pending(nullptr); ++i)
                g_main_context_iteration(nullptr, false);
            const auto now = QDateTime::currentMSecsSinceEpoch();
            for (auto& [connection, client] : clients_)
                if (client.pending && client.family != "Sleep" && now >= client.deadline) {
                    const auto req = *client.pending;
                    client.pending.reset();
                    reply(connection, req, 604,
                          "Renderer operation timed out; check output status before retrying.");
                }
        });
        timer->start(10);
        connect(&document_, &Document::changed, this, [this] { documentChanged(); });
        connect(&preview_, &Preview::recordingChanged, this, [this] { outputChanged(); });
        connect(&preview_, &Preview::replaySaved, this,
                [this](const QString& path, const QString& error) {
                    for (auto& [connection, client] : clients_)
                        if (client.pending && client.family == "Save" &&
                            (path.isEmpty() || path == client.savePath)) {
                            const auto req = *client.pending;
                            client.pending.reset();
                            reply(connection, req, error.isEmpty() ? 100 : 205, error);
                        }
                    if (error.isEmpty())
                        event("ReplayBufferSaved", 64, {{"savedReplayPath", path}});
                    nativeEvent();
                });
    }

    ~ReplayWebsocket() override {
        for (auto& [connection, client] : clients_) {
            (void)client;
            g_signal_handlers_disconnect_by_data(connection, this);
            soup_websocket_connection_close(connection, 1001, "Studio closed");
            g_object_unref(connection);
        }
        soup_server_disconnect(server_);
        g_object_unref(server_);
    }
};
} // namespace

WebsocketServer* WebsocketServer::create(QQmlEngine*, QJSEngine*) {
    return new WebsocketServer(nullptr);
}

void WebsocketServer::initialize(Preview& preview, Document& document,
                                 const std::filesystem::path& path, bool useEnvironment,
                                 Shortcuts* shortcuts) {
    preview_ = &preview;
    document_ = &document;
    shortcuts_ = shortcuts;
    path_ = QString::fromStdString(path.string());
    QFile file(path_);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 8192) {
        const auto settings = QJsonDocument::fromJson(file.readAll()).object();
        enabled_ = settings.value("enabled").toBool();
        port_ = settings.value("port").toInt(4455);
        password_ = settings.value("password").toString();
        mode_ = settings.value("mode").toString("munkicast");
    }
    if (useEnvironment) {
        const auto password = qEnvironmentVariable("MUNKICAST_WEBSOCKET_PASSWORD");
        if (!password.isEmpty()) {
            enabled_ = true;
            password_ = password;
        }
        const auto port = qEnvironmentVariable("MUNKICAST_WEBSOCKET_PORT");
        if (!port.isEmpty())
            port_ = port.toInt();
    }
    if (useEnvironment && qEnvironmentVariableIsSet("MUNKICAST_WEBSOCKET_MODE"))
        mode_ = qEnvironmentVariable("MUNKICAST_WEBSOCKET_MODE");
    restart();
    emit changed();
}

void WebsocketServer::restart() {
    delete server_.data();
    server_ = nullptr;
    error_.clear();
    if (!enabled_)
        return;
    if (mode_ != "munkicast" && mode_ != "obs") {
        error_ = QStringLiteral("Mode must be munkicast or obs.");
        return;
    }
    if (port_ < 1024 || port_ > 65535 || password_.isEmpty() || password_.toUtf8().size() > 1024) {
        error_ = QStringLiteral("Use port 1024–65535 and a password of 1–1024 bytes.");
        return;
    }
    auto* server = new ReplayWebsocket(*preview_, *document_, password_.toUtf8(),
                                       static_cast<unsigned>(port_), mode_ == "obs", shortcuts_,
                                       QFileInfo(path_).absolutePath() + "/websocket-data.json");
    if (!server->listening) {
        delete server;
        error_ = QStringLiteral("Could not listen on 0.0.0.0:%1. Check whether the port is in use.")
                     .arg(port_);
        return;
    }
    server->setParent(this);
    server_ = server;
}

bool WebsocketServer::apply(bool enabled, int port, const QString& password) {
    return applyMode(enabled, port, password, mode_);
}

bool WebsocketServer::applyMode(bool enabled, int port, const QString& password,
                                const QString& mode) {
    error_.clear();
    if (mode != "munkicast" && mode != "obs") {
        error_ = QStringLiteral("Mode must be munkicast or obs.");
        emit changed();
        return false;
    }
    if (port < 1024 || port > 65535 || password.toUtf8().size() > 1024 ||
        (enabled && password.isEmpty())) {
        error_ = QStringLiteral("Use port 1024–65535 and a password of 1–1024 bytes.");
        emit changed();
        return false;
    }
    QSaveFile file(path_);
    const auto data = QJsonDocument(QJsonObject{{"enabled", enabled},
                                                {"port", port},
                                                {"password", password},
                                                {"mode", mode}})
                          .toJson();
    if (!preview_ || !QDir().mkpath(QFileInfo(path_).absolutePath()) ||
        !file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(data) != data.size() || !file.commit()) {
        error_ = QStringLiteral("Could not save WebSocket settings.");
        emit changed();
        return false;
    }
    enabled_ = enabled;
    port_ = port;
    password_ = password;
    mode_ = mode;
    restart();
    emit changed();
    return error_.isEmpty();
}
} // namespace munkicast::ui

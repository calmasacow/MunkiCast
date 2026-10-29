// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcpserver.hpp"

#include "document.hpp"
#include "ldoc/json.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJSValue>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace libertas::ui {
namespace {
constexpr auto maximumBody = 1024 * 1024;
const QStringList versions{"2025-11-25", "2025-06-18", "2025-03-26"};

[[noreturn]] void refuse(const QString& why) {
    throw std::runtime_error(why.toStdString());
}

QJsonObject schema(QMetaType type) {
    if (type.flags().testFlag(QMetaType::PointerToQObject))
        return {{"type", "string"}, {"description", "Live object ID from UI/native discovery"}};
    switch (type.id()) {
    case QMetaType::Bool:
        return {{"type", "boolean"}};
    case QMetaType::QString:
    case QMetaType::QByteArray:
    case QMetaType::QUrl:
    case QMetaType::QColor:
        return {{"type", "string"}};
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return {{"type", "integer"}};
    case QMetaType::Double:
    case QMetaType::Float:
        return {{"type", "number"}};
    case QMetaType::QVariantMap:
    case QMetaType::QJsonObject:
        return {{"type", "object"}};
    case QMetaType::QVariantList:
    case QMetaType::QJsonArray:
        return {{"type", "array"}, {"items", QJsonObject{}}};
    case QMetaType::QStringList:
        return {{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}};
    default:
        return {};
    }
}

bool argumentType(QMetaType type) {
    if (type.flags().testFlag(QMetaType::PointerToQObject))
        return false;
    return !schema(type).isEmpty() || type.id() == QMetaType::QVariant ||
           type.id() == qMetaTypeId<QJSValue>();
}

bool callable(const QMetaMethod& method, bool ui = false) {
    const QSet<QByteArray> actions{"clicked",  "triggered", "toggled",   "committed", "accepted",
                                   "rejected", "click",     "trigger",   "toggle",    "open",
                                   "close",    "popup",     "selectAll", "copy",      "paste",
                                   "cut",      "clear"};
    if (method.access() != QMetaMethod::Public || method.parameterCount() > 10 ||
        (method.methodType() != QMetaMethod::Method && !(ui && actions.contains(method.name()))))
        return false;
    for (int i = 0; i < method.parameterCount(); ++i)
        if (!argumentType(method.parameterMetaType(i)) &&
            !method.parameterMetaType(i).flags().testFlag(QMetaType::PointerToQObject))
            return false;
    const auto result = method.returnMetaType();
    return argumentType(result) || result.id() == QMetaType::Void ||
           result.flags().testFlag(QMetaType::PointerToQObject) ||
           result.id() == QMetaType::QSize || result.id() == QMetaType::QSizeF ||
           result.id() == QMetaType::QRectF || result.id() == QMetaType::QPointF;
}

QVariant convert(const QJsonValue& value, QMetaType type, QQmlEngine* engine) {
    const auto kind = schema(type)["type"].toString();
    if ((kind == "boolean" && !value.isBool()) || (kind == "string" && !value.isString()) ||
        ((kind == "integer" || kind == "number") && !value.isDouble()) ||
        (kind == "object" && !value.isObject()) || (kind == "array" && !value.isArray()))
        refuse("Argument type mismatch: expected " + kind);
    if (kind == "integer") {
        const auto number = value.toDouble();
        double low = -9007199254740991.0, high = 9007199254740991.0;
        if (type.id() == QMetaType::Int) {
            low = std::numeric_limits<int>::min();
            high = std::numeric_limits<int>::max();
        }
        if (type.id() == QMetaType::UInt) {
            low = 0;
            high = std::numeric_limits<unsigned>::max();
        }
        if (type.id() == QMetaType::ULongLong)
            low = 0;
        if (!std::isfinite(number) || number != std::trunc(number) || number < low || number > high)
            refuse("Integer outside exact supported range");
    }
    if (type.id() == QMetaType::QStringList)
        for (const auto entry : value.toArray())
            if (!entry.isString())
                refuse("String-list entries must be strings");
    QVariant result = value.toVariant();
    if (type.id() == qMetaTypeId<QJSValue>())
        return QVariant::fromValue(engine->toScriptValue(result));
    if (type.id() != QMetaType::QVariant && !result.convert(type))
        refuse("Argument cannot be converted to " + QString::fromUtf8(type.name()));
    return result;
}

QJsonObject textResult(const QJsonValue& value, bool error = false) {
    const auto bytes =
        value.isObject()  ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
        : value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                          : QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return {
        {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(bytes)}}}},
        {"isError", error}};
}

QJsonObject tool(const QString& name, const QString& description, const QJsonObject& fields = {},
                 const QJsonArray& required = {}) {
    return {{"name", name},
            {"description", description},
            {"inputSchema", QJsonObject{{"type", "object"},
                                        {"properties", fields},
                                        {"required", required},
                                        {"additionalProperties", false}}}};
}

QJsonObject stringField() {
    return {{"type", "string"}};
}

QJsonObject integerField(int minimum, int maximum) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

void response(QTcpSocket* socket, int status, const QByteArray& body = {}) {
    if (socket->state() != QAbstractSocket::ConnectedState)
        return;
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " " + (status < 300 ? "OK" : "Error") +
                  "\r\nConnection: close\r\n" +
                  (status == 401
                       ? QByteArray("WWW-Authenticate: Bearer realm=\"Libertas Studio MCP\"\r\n")
                       : QByteArray{}) +
                  "Content-Type: application/json\r\nCache-Control: no-store\r\n"
                  "X-Content-Type-Options: nosniff\r\nContent-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->disconnectFromHost();
}
} // namespace

McpServer::McpServer(QObject* parent) : QObject(parent) {
    connect(&listener_, &QTcpServer::newConnection, this, &McpServer::accept);
}

McpServer* McpServer::create(QQmlEngine*, QJSEngine*) {
    return new McpServer;
}

QString McpServer::endpoint() const {
    return QString("http://127.0.0.1:%1/mcp").arg(port_);
}

QString McpServer::newToken() const {
    QByteArray bytes;
    for (int i = 0; i < 8; ++i)
        bytes +=
            QByteArray::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');
    return QString::fromLatin1(bytes);
}

void McpServer::initialize(QQmlEngine& engine, Document& document,
                           const std::filesystem::path& path,
                           const QMap<QString, QObject*>& targets) {
    engine_ = &engine;
    document_ = &document;
    targets_ = targets;
    for (auto it = targets_.begin(); it != targets_.end();) {
        if (!it.value())
            it = targets_.erase(it);
        else
            ++it;
    }
    path_ = QString::fromStdString(path.string());
    QFile file(path_);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 8192) {
        const auto data = QJsonDocument::fromJson(file.readAll()).object();
        enabled_ = data["enabled"].toBool();
        port_ = data["port"].toInt(4466);
        token_ = data["token"].toString();
    }
    if (token_.isEmpty())
        token_ = newToken();
    tools();
    restart();
    emit changed();
}

bool McpServer::apply(bool enabled, int port, const QString& token) {
    error_.clear();
    if (port < 1024 || port > 65535 || token.size() < 32 || token.size() > 256 ||
        std::ranges::any_of(token,
                            [](QChar c) { return !c.isLetterOrNumber() || c.unicode() > 127; })) {
        error_ = "Use port 1024–65535 and a token of 32–256 ASCII letters/digits.";
        emit changed();
        return false;
    }
    QSaveFile file(path_);
    const auto bytes =
        QJsonDocument(QJsonObject{{"enabled", enabled}, {"port", port}, {"token", token}}).toJson();
    if (!engine_ || !QDir().mkpath(QFileInfo(path_).absolutePath()) ||
        !file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        error_ = "Could not save MCP settings.";
        emit changed();
        return false;
    }
    enabled_ = enabled;
    port_ = port;
    token_ = token;
    restart();
    emit changed();
    return error_.isEmpty();
}

void McpServer::restart() {
    listener_.close();
    const auto sockets = sockets_;
    for (const auto& socket : sockets)
        if (socket)
            socket->abort();
    sockets_.clear();
    error_.clear();
    if (!enabled_)
        return;
    if (port_ < 1024 || port_ > 65535 || token_.size() < 32) {
        error_ = "Invalid MCP configuration. Open Tools > MCP Server Settings.";
        return;
    }
    if (!listener_.listen(QHostAddress::LocalHost, static_cast<quint16>(port_)))
        error_ = "Could not start MCP: " + listener_.errorString();
}

void McpServer::accept() {
    while (auto* socket = listener_.nextPendingConnection()) {
        if (sockets_.size() >= 16) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        sockets_.append(socket);
        socket->setReadBufferSize(maximumBody + 16384);
        auto buffer = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            sockets_.removeAll(socket);
            socket->deleteLater();
        });
        QTimer::singleShot(15000, socket, [socket] { socket->abort(); });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, handled] {
            if (*handled)
                return;
            *buffer += socket->readAll();
            const auto end = buffer->indexOf("\r\n\r\n");
            if ((end < 0 && buffer->size() > 16384) || end > 16384 ||
                buffer->size() > maximumBody + 16384) {
                *handled = true;
                response(socket, 413);
                return;
            }
            if (end < 0)
                return;
            qint64 length = 0;
            bool haveLength = false;
            const auto lines = buffer->left(end).split('\n');
            for (const auto& line : lines) {
                const auto lower = line.trimmed().toLower();
                if (lower.startsWith("transfer-encoding:")) {
                    *handled = true;
                    response(socket, 400);
                    return;
                }
                if (lower.startsWith("content-length:")) {
                    bool valid = false;
                    length = lower.mid(15).trimmed().toLongLong(&valid);
                    if (haveLength || !valid || length < 0 || length > maximumBody) {
                        *handled = true;
                        response(socket, 413);
                        return;
                    }
                    haveLength = true;
                }
            }
            if (buffer->size() < end + 4 + length)
                return;
            *handled = true;
            http(socket, buffer->left(end + 4 + length));
        });
    }
}

void McpServer::http(QTcpSocket* socket, const QByteArray& bytes) {
    const auto end = bytes.indexOf("\r\n\r\n");
    const auto lines = bytes.left(end).split('\n');
    const auto request = lines.first().trimmed().split(' ');
    QMap<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const auto colon = lines[i].indexOf(':');
        if (colon <= 0) {
            response(socket, 400);
            return;
        }
        const auto key = lines[i].left(colon).trimmed().toLower();
        if (headers.contains(key)) {
            response(socket, 400);
            return;
        }
        headers[key] = lines[i].mid(colon + 1).trimmed();
    }
    const auto host = headers.value("host").toLower();
    if ((host != "127.0.0.1:" + QByteArray::number(port_) &&
         host != "localhost:" + QByteArray::number(port_)) ||
        headers.contains("origin")) {
        response(socket, 403);
        return;
    }
    const auto expected = "Bearer " + token_.toUtf8(), actual = headers.value("authorization");
    unsigned difference = static_cast<unsigned>(expected.size() ^ actual.size());
    for (qsizetype i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(expected[i] ^ (i < actual.size() ? actual[i] : 0));
    if (difference) {
        response(socket, 401);
        return;
    }
    if (request.size() != 3 || request[1] != "/mcp") {
        response(socket, 404);
        return;
    }
    if (request[0] != "POST") {
        response(socket, 405);
        return;
    }
    if (!headers.value("content-type").toLower().startsWith("application/json")) {
        response(socket, 415);
        return;
    }
    if (headers.contains("mcp-protocol-version") &&
        !versions.contains(QString::fromLatin1(headers["mcp-protocol-version"]))) {
        response(socket, 400);
        return;
    }
    QJsonParseError error;
    const auto data = QJsonDocument::fromJson(bytes.mid(end + 4), &error);
    if (error.error != QJsonParseError::NoError || !data.isObject()) {
        response(
            socket, 400,
            QJsonDocument(
                QJsonObject{{"jsonrpc", "2.0"},
                            {"id", QJsonValue::Null},
                            {"error", QJsonObject{{"code", -32700},
                                                  {"message", "Expected one JSON-RPC object"}}}})
                .toJson());
        return;
    }
    QPointer<QTcpSocket> guarded = socket;
    const auto result = rpc(data.object());
    if (!guarded)
        return;
    response(guarded, result.isEmpty() ? 202 : 200,
             result.isEmpty() ? QByteArray{}
                              : QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QString McpServer::objectId(QObject* value) {
    if (!value)
        return {};
    if (ids_.contains(value))
        return ids_.value(value);
    const auto id = "object_" + QString::number(++serial_);
    ids_[value] = id;
    objects_[id] = value;
    connect(value, &QObject::destroyed, this, [this, value, id] {
        ids_.remove(value);
        objects_.remove(id);
    });
    return id;
}

QObject* McpServer::object(const QString& id) const {
    if (targets_.contains(id))
        return targets_.value(id);
    const auto result = objects_.value(id);
    if (!result)
        refuse("Unknown or expired object ID; inspect the UI again");
    return result;
}

QJsonValue McpServer::json(const QVariant& value) {
    if (value.metaType().flags().testFlag(QMetaType::PointerToQObject)) {
        auto* target = value.value<QObject*>();
        if (!target)
            return QJsonValue::Null;
        return QJsonObject{{"objectId", objectId(target)},
                           {"class", target->metaObject()->className()}};
    }
    if (value.metaType().id() == qMetaTypeId<QJSValue>())
        return json(value.value<QJSValue>().toVariant());
    if (value.metaType().id() == QMetaType::QSize || value.metaType().id() == QMetaType::QSizeF) {
        const auto size = value.toSizeF();
        return QJsonObject{{"width", size.width()}, {"height", size.height()}};
    }
    if (value.metaType().id() == QMetaType::QPointF) {
        const auto p = value.toPointF();
        return QJsonObject{{"x", p.x()}, {"y", p.y()}};
    }
    if (value.metaType().id() == QMetaType::QRectF) {
        const auto r = value.toRectF();
        return QJsonObject{
            {"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}};
    }
    return QJsonValue::fromVariant(value);
}

QJsonObject McpServer::properties(QObject* target) {
    QJsonObject result;
    const auto* meta = target->metaObject();
    const bool sensitive = target->objectName().contains("token", Qt::CaseInsensitive) ||
                           target->objectName().contains("password", Qt::CaseInsensitive) ||
                           target->property("echoMode").toInt() != 0;
    for (int i = 0; i < meta->propertyCount(); ++i) {
        const auto p = meta->property(i);
        const QString name = QString::fromUtf8(p.name());
        if ((sensitive && (name == "text" || name == "value")) || !p.isReadable() ||
            name.contains("password", Qt::CaseInsensitive) ||
            name.contains("token", Qt::CaseInsensitive) ||
            name.contains("secret", Qt::CaseInsensitive))
            continue;
        if (argumentType(p.metaType()) ||
            p.metaType().flags().testFlag(QMetaType::PointerToQObject) || p.isEnumType())
            result[name] = json(p.read(target));
    }
    return result;
}

QJsonValue McpServer::invoke(QObject* target, const QMetaMethod& method, const QJsonArray& values) {
    if (values.size() != method.parameterCount())
        refuse("Wrong argument count");
    QVariant storage[10];
    QGenericArgument args[10];
    for (int i = 0; i < values.size(); ++i) {
        const auto type = method.parameterMetaType(i);
        if (type.flags().testFlag(QMetaType::PointerToQObject)) {
            if (!values[i].isString())
                refuse("Expected live object ID");
            storage[i] = QVariant::fromValue(object(values[i].toString()));
            if (!storage[i].convert(type))
                refuse("Object has the wrong application type");
        } else {
            storage[i] = convert(values[i], type, engine_);
        }
        args[i] = QGenericArgument(
            type.name(), type.id() == QMetaType::QVariant ? &storage[i] : storage[i].constData());
    }
    QVariant result(method.returnMetaType());
    QGenericReturnArgument ret;
    if (method.returnMetaType().id() != QMetaType::Void)
        ret = QGenericReturnArgument(
            method.typeName(),
            method.returnMetaType().id() == QMetaType::QVariant ? &result : result.data());
    QString error;
    const auto notice =
        connect(document_, &Document::notice, this, [&](const QString& why) { error = why; });
    const bool invoked =
        method.invoke(target, Qt::DirectConnection, ret, args[0], args[1], args[2], args[3],
                      args[4], args[5], args[6], args[7], args[8], args[9]);
    disconnect(notice);
    if (!invoked)
        refuse("Native invocation failed");
    if (!error.isEmpty())
        refuse(error);
    if (result.metaType().id() == qMetaTypeId<QJSValue>() && result.value<QJSValue>().isError())
        refuse(result.value<QJSValue>().toString());
    if (!method.isConst() && method.returnMetaType().id() == QMetaType::Bool && !result.toBool())
        refuse("Application refused the operation (returned false)");
    return json(result);
}

QList<QObject*> McpServer::uiObjects() const {
    QList<QObject*> queue;
    if (engine_)
        queue = qobject_cast<QQmlApplicationEngine*>(engine_.data())->rootObjects();
    for (auto* window : QGuiApplication::allWindows())
        queue.append(window);
    for (auto* widget : QApplication::topLevelWidgets())
        queue.append(widget);
    QSet<QObject*> visited;
    QList<QObject*> result;
    for (qsizetype i = 0; i < queue.size(); ++i) {
        auto* item = queue[i];
        if (!item || visited.contains(item))
            continue;
        visited.insert(item);
        result.append(item);
        queue.append(item->children());
        if (auto* quick = qobject_cast<QQuickItem*>(item))
            for (auto* child : quick->childItems())
                queue.append(child);
    }
    return result;
}

QJsonArray McpServer::tools() {
    nativeTools_.clear();
    QJsonArray result;
    for (auto it = targets_.begin(); it != targets_.end(); ++it) {
        const auto* meta = it.value()->metaObject();
        for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
            const auto method = meta->method(i);
            if (!callable(method))
                continue;
            QString name = it.key() + "_" + QString::fromUtf8(method.name());
            if (nativeTools_.contains(name))
                name += "_" + QString::number(method.parameterCount());
            if (nativeTools_.contains(name))
                name += "_" + QString::number(i);
            QJsonObject fields;
            QJsonArray required;
            for (int a = 0; a < method.parameterCount(); ++a) {
                auto arg = QString::fromUtf8(method.parameterNames()[a]);
                if (arg.isEmpty())
                    arg = "arg" + QString::number(a);
                fields[arg] = schema(method.parameterMetaType(a));
                required.append(arg);
            }
            auto entry = tool(
                name,
                "Native " + it.key() + "." + QString::fromUtf8(method.methodSignature()) +
                    ". Uses the same validated operation as the UI. Inspect application properties "
                    "after mutations; output/device work may finish asynchronously.",
                fields, required);
            entry["annotations"] = QJsonObject{{"readOnlyHint", method.isConst()},
                                               {"destructiveHint", !method.isConst()},
                                               {"openWorldHint", true}};
            result.append(entry);
            nativeTools_[name] = {it.value(), method};
        }
    }
    result.append(
        tool("app_properties",
             "Read a native target's properties, selection and model references. Targets: " +
                 targets_.keys().join(", "),
             {{"target", stringField()}}, {"target"}));
    result.append(tool("app_model",
                       "Read model rows and named roles from an objectId returned by a property or "
                       "native tool. Pagination is explicit.",
                       {{"objectId", stringField()},
                        {"offset", integerField(0, 1000000)},
                        {"limit", integerField(1, 500)}},
                       {"objectId"}));
    result.append(tool("app_project",
                       "Read complete native project, scene documents and assets, including "
                       "source settings, transforms, filters, routing and output graphs."));
    result.append(tool("app_command",
                       "Apply one serialized native document command through validation and undo. "
                       "Use app_command_templates to discover command shapes.",
                       {{"command", QJsonObject{{"type", "object"}}}}, {"command"}));
    result.append(tool("app_command_templates",
                       "Get native command templates and document schema. IDs and values in "
                       "templates are examples; use actual IDs from app_project."));
    result.append(tool("ui_tree",
                       "Inspect application windows, menus, dialogs and controls. IDs remain valid "
                       "until objects are destroyed. Search matches text, title, objectName and "
                       "class. Open menus/dialogs then inspect again for dynamic controls.",
                       {{"search", stringField()},
                        {"offset", integerField(0, 1000000)},
                        {"limit", integerField(1, 500)}},
                       {}));
    result.append(tool("ui_inspect",
                       "Read an object's properties and available UI method signatures. Inspect "
                       "before calling ui_invoke or ui_write.",
                       {{"objectId", stringField()}}, {"objectId"}));
    result.append(tool(
        "ui_invoke",
        "Invoke an advertised UI method or action signal by exact signature. Disabled controls "
        "refuse activation. Use real pointer/key input for control-specific event behavior.",
        {{"objectId", stringField()},
         {"method", stringField()},
         {"arguments", QJsonObject{{"type", "array"}, {"items", QJsonObject{}}}}},
        {"objectId", "method", "arguments"}));
    result.append(
        tool("ui_write",
             "Set one writable control property using its declared type. Prefer native tools for "
             "project edits. Does not implicitly emit clicked/committed/toggled; invoke the "
             "control's advertised action when required.",
             {{"objectId", stringField()}, {"property", stringField()}, {"value", QJsonObject{}}},
             {"objectId", "property", "value"}));
    result.append(tool("ui_screenshot",
                       "Capture an application window as PNG MCP image content. objectId is "
                       "optional; defaults to the main window.",
                       {{"objectId", stringField()}}));
    result.append(tool(
        "ui_pointer",
        "Send real mouse input to an application window or control. Coordinates are logical pixels "
        "relative to objectId. press/move/release support drags. No desktop/global input.",
        {{"objectId", stringField()},
         {"action", QJsonObject{{"type", "string"},
                                {"enum", QJsonArray{"click", "doubleClick", "press", "move",
                                                    "release", "wheel"}}}},
         {"x", QJsonObject{{"type", "number"}}},
         {"y", QJsonObject{{"type", "number"}}},
         {"button",
          QJsonObject{{"type", "string"}, {"enum", QJsonArray{"left", "right", "middle"}}}},
         {"wheelY", integerField(-12000, 12000)},
         {"modifiers", integerField(0, 0x1e000000)}},
        {"objectId", "action", "x", "y"}));
    result.append(tool("ui_key",
                       "Send one portable Qt key combination (e.g. Ctrl+A, Escape, Tab, Return) to "
                       "the focused control in an application window.",
                       {{"objectId", stringField()}, {"key", stringField()}}, {"objectId", "key"}));
    result.append(tool("ui_text",
                       "Insert text through the focused control's input method. Use ui_pointer or "
                       "ui_invoke(forceActiveFocus()) to focus first.",
                       {{"objectId", stringField()}, {"text", stringField()}},
                       {"objectId", "text"}));
    return result;
}

QJsonObject McpServer::call(const QString& name, const QJsonObject& args) {
    if (nativeTools_.contains(name)) {
        const auto [target, method] = nativeTools_.value(name);
        QJsonArray values;
        for (int i = 0; i < method.parameterCount(); ++i) {
            auto key = QString::fromUtf8(method.parameterNames()[i]);
            if (key.isEmpty())
                key = "arg" + QString::number(i);
            values.append(args[key]);
        }
        return textResult(QJsonObject{{"result", invoke(target, method, values)},
                                      {"revision", document_->revision()}});
    }
    if (name == "app_project") {
        const auto& p = document_->projectForBindings();
        auto project =
            QJsonDocument::fromJson(QByteArray::fromStdString(ldoc::project_to_json(p))).object();
        QJsonArray scenes;
        for (const auto& s : p.scenes)
            scenes.append(QJsonDocument::fromJson(QByteArray::fromStdString(ldoc::scene_to_json(s)))
                              .object());
        project["sceneDocuments"] = scenes;
        project["assets"] =
            QJsonDocument::fromJson(QByteArray::fromStdString(ldoc::assets_to_json(p.assets)))
                .object()["assets"];
        return textResult(project);
    }
    if (name == "app_command") {
        const auto command = ldoc::command_from_json(
            QJsonDocument(args["command"].toObject()).toJson().toStdString());
        if (!command)
            refuse(QString::fromStdString(command.error()));
        const auto result = document_->remoteCommand(*command);
        if (!result)
            refuse(QString::fromStdString(result.error()));
        return textResult(QJsonObject{{"revision", document_->revision()}});
    }
    if (name == "app_command_templates") {
        QJsonArray templates;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            const auto append = [&]<std::size_t N>() {
                using T = std::variant_alternative_t<N, ldoc::Command>;
                if constexpr (std::is_same_v<T, ldoc::Batch>)
                    templates.append(
                        QJsonDocument::fromJson(QByteArray::fromStdString(ldoc::command_to_json(
                                                    ldoc::make_batch({}, "MCP batch"))))
                            .object());
                else
                    templates.append(QJsonDocument::fromJson(
                                         QByteArray::fromStdString(ldoc::command_to_json(T{})))
                                         .object());
            };
            (append.template operator()<I>(), ...);
        }(std::make_index_sequence<std::variant_size_v<ldoc::Command>>{});
        return textResult(QJsonObject{{"templates", templates}});
    }
    if (name == "app_properties")
        return textResult(properties(object(args["target"].toString())));
    if (name == "app_model") {
        auto* model = qobject_cast<QAbstractItemModel*>(object(args["objectId"].toString()));
        if (!model)
            refuse("Object is not a model");
        const auto offset = args["offset"].toInt(), limit = args["limit"].toInt(100),
                   count = model->rowCount();
        QJsonArray rows;
        const auto roles = model->roleNames();
        for (int row = offset; row < std::min(count, offset + limit); ++row) {
            QJsonObject value;
            for (auto role = roles.begin(); role != roles.end(); ++role)
                value[QString::fromUtf8(role.value())] =
                    json(model->data(model->index(row, 0), role.key()));
            rows.append(value);
        }
        return textResult(QJsonObject{{"rows", rows},
                                      {"total", count},
                                      {"nextOffset", offset + rows.size() < count
                                                         ? QJsonValue(offset + rows.size())
                                                         : QJsonValue::Null}});
    }
    if (name == "ui_tree") {
        const auto search = args["search"].toString();
        const auto offset = args["offset"].toInt(), limit = args["limit"].toInt(200);
        QJsonArray entries;
        int count = 0;
        for (auto* target : uiObjects()) {
            QJsonObject entry{{"class", target->metaObject()->className()},
                              {"objectName", target->objectName()}};
            for (const auto* field : {"text", "title", "label", "visible", "enabled", "checked",
                                      "width", "height", "x", "y"}) {
                const auto value = target->property(field);
                if (value.isValid() && argumentType(value.metaType()))
                    entry[field] = json(value);
            }
            if (!search.isEmpty() && !QString::fromUtf8(QJsonDocument(entry).toJson())
                                          .contains(search, Qt::CaseInsensitive))
                continue;
            if (count++ < offset || entries.size() >= limit)
                continue;
            entry["objectId"] = objectId(target);
            entry["parentId"] = objectId(target->parent());
            if (auto* item = qobject_cast<QQuickItem*>(target)) {
                const auto point = item->mapToScene({});
                entry["windowId"] = objectId(item->window());
                entry["windowX"] = point.x();
                entry["windowY"] = point.y();
                entry["visualParentId"] = objectId(item->parentItem());
            }
            entries.append(entry);
        }
        return textResult(QJsonObject{{"objects", entries},
                                      {"total", count},
                                      {"nextOffset", offset + entries.size() < count
                                                         ? QJsonValue(offset + entries.size())
                                                         : QJsonValue::Null}});
    }
    auto* target = object(
        args["objectId"].toString().isEmpty() && name == "ui_screenshot"
            ? objectId(qobject_cast<QQmlApplicationEngine*>(engine_.data())->rootObjects().value(0))
            : args["objectId"].toString());
    if (name == "ui_inspect") {
        QJsonArray methods;
        const auto* meta = target->metaObject();
        for (int i = QObject::staticMetaObject.methodCount(); i < meta->methodCount(); ++i) {
            const auto method = meta->method(i);
            if (callable(method, true)) {
                QJsonArray parameters;
                for (int n = 0; n < method.parameterCount(); ++n)
                    parameters.append(
                        QJsonObject{{"name", QString::fromUtf8(method.parameterNames()[n])},
                                    {"type", QString::fromUtf8(method.parameterTypes()[n])}});
                methods.append(QJsonObject{{"method", QString::fromUtf8(method.methodSignature())},
                                           {"arguments", parameters}});
            }
        }
        QJsonArray writable;
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const auto p = meta->property(i);
            if (p.isWritable() && argumentType(p.metaType()))
                writable.append(QString::fromUtf8(p.name()));
        }
        return textResult(QJsonObject{
            {"properties", properties(target)}, {"methods", methods}, {"writable", writable}});
    }
    if (name == "ui_write") {
        const auto key = args["property"].toString().toUtf8();
        if (key == "objectName")
            refuse("Object identity cannot be changed remotely");
        const auto* meta = target->metaObject();
        const auto index = meta->indexOfProperty(key.constData());
        if (index < 0)
            refuse("Unknown property");
        const auto p = meta->property(index);
        if (!p.isWritable() || !argumentType(p.metaType()))
            refuse("Property is not writable from JSON");
        const auto value = convert(args["value"], p.metaType(), engine_);
        if (!p.write(target, value))
            refuse("Property write failed");
        return textResult(QJsonObject{{"written", true}});
    }
    if (name == "ui_invoke") {
        const auto* meta = target->metaObject();
        const auto signature = args["method"].toString().toUtf8();
        const int index = meta->indexOfMethod(signature.constData());
        if (index < QObject::staticMetaObject.methodCount() || !callable(meta->method(index), true))
            refuse("Method is not an advertised UI action");
        const auto enabled = target->property("enabled");
        if (enabled.isValid() && !enabled.toBool())
            refuse("Control is disabled");
        return textResult(QJsonObject{
            {"result", invoke(target, meta->method(index), args["arguments"].toArray())}});
    }
    auto* item = qobject_cast<QQuickItem*>(target);
    auto* widget = qobject_cast<QWidget*>(target);
    auto* window = qobject_cast<QWindow*>(target);
    if (item)
        window = item->window();
    if (name == "ui_screenshot") {
        QImage image;
        if (auto* quick = qobject_cast<QQuickWindow*>(window))
            image = quick->grabWindow();
        else if (widget)
            image = widget->grab().toImage();
        if (image.isNull())
            refuse("Window capture unavailable");
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG"))
            refuse("PNG encoding failed");
        return {
            {"content", QJsonArray{QJsonObject{{"type", "image"},
                                               {"mimeType", "image/png"},
                                               {"data", QString::fromLatin1(bytes.toBase64())}}}}};
    }
    if (!window && !widget)
        refuse("Input target must be an application control or window");
    if ((item && (!item->isEnabled() || !item->isVisible())) ||
        (widget && (!widget->isEnabled() || !widget->isVisible())))
        refuse("Input target is hidden or disabled");
    if (name == "ui_pointer") {
        QPointF position(args["x"].toDouble(), args["y"].toDouble());
        QPointer<QObject> receiver = widget ? static_cast<QObject*>(widget) : window;
        if (item)
            position = item->mapToScene(position);
        const auto global = widget ? widget->mapToGlobal(position.toPoint())
                                   : window->mapToGlobal(position.toPoint());
        const auto modifiers = Qt::KeyboardModifiers(args["modifiers"].toInt());
        const auto buttonName = args["button"].toString("left");
        const auto button = buttonName == "right"    ? Qt::RightButton
                            : buttonName == "middle" ? Qt::MiddleButton
                                                     : Qt::LeftButton;
        const auto action = args["action"].toString();
        const auto send = [&](QEvent::Type type, Qt::MouseButton b) {
            QMouseEvent event(type, position, position, global, b, buttons_, modifiers);
            if (receiver)
                QCoreApplication::sendEvent(receiver, &event);
        };
        if (action == "press" || action == "click" || action == "doubleClick") {
            buttons_ |= button;
            send(QEvent::MouseButtonPress, button);
        }
        if (action == "move")
            send(QEvent::MouseMove, Qt::NoButton);
        if (action == "doubleClick") {
            buttons_ &= ~button;
            send(QEvent::MouseButtonRelease, button);
            buttons_ |= button;
            send(QEvent::MouseButtonDblClick, button);
        }
        if (action == "release" || action == "click" || action == "doubleClick") {
            buttons_ &= ~button;
            send(QEvent::MouseButtonRelease, button);
        }
        if (action == "wheel") {
            QWheelEvent event(position, global, {}, {0, args["wheelY"].toInt()}, buttons_,
                              modifiers, Qt::NoScrollPhase, false);
            if (receiver)
                QCoreApplication::sendEvent(receiver, &event);
        }
        return textResult(QJsonObject{{"sent", true}});
    }
    QObject* focus =
        widget ? static_cast<QObject*>(widget->focusWidget() ? widget->focusWidget() : widget)
               : window->focusObject();
    if (!focus)
        focus = window;
    if (name == "ui_text") {
        QInputMethodEvent event;
        event.setCommitString(args["text"].toString());
        QCoreApplication::sendEvent(focus, &event);
        return textResult(QJsonObject{{"sent", true}});
    }
    if (name == "ui_key") {
        const auto sequence =
            QKeySequence::fromString(args["key"].toString(), QKeySequence::PortableText);
        if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown)
            refuse("Expected one portable key combination");
        const auto combination = sequence[0];
        QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
        QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers());
        QPointer<QObject> receiver = widget ? focus : static_cast<QObject*>(window);
        QCoreApplication::sendEvent(receiver, &press);
        if (receiver)
            QCoreApplication::sendEvent(receiver, &release);
        return textResult(QJsonObject{{"sent", true}});
    }
    refuse("Unknown tool");
}

QJsonObject McpServer::rpc(const QJsonObject& request) {
    const auto id = request.value("id");
    const auto method = request["method"].toString();
    const auto failure = [&](int code, const QString& message) {
        return QJsonObject{{"jsonrpc", "2.0"},
                           {"id", id.isUndefined() ? QJsonValue::Null : id},
                           {"error", QJsonObject{{"code", code}, {"message", message}}}};
    };
    if (request["jsonrpc"] != "2.0" || method.isEmpty() ||
        (!id.isUndefined() && !id.isString() && !id.isDouble()) ||
        (request.contains("params") && !request["params"].isObject()))
        return failure(-32600, "Invalid JSON-RPC request");
    // ponytail: stateless JSON responses; add per-client SSE/session state for subscriptions.
    if (id.isUndefined())
        return {}; // Notifications have no JSON-RPC response.
    const auto params = request["params"].toObject();
    QJsonObject result;
    try {
        if (method == "initialize") {
            const auto requested = params["protocolVersion"].toString();
            result = {
                {"protocolVersion", versions.contains(requested) ? requested : versions.first()},
                {"capabilities", QJsonObject{{"tools", QJsonObject{}},
                                             {"resources", QJsonObject{}},
                                             {"prompts", QJsonObject{}}}},
                {"serverInfo",
                 QJsonObject{{"name", "libertas-studio-native"}, {"version", "1.0.0"}}},
                {"instructions",
                 "Full native Libertas Studio application control. Independent of "
                 "OBS/WebSocket/Companion. Read studio://application, studio://project and "
                 "studio://coverage first. Native tools mirror the app's typed invokable methods, "
                 "including scene/source creation, transforms, filters, routing, outputs, "
                 "workspaces, preferences and shortcuts. UI tools inspect/control application "
                 "menus, dialogs and gestures. Prefer native tools for data edits. Discover IDs "
                 "and source settings schemas rather than guessing. Read state after mutations and "
                 "asynchronous renderer/device operations. Operations can change live outputs or "
                 "delete data; follow the user's authorization. External portal dialogs, "
                 "browser/account authorization and unavailable hardware may require the user. Do "
                 "not claim an untested feature succeeded."}};
        } else if (method == "ping")
            result = {};
        else if (method == "tools/list")
            result = {{"tools", tools()}};
        else if (method == "tools/call") {
            const auto name = params["name"].toString();
            QJsonObject definition;
            for (const auto entry : tools())
                if (entry.toObject()["name"] == name) {
                    definition = entry.toObject();
                    break;
                }
            if (definition.isEmpty())
                return failure(-32602, "Unknown tool");
            if (params.contains("arguments") && !params["arguments"].isObject())
                return failure(-32602, "Tool arguments must be an object");
            const auto args = params["arguments"].toObject();
            const auto input = definition["inputSchema"].toObject();
            const auto fields = input["properties"].toObject();
            for (const auto field : input["required"].toArray())
                if (!args.contains(field.toString()))
                    refuse("Missing argument: " + field.toString());
            for (auto it = args.begin(); it != args.end(); ++it) {
                if (!fields.contains(it.key()))
                    refuse("Unknown argument: " + it.key());
                const auto field = fields[it.key()].toObject();
                const auto type = field["type"].toString();
                const auto value = it.value();
                if ((type == "string" && !value.isString()) ||
                    (type == "boolean" && !value.isBool()) ||
                    ((type == "number" || type == "integer") && !value.isDouble()) ||
                    (type == "object" && !value.isObject()) ||
                    (type == "array" && !value.isArray()))
                    refuse("Wrong argument type: " + it.key());
                if (type == "integer" && value.toDouble() != std::trunc(value.toDouble()))
                    refuse("Expected integer: " + it.key());
                if ((field.contains("minimum") && value.toDouble() < field["minimum"].toDouble()) ||
                    (field.contains("maximum") && value.toDouble() > field["maximum"].toDouble()))
                    refuse("Argument outside range: " + it.key());
                if (field.contains("enum") && !field["enum"].toArray().contains(value))
                    refuse("Invalid choice: " + it.key());
            }
            try {
                result = call(name, args);
            } catch (const std::exception& error) {
                result = textResult(QJsonObject{{"error", QString::fromUtf8(error.what())}}, true);
            }
        } else if (method == "resources/list") {
            QJsonArray resources;
            for (const auto* name : {"application", "project", "coverage"})
                resources.append(QJsonObject{{"uri", "studio://" + QString(name)},
                                             {"name", QString(name)},
                                             {"mimeType", "application/json"}});
            result = {{"resources", resources}};
        } else if (method == "resources/templates/list")
            result = {{"resourceTemplates", QJsonArray{}}};
        else if (method == "resources/read") {
            const auto uri = params["uri"].toString();
            QJsonObject data;
            if (uri == "studio://application") {
                for (auto it = targets_.begin(); it != targets_.end(); ++it)
                    data[it.key()] = properties(it.value());
            } else if (uri == "studio://project") {
                const auto content =
                    call("app_project", {})["content"].toArray()[0].toObject()["text"].toString();
                data = QJsonDocument::fromJson(content.toUtf8()).object();
            } else if (uri == "studio://coverage") {
                QJsonArray skipped;
                for (auto it = targets_.begin(); it != targets_.end(); ++it) {
                    const auto* meta = it.value()->metaObject();
                    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
                        const auto m = meta->method(i);
                        if (m.methodType() == QMetaMethod::Method && !callable(m))
                            skipped.append(
                                QJsonObject{{"target", it.key()},
                                            {"method", QString::fromUtf8(m.methodSignature())},
                                            {"reason", "Not representable as a JSON invocation; "
                                                       "use the corresponding UI control/input."}});
                    }
                }
                data = {
                    {"transport",
                     "Native, authenticated, loopback Streamable HTTP; no WebSocket dependency"},
                    {"tools", tools()},
                    {"nonJsonMethods", skipped},
                    {"ui", "Application windows, control properties, action methods/signals, mouse "
                           "gestures, keyboard, text entry and PNG screenshots"},
                    {"limitations",
                     QJsonArray{
                         "Disabled/unimplemented app features cannot be manufactured by MCP.",
                         "External desktop portals and OAuth browser windows are outside this "
                         "application's UI tree. Native APIs accept file paths/settings directly; "
                         "account consent may require the user.",
                         "Device/backend availability follows runtime hardware capabilities.",
                         "Tool discovery is exhaustive for JSON-callable native methods; not every "
                         "feature/hardware combination has acceptance coverage."}}};
            } else
                return failure(-32602, "Unknown resource");
            result = {{"contents",
                       QJsonArray{QJsonObject{{"uri", uri},
                                              {"mimeType", "application/json"},
                                              {"text", QString::fromUtf8(QJsonDocument(data).toJson(
                                                           QJsonDocument::Compact))}}}}};
        } else if (method == "prompts/list")
            result = {
                {"prompts", QJsonArray{QJsonObject{
                                {"name", "studio-control"},
                                {"description",
                                 "Inspect and operate the native Libertas Studio application"}}}}};
        else if (method == "prompts/get") {
            if (params["name"] != "studio-control")
                return failure(-32602, "Unknown prompt");
            result = {
                {"messages",
                 QJsonArray{QJsonObject{
                     {"role", "user"},
                     {"content",
                      QJsonObject{
                          {"type", "text"},
                          {"text",
                           "Read studio://application, studio://project and studio://coverage. "
                           "Discover source types/settings and current IDs. Use native tools to "
                           "make my requested edits, ui_tree/ui_inspect and UI actions/input for "
                           "menus/dialogs. Verify the result through native state and screenshots. "
                           "Follow my authorization for on-air changes. Report actual failures or "
                           "external/hardware blockers; do not claim complete feature "
                           "validation."}}}}}}};
        } else
            return failure(-32601, "Method not found");
    } catch (const std::exception& error) {
        return failure(-32602, QString::fromUtf8(error.what()));
    }
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
} // namespace libertas::ui

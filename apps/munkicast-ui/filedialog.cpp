// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filedialog.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QUrl>
#include <cstdio>

namespace munkicast::ui {

struct FilterRule {
    quint32 kind = 0;
    QString pattern;
};

struct ChooserFilter {
    QString name;
    QList<FilterRule> rules;
};

QDBusArgument& operator<<(QDBusArgument& arg, const FilterRule& rule) {
    arg.beginStructure();
    arg << rule.kind << rule.pattern;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, FilterRule& rule) {
    arg.beginStructure();
    arg >> rule.kind >> rule.pattern;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const ChooserFilter& filter) {
    arg.beginStructure();
    arg << filter.name << filter.rules;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, ChooserFilter& filter) {
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}

} // namespace munkicast::ui

Q_DECLARE_METATYPE(munkicast::ui::FilterRule)
Q_DECLARE_METATYPE(munkicast::ui::ChooserFilter)

namespace munkicast::ui {
namespace {

constexpr auto kDesktop = "org.freedesktop.portal.Desktop";
constexpr auto kObject = "/org/freedesktop/portal/desktop";
constexpr auto kChooser = "org.freedesktop.portal.FileChooser";
constexpr auto kRequest = "org.freedesktop.portal.Request";
const auto kResponse = SLOT(portalResponse(uint, QVariantMap));

void register_filter_types() {
    static const bool once = [] {
        qDBusRegisterMetaType<FilterRule>();
        qDBusRegisterMetaType<ChooserFilter>();
        qDBusRegisterMetaType<QList<FilterRule>>();
        qDBusRegisterMetaType<QList<ChooserFilter>>();
        return true;
    }();
    (void)once;
}

void add_image_filters(QVariantMap& options) {
    register_filter_types();
    ChooserFilter images;
    images.name = QStringLiteral("Images");
    images.rules = {
        {1, QStringLiteral("image/png")},  {1, QStringLiteral("image/jpeg")},
        {1, QStringLiteral("image/webp")}, {0, QStringLiteral("*.png")},
        {0, QStringLiteral("*.jpg")},      {0, QStringLiteral("*.jpeg")},
        {0, QStringLiteral("*.webp")},     {0, QStringLiteral("*.PNG")},
        {0, QStringLiteral("*.JPG")},      {0, QStringLiteral("*.JPEG")},
        {0, QStringLiteral("*.WEBP")},
    };
    ChooserFilter all;
    all.name = QStringLiteral("All files");
    all.rules = {{0, QStringLiteral("*")}};
    options.insert(QStringLiteral("filters"),
                   QVariant::fromValue(QList<ChooserFilter>{images, all}));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(images));
}

// The sender is matched loosely. The portal emits Response from its unique name, and requiring
// the well-known name here drops that signal, which would leave the shell waiting forever.
bool subscribe(QDBusConnection& bus, const QString& request, FileDialog* dialog) {
    return bus.connect(QString(), request, QString::fromLatin1(kRequest),
                       QStringLiteral("Response"), QStringLiteral("ua{sv}"), dialog, kResponse);
}

void unsubscribe(QDBusConnection& bus, const QString& request, FileDialog* dialog) {
    bus.disconnect(QString(), request, QString::fromLatin1(kRequest), QStringLiteral("Response"),
                   QStringLiteral("ua{sv}"), dialog, kResponse);
}

QDBusMessage open_file(QDBusConnection& bus, const QString& title, const QVariantMap& options, bool save) {
    QDBusMessage message =
        QDBusMessage::createMethodCall(QString::fromLatin1(kDesktop), QString::fromLatin1(kObject),
                                       QString::fromLatin1(kChooser), save ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"));
    message << QString() << title << options;
    return bus.call(message);
}

} // namespace

void FileDialog::portalResponse(uint response, const QVariantMap& results) {
    response_ = response;
    results_ = results;
    got_ = true;
    if (loop_ != nullptr)
        loop_->quit();
}

QString FileDialog::pick(const QString& title, const QString& currentPath, bool imagesOnly) {
    return choose(title, currentPath, imagesOnly, false, false);
}

QString FileDialog::directory(const QString& title, const QString& currentPath) {
    return choose(title, currentPath, false, true, false);
}

QString FileDialog::saveFile(const QString& title, const QString& currentPath) {
    return choose(title, currentPath, false, false, true);
}

QString FileDialog::choose(const QString& title, const QString& currentPath, bool imagesOnly,
                           bool directory, bool save) {
    last_error_.clear();
    got_ = false;
    response_ = 2;
    results_.clear();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        last_error_ = QStringLiteral("The file chooser is not available.");
        return {};
    }

    // The portal names the request from this token, so the reply can be subscribed to before
    // OpenFile returns. Subscribing afterwards misses a chooser that closes immediately.
    const QString token = QStringLiteral("munkicast%1")
                              .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    const QString sender = bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    QString request =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    if (!subscribe(bus, request, this)) {
        last_error_ = QStringLiteral("The file chooser is not available.");
        return {};
    }

    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("modal"), true);
    if (directory) options.insert(QStringLiteral("directory"), true);
    if (save) options.insert(QStringLiteral("current_name"), QFileInfo(currentPath).fileName());
    if (imagesOnly)
        add_image_filters(options);
    if (!currentPath.isEmpty()) {
        const QFileInfo info(currentPath);
        const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
        if (!folder.isEmpty() && QFileInfo::exists(folder)) {
            QByteArray encoded = QFile::encodeName(folder);
            if (!encoded.endsWith('\0'))
                encoded.append('\0');
            options.insert(QStringLiteral("current_folder"), encoded);
        }
    }

    // An empty parent still opens the desktop's chooser. Naming the Wayland surface would need
    // an xdg-foreign handle, which this window does not export.
    QDBusMessage reply = open_file(bus, title, options, save);
    if (reply.type() != QDBusMessage::ReplyMessage && imagesOnly) {
        options.remove(QStringLiteral("filters"));
        options.remove(QStringLiteral("current_filter"));
        reply = open_file(bus, title, options, save);
    }
    if (reply.type() != QDBusMessage::ReplyMessage) {
        std::fprintf(stderr, "file chooser: %s\n", qPrintable(reply.errorMessage()));
        unsubscribe(bus, request, this);
        last_error_ = QStringLiteral("The file chooser did not open.");
        return {};
    }

    const QString returned = reply.arguments().value(0).toString();
    if (!returned.isEmpty() && returned != request) {
        unsubscribe(bus, request, this);
        request = returned;
        if (!subscribe(bus, request, this)) {
            last_error_ = QStringLiteral("The file chooser did not open.");
            return {};
        }
    }

    if (!got_) {
        QEventLoop loop;
        loop_ = &loop;
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        loop_ = nullptr;
    }
    unsubscribe(bus, request, this);

    // 0 is a choice, 1 is a cancel. Anything else is the portal giving up.
    if (response_ == 1)
        return {};
    if (response_ != 0) {
        last_error_ = QStringLiteral("The file chooser did not return a file.");
        return {};
    }
    const QStringList uris = results_.value(QStringLiteral("uris")).toStringList();
    if (uris.isEmpty())
        return {};
    const QUrl url(uris.first());
    if (!url.isLocalFile()) {
        last_error_ = QStringLiteral("The file chooser did not return a local file.");
        return {};
    }
    return url.toLocalFile();
}

} // namespace munkicast::ui

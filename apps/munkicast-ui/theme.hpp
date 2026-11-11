// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ltheme/palette.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace munkicast::ui {

/// Live Omarchy theme following.
///
/// `ltheme` does the reading; this class owns the watcher, because the right watcher depends on
/// the event loop and a library has no business assuming one.
///
/// Colours reach QML as a single map with camelCase keys, so a binding reads `theme.c.accent`.
/// One map means one notification, which means a theme switch repaints everything in one frame
/// instead of cascading through fifteen separate property changes.
class Theme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantMap c READ colors NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QString source READ source NOTIFY changed)

public:
    explicit Theme(QObject* parent = nullptr);

    static Theme* create(QQmlEngine*, QJSEngine*) { return new Theme; }

    [[nodiscard]] QVariantMap colors() const { return colors_; }
    [[nodiscard]] QString name() const { return name_; }
    [[nodiscard]] bool dark() const { return palette_.dark(); }
    [[nodiscard]] QString source() const { return QString::fromStdString(palette_.source()); }

    /// Re-read the theme now. Safe to call when there is no Omarchy on the machine.
    Q_INVOKABLE void reload();

signals:
    void changed();
    void notice(const QString& text);

private:
    void rewatch();
    void apply(const ltheme::Palette& p, QString name);

    ltheme::Palette palette_ = ltheme::Palette::fallback();
    QVariantMap colors_;
    QString name_ = QStringLiteral("built-in");

    QFileSystemWatcher watcher_;
    /// A theme switch rewrites several files at once; coalesce so we read the finished state.
    QTimer debounce_;
};

} // namespace munkicast::ui

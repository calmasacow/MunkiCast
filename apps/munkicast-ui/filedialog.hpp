// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace munkicast::ui {

/// Opens the desktop's file chooser.
///
/// The call is the XDG file-chooser portal. Which window appears is the desktop's choice, and
/// on this machine that choice is Omanta (`org.freedesktop.impl.portal.FileChooser=omanta`).
/// Qt's own dialog is not this one, and it is not a fallback: a portal that does not answer
/// is reported, rather than replaced with a chooser the operator already refused.
class FileDialog : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

  public:
    explicit FileDialog(QObject* parent = nullptr) : QObject(parent) {}

    /// A local path, or empty when the chooser is cancelled or does not answer.
    ///
    /// `imagesOnly` asks the chooser to start on PNG, JPEG, and WebP. It is a hint. The path is
    /// not applied anywhere; the caller keeps it until the operator accepts the settings.
    Q_INVOKABLE QString pick(const QString& title, const QString& currentPath, bool imagesOnly);

    Q_INVOKABLE QString directory(const QString& title, const QString& currentPath);
    Q_INVOKABLE QString saveFile(const QString& title, const QString& currentPath);

    /// Why `pick` returned empty, when it was not a cancel. Empty after a cancel.
    Q_INVOKABLE QString lastError() const { return last_error_; }

  private:
    QString choose(const QString& title, const QString& currentPath, bool imagesOnly,
                   bool directory, bool save);
    Q_SLOT void portalResponse(uint response, const QVariantMap& results);

    QString last_error_;
    bool got_ = false;
    quint32 response_ = 2;
    QVariantMap results_;
    QEventLoop* loop_ = nullptr;
};

} // namespace munkicast::ui

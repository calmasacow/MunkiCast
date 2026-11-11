// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace munkicast::ui {

/// Opens Qt's own colour dialog.
///
/// The dialog is the picker. Its custom-colour slots are shared by every dialog, so a colour
/// saved on one source is there when the dialog is opened on another. Qt keeps them for the
/// run of the program; this type also writes them down so they are still there next launch.
class ColourDialog : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

  public:
    explicit ColourDialog(QObject* parent = nullptr) : QObject(parent) {}

    /// The chosen `#rrggbb`, or empty when the dialog is cancelled.
    Q_INVOKABLE QString pick(const QString& current);

  private:
    bool restored_ = false;
};

} // namespace munkicast::ui

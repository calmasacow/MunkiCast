// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "colourdialog.hpp"

#include <QColor>
#include <QColorDialog>
#include <QSettings>

namespace munkicast::ui {
namespace {

constexpr auto kKey = "colourDialog/custom";

void restore_custom(QSettings& settings) {
    const QStringList saved = settings.value(QLatin1String(kKey)).toStringList();
    const int n = QColorDialog::customCount();
    for (int i = 0; i < n && i < saved.size(); ++i) {
        const QColor colour(saved.at(i));
        if (colour.isValid())
            QColorDialog::setCustomColor(i, colour);
    }
}

void store_custom(QSettings& settings) {
    QStringList saved;
    const int n = QColorDialog::customCount();
    saved.reserve(n);
    for (int i = 0; i < n; ++i)
        saved.push_back(QColorDialog::customColor(i).name(QColor::HexRgb));
    settings.setValue(QLatin1String(kKey), saved);
}

} // namespace

QString ColourDialog::pick(const QString& current) {
    QSettings settings;
    if (!restored_) {
        restore_custom(settings);
        restored_ = true;
    }

    QColor initial(current);
    if (!initial.isValid())
        initial = Qt::white;
    // Qt's own dialog, not the platform one: the custom-colour slots are the part that is
    // wanted, and a native dialog does not have to offer them.
    const QColor chosen = QColorDialog::getColor(initial, nullptr, QStringLiteral("Colour"),
                                                 QColorDialog::DontUseNativeDialog);
    store_custom(settings);
    if (!chosen.isValid())
        return {};
    return chosen.name(QColor::HexRgb);
}

} // namespace munkicast::ui

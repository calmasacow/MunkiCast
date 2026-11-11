// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>

#include <vector>

namespace munkicast::ui {

/// The shared half of every list model in the shell.
///
/// Same shape as the layout models, and for the same reason: reset only when the row count
/// changes, and otherwise emit dataChanged for the rows that actually moved, so a delegate
/// holding a text cursor or a hover state survives its neighbour being renamed.
///
/// Rows hold plain strings rather than ULIDs, because QML has no ULID type and because a model
/// that cannot be built without a document is a model that cannot be tested without one.
///
/// The shared part is a member template on a *non-template* base. moc takes the first base
/// class's staticMetaObject, so a templated base would leave it referring to one that does not
/// exist.
class RowModelBase : public QAbstractListModel {
    Q_OBJECT

public:
    using QAbstractListModel::QAbstractListModel;

protected:
    template <typename Row> void sync(std::vector<Row>& rows, std::vector<Row> next) {
        if (rows.size() != next.size()) {
            beginResetModel();
            rows = std::move(next);
            endResetModel();
            return;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i] == next[i])
                continue;
            rows[i] = next[i];
            const QModelIndex idx = index(static_cast<int>(i), 0);
            emit dataChanged(idx, idx);
        }
    }

    template <typename Row>
    [[nodiscard]] static const Row* at(const std::vector<Row>& rows, const QModelIndex& idx) {
        if (!idx.isValid() || idx.row() < 0 ||
            static_cast<std::size_t>(idx.row()) >= rows.size())
            return nullptr;
        return &rows[static_cast<std::size_t>(idx.row())];
    }
};

} // namespace munkicast::ui

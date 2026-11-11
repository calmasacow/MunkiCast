// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rowmodel.hpp"

#include <QString>
#include <QStringList>

#include <vector>

namespace munkicast::ui {

/// Panel groups, as rows with roles.
///
/// This started as a count plus an indexed accessor, with a revision counter touched inside each
/// delegate's binding to make it re-evaluate. It did not work: `Workspace.revision;` as a bare
/// statement has no effect, so it is exactly the kind of thing a compiler is entitled to delete,
/// and the QML compiler deleted it. The delegates were created once at a zero-size viewport and
/// never updated again -- a window of empty panels, with the dependency that was supposed to fix
/// it optimised out of existence. A real model does not depend on a side effect surviving.
///
/// `set_rows` resets only when the row *count* changes. When it does not -- which is every frame
/// of a divider drag -- it emits dataChanged for the rows that actually moved, so delegates stay
/// alive and only the changed properties re-evaluate.
class PanelModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        NodeRole = Qt::UserRole + 1,
        RectXRole,
        RectYRole,
        RectWRole,
        RectHRole,
        TabsRole,
        TitlesRole,
        ActiveRole,
        ActiveIdRole,
    };

    struct Row {
        int node = 0;
        double x = 0.0;
        double y = 0.0;
        double w = 0.0;
        double h = 0.0;
        QStringList tabs;
        QStringList titles;
        int active = 0;
        QString activeId;

        friend bool operator==(const Row&, const Row&) = default;
    };

    using QAbstractListModel::QAbstractListModel;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<Row> rows);

private:
    std::vector<Row> rows_;
};

/// Divider handles. Drawn, but not interactive -- see Main.qml for why the pointer grab cannot
/// live in these delegates.
class DividerModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        SplitRole = Qt::UserRole + 1,
        HandleIndexRole,
        HorizontalRole,
        RectXRole,
        RectYRole,
        RectWRole,
        RectHRole,
    };

    struct Row {
        int split = 0;
        int handle_index = 0;
        bool horizontal = true;
        double x = 0.0;
        double y = 0.0;
        double w = 0.0;
        double h = 0.0;

        friend bool operator==(const Row&, const Row&) = default;
    };

    using QAbstractListModel::QAbstractListModel;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<Row> rows);

private:
    std::vector<Row> rows_;
};

struct WorkspaceTabRow {
    QString name;
    /// Build and Live: renaming and removing are refused, resetting is not.
    bool builtin = false;
    bool active = false;

    friend bool operator==(const WorkspaceTabRow&, const WorkspaceTabRow&) = default;
};

class WorkspaceTabModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        BuiltinRole,
        ActiveRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<WorkspaceTabRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<WorkspaceTabRow>& rows() const { return rows_; }

private:
    std::vector<WorkspaceTabRow> rows_;
};

} // namespace munkicast::ui

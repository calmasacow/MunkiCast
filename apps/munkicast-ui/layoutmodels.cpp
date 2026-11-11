// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "layoutmodels.hpp"

#include <utility>

namespace munkicast::ui {
int PanelModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant PanelModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= rows_.size())
        return {};
    const Row& r = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case NodeRole:     return r.node;
    case RectXRole:    return r.x;
    case RectYRole:    return r.y;
    case RectWRole:    return r.w;
    case RectHRole:    return r.h;
    case TabsRole:     return r.tabs;
    case TitlesRole:   return r.titles;
    case ActiveRole:   return r.active;
    case ActiveIdRole: return r.activeId;
    default:           return {};
    }
}

QHash<int, QByteArray> PanelModel::roleNames() const {
    return {
        {NodeRole, "node"},     {RectXRole, "rectX"},     {RectYRole, "rectY"},
        {RectWRole, "rectW"},   {RectHRole, "rectH"},     {TabsRole, "tabs"},
        {TitlesRole, "titles"}, {ActiveRole, "activeTab"}, {ActiveIdRole, "activeId"},
    };
}

void PanelModel::set_rows(std::vector<Row> rows) {
    if (rows_.size() != rows.size()) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
        return;
    }
    // Same count: report only the rows that moved. This is every frame of a divider drag, and it
    // is why the delegates survive it.
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i] == rows[i])
            continue;
        rows_[i] = rows[i];
        const QModelIndex idx = index(static_cast<int>(i), 0);
        emit dataChanged(idx, idx);
    }
}

int DividerModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DividerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= rows_.size())
        return {};
    const Row& r = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case SplitRole:       return r.split;
    case HandleIndexRole: return r.handle_index;
    case HorizontalRole:  return r.horizontal;
    case RectXRole:       return r.x;
    case RectYRole:       return r.y;
    case RectWRole:       return r.w;
    case RectHRole:       return r.h;
    default:              return {};
    }
}

QHash<int, QByteArray> DividerModel::roleNames() const {
    return {
        {SplitRole, "split"},   {HandleIndexRole, "handleIndex"},
        {HorizontalRole, "horizontal"}, {RectXRole, "rectX"},
        {RectYRole, "rectY"},   {RectWRole, "rectW"},
        {RectHRole, "rectH"},
    };
}

void DividerModel::set_rows(std::vector<Row> rows) {
    if (rows_.size() != rows.size()) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
        return;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i] == rows[i])
            continue;
        rows_[i] = rows[i];
        const QModelIndex idx = index(static_cast<int>(i), 0);
        emit dataChanged(idx, idx);
    }
}

int WorkspaceTabModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant WorkspaceTabModel::data(const QModelIndex& index, int role) const {
    const WorkspaceTabRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case NameRole:    return r->name;
    case BuiltinRole: return r->builtin;
    case ActiveRole:  return r->active;
    default:          return {};
    }
}

QHash<int, QByteArray> WorkspaceTabModel::roleNames() const {
    return {
        {NameRole, "name"},
        {BuiltinRole, "builtin"},
        // Not "active": that would be fine, but "isActive" keeps it clear of the tab-group
        // sense of the word used elsewhere in the layout engine.
        {ActiveRole, "isActive"},
    };
}

} // namespace munkicast::ui

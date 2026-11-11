// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "documentmodels.hpp"

#include "lsession/actions.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace munkicast::ui {

class Document;
class Workspace;

struct BindingRow {
    QString ref;
    QString title;
    QString category;
    QString scope;
    QString trigger;
    bool isDefault = true;
    /// True when nothing in this build can perform it yet, so the table can say so instead of
    /// letting the operator bind a key that quietly does nothing.
    bool available = true;
    QString conflict;

    friend bool operator==(const BindingRow&, const BindingRow&) = default;
};

class BindingModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        RefRole = Qt::UserRole + 1,
        TitleRole,
        CategoryRole,
        ScopeRole,
        TriggerRole,
        IsDefaultRole,
        AvailableRole,
        ConflictRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<BindingRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<BindingRow>& rows() const { return rows_; }

private:
    std::vector<BindingRow> rows_;
};

/// Keyboard dispatch and the bindings table.
///
/// Application-scope actions are delivered here, by Qt, while the window has focus. Global ones
/// will be delivered by the portal to the session process (ADR-0024) -- they appear in this table
/// and can be bound, but nothing here listens for them, because listening for them in the
/// frontend is exactly the design that cannot survive the frontend dying.
class Shortcuts : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel* bindings READ bindingsModel CONSTANT)
    /// `{ref, trigger}` for every application action that has a key, which is what the shell
    /// instantiates `Shortcut` objects from.
    Q_PROPERTY(QVariantList applicationBindings READ applicationBindings NOTIFY changed)
    Q_PROPERTY(int unboundGlobalCount READ unboundGlobalCount NOTIFY changed)

public:
    ~Shortcuts() override;

    static void configure(std::filesystem::path bindings_file);
    static Shortcuts* create(QQmlEngine* engine, QJSEngine*);

    [[nodiscard]] QAbstractItemModel* bindingsModel() { return &bindings_model_; }
    [[nodiscard]] QVariantList applicationBindings() const;
    [[nodiscard]] int unboundGlobalCount() const;

    /// Perform an action by reference. Everything the keyboard can do goes through here, so the
    /// hotkey path and the menu path cannot drift apart.
    Q_INVOKABLE void perform(const QString& ref);

    Q_INVOKABLE void setTrigger(const QString& ref, const QString& trigger);
    Q_INVOKABLE void resetTrigger(const QString& ref);
    Q_INVOKABLE void resetAll();
    /// Turn a key event into the text form the table and `Shortcut` both use.
    Q_INVOKABLE [[nodiscard]] QString formatKey(int key, int modifiers) const;

signals:
    void changed();
    void notice(const QString& text);

private:
    /// Rebuild if, and only if, the set of bindable actions has changed.
    ///
    /// The parameterised ones -- one row per scene -- change whenever a scene is added or
    /// removed, and without this the table was frozen at whatever existed when the singleton was
    /// built: a new scene could not be given a hotkey until the application was restarted. The
    /// guard is because the document changes on nearly every keystroke and rebuilding the whole
    /// model each time would churn a table nobody is looking at.
    void refresh_if_actions_changed();

    /// Private so that `create` is the only way in.
    ///
    /// A QML singleton with a public default constructor gets default-constructed, and the
    /// `create` overload -- which is where the other singletons are wired up -- is never called.
    /// That failed quietly: the bindings table came up empty and nothing said why.
    explicit Shortcuts(QObject* parent = nullptr);

    void rebuild();
    void persist();

    lsession::Bindings bindings_;
    BindingModel bindings_model_;
    Document* document_ = nullptr;
    Workspace* workspace_ = nullptr;
    std::filesystem::path file_;
    std::vector<QString> known_actions_;
};

} // namespace munkicast::ui

// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "layoutmodels.hpp"

#include "llayout/history.hpp"
#include "llayout/layout.hpp"
#include "llayout/workspaces.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QPointF>
#include <QtQml/qqmlregistration.h>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVariantList>

#include <filesystem>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

/// The QML-facing face of the layout engine.
///
/// This class holds no layout logic. Every gesture is translated into a call on `llayout` and
/// every answer -- including whether a drop is even legal -- comes back from there, so the
/// behaviour the operator sees is the behaviour the headless tests cover.
///
/// Panels and dividers are exposed as a count plus an indexed accessor, with a `revision` counter
/// that ticks on every change, rather than as a list property. QML's Repeater rebuilds all its
/// delegates when a list model is replaced, which during a divider drag means destroying the item
/// holding the mouse grab sixty times a second. A stable count keeps the delegates alive and lets
/// the revision counter drive their bindings instead.
class Workspace : public QObject {
    Q_OBJECT
    // A QML singleton rather than a context property. Context properties are invisible to
    // qmllint and to the QML compiler, so every binding that touched one was unqualified and
    // unchecked -- a typo in a property name became a silent runtime warning instead of a build
    // failure. In the layer this whole project is betting on, that is the wrong trade.
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel* panels READ panelsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* dividers READ dividersModel CONSTANT)
    Q_PROPERTY(QVariantList browserDocks READ browserDocks NOTIFY browserDocksChanged)
    Q_PROPERTY(QString browserDockError READ browserDockError NOTIFY browserDockErrorChanged)
    Q_PROPERTY(int revision READ revision NOTIFY layoutChanged)
    Q_PROPERTY(int panelCount READ panelCount NOTIFY layoutChanged)
    Q_PROPERTY(int dividerCount READ dividerCount NOTIFY layoutChanged)
    Q_PROPERTY(QStringList closedPanels READ closedPanels NOTIFY layoutChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY layoutChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY layoutChanged)
    Q_PROPERTY(QString layoutSource READ layoutSource NOTIFY layoutChanged)

    // --- saved workspaces: Build, Live, and whatever the operator adds -----------------------
    //
    // Not compositor workspaces. These are saved arrangements of *this* surface (ADR-0015):
    // which panels are on screen and how the space is divided. Building a show and running one
    // want different things visible, and switching by hand every time is what leaves the wrong
    // one up when it matters.
    Q_PROPERTY(QAbstractItemModel* workspaces READ workspacesModel CONSTANT)
    Q_PROPERTY(int workspaceCount READ workspaceCount NOTIFY workspacesChanged)
    Q_PROPERTY(int activeWorkspace READ activeWorkspace NOTIFY workspacesChanged)
    Q_PROPERTY(QString encodeOutput READ encodeOutput NOTIFY encodeOutputChanged)
    Q_PROPERTY(QString activeWorkspaceName READ activeWorkspaceName NOTIFY workspacesChanged)

    /// The panel the keyboard acts on. ADR-0016's gesture is entirely pointer-driven, and an
    /// operator who cannot aim a pointer precisely could not rearrange the workspace at all.
    Q_PROPERTY(QString focusedPanel READ focusedPanel NOTIFY focusChanged)
    Q_PROPERTY(int focusedNode READ focusedNode NOTIFY focusChanged)

    Q_PROPERTY(bool dragging READ dragging NOTIFY dragChanged)
    Q_PROPERTY(QString dragPanel READ dragPanel NOTIFY dragChanged)
    Q_PROPERTY(QString dragTitle READ dragTitle NOTIFY dragChanged)
    Q_PROPERTY(QPointF dragPos READ dragPos NOTIFY dragChanged)
    /// The region the dragged panel would occupy. Empty when there is no legal drop here.
    Q_PROPERTY(QRectF dropPreview READ dropPreview NOTIFY dragChanged)
    /// The whole panel being hovered, for the five-zone wireframe. Empty for an edge drop.
    Q_PROPERTY(QRectF dropTargetRect READ dropTargetRect NOTIFY dragChanged)
    Q_PROPERTY(QRectF dropCentreRect READ dropCentreRect NOTIFY dragChanged)
    Q_PROPERTY(bool dropValid READ dropValid NOTIFY dragChanged)
    /// Plain words for what releasing here would do, or why it would not work.
    Q_PROPERTY(QString dropDescription READ dropDescription NOTIFY dragChanged)

public:
    explicit Workspace(std::filesystem::path config_path, QObject* parent = nullptr);
    ~Workspace() override;

    /// Configuration the singleton needs at construction, which QML cannot supply. Call before
    /// the engine loads any QML.
    static void configure(std::filesystem::path config_path);
    static Workspace* create(QQmlEngine*, QJSEngine*);

    [[nodiscard]] QAbstractItemModel* panelsModel() { return &panels_; }
    [[nodiscard]] QAbstractItemModel* dividersModel() { return &dividers_; }
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] int panelCount() const;
    [[nodiscard]] int dividerCount() const;
    [[nodiscard]] QStringList closedPanels() const;
    [[nodiscard]] bool canUndo() const { return history_.can_undo(); }
    [[nodiscard]] bool canRedo() const { return history_.can_redo(); }
    [[nodiscard]] QString layoutSource() const { return layout_source_; }

    [[nodiscard]] bool dragging() const { return drag_.has_value(); }
    [[nodiscard]] QString dragPanel() const;
    [[nodiscard]] QString dragTitle() const;
    [[nodiscard]] QPointF dragPos() const;
    [[nodiscard]] QRectF dropPreview() const;
    [[nodiscard]] QRectF dropTargetRect() const;
    [[nodiscard]] QRectF dropCentreRect() const;
    [[nodiscard]] bool dropValid() const;
    [[nodiscard]] QString dropDescription() const;

    /// {node, x, y, w, h, tabs, titles, active}
    Q_INVOKABLE [[nodiscard]] QVariantMap panelAt(int index) const;
    /// {split, index, horizontal, x, y, w, h}
    Q_INVOKABLE [[nodiscard]] QVariantMap dividerAt(int index) const;

    Q_INVOKABLE void setViewport(qreal w, qreal h);

    // --- panel drag (ADR-0016 rules 1, 2, 3, 4, 7) ---
    Q_INVOKABLE void beginDrag(const QString& panel);
    Q_INVOKABLE void updateDrag(qreal x, qreal y);
    /// Returns true if the drop changed the layout.
    Q_INVOKABLE bool endDrag();
    Q_INVOKABLE void cancelDrag();

    // --- dividers ---
    /// -1 when there is no divider under the point, else 0 for a vertical divider (drag it
    /// sideways) and 1 for a horizontal one.
    Q_INVOKABLE [[nodiscard]] int dividerAxisAtPoint(qreal x, qreal y) const;
    /// Index into dividerAt(), or -1. For the hover highlight, which cannot use a
    /// MouseArea of its own without stealing the grab the resize drag needs.
    Q_INVOKABLE [[nodiscard]] int dividerIndexAtPoint(qreal x, qreal y) const;
    Q_INVOKABLE bool beginDividerDrag(qreal x, qreal y);
    Q_INVOKABLE void dragDividerTo(qreal x, qreal y);
    Q_INVOKABLE void endDividerDrag();

    // --- keyboard navigation and docking ---
    //
    // ADR-0016's gesture is entirely pointer-driven, so without this an operator who cannot aim
    // a pointer precisely could not rearrange the workspace at all.

    [[nodiscard]] QString focusedPanel() const { return focused_; }
    [[nodiscard]] int focusedNode() const;

    Q_INVOKABLE void focusPanel(const QString& panel);
    /// "left", "right", "up" or "down".
    Q_INVOKABLE void focusNeighbour(const QString& direction);
    Q_INVOKABLE void focusNext(bool backwards);
    /// Move the focused panel one step in a direction, splitting whatever is already there.
    Q_INVOKABLE void movePanel(const QString& direction);
    /// Merge the focused panel into its neighbour as a tab.
    Q_INVOKABLE void tabPanelWithNeighbour(const QString& direction);
    /// Dock the focused panel against the whole workspace.
    Q_INVOKABLE void movePanelToEdge(const QString& direction);

    // --- panels ---
    Q_INVOKABLE void activate(const QString& panel);
    Q_INVOKABLE void closePanel(const QString& panel);
    Q_INVOKABLE void openPanel(const QString& panel);
    Q_INVOKABLE [[nodiscard]] QString title(const QString& panel) const;

    /// Tell the workspace which canvases exist, so it can keep a Scenes and a Sources panel
    /// per canvas and title them "Scenes: Vertical".
    ///
    /// Called by the shell whenever the document changes. Panels for a canvas are created the
    /// first time that canvas is seen and removed when it goes; a panel the operator closed
    /// afterwards stays closed, because "the canvas exists" and "the operator wants it on
    /// screen" are different questions and only the first one is being answered here.
    ///
    /// The first canvas is Main, and its two panels keep the plain ids "scenes" and
    /// "sources": it always exists, it cannot be removed and it cannot change places
    /// (ADR-0027), so it is the one canvas whose panel id can be a constant -- which is also
    /// what lets a layout written before any of this existed keep working.
    void syncCanvases(const QStringList& ids, const QStringList& names);

    /// Whether the active workspace currently holds this panel.
    Q_INVOKABLE [[nodiscard]] bool layoutHasPanel(const QString& panel) const;

    [[nodiscard]] QVariantList browserDocks() const { return browser_docks_; }
    [[nodiscard]] QString browserDockError() const { return browser_dock_error_; }
    Q_INVOKABLE bool saveBrowserDock(const QString& id, const QString& name, const QString& url);
    Q_INVOKABLE bool removeBrowserDock(const QString& id);
    Q_INVOKABLE [[nodiscard]] QString browserDockUrl(const QString& id) const;

    // --- saved workspaces ---
    [[nodiscard]] QAbstractItemModel* workspacesModel() { return &workspace_tabs_; }
    [[nodiscard]] int workspaceCount() const;
    [[nodiscard]] int activeWorkspace() const;
    [[nodiscard]] QString activeWorkspaceName() const;

    /// Switch to a saved arrangement. The one being left keeps the arrangement it had.
    [[nodiscard]] QString encodeOutput() const { return encode_output_; }
    Q_INVOKABLE void openEncode(const QString& outputId = QString{});
    Q_INVOKABLE void selectWorkspace(int index);
    /// Add one, starting from the arrangement currently on screen, which is almost always what
    /// someone adding one has just finished setting up.
    Q_INVOKABLE void addWorkspace(const QString& name);
    Q_INVOKABLE void removeWorkspace(int index);
    Q_INVOKABLE void renameWorkspace(int index, const QString& name);
    /// Back to how it shipped. The only thing that can be done to Build and Live besides
    /// rearranging them.
    Q_INVOKABLE void resetWorkspace(int index);
    Q_INVOKABLE [[nodiscard]] bool workspaceIsBuiltin(int index) const;

    // --- history and persistence ---
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void resetLayout();

    /// Read the saved arrangement back off disk, discarding what is on screen. The other half
    /// of Save Layout: without it, saving is a promise the operator cannot cash.
    Q_INVOKABLE void recallLayout();
    Q_INVOKABLE void saveNow();
    /// Same save, but it says so when it worked: Docks > Save Layout is an explicit action, and
    /// silence there cannot be told from a save that failed.
    Q_INVOKABLE void saveLayoutNow();

signals:
    void browserDocksChanged();
    void browserDockErrorChanged();
    void layoutChanged();
    void workspacesChanged();
    void encodeOutputChanged();
    void focusChanged();
    void dragChanged();
    /// Something the status line should say: a refused drop, a failed save, a fallback on load.
    void notice(const QString& text);

private:
    QVariantList browser_docks_;
    QString browser_dock_error_;
    [[nodiscard]] QString browser_docks_path() const;
    bool write_browser_docks(const QVariantList& docks);
    void prune_browser_docks();
    QString encode_output_;
    struct Drag {
        QString panel;
        QPointF pos;
        std::optional<llayout::DropTarget> target;
        QString refusal;
    };

    [[nodiscard]] const llayout::Layout& layout() const;
    void touch();
    /// Rebuild the QML-facing rows from the current layout and viewport.
    void refresh_models();
    /// Keep the focus on something that still exists.
    void validate_focus();
    [[nodiscard]] std::optional<llayout::Edge> direction_from(const QString& name) const;
    void scheduleSave();
    /// Copy the live layout back into the active workspace. Called before anything reads or
    /// writes the set, because `history_` is where edits actually land.
    void sync_active();
    void refresh_workspace_tabs();
    void recompute_drag_target();

    PanelModel panels_;
    DividerModel dividers_;

    /// Every saved arrangement. The active one's layout is mirrored into `history_` so that
    /// undo works on it; `sync_active()` writes it back before anything reads the set.
    llayout::Workspaces workspaces_;
    WorkspaceTabModel workspace_tabs_;

    llayout::History history_;
    /// Held only while a divider is being dragged, so the whole drag is one undo step.
    std::optional<llayout::Layout> working_;

    llayout::Viewport vp_{};
    llayout::Constraints constraints_ = llayout::Constraints::studio_defaults();

    std::optional<Drag> drag_;
    struct DividerDrag {
        llayout::NodeId split = llayout::kInvalidNode;
        std::size_t index = 0;
        llayout::Axis axis = llayout::Axis::Horizontal;
        QPointF last;
        llayout::Layout before;
    };
    std::optional<DividerDrag> divider_drag_;

    /// Which panel the keyboard acts on, and the one the UI draws a ring around.
    QString focused_;

    /// The canvases, in order, as the document last reported them. Index 0 is Main.
    QStringList canvas_ids_;
    QStringList canvas_names_;
    /// Canvases whose panels have been created once already. A canvas is given its pair of
    /// panels the first time it is seen and never again, so closing one stays closed.
    QSet<QString> canvases_seen_;
    /// Whether any canvas panels have been created yet. The batch made when a project is
    /// opened must not steal the tab: the operator is looking at Main. A canvas created
    /// afterwards should come forward, because they just made it.
    bool canvas_panels_built_ = false;

    std::filesystem::path config_path_;
    QString layout_source_;
    QTimer save_timer_;
    int revision_ = 0;
};

} // namespace libertas::ui

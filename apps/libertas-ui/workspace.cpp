// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "workspace.hpp"

#include "llayout/serialize.hpp"

#include <QVariantList>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <array>
#include <utility>

namespace libertas::ui {
namespace {

/// The panels the shell knows how to open. A panel not in this table still works -- the layout
/// engine only ever sees strings -- but it will not appear in the reopen list, which is the one
/// place a human-readable name is required.
struct KnownPanel {
    const char* id;
    const char* title;
};

constexpr std::array kKnownPanels = {
    KnownPanel{"program", "Program"},
    KnownPanel{"scenes", "Scenes"},
    KnownPanel{"sources", "Sources"},
    KnownPanel{"favourites", "Favourites"},
    KnownPanel{"properties", "Properties"},
    KnownPanel{"audio-mixer", "Audio Mixer"},
    KnownPanel{"controls", "Transitions"},
    KnownPanel{"countdowns", "Countdown Timers"},
    KnownPanel{"outputs", "Outputs"},
    KnownPanel{"canvas", "Canvas"},
    KnownPanel{"info", "Info"},
    KnownPanel{"broadcast-stats", "Broadcast Stats"},
    KnownPanel{"log", "Log"},
    KnownPanel{"shortcuts", "Shortcuts"},
};

QRectF to_qrect(const llayout::Rect& r) {
    return QRectF{r.x, r.y, r.w, r.h};
}

std::string to_std(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string{b.constData(), static_cast<std::size_t>(b.size())};
}

/// Set by configure() before the QML engine instantiates the singleton.
std::filesystem::path g_config_path;

} // namespace

void Workspace::configure(std::filesystem::path config_path) {
    g_config_path = std::move(config_path);
}

Workspace* Workspace::create(QQmlEngine*, QJSEngine*) {
    // The engine takes ownership of what it is handed here.
    return new Workspace{g_config_path};
}

Workspace::Workspace(std::filesystem::path config_path, QObject* parent)
    : QObject(parent), history_(llayout::Layout::default_studio()),
      config_path_(std::move(config_path)) {
    llayout::WorkspacesLoadOutcome outcome = llayout::load_workspaces_or_default(config_path_);
    workspaces_ = std::move(outcome.workspaces);
    history_.reset(workspaces_.active_layout());
    if (!outcome.complaint.empty()) {
        // ADR-0015 rule 5: fall back to the default *and say so*. A silent fallback is
        // indistinguishable from the bug it is covering for.
        layout_source_ = QStringLiteral("default workspaces (%1 could not be read: %2)")
                             .arg(QString::fromStdString(config_path_.string()),
                                  QString::fromStdString(outcome.complaint));
    } else if (outcome.used_default) {
        layout_source_ = QStringLiteral("default workspaces");
    } else {
        layout_source_ = QString::fromStdString(config_path_.string());
    }

    // Layout writes are debounced: a divider drag produces a change per mouse move and none of
    // the intermediate states are worth an fsync.
    save_timer_.setSingleShot(true);
    save_timer_.setInterval(600);
    connect(&save_timer_, &QTimer::timeout, this, &Workspace::saveNow);

    QFile docks_file(browser_docks_path());
    if (docks_file.exists()) {
        QJsonParseError error;
        const bool opened = docks_file.open(QIODevice::ReadOnly);
        const auto json = QJsonDocument::fromJson(opened ? docks_file.readAll() : QByteArray{}, &error);
        if (!opened || error.error != QJsonParseError::NoError || !json.isArray()) {
            browser_dock_error_ = QStringLiteral("Could not read custom browser docks: %1")
                                      .arg(opened ? error.errorString() : docks_file.errorString());
        } else {
            QSet<QString> ids;
            QSet<QString> names;
            for (const auto& value : json.array()) {
                const auto row = value.toObject().toVariantMap();
                const auto id = row.value("id").toString();
                const auto name = row.value("name").toString().trimmed();
                const QUrl url(row.value("url").toString(), QUrl::StrictMode);
                if (!id.startsWith(QLatin1String("browser:")) ||
                    QUuid(id.mid(8)).isNull() || name.isEmpty() || ids.contains(id) ||
                    names.contains(name.toCaseFolded()) || !url.isValid() || url.host().isEmpty() ||
                    (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
                    browser_dock_error_ = QStringLiteral("Custom browser docks contain an invalid entry.");
                    browser_docks_.clear();
                    break;
                }
                ids.insert(id);
                names.insert(name.toCaseFolded());
                browser_docks_.append(row);
            }
        }
    }
    // A removed dock must not return through an old saved layout.
    prune_browser_docks();
    refresh_models();
    refresh_workspace_tabs();
    validate_focus();
}

Workspace::~Workspace() {
    if (save_timer_.isActive())
        saveNow();
}

const llayout::Layout& Workspace::layout() const {
    return working_ ? *working_ : history_.current();
}

void Workspace::refresh_models() {
    const llayout::Layout& l = layout();

    std::vector<PanelModel::Row> panels;
    for (const llayout::LeafRect& lf : l.leaves(vp_, constraints_)) {
        const llayout::Node* n = l.find(lf.node);
        if (!n)
            continue;
        PanelModel::Row r;
        r.node = static_cast<int>(lf.node);
        r.x = lf.rect.x;
        r.y = lf.rect.y;
        r.w = lf.rect.w;
        r.h = lf.rect.h;
        for (const std::string& t : n->tabs) {
            r.tabs << QString::fromStdString(t);
            r.titles << title(QString::fromStdString(t));
        }
        r.active = static_cast<int>(n->active);
        r.activeId = n->active < n->tabs.size()
                         ? QString::fromStdString(n->tabs[n->active])
                         : QString{};
        panels.push_back(std::move(r));
    }
    panels_.set_rows(std::move(panels));

    std::vector<DividerModel::Row> dividers;
    for (const llayout::DividerHandle& d : l.dividers(vp_, constraints_)) {
        DividerModel::Row r;
        r.split = static_cast<int>(d.split);
        r.handle_index = static_cast<int>(d.index);
        r.horizontal = d.axis == llayout::Axis::Horizontal;
        r.x = d.rect.x;
        r.y = d.rect.y;
        r.w = d.rect.w;
        r.h = d.rect.h;
        dividers.push_back(r);
    }
    dividers_.set_rows(std::move(dividers));
}

void Workspace::touch() {
    prune_browser_docks();
    ++revision_;
    refresh_models();
    validate_focus();
    emit layoutChanged();
    scheduleSave();
}

void Workspace::validate_focus() {
    const llayout::Layout& l = layout();
    if (!focused_.isEmpty() && l.has_panel(to_std(focused_)))
        return;
    // Whatever was focused has been closed or merged away. Land on something real rather than
    // leaving the keyboard pointed at nothing.
    const auto panels = l.panels();
    const QString next = panels.empty() ? QString{} : QString::fromStdString(panels.front());
    if (next != focused_) {
        focused_ = next;
        emit focusChanged();
    }
}

int Workspace::focusedNode() const {
    return static_cast<int>(layout().node_of_panel(to_std(focused_)));
}

std::optional<llayout::Edge> Workspace::direction_from(const QString& name) const {
    if (name == QLatin1String("left"))
        return llayout::Edge::Left;
    if (name == QLatin1String("right"))
        return llayout::Edge::Right;
    if (name == QLatin1String("up"))
        return llayout::Edge::Top;
    if (name == QLatin1String("down"))
        return llayout::Edge::Bottom;
    return std::nullopt;
}

void Workspace::focusPanel(const QString& panel) {
    if (focused_ == panel || !layout().has_panel(to_std(panel)))
        return;
    focused_ = panel;
    emit focusChanged();
}

void Workspace::focusNeighbour(const QString& direction) {
    const auto edge = direction_from(direction);
    const llayout::NodeId from = layout().node_of_panel(to_std(focused_));
    if (!edge || from == llayout::kInvalidNode)
        return;
    const auto next = layout().neighbour(from, *edge, vp_, constraints_);
    if (!next) {
        emit notice(QStringLiteral("There is no panel to the %1.").arg(direction));
        return;
    }
    const llayout::Node* n = layout().find(*next);
    if (n && !n->tabs.empty())
        focusPanel(QString::fromStdString(n->tabs[n->active]));
}

void Workspace::focusNext(bool backwards) {
    const auto panels = layout().panels();
    if (panels.empty())
        return;
    const std::string current = to_std(focused_);
    auto it = std::find(panels.begin(), panels.end(), current);
    std::size_t index = it == panels.end() ? 0 : static_cast<std::size_t>(it - panels.begin());
    index = backwards ? (index + panels.size() - 1) % panels.size()
                      : (index + 1) % panels.size();
    focusPanel(QString::fromStdString(panels[index]));
}

void Workspace::movePanel(const QString& direction) {
    const auto edge = direction_from(direction);
    const llayout::NodeId from = layout().node_of_panel(to_std(focused_));
    if (!edge || from == llayout::kInvalidNode)
        return;
    const auto target = layout().neighbour(from, *edge, vp_, constraints_);
    if (!target) {
        emit notice(QStringLiteral("%1 is already at the %2 edge.")
                        .arg(title(focused_), direction));
        return;
    }

    // Docking into the neighbour's zone on the same side puts the panel past it, which is what
    // "move it left" means: one step, not a teleport to the workspace edge.
    const llayout::Zone zone = *edge == llayout::Edge::Left     ? llayout::Zone::Left
                               : *edge == llayout::Edge::Right  ? llayout::Zone::Right
                               : *edge == llayout::Edge::Top    ? llayout::Zone::Top
                                                                : llayout::Zone::Bottom;
    llayout::Layout next = history_.current();
    const llayout::DockResult r = next.dock(to_std(focused_), *target, zone, vp_, constraints_);
    if (r.status == llayout::DockStatus::Rejected) {
        emit notice(QString::fromStdString(r.reason));
        return;
    }
    if (history_.commit(std::move(next)))
        touch();
}

void Workspace::tabPanelWithNeighbour(const QString& direction) {
    const auto edge = direction_from(direction);
    const llayout::NodeId from = layout().node_of_panel(to_std(focused_));
    if (!edge || from == llayout::kInvalidNode)
        return;
    const auto target = layout().neighbour(from, *edge, vp_, constraints_);
    if (!target) {
        emit notice(QStringLiteral("There is no panel to the %1 to group with.").arg(direction));
        return;
    }
    llayout::Layout next = history_.current();
    const llayout::DockResult r =
        next.dock(to_std(focused_), *target, llayout::Zone::Center, vp_, constraints_);
    if (r.status == llayout::DockStatus::Rejected) {
        emit notice(QString::fromStdString(r.reason));
        return;
    }
    if (history_.commit(std::move(next)))
        touch();
}

void Workspace::movePanelToEdge(const QString& direction) {
    const auto edge = direction_from(direction);
    if (!edge || focused_.isEmpty())
        return;
    llayout::Layout next = history_.current();
    const llayout::DockResult r = next.dock_edge(to_std(focused_), *edge, vp_, constraints_);
    if (r.status == llayout::DockStatus::Rejected) {
        emit notice(QString::fromStdString(r.reason));
        return;
    }
    if (history_.commit(std::move(next)))
        touch();
}

void Workspace::scheduleSave() {
    save_timer_.start();
}

void Workspace::sync_active() {
    // history_ is where every drag, divider move and undo actually lands, so the set is stale
    // until this runs. Everything that reads or writes workspaces_ calls it first.
    static_cast<void>(workspaces_.set_layout(workspaces_.active(), history_.current()));
}

void Workspace::refresh_workspace_tabs() {
    std::vector<WorkspaceTabRow> rows;
    for (std::size_t i = 0; i < workspaces_.size(); ++i) {
        const llayout::Workspace& item = workspaces_.items()[i];
        rows.push_back(WorkspaceTabRow{QString::fromStdString(item.name), item.builtin,
                                       i == workspaces_.active()});
    }
    workspace_tabs_.set_rows(std::move(rows));
    emit workspacesChanged();
}

int Workspace::workspaceCount() const {
    return static_cast<int>(workspaces_.size());
}

int Workspace::activeWorkspace() const {
    return static_cast<int>(workspaces_.active());
}

QString Workspace::activeWorkspaceName() const {
    return QString::fromStdString(workspaces_.items()[workspaces_.active()].name);
}

bool Workspace::workspaceIsBuiltin(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= workspaces_.size())
        return false;
    return workspaces_.items()[static_cast<std::size_t>(index)].builtin;
}

void Workspace::openEncode(const QString& outputId) {
    encode_output_ = outputId;
    for (std::size_t i = 0; i < workspaces_.size(); ++i) {
        if (workspaces_.items()[i].name == "Encode") {
            selectWorkspace(static_cast<int>(i));
            emit encodeOutputChanged();
            return;
        }
    }
}

void Workspace::selectWorkspace(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= workspaces_.size())
        return;
    if (static_cast<std::size_t>(index) == workspaces_.active())
        return;
    // A drag or a divider drag in flight belongs to the arrangement being left.
    cancelDrag();
    endDividerDrag();
    sync_active();
    static_cast<void>(workspaces_.set_active(static_cast<std::size_t>(index)));
    // The undo stack does not follow. Switching workspaces is not a layout edit -- undoing
    // after a switch should undo something done *here*, not silently rearrange the one just
    // left, which is not even on screen to see it happen.
    history_.reset(workspaces_.active_layout());
    refresh_models();
    refresh_workspace_tabs();
    validate_focus();
    touch();
    scheduleSave();
}

void Workspace::addWorkspace(const QString& name) {
    sync_active();
    // Starting from what is on screen: someone adding a workspace has almost always just
    // finished arranging the thing they want to keep.
    const auto added = workspaces_.add(name.trimmed().toStdString(), history_.current());
    if (!added) {
        emit notice(QString::fromStdString(added.error()));
        return;
    }
    refresh_workspace_tabs();
    selectWorkspace(static_cast<int>(*added));
    scheduleSave();
}

void Workspace::removeWorkspace(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= workspaces_.size())
        return;
    sync_active();
    if (const auto r = workspaces_.remove(static_cast<std::size_t>(index)); !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    history_.reset(workspaces_.active_layout());
    refresh_models();
    refresh_workspace_tabs();
    validate_focus();
    touch();
    scheduleSave();
}

void Workspace::renameWorkspace(int index, const QString& name) {
    if (index < 0 || static_cast<std::size_t>(index) >= workspaces_.size())
        return;
    if (const auto r = workspaces_.rename(static_cast<std::size_t>(index),
                                          name.trimmed().toStdString());
        !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    refresh_workspace_tabs();
    scheduleSave();
}

void Workspace::resetWorkspace(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= workspaces_.size())
        return;
    sync_active();
    if (const auto r = workspaces_.reset(static_cast<std::size_t>(index)); !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    if (static_cast<std::size_t>(index) == workspaces_.active()) {
        history_.reset(workspaces_.active_layout());
        refresh_models();
        validate_focus();
        touch();
    }
    refresh_workspace_tabs();
    scheduleSave();
}

void Workspace::saveNow() {
    save_timer_.stop();
    sync_active();
    const auto r = llayout::save(workspaces_, config_path_);
    if (!r) {
        emit notice(QStringLiteral("Could not save the workspaces: %1")
                        .arg(QString::fromStdString(r.error())));
    }
}

void Workspace::saveLayoutNow() {
    save_timer_.stop();
    sync_active();
    // Named, because "Layout saved." leaves the operator wondering saved *where*, and because
    // the answer is the thing to check when an arrangement does not come back.
    if (const auto r = llayout::save(workspaces_, config_path_); r)
        emit notice(QStringLiteral("Layout saved to %1.")
                        .arg(QString::fromStdString(config_path_.string())));
    else
        emit notice(QStringLiteral("Could not save the workspaces: %1")
                        .arg(QString::fromStdString(r.error())));
}

// ---- exposure ------------------------------------------------------------------------------

int Workspace::panelCount() const {
    return static_cast<int>(layout().leaves(vp_, constraints_).size());
}

int Workspace::dividerCount() const {
    return static_cast<int>(layout().dividers(vp_, constraints_).size());
}

QVariantMap Workspace::panelAt(int index) const {
    QVariantMap m;
    const auto leaves = layout().leaves(vp_, constraints_);
    if (index < 0 || static_cast<std::size_t>(index) >= leaves.size())
        return m;
    const llayout::LeafRect& lf = leaves[static_cast<std::size_t>(index)];
    const llayout::Node* n = layout().find(lf.node);
    if (!n)
        return m;

    QStringList tabs;
    QStringList titles;
    for (const std::string& t : n->tabs) {
        tabs << QString::fromStdString(t);
        titles << title(QString::fromStdString(t));
    }
    m["node"] = static_cast<int>(lf.node);
    m["x"] = lf.rect.x;
    m["y"] = lf.rect.y;
    m["w"] = lf.rect.w;
    m["h"] = lf.rect.h;
    m["tabs"] = tabs;
    m["titles"] = titles;
    m["active"] = static_cast<int>(n->active);
    m["activeId"] = n->active < n->tabs.size()
                        ? QString::fromStdString(n->tabs[n->active])
                        : QString{};
    return m;
}

QVariantMap Workspace::dividerAt(int index) const {
    QVariantMap m;
    const auto ds = layout().dividers(vp_, constraints_);
    if (index < 0 || static_cast<std::size_t>(index) >= ds.size())
        return m;
    const llayout::DividerHandle& d = ds[static_cast<std::size_t>(index)];
    m["split"] = static_cast<int>(d.split);
    m["index"] = static_cast<int>(d.index);
    m["horizontal"] = d.axis == llayout::Axis::Horizontal;
    m["x"] = d.rect.x;
    m["y"] = d.rect.y;
    m["w"] = d.rect.w;
    m["h"] = d.rect.h;
    return m;
}

namespace {

/// The panels every canvas has one of: its picture, its scene list and its source tree.
/// Main's three keep the bare ids; the rest are "<kind>:<canvasId>".
bool is_per_canvas(const QString& kind) {
    return kind == QLatin1String("program") || kind == QLatin1String("scenes") ||
           kind == QLatin1String("sources");
}

} // namespace

QStringList Workspace::closedPanels() const {
    // The answer to "losing the main canvas, or having it pop out and go off screen completely":
    // every panel the shell knows about, including the program canvas, can always be brought
    // back, because it was never an OS window to lose.
    QStringList out;
    for (const KnownPanel& p : kKnownPanels) {
        if (!layout().has_panel(p.id))
            out << QString::fromLatin1(p.id);
    }
    // And the per-canvas panels. The first canvas's three are already in the table above under
    // their plain ids, so this starts at the second.
    for (int i = 1; i < canvas_ids_.size(); ++i) {
        for (const char* kind : {"program", "scenes", "sources"}) {
            const QString id = QString::fromLatin1(kind) + QLatin1Char(':') + canvas_ids_[i];
            if (!layout().has_panel(to_std(id)))
                out << id;
        }
    }
    for (const auto& dock : browser_docks_) {
        const auto id = dock.toMap().value("id").toString();
        if (!layout().has_panel(to_std(id)))
            out << id;
    }
    return out;
}

QString Workspace::title(const QString& panel) const {
    for (const auto& dock : browser_docks_) {
        const auto row = dock.toMap();
        if (row.value("id").toString() == panel)
            return row.value("name").toString();
    }
    // "Scenes: Vertical". Which canvas a scene list belongs to is the one thing a second
    // scene list has to say, and saying it in the tab is the only place it is always visible
    // -- a panel can be the only one in its group, with no neighbour to contrast against.
    const qsizetype colon = panel.indexOf(QLatin1Char(':'));
    const QString kind = colon < 0 ? panel : panel.left(colon);
    if (is_per_canvas(kind)) {
        const QString canvas = colon < 0
                                   ? (canvas_ids_.isEmpty() ? QString{} : canvas_ids_.front())
                                   : panel.mid(colon + 1);
        const qsizetype at = canvas_ids_.indexOf(canvas);
        const QString name = at >= 0 && at < canvas_names_.size() ? canvas_names_[at] : QString{};
        const QString base = kind == QLatin1String("program") ? QStringLiteral("Program")
                             : kind == QLatin1String("scenes") ? QStringLiteral("Scenes")
                                                               : QStringLiteral("Sources");
        // No canvases yet, or one this workspace has not been told about: the bare word is
        // still right, and is better than "Scenes: ".
        return name.isEmpty() ? base : base + QStringLiteral(": ") + name;
    }

    const std::string id = to_std(panel);
    for (const KnownPanel& p : kKnownPanels) {
        if (id == p.id)
            return QString::fromLatin1(p.title);
    }
    // An unknown id still gets something readable rather than nothing.
    QString t = panel;
    t.replace(QLatin1Char('-'), QLatin1Char(' '));
    if (!t.isEmpty())
        t[0] = t[0].toUpper();
    return t;
}

bool Workspace::layoutHasPanel(const QString& panel) const {
    return layout().has_panel(to_std(panel));
}

void Workspace::syncCanvases(const QStringList& ids, const QStringList& names) {
    const bool list_changed = ids != canvas_ids_ || names != canvas_names_;
    canvas_ids_ = ids;
    canvas_names_ = names;

    // Panels of canvases that are gone. Removed rather than left behind: a tab called
    // "Scenes: Vertical" for a canvas that no longer exists is a tab that can never show
    // anything, and the operator did not close it so would not think to.
    QSet<QString> alive;
    for (const QString& id : ids)
        alive.insert(id);

    llayout::Layout next = history_.current();
    bool touched = false;
    for (const std::string& panel : next.panels()) {
        const QString q = QString::fromStdString(panel);
        const qsizetype colon = q.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString kind = q.left(colon);
        if (!is_per_canvas(kind))
            continue;
        if (!alive.contains(q.mid(colon + 1)))
            touched = next.close_panel(panel) || touched;
    }

    // A canvas's panels are created once, the first time it is seen. After that the operator
    // owns whether they are on screen: re-adding one they closed would be the shell arguing
    // with them every time the document changed.
    //
    // Only once there is a viewport. The first sync happens before the window has a size --
    // the document is loaded before the QML is -- and docking into a zero-area layout is
    // refused. Marking the canvas seen at that point would mean its panels were never made.
    const bool have_viewport = vp_.bounds.w > 0.0 && vp_.bounds.h > 0.0;
    if (have_viewport) {
        for (int i = 1; i < ids.size(); ++i) {
            if (canvases_seen_.contains(ids[i]))
                continue;
            bool made_all = true;
            for (const char* kind : {"program", "scenes", "sources"}) {
                const std::string want =
                    (QString::fromLatin1(kind) + QLatin1Char(':') + ids[i]).toStdString();
                if (next.has_panel(want))
                    continue;
                // Beside Main's, as a tab. That is where the operator is already looking for
                // a scene list, and it is the arrangement they asked for.
                const llayout::NodeId beside = next.node_of_panel(kind);
                if (beside == llayout::kInvalidNode) {
                    // Main's own panel is closed, so there is nothing to sit beside. Not an
                    // error and not a reason to give up on this canvas for good.
                    made_all = false;
                    continue;
                }
                const llayout::DockResult r =
                    next.dock(want, beside, llayout::Zone::Center, vp_, constraints_);
                if (r.ok()) {
                    touched = true;
                    // Docking brings the new tab forward. Right when the operator has just
                    // made the canvas; wrong when a project is merely being opened, where
                    // they are looking at Main and did not ask for anything.
                    if (!canvas_panels_built_)
                        static_cast<void>(next.activate(kind));
                } else {
                    made_all = false;
                }
            }
            // Seen only if it actually got its panels. A refused dock is a thing to try again
            // next time, not a canvas to write off.
            if (made_all)
                canvases_seen_.insert(ids[i]);
        }
        if (!ids.isEmpty())
            canvases_seen_.insert(ids.front());
    }
    if (have_viewport)
        canvas_panels_built_ = true;
    // Forget canvases that have gone, so one that comes back by undo gets its panels again.
    canvases_seen_.intersect(alive);

    if (touched && history_.commit(std::move(next))) {
        touch();
        return;
    }
    if (list_changed) {
        // The titles changed even though the tree did not -- a canvas was renamed.
        ++revision_;
        refresh_models();
        emit layoutChanged();
    }
}

void Workspace::setViewport(qreal w, qreal h) {
    if (qFuzzyCompare(vp_.bounds.w, w) && qFuzzyCompare(vp_.bounds.h, h))
        return;
    const bool was_unsized = vp_.bounds.w <= 0.0 || vp_.bounds.h <= 0.0;
    vp_.bounds = llayout::Rect{0.0, 0.0, w, h};
    // The document is loaded before the window has a size, so the first canvas sync could not
    // dock anything -- docking into a zero-area layout is refused. A project opened with a
    // second canvas already in it would otherwise never get that canvas's panels, because
    // nothing changes afterwards to trigger another sync.
    if (was_unsized && w > 0.0 && h > 0.0 && !canvas_ids_.isEmpty())
        syncCanvases(canvas_ids_, canvas_names_);
    // A resize is not a layout change: the tree holds proportions, so nothing to save and nothing
    // to undo. Only the derived geometry moved.
    ++revision_;
    refresh_models();
    emit layoutChanged();
}

// ---- panel drag ----------------------------------------------------------------------------

QString Workspace::dragPanel() const {
    return drag_ ? drag_->panel : QString{};
}

QString Workspace::dragTitle() const {
    return drag_ ? title(drag_->panel) : QString{};
}

QPointF Workspace::dragPos() const {
    return drag_ ? drag_->pos : QPointF{};
}

QRectF Workspace::dropPreview() const {
    if (!drag_ || !drag_->target || !drag_->refusal.isEmpty())
        return {};
    return to_qrect(drag_->target->preview);
}

QRectF Workspace::dropTargetRect() const {
    if (!drag_ || !drag_->target || drag_->target->is_edge)
        return {};
    const auto r = layout().rect_of(drag_->target->node, vp_, constraints_);
    return r ? to_qrect(*r) : QRectF{};
}

QRectF Workspace::dropCentreRect() const {
    const QRectF t = dropTargetRect();
    if (t.isEmpty())
        return {};
    const llayout::Rect r{t.x(), t.y(), t.width(), t.height()};
    return to_qrect(llayout::centre_rect(r));
}

bool Workspace::dropValid() const {
    return drag_ && drag_->target && drag_->refusal.isEmpty();
}

QString Workspace::dropDescription() const {
    if (!drag_)
        return {};
    if (!drag_->refusal.isEmpty())
        return drag_->refusal;
    if (!drag_->target)
        return QStringLiteral("Release outside the workspace to cancel");

    const llayout::DropTarget& t = *drag_->target;
    if (t.is_edge) {
        return QStringLiteral("Dock %1 against the %2 edge of the workspace")
            .arg(dragTitle(), QString::fromLatin1(llayout::to_string(t.edge)));
    }
    const llayout::Node* n = layout().find(t.node);
    const QString over = (n && !n->tabs.empty())
                             ? title(QString::fromStdString(n->tabs[n->active]))
                             : QStringLiteral("panel");
    if (t.zone == llayout::Zone::Center)
        return QStringLiteral("Group %1 with %2 as tabs").arg(dragTitle(), over);
    return QStringLiteral("Dock %1 to the %2 of %3")
        .arg(dragTitle(), QString::fromLatin1(llayout::to_string(t.zone)), over);
}

void Workspace::beginDrag(const QString& panel) {
    if (panel.isEmpty())
        return;
    drag_ = Drag{panel, QPointF{}, std::nullopt, QString{}};
    emit dragChanged();
}

void Workspace::recompute_drag_target() {
    if (!drag_)
        return;
    drag_->target =
        layout().hit_test(drag_->pos.x(), drag_->pos.y(), vp_, constraints_);
    drag_->refusal.clear();
    if (!drag_->target)
        return;

    // Ask the engine what *would* happen, on a copy, so the highlight tells the truth before the
    // operator commits. ADR-0016 rule 8: a zone that would starve a panel does not light up.
    llayout::Layout probe = layout();
    const llayout::DockResult r =
        probe.dock(to_std(drag_->panel), *drag_->target, vp_, constraints_);
    if (r.status == llayout::DockStatus::Rejected)
        drag_->refusal = QString::fromStdString(r.reason);
}

void Workspace::updateDrag(qreal x, qreal y) {
    if (!drag_)
        return;
    drag_->pos = QPointF{x, y};
    recompute_drag_target();
    emit dragChanged();
}

bool Workspace::endDrag() {
    if (!drag_) {
        return false;
    }
    const Drag d = *drag_;
    drag_.reset();
    emit dragChanged();

    if (!d.target || !d.refusal.isEmpty()) {
        if (!d.refusal.isEmpty())
            emit notice(d.refusal);
        return false;
    }

    llayout::Layout next = history_.current();
    const llayout::DockResult r = next.dock(to_std(d.panel), *d.target, vp_, constraints_);
    if (r.status == llayout::DockStatus::Rejected) {
        emit notice(QString::fromStdString(r.reason));
        return false;
    }
    if (!history_.commit(std::move(next)))
        return false;
    touch();
    return true;
}

void Workspace::cancelDrag() {
    if (!drag_)
        return;
    drag_.reset();
    emit dragChanged();
}

// ---- dividers ------------------------------------------------------------------------------

int Workspace::dividerAxisAtPoint(qreal x, qreal y) const {
    const auto d = layout().hit_test_divider(x, y, vp_, constraints_);
    if (!d)
        return -1;
    return d->axis == llayout::Axis::Horizontal ? 0 : 1;
}

int Workspace::dividerIndexAtPoint(qreal x, qreal y) const {
    const auto hit = layout().hit_test_divider(x, y, vp_, constraints_);
    if (!hit)
        return -1;
    const auto ds = layout().dividers(vp_, constraints_);
    for (std::size_t i = 0; i < ds.size(); ++i) {
        if (ds[i].split == hit->split && ds[i].index == hit->index)
            return static_cast<int>(i);
    }
    return -1;
}

bool Workspace::beginDividerDrag(qreal x, qreal y) {
    const auto d = layout().hit_test_divider(x, y, vp_, constraints_);
    if (!d)
        return false;
    working_ = history_.current();
    divider_drag_ = DividerDrag{d->split, d->index, d->axis, QPointF{x, y}, history_.current()};
    return true;
}

void Workspace::dragDividerTo(qreal x, qreal y) {
    if (!divider_drag_ || !working_)
        return;
    const bool horizontal = divider_drag_->axis == llayout::Axis::Horizontal;
    const qreal delta = horizontal ? x - divider_drag_->last.x() : y - divider_drag_->last.y();
    if (qFuzzyIsNull(delta))
        return;

    if (working_->move_divider(divider_drag_->split, divider_drag_->index, delta, vp_,
                               constraints_)) {
        // Only advance the reference point when the divider actually moved. Otherwise pushing
        // past the clamp accumulates phantom travel and the divider refuses to come back until
        // the pointer has returned all the way past where it stopped.
        divider_drag_->last = QPointF{x, y};
        ++revision_;
        refresh_models();
        emit layoutChanged();
    }
}

void Workspace::endDividerDrag() {
    if (!divider_drag_ || !working_) {
        working_.reset();
        divider_drag_.reset();
        return;
    }
    llayout::Layout final_layout = std::move(*working_);
    working_.reset();
    divider_drag_.reset();

    // One undo step for the whole drag, not one per mouse move.
    if (history_.commit(std::move(final_layout))) {
        touch();
    } else {
        refresh_models();
        emit layoutChanged();
    }
}

// ---- panels --------------------------------------------------------------------------------

void Workspace::activate(const QString& panel) {
    llayout::Layout next = history_.current();
    if (!next.activate(to_std(panel)))
        return;
    const auto kind = panel.section(':', 0, 0);
    if (kind == QLatin1String("scenes") || kind == QLatin1String("sources")) {
        const auto paired = (kind == QLatin1String("scenes") ? QStringLiteral("sources")
                                                             : QStringLiteral("scenes")) +
                            panel.mid(kind.size());
        if (next.has_panel(to_std(paired)) &&
            next.node_of_panel(to_std(paired)) != next.node_of_panel(to_std(panel)))
            static_cast<void>(next.activate(to_std(paired)));
    }
    if (history_.commit(std::move(next)))
        touch();
}

void Workspace::closePanel(const QString& panel) {
    llayout::Layout next = history_.current();
    if (!next.close_panel(to_std(panel)))
        return;
    if (history_.commit(std::move(next)))
        touch();
}

void Workspace::openPanel(const QString& panel) {
    if ((panel.startsWith(QLatin1String("browser:")) && browserDockUrl(panel).isEmpty()) ||
        panel.isEmpty() || layout().has_panel(to_std(panel)))
        return;

    llayout::Layout next = history_.current();
    // Put it where there is most room, as a tab. Guessing a split would be a layout decision the
    // operator did not make; a tab in the largest group is the least destructive answer, and the
    // drag gesture is right there to move it.
    const auto leaves = next.leaves(vp_, constraints_);
    llayout::NodeId biggest = next.root();
    double best = -1.0;
    for (const llayout::LeafRect& lf : leaves) {
        const double area = lf.rect.w * lf.rect.h;
        if (area > best) {
            best = area;
            biggest = lf.node;
        }
    }
    const llayout::DockResult r =
        next.dock(to_std(panel), biggest, llayout::Zone::Center, vp_, constraints_);
    if (r.status == llayout::DockStatus::Rejected) {
        emit notice(QString::fromStdString(r.reason));
        return;
    }
    if (history_.commit(std::move(next)))
        touch();
}

QString Workspace::browser_docks_path() const {
    return QString::fromStdString(config_path_.string()) + QStringLiteral(".browser-docks.json");
}

QString Workspace::browserDockUrl(const QString& id) const {
    for (const auto& dock : browser_docks_) {
        const auto row = dock.toMap();
        if (row.value("id").toString() == id)
            return row.value("url").toString();
    }
    return {};
}

bool Workspace::write_browser_docks(const QVariantList& docks) {
    QSaveFile file(browser_docks_path());
    const auto bytes = QJsonDocument(QJsonArray::fromVariantList(docks)).toJson();
    if (!QDir().mkpath(QFileInfo(file).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        browser_dock_error_ = QStringLiteral("Could not save custom browser docks: %1")
                                  .arg(file.errorString());
        emit browserDockErrorChanged();
        return false;
    }
    browser_docks_ = docks;
    browser_dock_error_.clear();
    emit browserDockErrorChanged();
    emit browserDocksChanged();
    return true;
}

bool Workspace::saveBrowserDock(const QString& id, const QString& name, const QString& address) {
    const auto label = name.trimmed();
    const QUrl url(address.trimmed(), QUrl::StrictMode);
    QString error;
    if (label.isEmpty())
        error = QStringLiteral("Enter a dock name.");
    else if (!url.isValid() || url.host().isEmpty() ||
             (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")))
        error = QStringLiteral("Enter a complete http:// or https:// URL.");
    else if (!id.isEmpty() && browserDockUrl(id).isEmpty())
        error = QStringLiteral("That browser dock no longer exists.");
    for (const auto& dock : browser_docks_) {
        const auto row = dock.toMap();
        if (row.value("id").toString() != id &&
            row.value("name").toString().compare(label, Qt::CaseInsensitive) == 0)
            error = QStringLiteral("A browser dock already has that name.");
    }
    if (!error.isEmpty()) {
        browser_dock_error_ = error;
        emit browserDockErrorChanged();
        return false;
    }
    const QString key = id.isEmpty() ? QStringLiteral("browser:") +
                                          QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    const QVariantMap row{{"id", key}, {"name", label}, {"url", url.toString(QUrl::FullyEncoded)}};
    auto docks = browser_docks_;
    bool replaced = false;
    for (auto& dock : docks) {
        if (dock.toMap().value("id").toString() == key) {
            dock = row;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        docks.append(row);
    if (!write_browser_docks(docks))
        return false;
    touch();
    openPanel(key);
    activate(key);
    return true;
}

void Workspace::prune_browser_docks() {
    auto next = history_.current();
    bool changed = false;
    for (const auto& panel : next.panels()) {
        const auto id = QString::fromStdString(panel);
        if (id.startsWith(QLatin1String("browser:")) && browserDockUrl(id).isEmpty())
            changed = next.close_panel(panel) || changed;
    }
    if (changed)
        history_.reset(std::move(next));
}

bool Workspace::removeBrowserDock(const QString& id) {
    auto docks = browser_docks_;
    const auto removed = docks.removeIf([&id](const QVariant& dock) {
        return dock.toMap().value("id").toString() == id;
    });
    if (removed == 0 || !write_browser_docks(docks))
        return false;
    cancelDrag();
    endDividerDrag();
    sync_active();
    for (std::size_t i = 0; i < workspaces_.size(); ++i) {
        auto next = workspaces_.items()[i].layout;
        if (next.close_panel(to_std(id)))
            static_cast<void>(workspaces_.set_layout(i, std::move(next)));
    }
    history_.reset(workspaces_.active_layout());
    touch();
    return true;
}

// ---- history and persistence ---------------------------------------------------------------

void Workspace::undo() {
    if (history_.undo())
        touch();
}

void Workspace::redo() {
    if (history_.redo())
        touch();
}

void Workspace::resetLayout() {
    // The arrangement on screen, back to how that workspace shipped -- Build's default for
    // Build, Live's for Live. Committed through the history so it is one undo away.
    const std::size_t active = workspaces_.active();
    llayout::Workspaces probe = workspaces_;
    if (const auto r = probe.reset(active); !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    if (history_.commit(probe.items()[active].layout)) {
        layout_source_ = QStringLiteral("default layout");
        touch();
    }
}

void Workspace::recallLayout() {
    // Straight off disk, not from history: this exists for the case where what is on screen is
    // wrong and the saved file is right, so reading anything else would defeat the point.
    llayout::WorkspacesLoadOutcome outcome = llayout::load_workspaces_or_default(config_path_);
    if (!outcome.complaint.empty()) {
        emit notice(QStringLiteral("Could not read %1: %2")
                        .arg(QString::fromStdString(config_path_.string()),
                             QString::fromStdString(outcome.complaint)));
        return;
    }
    if (outcome.used_default) {
        emit notice(QStringLiteral("No saved layout at %1 yet.")
                        .arg(QString::fromStdString(config_path_.string())));
        return;
    }
    working_.reset();
    workspaces_ = std::move(outcome.workspaces);
    // Through the history, so a recall that turns out to be the wrong call is one Ctrl+Z away.
    history_.commit(workspaces_.active_layout());
    layout_source_ = QString::fromStdString(config_path_.string());
    refresh_workspace_tabs();
    touch();
    emit notice(QStringLiteral("Layout recalled."));
}

} // namespace libertas::ui

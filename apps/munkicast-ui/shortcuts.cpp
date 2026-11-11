// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shortcuts.hpp"

#include "document.hpp"
#include "preview.hpp"
#include "workspace.hpp"

#include "lcore/atomic_file.hpp"

#include <QKeySequence>
#include <QQmlEngine>
#include <QVariantMap>

namespace munkicast::ui {
namespace {

std::filesystem::path g_bindings_file;

QString qstr(const std::string& s) { return QString::fromStdString(s); }

/// Performed by the shell before `resolve`. `resolve` still says these are not available,
/// because they are not document commands; the table should not claim nothing does them.
bool shell_performs(const std::string& action) {
    if (action.starts_with("transform."))
        return true;
    return action == "output.save-replay" || action == "output.toggle-replay-buffer" || action == "edit.copy" || action == "edit.paste" || action == "edit.group" ||
           action == "edit.ungroup" || action == "edit.select-all" || action == "layer.delete" ||
           action == "layer.move-forward" || action == "layer.move-back" ||
           action == "layer.move-to-front" || action == "layer.move-to-back";
}

std::string to_std(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string{b.constData(), static_cast<std::size_t>(b.size())};
}

} // namespace

int BindingModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant BindingModel::data(const QModelIndex& index, int role) const {
    const BindingRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case RefRole:       return r->ref;
    case TitleRole:     return r->title;
    case CategoryRole:  return r->category;
    case ScopeRole:     return r->scope;
    case TriggerRole:   return r->trigger;
    case IsDefaultRole: return r->isDefault;
    case AvailableRole: return r->available;
    case ConflictRole:  return r->conflict;
    default:            return {};
    }
}

QHash<int, QByteArray> BindingModel::roleNames() const {
    return {
        {RefRole, "ref"},           {TitleRole, "title"},
        {CategoryRole, "category"}, {ScopeRole, "scope"},
        {TriggerRole, "trigger"},   {IsDefaultRole, "isDefault"},
        {AvailableRole, "available"}, {ConflictRole, "conflict"},
    };
}

void Shortcuts::configure(std::filesystem::path bindings_file) {
    g_bindings_file = std::move(bindings_file);
}

Shortcuts* Shortcuts::create(QQmlEngine* engine, QJSEngine*) {
    auto* s = new Shortcuts;
    if (engine) {
        // Asking the engine for them creates them if they do not exist yet, so the order these
        // singletons are first touched in does not matter.
        s->document_ = engine->singletonInstance<Document*>("MunkiCast", "Document");
        s->workspace_ = engine->singletonInstance<Workspace*>("MunkiCast", "Workspace");
        if (s->document_)
            connect(s->document_, &Document::changed, s, &Shortcuts::refresh_if_actions_changed);
    }
    s->rebuild();
    return s;
}

Shortcuts::Shortcuts(QObject* parent) : QObject(parent), file_(g_bindings_file) {
    if (!file_.empty()) {
        if (const auto text = lc::read_file(file_)) {
            auto loaded = lsession::Bindings::from_toml(*text);
            if (loaded)
                bindings_ = std::move(*loaded);
            else
                QMetaObject::invokeMethod(
                    this,
                    [this, why = qstr(loaded.error())] {
                        emit notice(QStringLiteral("Key bindings could not be read (%1); the "
                                                   "defaults are in use.")
                                        .arg(why));
                    },
                    Qt::QueuedConnection);
        }
    }
}

Shortcuts::~Shortcuts() = default;

void Shortcuts::refresh_if_actions_changed() {
    if (!document_)
        return;
    std::vector<QString> now;
    for (const lsession::ActionRef& ref :
         lsession::available_actions(document_->projectForBindings()))
        now.push_back(qstr(ref.to_string()));
    if (now == known_actions_)
        return;
    rebuild();
}

void Shortcuts::rebuild() {
    std::vector<BindingRow> rows;
    const ldoc::Project empty;
    const ldoc::Project& project = document_ ? document_->projectForBindings() : empty;

    for (const lsession::ActionRef& ref : lsession::available_actions(project)) {
        const lsession::ActionDef* def = lsession::find_action(ref.action);
        if (!def)
            continue;
        BindingRow row;
        row.ref = qstr(ref.to_string());
        row.title = qstr(lsession::describe(ref, project));
        row.category = qstr(def->category);
        const auto scope = lsession::to_string(def->scope);
        row.scope = QString::fromUtf8(scope.data(), static_cast<qsizetype>(scope.size()));
        row.trigger = qstr(bindings_.trigger_for(ref));
        row.isDefault = bindings_.is_default(ref);
        const auto resolved = lsession::resolve(ref, project);
        row.available = shell_performs(ref.action) ||
                        (resolved.outcome != lsession::Outcome::NotYetAvailable &&
                         resolved.outcome != lsession::Outcome::Unknown);
        if (!row.trigger.isEmpty()) {
            if (const auto clash = bindings_.conflict(ref, to_std(row.trigger), project))
                row.conflict = qstr(lsession::describe(*clash, project));
        }
        rows.push_back(std::move(row));
    }
    known_actions_.clear();
    known_actions_.reserve(rows.size());
    for (const BindingRow& r : rows)
        known_actions_.push_back(r.ref);
    bindings_model_.set_rows(std::move(rows));
    emit changed();
}

QVariantList Shortcuts::applicationBindings() const {
    QVariantList out;
    for (const BindingRow& r : bindings_model_.rows()) {
        if (r.scope != QLatin1String("application") || r.trigger.isEmpty())
            continue;
        QVariantMap m;
        m["ref"] = r.ref;
        m["trigger"] = r.trigger;
        out.append(m);
    }
    return out;
}

int Shortcuts::unboundGlobalCount() const {
    int n = 0;
    for (const BindingRow& r : bindings_model_.rows())
        n += (r.scope == QLatin1String("global") && r.trigger.isEmpty()) ? 1 : 0;
    return n;
}

void Shortcuts::perform(const QString& ref) {
    const auto parsed = lsession::ActionRef::parse(to_std(ref));
    if (!parsed || !document_)
        return;

    if (parsed->action == "output.save-replay" || parsed->action == "output.toggle-replay-buffer") {
        auto* engine = qmlEngine(this);
        auto* preview = engine ? engine->singletonInstance<Preview*>("MunkiCast", "Preview") : nullptr;
        if (!preview) return;
        if (parsed->action == "output.save-replay") preview->saveReplay(document_->replayPath());
        else if (preview->replayActive()) preview->stopReplay();
        else preview->startReplay();
        return;
    }

    // Workspace actions are the one family that does not touch the document, because the layout
    // is not part of it (ADR-0021). Routed by prefix rather than one case each, so adding a
    // direction to the catalogue does not also mean adding a branch here.
    if (parsed->action.starts_with("workspace.") && workspace_) {
        const std::string_view verb =
            std::string_view{parsed->action}.substr(std::string_view{"workspace."}.size());
        const auto suffix = [verb](std::string_view prefix) {
            return QString::fromUtf8(verb.data() + prefix.size(),
                                     static_cast<qsizetype>(verb.size() - prefix.size()));
        };
        if (verb == "undo")
            workspace_->undo();
        else if (verb == "redo")
            workspace_->redo();
        else if (verb == "reset")
            workspace_->resetLayout();
        else if (verb == "focus-next")
            workspace_->focusNext(false);
        else if (verb == "focus-previous")
            workspace_->focusNext(true);
        else if (verb.starts_with("focus-"))
            workspace_->focusNeighbour(suffix("focus-"));
        else if (verb.starts_with("move-"))
            workspace_->movePanel(suffix("move-"));
        else if (verb.starts_with("tab-with-"))
            workspace_->tabPanelWithNeighbour(suffix("tab-with-"));
        else if (verb.starts_with("edge-"))
            workspace_->movePanelToEdge(suffix("edge-"));
        return;
    }

    // The clipboard is the shell's, not the session's: what it holds is a node or a scene out
    // of this project, and there is no command that carries "remember this for later"
    // (ADR-0036). Handled here for the same reason the workspace family is.
    if (parsed->action == "edit.copy") {
        document_->copySelectedNode();
        return;
    }
    if (parsed->action == "edit.paste") {
        document_->paste();
        return;
    }
    if (parsed->action == "edit.group") {
        document_->groupSelectedNodes();
        return;
    }
    if (parsed->action == "edit.select-all") {
        document_->selectAllNodes();
        return;
    }
    if (parsed->action == "edit.ungroup") {
        document_->ungroupNode(document_->selectedNode());
        return;
    }
    // Acts on the whole selection, not on one node, which is why it is here rather than
    // resolving to a RemoveNode in the session (ADR-0037).
    if (parsed->action == "layer.delete") {
        document_->removeSelectedNodes();
        return;
    }
    if (parsed->action == "layer.move-forward") {
        document_->moveNode(document_->selectedNode(), -1);
        return;
    }
    if (parsed->action == "layer.move-back") {
        document_->moveNode(document_->selectedNode(), 1);
        return;
    }
    if (parsed->action == "layer.move-to-front") {
        document_->moveSelectedToEnd(true);
        return;
    }
    if (parsed->action == "layer.move-to-back") {
        document_->moveSelectedToEnd(false);
        return;
    }
    if (parsed->action == "transform.edit") {
        if (workspace_)
            workspace_->focusPanel(QStringLiteral("properties"));
        return;
    }
    if (parsed->action == "transform.copy") {
        document_->copyTransform();
        return;
    }
    if (parsed->action == "transform.paste") {
        document_->pasteTransform();
        return;
    }
    if (parsed->action == "transform.reset") {
        document_->resetTransform();
        return;
    }
    if (parsed->action == "transform.rotate-cw") {
        document_->rotateBy(90.0);
        return;
    }
    if (parsed->action == "transform.rotate-ccw") {
        document_->rotateBy(-90.0);
        return;
    }
    if (parsed->action == "transform.rotate-180") {
        document_->rotateBy(180.0);
        return;
    }
    if (parsed->action == "transform.flip-h") {
        document_->flipHorizontal();
        return;
    }
    if (parsed->action == "transform.flip-v") {
        document_->flipVertical();
        return;
    }
    if (parsed->action == "transform.fit") {
        document_->fitToCanvas(false);
        return;
    }
    if (parsed->action == "transform.stretch") {
        document_->fitToCanvas(true);
        return;
    }
    if (parsed->action == "transform.center") {
        document_->centerOnCanvas(true, true);
        return;
    }
    if (parsed->action == "transform.center-h") {
        document_->centerOnCanvas(true, false);
        return;
    }
    if (parsed->action == "transform.center-v") {
        document_->centerOnCanvas(false, true);
        return;
    }

    const auto resolved = lsession::resolve(*parsed, document_->projectForBindings());
    switch (resolved.outcome) {
    case lsession::Outcome::Undo:
        document_->undo();
        return;
    case lsession::Outcome::Redo:
        document_->redo();
        return;
    case lsession::Outcome::Save:
        document_->save();
        return;
    case lsession::Outcome::Command:
        if (resolved.command)
            document_->runCommand(*resolved.command);
        return;
    case lsession::Outcome::Workspace:
        return; // routed above; nothing in the document changed
    case lsession::Outcome::NotYetAvailable:
    case lsession::Outcome::Unknown:
        emit notice(qstr(resolved.note));
        return;
    }
}

void Shortcuts::setTrigger(const QString& ref, const QString& trigger) {
    const auto parsed = lsession::ActionRef::parse(to_std(ref));
    if (!parsed)
        return;
    bindings_.set_trigger(*parsed, to_std(trigger));
    persist();
    rebuild();
}

void Shortcuts::resetTrigger(const QString& ref) {
    const auto parsed = lsession::ActionRef::parse(to_std(ref));
    if (!parsed)
        return;
    bindings_.reset(*parsed);
    persist();
    rebuild();
}

void Shortcuts::resetAll() {
    bindings_.reset_all();
    persist();
    rebuild();
}

QString Shortcuts::formatKey(int key, int modifiers) const {
    // A bare modifier is not a shortcut; reporting one would let the table record "Ctrl" and then
    // never fire.
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_unknown:
        return {};
    default:
        break;
    }
    const QKeySequence seq{QKeyCombination(static_cast<Qt::KeyboardModifiers>(modifiers),
                                           static_cast<Qt::Key>(key))};
    return seq.toString(QKeySequence::PortableText);
}

void Shortcuts::persist() {
    if (file_.empty())
        return;
    if (const auto r = lc::write_atomic(file_, bindings_.to_toml()); !r)
        emit notice(QStringLiteral("Key bindings could not be saved: %1").arg(qstr(r.error())));
}

} // namespace munkicast::ui

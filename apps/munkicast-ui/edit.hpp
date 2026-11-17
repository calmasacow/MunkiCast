// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <ldoc/workbench.hpp>

#include <filesystem>

namespace munkicast::ui {

/// The Edit workspace: the open timeline, and the edits made to it.
///
/// Its undo stack is its own. `Document` owns the show's history; this owns the timeline's, and
/// the two never meet -- which is the guarantee the operator asked for and the reason the
/// document types are separate in the first place. Undo on the Live tab reverses the last change
/// to the show; undo here reverses the last change to this timeline, whatever happened on the
/// other tab in between.
class Edit : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// The open timeline as a plain value tree, for the panel to draw. Rebuilt on every change
    /// rather than exposed as a model: a timeline is read whole by the Canvas that paints it,
    /// and a per-row model would be a second representation to keep in step.
    Q_PROPERTY(QVariantMap timeline READ timeline NOTIFY changed)
    Q_PROPERTY(bool hasTimeline READ hasTimeline NOTIFY changed)
    /// The project's timelines, for the bins list.
    Q_PROPERTY(QVariantList timelines READ timelines NOTIFY listChanged)
    Q_PROPERTY(QString selectedClip READ selectedClip NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedClipDetails READ selectedClipDetails NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap clipUnderPlayhead READ clipUnderPlayheadMap NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QString undoDescription READ undoDescription NOTIFY changed)
    Q_PROPERTY(QString redoDescription READ redoDescription NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)

public:
    /// Created by the QML engine, which is also where it finds `Document`. Asking the engine
    /// for a singleton creates it if it does not exist yet, so the order they are first touched
    /// in does not matter.
    static Edit* create(QQmlEngine* engine, QJSEngine*);



    [[nodiscard]] QVariantMap timeline() const;
    [[nodiscard]] bool hasTimeline() const { return !open_.is_nil(); }
    [[nodiscard]] QVariantList timelines() const;
    [[nodiscard]] QString selectedClip() const;
    [[nodiscard]] QVariantMap selectedClipDetails() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoDescription() const;
    [[nodiscard]] QString redoDescription() const;
    [[nodiscard]] bool dirty() const;

    /// Make a timeline, add it to the project and open it. Returns its id.
    Q_INVOKABLE QString createTimeline(const QString& name);
    Q_INVOKABLE void openTimeline(const QString& id);
    Q_INVOKABLE void closeTimeline();
    Q_INVOKABLE void save();

    /// Undo and redo *this timeline*. Never the project: see the class comment.
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void endGesture();

    Q_INVOKABLE void setPlayhead(int frame);
    Q_INVOKABLE QString timecode(int frame) const;
    Q_INVOKABLE void addTrack(const QString& kind);
    /// The clip covering `frame` on that track, or an empty map.
    Q_INVOKABLE QVariantMap clipAt(const QString& trackId, int frame) const;
    Q_INVOKABLE void selectClip(const QString& trackId, const QString& clipId);
    Q_INVOKABLE void moveSelectedClip(int start);
    Q_INVOKABLE void removeSelectedClip();
    Q_INVOKABLE void splitAtPlayhead();
    /// Put a media file on a track at a frame. The asset is registered with the project if it
    /// is not already known, so the clip has something that survives a reload.
    Q_INVOKABLE void appendMedia(const QString& path, const QString& trackId);
    /// Append to the first track that will take it: video for a file with a picture, audio for
    /// one without. What a double-click in the bins means, where there is no track to name.
    Q_INVOKABLE void appendToFirstTrack(const QString& path);
    Q_INVOKABLE void setClipGain(double db);
    /// The clip the playhead is over, with the second of its media that is showing, for the
    /// monitor. Empty when nothing is under it.
    Q_INVOKABLE [[nodiscard]] QVariantMap clipUnderPlayheadMap() const;

    /// Called by Document when a project opens or closes.
    void setProject(const std::filesystem::path& dir);
    /// Where the Workbench lives, so Document can ask what still has unsaved work.
    [[nodiscard]] ldoc::Workbench& workbench() { return bench_; }

signals:
    void changed();
    void listChanged();
    void selectionChanged();
    void notice(const QString& text);
    /// A timeline was created, removed or renamed: the project's manifest has to follow.
    void manifestChanged();

private:
    /// Private so that `create` is the only way in.
    ///
    /// A QML singleton with a public default constructor gets default-constructed, and the
    /// `create` overload -- which is where it is wired to `Document` -- is never called. That
    /// fails quietly: the workspace comes up with no project and refuses every edit, saying
    /// only "open a project first" about one that is already open.
    explicit Edit(QObject* parent = nullptr);

    [[nodiscard]] ldoc::TimelineHistory* history();
    [[nodiscard]] const ldoc::TimelineHistory* history() const;
    /// Apply, report a refusal, and tell QML. Every edit goes through here so that none of them
    /// can forget to do one of the three.
    void run(const ldoc::TimelineCommand& command, const QString& gesture = {});

    class Document* document_ = nullptr;
    ldoc::Workbench bench_;
    ldoc::Id open_;
    ldoc::Id selected_clip_;
    ldoc::Id selected_track_;
    std::filesystem::path dir_;
};

} // namespace munkicast::ui

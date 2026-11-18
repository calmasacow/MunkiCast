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

class QTimer;

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
    Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
    /// 1 while playing forward at normal speed, negative backwards, 0 stopped.
    Q_PROPERTY(double rate READ rate NOTIFY transportChanged)
    /// Per-track audio levels while playing, for the meters. Empty when stopped.
    Q_PROPERTY(QVariantList levels READ levels NOTIFY levelsChanged)
    Q_PROPERTY(double masterGain READ masterGain NOTIFY changed)
    /// The file loaded in the Source viewer, with the in and out marked on it.
    ///
    /// A source clip is not on the timeline, so it is not a document edit and carries no undo:
    /// it is the operator's scratch space for deciding which part of a file to use. Marking a
    /// range here and then inserting is what "three-point editing" means.
    Q_PROPERTY(QVariantMap source READ sourceMap NOTIFY sourceChanged)

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
    [[nodiscard]] bool playing() const { return rate_ != 0.0; }
    [[nodiscard]] double rate() const { return rate_; }
    [[nodiscard]] QVariantList levels() const;
    [[nodiscard]] double masterGain() const;
    [[nodiscard]] QVariantMap sourceMap() const;

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
    /// Where the artwork for a clip lives, for the timeline to draw into it. Empty until the
    /// cache has built it, which is why the panel asks again on `ClipArt::ready`.
    Q_INVOKABLE [[nodiscard]] QString clipArt(const QString& source, bool audio) const;
    /// How many of *this timeline's* frames the whole media file spans, or 0 until it has been
    /// looked at. What the panel needs to place artwork under a clip that has been trimmed.
    Q_INVOKABLE [[nodiscard]] int sourceFrames(const QString& source) const;
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

    // ---- track headers ------------------------------------------------------------------

    /// Toggle one of a track's flags: "enabled", "locked", "muted", "solo".
    Q_INVOKABLE void toggleTrack(const QString& trackId, const QString& flag);
    Q_INVOKABLE void setTrackGain(const QString& trackId, double db);
    /// Pan a track between the speakers: -1 hard left, 0 centre, +1 hard right.
    Q_INVOKABLE void setTrackPan(const QString& trackId, double pan);
    /// "off", "read", "touch", "latch" or "write".
    Q_INVOKABLE void setTrackAutomation(const QString& trackId, const QString& mode);
    Q_INVOKABLE void renameTrack(const QString& trackId, const QString& name);
    Q_INVOKABLE void removeTrack(const QString& trackId);
    Q_INVOKABLE void setMasterGain(double db);

    // ---- edit operations ----------------------------------------------------------------

    /// Trim an edge of the selected clip to a frame. `head` trims its start, moving the media
    /// window with it; otherwise its end.
    Q_INVOKABLE void trimSelectedClip(int frame, bool head);
    /// Remove the selected clip and close the gap it leaves, which is what "ripple" means.
    Q_INVOKABLE void rippleDeleteSelectedClip();
    /// Slip the selected clip's media window without moving it on the timeline.
    Q_INVOKABLE void slipSelectedClip(int byFrames);
    /// Cut every unlocked track at the playhead, which is what a razor does when nothing is
    /// selected in particular.
    Q_INVOKABLE void razorAtPlayhead();
    /// Put a transition on the selected clip's head. `kind` is "cut", "cross-dissolve",
    /// "dip-to-black" or "push".
    /// An empty `align` keeps the clip's current alignment.
    Q_INVOKABLE void setTransition(const QString& kind, int frames,
                                   const QString& align = {});
    /// Put an effect from the catalogue on the selected clip.
    Q_INVOKABLE void addClipEffect(const QString& type);
    /// Take one off again, by its id.
    Q_INVOKABLE void removeClipEffect(const QString& id);
    /// Turn one off without removing it, which is how an editor compares with and without.
    Q_INVOKABLE void toggleClipEffect(const QString& id);
    /// Change one parameter of one effect on the selected clip.
    Q_INVOKABLE void setClipEffectParam(const QString& id, const QString& key,
                                        const QVariant& value);
    /// The renderer's filter catalogue: what could be put on a clip.
    Q_INVOKABLE [[nodiscard]] QVariantList effectCatalogue() const;
    // ---- the Source viewer ----------------------------------------------------------------
    //
    // Three-point editing: open a file here, mark the part of it you want, then insert or
    // overwrite that part into the sequence at the playhead.

    /// Load a file into the Source viewer. Its range starts as the whole file.
    Q_INVOKABLE void openSource(const QString& path);
    /// Mark the Source viewer's in or out at a frame of the *file*.
    Q_INVOKABLE void setSourceIn(int frame);
    Q_INVOKABLE void setSourceOut(int frame);
    Q_INVOKABLE void clearSourceRange();
    /// Put the Source viewer's marked range into the sequence at the playhead.
    ///
    /// `overwrite` replaces what is already there; otherwise the clips after the playhead are
    /// pushed back to make room, which is what "insert" means.
    Q_INVOKABLE void insertSourceRange(bool overwrite);

    Q_INVOKABLE void setInPoint();
    Q_INVOKABLE void setOutPoint();
    Q_INVOKABLE void clearRange();
    Q_INVOKABLE void addMarkerAtPlayhead();

    // ---- transport ----------------------------------------------------------------------

    /// Playback runs the playhead on a timer; the monitor follows it.
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    /// Move by whole frames, for the arrow keys and the step buttons.
    Q_INVOKABLE void step(int frames);
    /// J-K-L: negative is backwards, 0 stops, and repeated presses multiply the rate.
    Q_INVOKABLE void shuttle(int direction);
    Q_INVOKABLE void goToStart();
    Q_INVOKABLE void goToEnd();
    /// The next or previous edit point on any track, which is how an editor walks a sequence.
    Q_INVOKABLE void stepEdit(bool forward);
    /// The clip the playhead is over, with the second of its media that is showing, for the
    /// monitor. Empty when nothing is under it.
    Q_INVOKABLE [[nodiscard]] QVariantMap clipUnderPlayheadMap() const;

    /// Called by Document when a project opens or closes.
    void setProject(const std::filesystem::path& dir);
    /// Where the Workbench lives, so Document can ask what still has unsaved work.
    [[nodiscard]] ldoc::Workbench& workbench() { return bench_; }

signals:
    void changed();
    void transportChanged();
    void levelsChanged();
    void listChanged();
    void selectionChanged();
    void sourceChanged();
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

    /// Advance the playhead by one tick of playback. Bound to a timer rather than driven by
    /// the monitor, so the timeline keeps time even where the monitor has nothing to show.
    void tick();
    /// Set the rate and start or stop the timer behind it.
    void setRate(double rate);

    /// The Source viewer's file and its marked range, in that file's own frames.
    QString source_path_;
    ldoc::Frame source_in_ = 0;
    ldoc::Frame source_out_ = 0;

    class Document* document_ = nullptr;
    /// The selected clip, or nothing. Every effect command needs it.
    [[nodiscard]] const ldoc::Clip* selectedClip_() const;

    class ClipArt* art_ = nullptr;
    ldoc::Workbench bench_;
    QTimer* transport_ = nullptr;
    double rate_ = 0.0;
    /// When the current playback run started, so the playhead follows the clock rather than
    /// counting timer ticks -- which drift, and drift differently under load.
    qint64 started_ms_ = 0;
    ldoc::Frame started_frame_ = 0;
    ldoc::Id open_;
    ldoc::Id selected_clip_;
    ldoc::Id selected_track_;
    std::filesystem::path dir_;
};

} // namespace munkicast::ui

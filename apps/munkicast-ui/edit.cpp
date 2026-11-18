// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "edit.hpp"

#include "assetcache.hpp"
#include "clipart.hpp"
#include "document.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QTimer>

#include <lcomp/filters.hpp>
#include <ldoc/json.hpp>
#include <ldoc/project_io.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace munkicast::ui {
namespace {

QString to_qt(const ldoc::Id& id) {
    return id.is_nil() ? QString{} : QString::fromStdString(id.to_string());
}

ldoc::Id from_qt(const QString& text) {
    const auto parsed = lc::Ulid::parse(text.toStdString());
    return parsed ? *parsed : ldoc::Id{};
}

QVariantList effects_list(const std::vector<ldoc::Effect>& effects) {
    QVariantList out;
    for (const ldoc::Effect& e : effects) {
        out.append(QVariantMap{{QStringLiteral("id"), to_qt(e.id)},
                               {QStringLiteral("type"), QString::fromStdString(e.type)},
                               {QStringLiteral("name"), QString::fromStdString(e.name)},
                               {QStringLiteral("enabled"), e.enabled}});
    }
    return out;
}

QVariantMap clip_map(const ldoc::Clip& clip) {
    return QVariantMap{
        {QStringLiteral("id"), to_qt(clip.id)},
        {QStringLiteral("source"), QString::fromStdString(clip.source)},
        {QStringLiteral("name"), QString::fromStdString(clip.name)},
        {QStringLiteral("start"), static_cast<qlonglong>(clip.start)},
        {QStringLiteral("duration"), static_cast<qlonglong>(clip.duration)},
        {QStringLiteral("sourceIn"), static_cast<qlonglong>(clip.source_in)},
        {QStringLiteral("gainDb"), clip.gain_db},
        {QStringLiteral("hasEffects"), clip.has_effects()},
        {QStringLiteral("effects"), effects_list(clip.effects)},
        {QStringLiteral("transition"),
         QString::fromStdString(std::string{to_string(clip.transition.kind)})},
        {QStringLiteral("transitionFrames"),
         static_cast<qlonglong>(clip.transition.real() ? clip.transition.duration : 0)},
        {QStringLiteral("transitionAlign"),
         QString::fromStdString(std::string{to_string(clip.transition.align)})},
        // How far the wedge reaches back before the cut. Derived once here rather than in the
        // painter, so the drawing and any future render cannot disagree about it.
        {QStringLiteral("transitionLead"),
         static_cast<qlonglong>(clip.transition.real() ? clip.transition.lead() : 0)},
    };
}

} // namespace

Edit* Edit::create(QQmlEngine* engine, QJSEngine*) {
    auto* e = new Edit;
    if (engine) {
        // Asking the engine for it creates it if it does not exist yet, so the order these
        // singletons are first touched in does not matter.
        e->document_ = engine->singletonInstance<Document*>("MunkiCast", "Document");
        e->art_ = engine->singletonInstance<ClipArt*>("MunkiCast", "ClipArt");
        if (e->document_) {
            connect(e->document_, &Document::projectOpened, e, [e](const QString& dir) {
                e->setProject(dir.isEmpty() ? std::filesystem::path{}
                                            : std::filesystem::path{dir.toStdString()});
            });
            // A QML singleton is created the first time something asks for it, which may be
            // long after the project was opened. Seeding it here is what stops it starting
            // with no project in a session that already has one.
            const QString open = e->document_->projectDirectory();
            if (!open.isEmpty())
                e->setProject(std::filesystem::path{open.toStdString()});
        }
    }
    return e;
}

Edit::Edit(QObject* parent) : QObject(parent) {}

ldoc::TimelineHistory* Edit::history() {
    return open_.is_nil() ? nullptr : bench_.timeline(open_);
}

const ldoc::TimelineHistory* Edit::history() const {
    return open_.is_nil() ? nullptr : bench_.timeline(open_);
}

QVariantMap Edit::timeline() const {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return {};
    const ldoc::Timeline& t = h->document();

    QVariantList tracks;
    // V1, V2, A1, A2 ... numbered per kind from the bottom up, which is the order every NLE
    // names them in and the order the timeline draws them.
    int video_n = 0;
    int audio_n = 0;
    for (const ldoc::Track& track : t.tracks) {
        QVariantList clips;
        for (const ldoc::Clip& clip : track.clips)
            clips.append(clip_map(clip));
        const bool is_audio = track.kind == ldoc::TrackKind::Audio;
        const int n = is_audio ? ++audio_n : ++video_n;
        tracks.append(QVariantMap{
            {QStringLiteral("id"), to_qt(track.id)},
            {QStringLiteral("name"), QString::fromStdString(track.name)},
            {QStringLiteral("shortName"), QStringLiteral("%1%2")
                                              .arg(is_audio ? QStringLiteral("A")
                                                            : QStringLiteral("V"))
                                              .arg(n)},
            {QStringLiteral("kind"), QString::fromStdString(std::string{to_string(track.kind)})},
            {QStringLiteral("enabled"), track.enabled},
            {QStringLiteral("locked"), track.locked},
            {QStringLiteral("muted"), track.muted},
            {QStringLiteral("solo"), track.solo},
            {QStringLiteral("patched"), track.patched},
            {QStringLiteral("armed"), track.armed},
            {QStringLiteral("gainDb"), track.gain_db},
            {QStringLiteral("pan"), track.pan},
            {QStringLiteral("audible"), t.audible(track)},
            {QStringLiteral("clips"), clips},
        });
    }

    QVariantList markers;
    for (const ldoc::Marker& m : t.markers) {
        markers.append(QVariantMap{{QStringLiteral("id"), to_qt(m.id)},
                                   {QStringLiteral("at"), static_cast<qlonglong>(m.at)},
                                   {QStringLiteral("name"), QString::fromStdString(m.name)},
                                   {QStringLiteral("color"), m.color}});
    }

    return QVariantMap{
        {QStringLiteral("id"), to_qt(t.id)},
        {QStringLiteral("name"), QString::fromStdString(t.name)},
        {QStringLiteral("fps"), t.fps.hz()},
        {QStringLiteral("width"), t.width},
        {QStringLiteral("height"), t.height},
        {QStringLiteral("playhead"), static_cast<qlonglong>(t.playhead)},
        {QStringLiteral("duration"), static_cast<qlonglong>(t.duration())},
        // -1 rather than absent: QML reads a missing key as undefined, and the panel compares
        // these against a number.
        {QStringLiteral("inPoint"),
         t.in_point ? static_cast<qlonglong>(*t.in_point) : qlonglong{-1}},
        {QStringLiteral("outPoint"),
         t.out_point ? static_cast<qlonglong>(*t.out_point) : qlonglong{-1}},
        {QStringLiteral("tracks"), tracks},
        {QStringLiteral("markers"), markers},
        {QStringLiteral("masterGainDb"), t.master_gain_db},
    };
}

QVariantList Edit::timelines() const {
    QVariantList rows;
    if (!document_)
        return rows;
    for (const ldoc::SubdocRef& ref : document_->projectForBindings().timelines) {
        rows.append(QVariantMap{{QStringLiteral("id"), to_qt(ref.id)},
                                {QStringLiteral("name"), QString::fromStdString(ref.name)},
                                {QStringLiteral("open"), ref.id == open_}});
    }
    return rows;
}

QString Edit::selectedClip() const {
    return to_qt(selected_clip_);
}

QVariantMap Edit::selectedClipDetails() const {
    const ldoc::TimelineHistory* h = history();
    if (!h || selected_clip_.is_nil())
        return {};
    const ldoc::Track* track = h->document().track(selected_track_);
    if (!track)
        return {};
    const ldoc::Clip* clip = track->clip(selected_clip_);
    if (!clip)
        return {};
    QVariantMap out = clip_map(*clip);
    out.insert(QStringLiteral("track"), QString::fromStdString(track->name));
    out.insert(QStringLiteral("trackId"), to_qt(track->id));
    return out;
}

bool Edit::canUndo() const {
    const ldoc::TimelineHistory* h = history();
    return h && h->can_undo();
}

bool Edit::canRedo() const {
    const ldoc::TimelineHistory* h = history();
    return h && h->can_redo();
}

QString Edit::undoDescription() const {
    const ldoc::TimelineHistory* h = history();
    return h ? QString::fromStdString(h->undo_description()) : QString{};
}

QString Edit::redoDescription() const {
    const ldoc::TimelineHistory* h = history();
    return h ? QString::fromStdString(h->redo_description()) : QString{};
}

bool Edit::dirty() const {
    const ldoc::TimelineHistory* h = history();
    return h && h->dirty();
}

void Edit::setProject(const std::filesystem::path& dir) {
    // Closing a project closes its documents: their undo stacks belong to work that is no
    // longer open, and keeping them would let an undo reach into a project that has gone.
    bench_.clear();
    open_ = {};
    selected_clip_ = {};
    selected_track_ = {};
    dir_ = dir;
    emit changed();
    emit listChanged();
    emit selectionChanged();
}

QString Edit::createTimeline(const QString& name) {
    if (dir_.empty()) {
        emit notice(tr("Open a project before making a timeline."));
        return {};
    }

    ldoc::Timeline t;
    t.id = ldoc::Id::generate();
    t.name = name.isEmpty() ? tr("Timeline").toStdString() : name.toStdString();

    // One video and one audio track to start, because a timeline with no tracks has nowhere to
    // put the first clip and the operator would have to make one before doing anything.
    ldoc::Track video;
    video.id = ldoc::Id::generate();
    video.name = "V1";
    video.kind = ldoc::TrackKind::Video;
    ldoc::Track audio;
    audio.id = ldoc::Id::generate();
    audio.name = "A1";
    audio.kind = ldoc::TrackKind::Audio;
    t.tracks = {std::move(video), std::move(audio)};

    // Written before it is announced: a manifest entry naming a file that does not exist is a
    // project that reports a missing document on its next open.
    if (const auto r = ldoc::save_timeline(dir_, t, 0); !r) {
        emit notice(tr("The timeline could not be written: %1")
                        .arg(QString::fromStdString(r.error())));
        return {};
    }

    ldoc::SubdocRef ref;
    ref.id = t.id;
    ref.name = t.name;
    if (document_)
        document_->runCommand(
            ldoc::AddTimelineRef{ref, document_->projectForBindings().timelines.size()});

    const QString id = to_qt(t.id);
    bench_.open_timeline(std::move(t));
    open_ = from_qt(id);
    emit changed();
    emit listChanged();
    return id;
}

void Edit::openTimeline(const QString& id) {
    const ldoc::Id wanted = from_qt(id);
    if (wanted.is_nil())
        return;
    // Already open: switch to it and keep its stack exactly where it was left. Reading the file
    // again here would discard undo history for work that is still in progress.
    if (!bench_.timeline(wanted)) {
        auto loaded = ldoc::load_timeline(dir_, wanted);
        if (!loaded) {
            emit notice(tr("That timeline could not be opened: %1")
                            .arg(QString::fromStdString(loaded.error())));
            return;
        }
        bench_.open_timeline(std::move(*loaded));
    }
    open_ = wanted;
    selected_clip_ = {};
    selected_track_ = {};
    emit changed();
    emit listChanged();
    emit selectionChanged();
}

void Edit::closeTimeline() {
    open_ = {};
    selected_clip_ = {};
    selected_track_ = {};
    emit changed();
    emit listChanged();
    emit selectionChanged();
}

void Edit::save() {
    ldoc::TimelineHistory* h = history();
    if (!h || dir_.empty())
        return;
    if (const auto r = ldoc::save_timeline(dir_, h->document(), 0); !r) {
        emit notice(tr("The timeline could not be saved: %1")
                        .arg(QString::fromStdString(r.error())));
        return;
    }
    h->note_saved();
    emit changed();
}

void Edit::run(const ldoc::TimelineCommand& command, const QString& gesture) {
    ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const auto applied = gesture.isEmpty() ? h->run(command)
                                           : h->run(command, gesture.toStdString());
    if (!applied) {
        emit notice(QString::fromStdString(applied.error()));
        return;
    }
    emit changed();
}

void Edit::undo() {
    ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    if (const auto r = h->undo(); !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    emit changed();
    emit selectionChanged();
}

void Edit::redo() {
    ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    if (const auto r = h->redo(); !r) {
        emit notice(QString::fromStdString(r.error()));
        return;
    }
    emit changed();
    emit selectionChanged();
}

void Edit::endGesture() {
    if (ldoc::TimelineHistory* h = history())
        h->end_gesture();
}

void Edit::setPlayhead(int frame) {
    ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    // Not a command: where the operator is looking is not something they undo, and a stack full
    // of cursor moves is one that cannot reach the edit underneath them. Written straight to
    // the document, which is why `SubHistory` exposes it as the one mutable piece.
    const_cast<ldoc::Timeline&>(h->document()).playhead = std::max(0, frame);
    emit changed();
}

QString Edit::timecode(int frame) const {
    const ldoc::TimelineHistory* h = history();
    const double fps = h ? h->document().fps.hz() : 30.0;
    if (fps <= 0.0)
        return QStringLiteral("00:00:00:00");
    const auto f = static_cast<qint64>(std::max(0, frame));
    const auto per = static_cast<qint64>(fps + 0.5);
    const qint64 total = f / per;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(total / 3600, 2, 10, QLatin1Char('0'))
        .arg((total % 3600) / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'))
        .arg(f % per, 2, 10, QLatin1Char('0'));
}

void Edit::addTrack(const QString& kind) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const bool audio = kind == QLatin1String("audio");

    // Numbered after the ones already there of the same kind, so a second video track is V2
    // whatever order the tracks were made in.
    int n = 0;
    for (const ldoc::Track& t : h->document().tracks) {
        if ((t.kind == ldoc::TrackKind::Audio) == audio)
            ++n;
    }

    ldoc::Track track;
    track.id = ldoc::Id::generate();
    track.kind = audio ? ldoc::TrackKind::Audio : ldoc::TrackKind::Video;
    track.name = (audio ? "A" : "V") + std::to_string(n + 1);
    run(ldoc::AddTrack{std::move(track), h->document().tracks.size()});
}

QVariantMap Edit::clipAt(const QString& trackId, int frame) const {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return {};
    const ldoc::Track* track = h->document().track(from_qt(trackId));
    if (!track)
        return {};
    const ldoc::Clip* clip = track->clip_at(frame);
    return clip ? clip_map(*clip) : QVariantMap{};
}

QString Edit::clipArt(const QString& source, bool audio) const {
    if (source.isEmpty())
        return {};
    // Asking is also what queues the build, so a clip that scrolls into view starts its own
    // artwork. Both calls are cheap once it is cached.
    art_->request(source, audio);
    return art_->source(source, audio);
}

int Edit::sourceFrames(const QString& source) const {
    const ldoc::TimelineHistory* h = history();
    if (!h || !art_ || source.isEmpty())
        return 0;
    const double seconds = art_->duration(source);
    const double fps = h->document().fps.hz();
    return seconds > 0.0 && fps > 0.0 ? static_cast<int>(seconds * fps) : 0;
}

void Edit::selectClip(const QString& trackId, const QString& clipId) {
    selected_track_ = from_qt(trackId);
    selected_clip_ = from_qt(clipId);
    emit selectionChanged();
    emit changed(); // the panel draws the selection
}

void Edit::moveSelectedClip(int start) {
    if (selected_clip_.is_nil())
        return;
    // One gesture per clip, so a drag across fifty frames is one undo step and two separate
    // drags of the same clip are two.
    run(ldoc::MoveClip{selected_track_, selected_clip_, std::max(0, start), ldoc::Id{}},
        QStringLiteral("clip.move:%1").arg(to_qt(selected_clip_)));
}

void Edit::removeSelectedClip() {
    if (selected_clip_.is_nil())
        return;
    run(ldoc::RemoveClip{selected_track_, selected_clip_});
    selected_clip_ = {};
    emit selectionChanged();
}

void Edit::splitAtPlayhead() {
    const ldoc::TimelineHistory* h = history();
    if (!h || selected_clip_.is_nil())
        return;
    run(ldoc::SplitClip{selected_track_, selected_clip_, h->document().playhead,
                        ldoc::Id::generate()});
}

void Edit::appendMedia(const QString& path, const QString& trackId) {
    ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Track* track = h->document().track(from_qt(trackId));
    if (!track) {
        emit notice(tr("Choose a track to put that on."));
        return;
    }
    if (track->locked) {
        emit notice(tr("Track '%1' is locked.").arg(QString::fromStdString(track->name)));
        return;
    }

    // The length is the file's own, in this timeline's frames. A file that cannot be probed
    // gets five seconds rather than nothing: a clip of unknown length is still an edit the
    // operator can trim, and a refusal here would stop them working over a probe failure.
    const double seconds = probe_asset(path).duration;
    const double fps = h->document().fps.hz();
    const auto frames = static_cast<ldoc::Frame>(
        seconds > 0.0 ? seconds * fps : 5.0 * fps);

    ldoc::Clip clip;
    clip.id = ldoc::Id::generate();
    clip.source = path.toStdString();
    clip.name = QFileInfo(path).fileName().toStdString();
    // Appended after the last clip on the track, which is what "add this next" means and never
    // overlaps anything already there.
    clip.start = track->clips.empty() ? 0 : track->clips.back().end();
    clip.duration = std::max<ldoc::Frame>(1, frames);
    run(ldoc::AddClip{track->id, std::move(clip)});
}

void Edit::setClipGain(double db) {
    if (selected_clip_.is_nil())
        return;
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Track* track = h->document().track(selected_track_);
    const ldoc::Clip* clip = track ? track->clip(selected_clip_) : nullptr;
    if (!clip)
        return;
    // SetClip carries the whole value, so the name has to come along or it would be cleared.
    run(ldoc::SetClip{selected_track_, selected_clip_, clip->name, db, clip->effects});
}

QVariantMap Edit::clipUnderPlayheadMap() const {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return {};
    const ldoc::Timeline& t = h->document();
    const double fps = t.fps.hz();

    // The topmost video track that has a clip there, searched from the top down, which is the
    // order the picture is layered in.
    for (auto it = t.tracks.rbegin(); it != t.tracks.rend(); ++it) {
        if (it->kind != ldoc::TrackKind::Video || !it->enabled)
            continue;
        const ldoc::Clip* clip = it->clip_at(t.playhead);
        if (!clip)
            continue;
        QVariantMap out = clip_map(*clip);
        // Where in the *file* that frame is, which is what the monitor seeks to.
        out.insert(QStringLiteral("sourceSeconds"),
                   fps > 0.0 ? static_cast<double>(clip->source_at(t.playhead)) / fps : 0.0);
        return out;
    }
    return {};
}

// ---- track headers -----------------------------------------------------------------------

namespace {

/// `SetTrack` carrying a track's current value, for a caller to change one field of.
///
/// Every setter needs all nine fields, and spelling them out at each call site is how one of
/// them ends up quietly reset when a tenth is added -- which is exactly what happened when pan
/// and automation arrived.
[[nodiscard]] ldoc::SetTrack track_as_is(const ldoc::Track& t) {
    return ldoc::SetTrack{t.id,    t.name, t.enabled,    t.locked,  t.muted,
                          t.solo,  t.gain_db, t.pan,     t.automation,
                          t.patched, t.armed};
}

} // namespace

void Edit::toggleTrack(const QString& trackId, const QString& flag) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Track* track = h->document().track(from_qt(trackId));
    if (!track)
        return;

    // SetTrack carries the whole value, so every field starts from the current one: a setter
    // that forgot a field would quietly reset it.
    ldoc::SetTrack edit = track_as_is(*track);
    if (flag == QLatin1String("enabled"))
        edit.enabled = !track->enabled;
    else if (flag == QLatin1String("locked"))
        edit.locked = !track->locked;
    else if (flag == QLatin1String("muted"))
        edit.muted = !track->muted;
    else if (flag == QLatin1String("solo"))
        edit.solo = !track->solo;
    else if (flag == QLatin1String("patched"))
        edit.patched = !track->patched;
    else if (flag == QLatin1String("armed"))
        edit.armed = !track->armed;
    else
        return;
    run(edit);
}

void Edit::setTrackGain(const QString& trackId, double db) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Track* track = h->document().track(from_qt(trackId));
    if (!track)
        return;
    ldoc::SetTrack edit = track_as_is(*track);
    edit.gain_db = db;
    run(edit, QStringLiteral("track.gain:%1").arg(trackId));
}

void Edit::setTrackPan(const QString& trackId, double pan) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Track* track = h->document().track(from_qt(trackId));
    if (!track)
        return;
    ldoc::SetTrack edit = track_as_is(*track);
    edit.pan = std::clamp(pan, -1.0, 1.0);
    run(edit, QStringLiteral("track.pan:%1").arg(trackId));
}

void Edit::setTrackAutomation(const QString& trackId, const QString& mode) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Track* track = h->document().track(from_qt(trackId));
    if (!track)
        return;
    const auto parsed = ldoc::automation_mode_from_string(mode.toStdString());
    if (!parsed) {
        emit notice(tr("There is no automation mode called '%1'.").arg(mode));
        return;
    }
    ldoc::SetTrack edit = track_as_is(*track);
    edit.automation = *parsed;
    run(edit);
}

void Edit::renameTrack(const QString& trackId, const QString& name) {
    const ldoc::TimelineHistory* h = history();
    if (!h || name.trimmed().isEmpty())
        return;
    const ldoc::Track* track = h->document().track(from_qt(trackId));
    if (!track)
        return;
    ldoc::SetTrack edit = track_as_is(*track);
    edit.name = name.trimmed().toStdString();
    run(edit);
}

void Edit::removeTrack(const QString& trackId) {
    const ldoc::Id id = from_qt(trackId);
    if (id == selected_track_) {
        selected_clip_ = {};
        selected_track_ = {};
        emit selectionChanged();
    }
    run(ldoc::RemoveTrack{id});
}

void Edit::setMasterGain(double db) {
    run(ldoc::SetMasterGain{db}, QStringLiteral("timeline.master"));
}

double Edit::masterGain() const {
    const ldoc::TimelineHistory* h = history();
    return h ? h->document().master_gain_db : 0.0;
}

// ---- edit operations ---------------------------------------------------------------------

void Edit::trimSelectedClip(int frame, bool head) {
    const ldoc::TimelineHistory* h = history();
    if (!h || selected_clip_.is_nil())
        return;
    const ldoc::Track* track = h->document().track(selected_track_);
    const ldoc::Clip* clip = track ? track->clip(selected_clip_) : nullptr;
    if (!clip)
        return;

    ldoc::TrimClip edit{selected_track_, selected_clip_, clip->start, clip->duration,
                        clip->source_in};
    if (head) {
        // The media window moves with the edge, so the picture under the cursor stays still
        // rather than sliding as the clip is trimmed.
        const ldoc::Frame at = std::clamp<ldoc::Frame>(frame, 0, clip->end() - 1);
        const ldoc::Frame delta = at - clip->start;
        if (clip->source_in + delta < 0)
            return;
        edit.start = at;
        edit.duration = clip->end() - at;
        edit.source_in = clip->source_in + delta;
    } else {
        const ldoc::Frame at = std::max<ldoc::Frame>(frame, clip->start + 1);
        edit.duration = at - clip->start;
    }
    run(edit, QStringLiteral("clip.trim:%1").arg(to_qt(selected_clip_)));
}

void Edit::rippleDeleteSelectedClip() {
    const ldoc::TimelineHistory* h = history();
    if (!h || selected_clip_.is_nil())
        return;
    const ldoc::Track* track = h->document().track(selected_track_);
    const ldoc::Clip* clip = track ? track->clip(selected_clip_) : nullptr;
    if (!clip)
        return;

    // Remove, then pull everything after it back by its length. One batch, so it is one undo
    // step: an operator who ripples a clip out means the whole move, not half of it.
    const ldoc::Frame gap = clip->duration;
    const ldoc::Frame from = clip->start;
    std::vector<ldoc::TimelineCommand> steps{ldoc::RemoveClip{selected_track_, selected_clip_}};
    for (const ldoc::Clip& later : track->clips) {
        if (later.start > from)
            steps.push_back(ldoc::MoveClip{selected_track_, later.id, later.start - gap,
                                           ldoc::Id{}});
    }
    const std::string name = clip->name;
    selected_clip_ = {};
    emit selectionChanged();
    run(ldoc::make_timeline_batch(std::move(steps), std::format("Ripple delete '{}'", name)));
}

void Edit::slipSelectedClip(int byFrames) {
    const ldoc::TimelineHistory* h = history();
    if (!h || selected_clip_.is_nil() || byFrames == 0)
        return;
    const ldoc::Track* track = h->document().track(selected_track_);
    const ldoc::Clip* clip = track ? track->clip(selected_clip_) : nullptr;
    if (!clip)
        return;
    // The clip stays where it is and shows different media: start and duration are untouched,
    // only the window into the file moves.
    const ldoc::Frame source_in = clip->source_in + byFrames;
    if (source_in < 0)
        return;
    run(ldoc::TrimClip{selected_track_, selected_clip_, clip->start, clip->duration, source_in},
        QStringLiteral("clip.slip:%1").arg(to_qt(selected_clip_)));
}

void Edit::razorAtPlayhead() {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Frame at = h->document().playhead;

    // Every unlocked track at once, which is what a razor across a sequence means. One batch,
    // so the whole cut is one undo step.
    std::vector<ldoc::TimelineCommand> steps;
    for (const ldoc::Track& track : h->document().tracks) {
        if (track.locked)
            continue;
        const ldoc::Clip* clip = track.clip_at(at);
        if (!clip || clip->start == at)
            continue;
        steps.push_back(ldoc::SplitClip{track.id, clip->id, at, ldoc::Id::generate()});
    }
    if (steps.empty()) {
        emit notice(tr("There is nothing to cut at the playhead."));
        return;
    }
    run(ldoc::make_timeline_batch(std::move(steps), "Cut at the playhead"));
}

void Edit::setTransition(const QString& kind, int frames, const QString& align) {
    if (selected_clip_.is_nil())
        return;
    const auto parsed = ldoc::transition_kind_from_string(kind.toStdString());
    if (!parsed) {
        emit notice(tr("There is no transition called '%1'.").arg(kind));
        return;
    }
    // An empty alignment keeps whatever the clip already has, so changing a dissolve's length
    // from the properties panel does not quietly re-centre one the operator moved.
    auto how = ldoc::TransitionAlign::Centre;
    if (align.isEmpty()) {
        const ldoc::TimelineHistory* h = history();
        const ldoc::Track* track = h ? h->document().track(selected_track_) : nullptr;
        const ldoc::Clip* clip = track ? track->clip(selected_clip_) : nullptr;
        if (clip)
            how = clip->transition.align;
    } else if (const auto parsed_align =
                   ldoc::transition_align_from_string(align.toStdString())) {
        how = *parsed_align;
    } else {
        emit notice(tr("There is no transition alignment called '%1'.").arg(align));
        return;
    }
    run(ldoc::SetTransition{selected_track_, selected_clip_,
                            ldoc::Transition{*parsed, std::max(0, frames), how}});
}

QVariantMap Edit::sourceMap() const {
    if (source_path_.isEmpty())
        return {};
    const int frames = sourceFrames(source_path_);
    const ldoc::TimelineHistory* h = history();
    const double fps = h ? h->document().fps.hz() : 0.0;
    const ldoc::Frame out = source_out_ > source_in_ ? source_out_ : frames;
    return QVariantMap{
        {QStringLiteral("path"), source_path_},
        {QStringLiteral("name"), QFileInfo(source_path_).fileName()},
        {QStringLiteral("frames"), frames},
        {QStringLiteral("in"), static_cast<qlonglong>(source_in_)},
        {QStringLiteral("out"), static_cast<qlonglong>(out)},
        {QStringLiteral("rangeFrames"), static_cast<qlonglong>(std::max<ldoc::Frame>(0, out - source_in_))},
        {QStringLiteral("inSeconds"), fps > 0.0 ? source_in_ / fps : 0.0},
        {QStringLiteral("seconds"), fps > 0.0 ? frames / fps : 0.0},
    };
}

void Edit::openSource(const QString& path) {
    if (path.isEmpty())
        return;
    source_path_ = path;
    source_in_ = 0;
    source_out_ = 0;
    // Asking for the artwork is also what measures the file, and `sourceMap` needs that
    // length to know where the out point is.
    if (art_) {
        art_->request(path, false);
        art_->request(path, true);
    }
    emit sourceChanged();
}

void Edit::setSourceIn(int frame) {
    if (source_path_.isEmpty())
        return;
    source_in_ = std::max(0, frame);
    // An in at or past the out clears the out rather than refusing: the operator is marking a
    // new range, and making them clear the old one first is a step for nothing.
    if (source_out_ > 0 && source_out_ <= source_in_)
        source_out_ = 0;
    emit sourceChanged();
}

void Edit::setSourceOut(int frame) {
    if (source_path_.isEmpty())
        return;
    source_out_ = std::max(0, frame);
    if (source_out_ <= source_in_)
        source_in_ = 0;
    emit sourceChanged();
}

void Edit::clearSourceRange() {
    source_in_ = 0;
    source_out_ = 0;
    emit sourceChanged();
}

void Edit::insertSourceRange(bool overwrite) {
    const ldoc::TimelineHistory* h = history();
    if (!h) {
        emit notice(tr("Open a timeline first."));
        return;
    }
    if (source_path_.isEmpty()) {
        emit notice(tr("Open a file in the Source viewer first."));
        return;
    }

    const ldoc::Timeline& t = h->document();
    const int frames = sourceFrames(source_path_);
    const ldoc::Frame out = source_out_ > source_in_ ? source_out_ : frames;
    const ldoc::Frame length = out - source_in_;
    if (length <= 0) {
        emit notice(tr("That range is empty."));
        return;
    }

    const Probe probed = probe_asset(source_path_);
    const bool has_video = !probed.size.isEmpty();
    const ldoc::Track* video = nullptr;
    const ldoc::Track* audio = nullptr;
    // The source patch decides where it lands, which is what the V1/A1 badges on the track
    // heads are for. A patched track that is locked is still skipped: locked means locked.
    for (const ldoc::Track& track : t.tracks) {
        if (track.locked || !track.patched)
            continue;
        if (!video && track.kind == ldoc::TrackKind::Video)
            video = &track;
        if (!audio && track.kind == ldoc::TrackKind::Audio)
            audio = &track;
    }
    if ((has_video && !video) && !audio) {
        emit notice(tr("There is no unlocked track to put that on."));
        return;
    }

    const ldoc::Frame at = t.playhead;
    std::vector<ldoc::TimelineCommand> steps;

    const auto lay_down = [&](const ldoc::Track* track) {
        if (!track)
            return;
        if (overwrite) {
            // Whatever is under the new clip has to go first: AddClip refuses an overlap, and
            // silently trimming around one would be a different edit from the one asked for.
            for (const ldoc::Clip& existing : track->clips) {
                if (existing.end() > at && existing.start < at + length)
                    steps.push_back(ldoc::RemoveClip{track->id, existing.id});
            }
        } else {
            // Insert: push everything from the playhead on back by the new clip's length.
            for (const ldoc::Clip& later : track->clips) {
                if (later.start >= at)
                    steps.push_back(ldoc::MoveClip{track->id, later.id, later.start + length,
                                                   ldoc::Id{}});
            }
        }
        ldoc::Clip clip;
        clip.id = ldoc::Id::generate();
        clip.source = source_path_.toStdString();
        clip.name = QFileInfo(source_path_).fileName().toStdString();
        clip.start = at;
        clip.duration = length;
        clip.source_in = source_in_;
        steps.push_back(ldoc::AddClip{track->id, std::move(clip)});
    };

    if (has_video)
        lay_down(video);
    if (!probed.audio_codec.isEmpty())
        lay_down(audio);

    if (steps.empty()) {
        emit notice(tr("There is no unlocked track to put that on."));
        return;
    }
    run(ldoc::make_timeline_batch(
        std::move(steps),
        std::format("{} '{}'", overwrite ? "Overwrite" : "Insert",
                    QFileInfo(source_path_).fileName().toStdString())));
}

void Edit::addClipEffect(const QString& type) {
    const ldoc::Clip* clip = selectedClip_();
    if (!clip)
        return;
    const auto& catalog = lcomp::filter_catalog();
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& info) {
        return QString::fromStdString(info.type) == type;
    });
    if (found == catalog.end()) {
        emit notice(tr("There is no effect called '%1'.").arg(type));
        return;
    }

    std::vector<ldoc::Effect> effects = clip->effects;
    ldoc::Effect added;
    added.id = ldoc::Id::generate();
    added.type = found->type;
    added.name = found->name;
    effects.push_back(std::move(added));
    run(ldoc::SetClip{selected_track_, selected_clip_, clip->name, clip->gain_db,
                      std::move(effects)});
}

void Edit::removeClipEffect(const QString& id) {
    const ldoc::Clip* clip = selectedClip_();
    if (!clip)
        return;
    const ldoc::Id wanted = from_qt(id);
    std::vector<ldoc::Effect> effects;
    for (const ldoc::Effect& e : clip->effects) {
        if (e.id != wanted)
            effects.push_back(e);
    }
    if (effects.size() == clip->effects.size())
        return;
    run(ldoc::SetClip{selected_track_, selected_clip_, clip->name, clip->gain_db,
                      std::move(effects)});
}

void Edit::toggleClipEffect(const QString& id) {
    const ldoc::Clip* clip = selectedClip_();
    if (!clip)
        return;
    const ldoc::Id wanted = from_qt(id);
    std::vector<ldoc::Effect> effects = clip->effects;
    bool hit = false;
    for (ldoc::Effect& e : effects) {
        if (e.id == wanted) {
            e.enabled = !e.enabled;
            hit = true;
        }
    }
    if (!hit)
        return;
    run(ldoc::SetClip{selected_track_, selected_clip_, clip->name, clip->gain_db,
                      std::move(effects)});
}

const ldoc::Clip* Edit::selectedClip_() const {
    if (selected_clip_.is_nil())
        return nullptr;
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return nullptr;
    const ldoc::Track* track = h->document().track(selected_track_);
    return track ? track->clip(selected_clip_) : nullptr;
}

QVariantList Edit::effectCatalogue() const {
    QVariantList out;
    for (const auto& info : lcomp::filter_catalog()) {
        out.push_back(QVariantMap{{QStringLiteral("type"), QString::fromStdString(info.type)},
                                  {QStringLiteral("name"), QString::fromStdString(info.name)},
                                  {QStringLiteral("audio"), info.audio}});
    }
    return out;
}

void Edit::setInPoint() {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Timeline& t = h->document();
    // An in point at or after the out clears the out rather than being refused: the operator
    // is marking a new range, and making them clear the old one first is a step for nothing.
    const auto out = t.out_point && *t.out_point > t.playhead ? t.out_point
                                                              : std::optional<ldoc::Frame>{};
    run(ldoc::SetTimelineRange{t.playhead, out});
}

void Edit::setOutPoint() {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Timeline& t = h->document();
    const auto in = t.in_point && *t.in_point < t.playhead ? t.in_point
                                                           : std::optional<ldoc::Frame>{};
    run(ldoc::SetTimelineRange{in, t.playhead});
}

void Edit::clearRange() {
    run(ldoc::SetTimelineRange{std::nullopt, std::nullopt});
}

void Edit::addMarkerAtPlayhead() {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    ldoc::Marker m;
    m.id = ldoc::Id::generate();
    m.at = h->document().playhead;
    m.name = tr("Marker").toStdString();
    run(ldoc::AddMarker{std::move(m)});
}

// ---- transport ---------------------------------------------------------------------------

void Edit::setRate(double rate) {
    const ldoc::TimelineHistory* h = history();
    if (!h) {
        rate_ = 0.0;
        emit transportChanged();
        return;
    }
    rate_ = rate;
    if (rate_ == 0.0) {
        if (transport_)
            transport_->stop();
    } else {
        // Where and when this run started, so the playhead is computed from the clock rather
        // than accumulated from timer ticks -- which drift, and drift more under load.
        started_frame_ = h->document().playhead;
        started_ms_ = QDateTime::currentMSecsSinceEpoch();
        if (!transport_) {
            transport_ = new QTimer(this);
            // Faster than any timeline's frame rate, so the playhead lands on the right frame
            // rather than on the nearest tick of a coarser timer.
            transport_->setInterval(8);
            connect(transport_, &QTimer::timeout, this, &Edit::tick);
        }
        transport_->start();
    }
    emit transportChanged();
}

void Edit::tick() {
    ldoc::TimelineHistory* h = history();
    if (!h || rate_ == 0.0) {
        setRate(0.0);
        return;
    }
    const ldoc::Timeline& t = h->document();
    const double fps = t.fps.hz();
    if (fps <= 0.0)
        return;

    const double elapsed = (QDateTime::currentMSecsSinceEpoch() - started_ms_) / 1000.0;
    auto at = static_cast<ldoc::Frame>(started_frame_ + elapsed * fps * rate_);

    // Playback stops at the end, and at the out point when a range is set: an operator who
    // marked a range is reviewing it, and running past it is not what they asked for.
    const ldoc::Frame last = t.out_point ? *t.out_point : t.duration();
    if (at >= last && rate_ > 0.0) {
        at = last;
        setRate(0.0);
    }
    const ldoc::Frame first = t.in_point.value_or(0);
    if (at <= first && rate_ < 0.0) {
        at = first;
        setRate(0.0);
    }
    setPlayhead(static_cast<int>(std::max<ldoc::Frame>(at, 0)));
    emit levelsChanged();
}

void Edit::play() {
    if (rate_ == 0.0)
        setRate(1.0);
}

void Edit::pause() {
    setRate(0.0);
}

void Edit::togglePlay() {
    rate_ == 0.0 ? play() : pause();
}

void Edit::step(int frames) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    pause();
    setPlayhead(static_cast<int>(h->document().playhead + frames));
}

void Edit::shuttle(int direction) {
    if (direction == 0) {
        pause();
        return;
    }
    // J and L again go faster, as they do everywhere: 1, 2, 4, 8. Pressing the opposite one
    // returns to normal speed in that direction rather than slowing through the gears.
    const double current = rate_;
    const bool same_way = (current > 0) == (direction > 0) && current != 0.0;
    const double speed = same_way ? std::min(std::abs(current) * 2.0, 8.0) : 1.0;
    setRate(speed * (direction > 0 ? 1.0 : -1.0));
}

void Edit::goToStart() {
    pause();
    setPlayhead(0);
}

void Edit::goToEnd() {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    pause();
    setPlayhead(static_cast<int>(h->document().duration()));
}

void Edit::stepEdit(bool forward) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;
    const ldoc::Timeline& t = h->document();
    const ldoc::Frame at = t.playhead;

    // Every cut on every track, which is what an editor walks between: the nearest one in the
    // direction asked for, or the end of the sequence.
    ldoc::Frame best = forward ? t.duration() : 0;
    bool found = false;
    for (const ldoc::Track& track : t.tracks) {
        for (const ldoc::Clip& clip : track.clips) {
            for (const ldoc::Frame edge : {clip.start, clip.end()}) {
                if (forward ? (edge > at && (!found || edge < best))
                            : (edge < at && (!found || edge > best))) {
                    best = edge;
                    found = true;
                }
            }
        }
    }
    pause();
    setPlayhead(static_cast<int>(best));
}

QVariantList Edit::levels() const {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return {};
    const ldoc::Timeline& t = h->document();

    QVariantList out;
    for (const ldoc::Track& track : t.tracks) {
        if (track.kind != ldoc::TrackKind::Audio)
            continue;
        const bool heard = t.audible(track) && rate_ != 0.0;
        const ldoc::Clip* clip = track.clip_at(t.playhead);
        // ponytail: the level is what the document says it is, not what the samples are.
        // There is no audio engine on this tab yet; a meter driven from gain is honest about
        // routing and mute, and becomes a real meter when playback goes through one.
        const double db = heard && clip ? t.master_gain_db + track.gain_db + clip->gain_db
                                        : -60.0;
        out.append(QVariantMap{
            {QStringLiteral("track"), to_qt(track.id)},
            {QStringLiteral("name"), QString::fromStdString(track.name)},
            {QStringLiteral("db"), heard && clip ? db : -60.0},
            {QStringLiteral("gain"), track.gain_db},
            {QStringLiteral("pan"), track.pan},
            {QStringLiteral("automation"),
             QString::fromStdString(std::string{to_string(track.automation)})},
            {QStringLiteral("muted"), track.muted},
            {QStringLiteral("solo"), track.solo},
            {QStringLiteral("audible"), t.audible(track)}});
    }
    return out;
}

void Edit::appendToFirstTrack(const QString& path) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;

    // A file with no picture is audio; anything else has a picture. Most recordings have
    // both, and both halves are laid down together -- a clip whose sound stays behind on the
    // original file is not what dropping a recording on a timeline means.
    const Probe probed = probe_asset(path);
    const bool has_video = !probed.size.isEmpty();

    const ldoc::Track* video = nullptr;
    const ldoc::Track* audio = nullptr;
    for (const ldoc::Track& track : h->document().tracks) {
        if (track.locked)
            continue;
        if (!video && track.kind == ldoc::TrackKind::Video)
            video = &track;
        if (!audio && track.kind == ldoc::TrackKind::Audio)
            audio = &track;
    }

    if (has_video && video)
        appendMedia(path, to_qt(video->id));
    // The audio half only when the file has one: a silent clip on an audio track is a lie
    // about what is in the edit.
    if (!probed.audio_codec.isEmpty() && audio)
        appendMedia(path, to_qt(audio->id));

    if ((has_video && !video) || (!has_video && !audio))
        emit notice(tr("There is no unlocked %1 track to put that on.")
                        .arg(has_video ? tr("video") : tr("audio")));
}

} // namespace munkicast::ui

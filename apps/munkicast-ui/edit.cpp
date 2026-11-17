// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "edit.hpp"

#include "assetcache.hpp"
#include "document.hpp"

#include <QFileInfo>

#include <ldoc/json.hpp>
#include <ldoc/project_io.hpp>

#include <algorithm>

namespace munkicast::ui {
namespace {

QString to_qt(const ldoc::Id& id) {
    return id.is_nil() ? QString{} : QString::fromStdString(id.to_string());
}

ldoc::Id from_qt(const QString& text) {
    const auto parsed = lc::Ulid::parse(text.toStdString());
    return parsed ? *parsed : ldoc::Id{};
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
    };
}

} // namespace

Edit* Edit::create(QQmlEngine* engine, QJSEngine*) {
    auto* e = new Edit;
    if (engine) {
        // Asking the engine for it creates it if it does not exist yet, so the order these
        // singletons are first touched in does not matter.
        e->document_ = engine->singletonInstance<Document*>("MunkiCast", "Document");
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
    for (const ldoc::Track& track : t.tracks) {
        QVariantList clips;
        for (const ldoc::Clip& clip : track.clips)
            clips.append(clip_map(clip));
        tracks.append(QVariantMap{
            {QStringLiteral("id"), to_qt(track.id)},
            {QStringLiteral("name"), QString::fromStdString(track.name)},
            {QStringLiteral("kind"), QString::fromStdString(std::string{to_string(track.kind)})},
            {QStringLiteral("enabled"), track.enabled},
            {QStringLiteral("locked"), track.locked},
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
    run(ldoc::SetClip{selected_track_, selected_clip_, clip->name, db});
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

void Edit::appendToFirstTrack(const QString& path) {
    const ldoc::TimelineHistory* h = history();
    if (!h)
        return;

    // A file with no video stream belongs on an audio track; anything else goes on video. The
    // probe is the same one the clip length comes from, so this costs nothing extra.
    const Probe probed = probe_asset(path);
    const bool audio_only = probed.size.isEmpty();
    const auto wanted = audio_only ? ldoc::TrackKind::Audio : ldoc::TrackKind::Video;

    for (const ldoc::Track& track : h->document().tracks) {
        if (track.kind == wanted && !track.locked) {
            appendMedia(path, to_qt(track.id));
            return;
        }
    }
    emit notice(tr("There is no unlocked %1 track to put that on.")
                    .arg(audio_only ? tr("audio") : tr("video")));
}

} // namespace munkicast::ui

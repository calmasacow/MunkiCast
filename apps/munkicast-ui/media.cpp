// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media.hpp"

#include "assetcache.hpp"
#include "document.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <lcore/ulid.hpp>

#include <algorithm>

namespace munkicast::ui {
namespace {

/// What the table lists. Anything else in the folder is someone else's file and is not shown,
/// rather than listed as a row that cannot be played or exported.
const QStringList& media_suffixes() {
    static const QStringList kSuffixes{
        // Video.
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("flv"),
        QStringLiteral("m4v"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
        QStringLiteral("ts"), QStringLiteral("mxf"), QStringLiteral("wmv"),
        // Audio.
        QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("mp3"),
        QStringLiteral("aac"), QStringLiteral("m4a"), QStringLiteral("ogg"),
        QStringLiteral("opus"),
        // Stills.
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("bmp"),
        QStringLiteral("tiff"),
    };
    return kSuffixes;
}

struct Preset {
    const char* name;
    const char* suffix;
    /// Empty means "remux": copy both streams into a new container, which is the fast path and
    /// the one that loses nothing.
    const char* video;
    const char* audio;
    /// The NVENC equivalent, used when the machine has it. Empty where there is none.
    const char* gpu_video;
};

/// Deliberately short. Every entry here is one somebody asked for by name; a list of forty
/// codec permutations is a list nobody reads, and the recipe for an unusual target belongs in
/// the encoder flow where it can be seen.
constexpr std::array kPresets = {
    Preset{"Remux to MP4", "mp4", "", "", ""},
    Preset{"Remux to MKV", "mkv", "", "", ""},
    Preset{"H.264 MP4", "mp4", "libx264", "aac", "h264_nvenc"},
    Preset{"HEVC MP4", "mp4", "libx265", "aac", "hevc_nvenc"},
    Preset{"ProRes MOV", "mov", "prores_ks", "pcm_s16le", ""},
    Preset{"Audio only (WAV)", "wav", "", "pcm_s16le", ""},
};

const Preset* preset_named(const QString& name) {
    for (const Preset& p : kPresets) {
        if (name == QLatin1String(p.name))
            return &p;
    }
    return nullptr;
}

/// Whether this machine can encode on the GPU, asked once.
///
/// The operator's standing preference is the GPU path wherever one exists, so this decides the
/// encoder rather than offering a choice. A machine without NVENC falls back silently; the
/// output is the same file either way.
bool nvenc_available() {
    static const bool available = [] {
        QProcess probe;
        probe.start(QStringLiteral("ffmpeg"),
                    {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
        if (!probe.waitForFinished(8000) || probe.exitCode() != 0)
            return false;
        return QString::fromUtf8(probe.readAllStandardOutput())
            .contains(QLatin1String("h264_nvenc"));
    }();
    return available;
}

QString human_size(qint64 bytes) {
    constexpr qint64 kKib = 1024;
    if (bytes < kKib)
        return Media::tr("%1 B").arg(bytes);
    const double kib = static_cast<double>(bytes) / kKib;
    if (kib < kKib)
        return Media::tr("%1 KiB").arg(kib, 0, 'f', 0);
    const double mib = kib / kKib;
    if (mib < kKib)
        return Media::tr("%1 MiB").arg(mib, 0, 'f', 1);
    return Media::tr("%1 GiB").arg(mib / kKib, 0, 'f', 2);
}

QString human_duration(double seconds) {
    if (seconds <= 0.0)
        return QString{};
    const auto total = static_cast<qint64>(seconds + 0.5);
    const qint64 h = total / 3600;
    const qint64 m = (total % 3600) / 60;
    const qint64 s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3")
                       .arg(h)
                       .arg(m, 2, 10, QLatin1Char('0'))
                       .arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace

/// Runs ffprobe off the UI thread, one file at a time.
class Media::Prober : public QObject {
    Q_OBJECT

public slots:
    void probe(const QString& path) {
        MediaItem found;
        found.path = path;
        found.probed = true;

        const Probe v = probe_asset(path);
        found.width = v.size.width();
        found.height = v.size.height();
        found.duration = v.duration;
        found.codec = v.codec;

        // The audio stream and the frame rate, which the video probe does not ask for. One more
        // process rather than a second parser: ffprobe's own output is the format we already
        // read, and the cost is paid on a worker thread.
        QProcess audio;
        audio.start(QStringLiteral("ffprobe"),
                    {QStringLiteral("-v"), QStringLiteral("error"),
                     QStringLiteral("-select_streams"), QStringLiteral("a:0"),
                     QStringLiteral("-show_entries"), QStringLiteral("stream=codec_name"),
                     QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"), path});
        if (audio.waitForFinished(15000) && audio.exitCode() == 0)
            found.audioCodec = QString::fromUtf8(audio.readAllStandardOutput()).trimmed();

        QProcess rate;
        rate.start(QStringLiteral("ffprobe"),
                   {QStringLiteral("-v"), QStringLiteral("error"),
                    QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                    QStringLiteral("-show_entries"), QStringLiteral("stream=r_frame_rate"),
                    QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"), path});
        if (rate.waitForFinished(15000) && rate.exitCode() == 0) {
            // "30000/1001", which is the only honest way to write 29.97.
            const QString text = QString::fromUtf8(rate.readAllStandardOutput()).trimmed();
            const auto slash = text.indexOf(QLatin1Char('/'));
            if (slash > 0) {
                const double num = text.left(slash).toDouble();
                const double den = text.mid(slash + 1).toDouble();
                if (den > 0.0)
                    found.frameRate = num / den;
            }
        }
        emit probed(path, found);
    }

signals:
    void probed(const QString& path, const MediaItem& found);
};

Media* Media::create(QQmlEngine* engine, QJSEngine*) {
    auto* m = new Media;
    if (engine) {
        if (auto* document = engine->singletonInstance<Document*>("MunkiCast", "Document")) {
            connect(document, &Document::projectOpened, m,
                    [m](const QString& dir) { m->setProjectDirectory(dir); });
            // Created lazily, so it can miss the signal for a project already open.
            m->setProjectDirectory(document->projectDirectory());
        }
    }
    return m;
}

Media::Media(QObject* parent) : QAbstractListModel(parent) {
    prober_object_ = new Prober;
    prober_object_->moveToThread(&prober_);
    connect(&prober_, &QThread::finished, prober_object_, &QObject::deleteLater);
    connect(prober_object_, &Prober::probed, this, &Media::applyProbe);
    prober_.start();

    // A recording that finishes while the folder is open appears without being asked for.
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { scan(); });
}

Media::~Media() {
    if (running_) {
        running_->kill();
        running_->waitForFinished(2000);
    }
    // The prober may be inside an ffprobe run, where `quit()` is not seen until it returns.
    // `~QThread` aborts the process if its wait times out, so interrupt first and fall back to
    // terminating rather than taking the whole application down on the way out.
    prober_.requestInterruption();
    prober_.quit();
    if (!prober_.wait(5000))
        prober_.terminate();
    prober_.wait(1000);
}

int Media::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant Media::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
        return {};
    const MediaItem& item = items_[index.row()];
    switch (role) {
    case PathRole:
        return item.path;
    case NameRole:
        return item.name;
    case FolderRole:
        return item.folder;
    case SizeRole:
        return human_size(item.bytes);
    case ModifiedRole:
        return QDateTime::fromSecsSinceEpoch(item.modified).toString(QStringLiteral("yyyy-MM-dd hh:mm"));
    case DurationRole:
        return human_duration(item.duration);
    case ResolutionRole:
        return item.width > 0 ? QStringLiteral("%1x%2").arg(item.width).arg(item.height)
                              : QString{};
    case CodecRole:
        return item.codec;
    case ProbedRole:
        return item.probed;
    default:
        return {};
    }
}

QHash<int, QByteArray> Media::roleNames() const {
    return {
        {PathRole, "path"},         {NameRole, "name"},     {FolderRole, "folder"},
        {SizeRole, "size"},         {ModifiedRole, "modified"}, {DurationRole, "duration"},
        {ResolutionRole, "resolution"}, {CodecRole, "codec"},   {ProbedRole, "probed"},
    };
}

void Media::setProjectDirectory(const QString& dir) {
    projectDir_ = dir;
    // Straight to Recordings: on the Media tab, the overwhelmingly common errand is "play back
    // what I just recorded".
    openFolder(dir.isEmpty() ? QString{} : QStringLiteral("Recordings"));
    emit folderChanged();
}

bool Media::hasProject() const {
    return !projectDir_.isEmpty();
}

QVariantList Media::projectFolders() const {
    QVariantList rows;
    if (projectDir_.isEmpty())
        return rows;
    // The same names `ldoc::working_folders()` creates, in the same order. Written here as the
    // operator sees them rather than read back from the library, because this list is also what
    // the tree labels and one of them (Exports) is where this panel writes.
    for (const QString& name : {QStringLiteral("Recordings"), QStringLiteral("Clips"),
                                QStringLiteral("SourceMedia"), QStringLiteral("Graphics"),
                                QStringLiteral("Exports"), QStringLiteral("CacheMedia")}) {
        const QString path = QDir(projectDir_).filePath(name);
        // How many files are in it, for the tile to say so. A count, not a listing: the tile
        // shows a number and opening the folder is what produces the rows.
        const QDir dir(path);
        const int items =
            dir.exists() ? static_cast<int>(dir.entryList(QDir::Files | QDir::NoDotAndDotDot)
                                                .size())
                         : 0;
        rows.append(QVariantMap{{QStringLiteral("name"), name},
                                {QStringLiteral("path"), path},
                                {QStringLiteral("items"), items},
                                {QStringLiteral("exists"), QFileInfo::exists(path)},
                                {QStringLiteral("current"), path == folder_}});
    }
    return rows;
}

QString Media::folderLabel() const {
    if (folder_.isEmpty())
        return {};
    if (!projectDir_.isEmpty() && folder_.startsWith(projectDir_)) {
        const QString relative = QDir(projectDir_).relativeFilePath(folder_);
        if (!relative.startsWith(QLatin1String("..")))
            return relative;
    }
    return folder_;
}

void Media::openFolder(const QString& pathOrName) {
    QString path = pathOrName;
    if (!path.isEmpty() && !QDir::isAbsolutePath(path) && !projectDir_.isEmpty())
        path = QDir(projectDir_).filePath(path);
    folder_ = path;
    scan();
    emit folderChanged();
}

void Media::refresh() {
    scan();
}

void Media::setFilter(const QString& text) {
    if (filter_ == text)
        return;
    filter_ = text;
    emit filterChanged();
    scan();
}

void Media::scan() {
    // The watcher follows the folder on screen and nothing else: one directory, re-read when it
    // changes, rather than a tree of watches that outlives the panel being looked at.
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());

    QVector<MediaItem> found;
    if (!folder_.isEmpty() && QFileInfo(folder_).isDir()) {
        watcher_.addPath(folder_);
        QDir dir(folder_);
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Time);
        for (const QFileInfo& info : entries) {
            if (!media_suffixes().contains(info.suffix().toLower()))
                continue;
            if (!filter_.isEmpty() &&
                !info.fileName().contains(filter_, Qt::CaseInsensitive))
                continue;
            MediaItem item;
            item.path = info.absoluteFilePath();
            item.name = info.fileName();
            item.folder = dir.dirName();
            item.bytes = info.size();
            item.modified = info.lastModified().toSecsSinceEpoch();
            found.push_back(std::move(item));
        }
    }

    // Probe results already in hand are kept across a rescan, so a watcher firing on an
    // unrelated write does not blank the detail the table has already filled in.
    QHash<QString, MediaItem> known;
    for (const MediaItem& item : items_) {
        if (item.probed)
            known.insert(item.path, item);
    }

    const QString was_selected = selected_ >= 0 && selected_ < items_.size()
                                     ? items_[selected_].path
                                     : QString{};

    beginResetModel();
    items_ = std::move(found);
    for (MediaItem& item : items_) {
        const auto it = known.constFind(item.path);
        if (it == known.constEnd())
            continue;
        // Only when the file is unchanged: same size and same mtime. A recording still being
        // written grows, and its old duration would be a lie the table kept repeating.
        if (it->bytes == item.bytes && it->modified == item.modified) {
            const qint64 bytes = item.bytes;
            const qint64 modified = item.modified;
            item = *it;
            item.bytes = bytes;
            item.modified = modified;
        }
    }
    endResetModel();
    emit countChanged();

    // Keep the selection on the same file where it survived the rescan.
    selected_ = -1;
    if (!was_selected.isEmpty()) {
        for (int i = 0; i < items_.size(); ++i) {
            if (items_[i].path == was_selected) {
                selected_ = i;
                break;
            }
        }
    }
    emit selectedChanged();

    probing_ = -1;
    probeNext();
}

void Media::probeNext() {
    if (probe_in_flight_)
        return;
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].probed)
            continue;
        probing_ = i;
        probe_in_flight_ = true;
        QMetaObject::invokeMethod(prober_object_, "probe", Qt::QueuedConnection,
                                  Q_ARG(QString, items_[i].path));
        return;
    }
    probing_ = -1;
}

void Media::applyProbe(const QString& path, const MediaItem& found) {
    probe_in_flight_ = false;
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].path != path)
            continue;
        // Only the probed fields: the row's size and mtime came from the directory and are
        // newer than whatever the prober was told.
        items_[i].probed = true;
        items_[i].width = found.width;
        items_[i].height = found.height;
        items_[i].duration = found.duration;
        items_[i].codec = found.codec;
        items_[i].audioCodec = found.audioCodec;
        items_[i].frameRate = found.frameRate;
        const QModelIndex changed = index(i);
        emit dataChanged(changed, changed);
        if (i == selected_)
            emit selectedChanged();
        break;
    }
    probeNext();
}

void Media::setSelected(int row) {
    const int clamped = row >= 0 && row < items_.size() ? row : -1;
    if (clamped == selected_)
        return;
    selected_ = clamped;
    emit selectedChanged();
}

QVariantMap Media::selectedItem() const {
    if (selected_ < 0 || selected_ >= items_.size())
        return {};
    const MediaItem& item = items_[selected_];
    return QVariantMap{
        {QStringLiteral("path"), item.path},
        {QStringLiteral("name"), item.name},
        {QStringLiteral("folder"), item.folder},
        {QStringLiteral("size"), human_size(item.bytes)},
        {QStringLiteral("modified"),
         QDateTime::fromSecsSinceEpoch(item.modified).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))},
        {QStringLiteral("duration"), human_duration(item.duration)},
        {QStringLiteral("seconds"), item.duration},
        {QStringLiteral("resolution"),
         item.width > 0 ? QStringLiteral("%1x%2").arg(item.width).arg(item.height) : QString{}},
        {QStringLiteral("codec"), item.codec},
        {QStringLiteral("audioCodec"), item.audioCodec},
        {QStringLiteral("frameRate"),
         item.frameRate > 0.0 ? QStringLiteral("%1 fps").arg(item.frameRate, 0, 'f', 3)
                              : QString{}},
        {QStringLiteral("probed"), item.probed},
    };
}

QString Media::selectedUrl() const {
    if (selected_ < 0 || selected_ >= items_.size())
        return {};
    return QUrl::fromLocalFile(items_[selected_].path).toString();
}

QStringList Media::presets() const {
    QStringList names;
    for (const Preset& p : kPresets)
        names << QString::fromLatin1(p.name);
    return names;
}

QVariantList Media::queue() const {
    QVariantList rows;
    for (const ExportJob& job : queue_) {
        rows.append(QVariantMap{{QStringLiteral("id"), job.id},
                                {QStringLiteral("name"), QFileInfo(job.source).fileName()},
                                {QStringLiteral("destination"), job.destination},
                                {QStringLiteral("preset"), job.preset},
                                {QStringLiteral("state"), job.state},
                                {QStringLiteral("progress"), job.progress},
                                {QStringLiteral("error"), job.error}});
    }
    return rows;
}

void Media::enqueue(const QString& sourcePath, const QString& preset) {
    const Preset* recipe = preset_named(preset);
    if (!recipe) {
        emit notice(tr("There is no export preset called '%1'.").arg(preset));
        return;
    }
    if (projectDir_.isEmpty()) {
        emit notice(tr("Open a project before exporting; its Exports folder is where the file "
                       "would go."));
        return;
    }
    const QString exports = QDir(projectDir_).filePath(QStringLiteral("Exports"));
    QDir().mkpath(exports);

    const QFileInfo source(sourcePath);
    QString destination =
        QDir(exports).filePath(source.completeBaseName() + QLatin1Char('.') +
                               QLatin1String(recipe->suffix));
    // Never silently overwrite: an export that replaces last week's master because the names
    // matched is not something the operator can get back.
    int n = 2;
    while (QFileInfo::exists(destination)) {
        destination = QDir(exports).filePath(QStringLiteral("%1 (%2).%3")
                                                 .arg(source.completeBaseName())
                                                 .arg(n++)
                                                 .arg(QLatin1String(recipe->suffix)));
    }

    ExportJob job;
    job.id = QString::fromStdString(lc::Ulid::generate().to_string());
    job.source = sourcePath;
    job.destination = destination;
    job.preset = preset;
    job.state = QStringLiteral("Queued");
    queue_.push_back(std::move(job));
    emit queueChanged();
    startNextJob();
}

void Media::enqueueAll(const QString& preset) {
    for (const MediaItem& item : items_)
        enqueue(item.path, preset);
}

void Media::cancelJob(const QString& id) {
    for (ExportJob& job : queue_) {
        if (job.id != id)
            continue;
        if (job.state == QLatin1String("Queued")) {
            job.state = QStringLiteral("Cancelled");
            emit queueChanged();
            return;
        }
        if (job.state == QLatin1String("Running") && running_) {
            // terminate() first: ffmpeg closes its output file on SIGTERM, so the partial is a
            // readable file rather than a truncated one, and it is removed either way.
            running_->terminate();
            if (!running_->waitForFinished(3000))
                running_->kill();
        }
        return;
    }
}

void Media::clearFinished() {
    const auto was = queue_.size();
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [](const ExportJob& job) {
                                    return job.state != QLatin1String("Queued") &&
                                           job.state != QLatin1String("Running");
                                }),
                 queue_.end());
    if (queue_.size() != was)
        emit queueChanged();
}

void Media::startNextJob() {
    if (running_)
        return;
    const auto next = std::find_if(queue_.begin(), queue_.end(), [](const ExportJob& job) {
        return job.state == QLatin1String("Queued");
    });
    if (next == queue_.end())
        return;

    const Preset* recipe = preset_named(next->preset);
    if (!recipe) {
        next->state = QStringLiteral("Failed");
        next->error = tr("unknown preset");
        emit queueChanged();
        return;
    }

    next->state = QStringLiteral("Running");
    running_id_ = next->id;
    running_destination_ = next->destination;
    running_partial_ = next->destination + QStringLiteral(".part");
    running_duration_ = 0.0;
    for (const MediaItem& item : items_) {
        if (item.path == next->source)
            running_duration_ = item.duration;
    }
    // Not in the table, or not probed yet: ask directly, so the progress bar is a real fraction
    // rather than a spinner. One blocking probe at the start of a job that will run for minutes.
    if (running_duration_ <= 0.0)
        running_duration_ = probe_asset(next->source).duration;

    const bool gpu = recipe->gpu_video[0] != '\0' && nvenc_available();

    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-y")};
    if (gpu)
        args << QStringLiteral("-hwaccel") << QStringLiteral("cuda");
    args << QStringLiteral("-i") << next->source;

    const bool audio_only = QLatin1String(recipe->suffix) == QLatin1String("wav");
    if (audio_only) {
        args << QStringLiteral("-vn");
    } else if (recipe->video[0] == '\0') {
        // Remux: the streams are copied, which is why this takes seconds rather than minutes.
        args << QStringLiteral("-c:v") << QStringLiteral("copy");
    } else {
        args << QStringLiteral("-c:v")
             << QString::fromLatin1(gpu ? recipe->gpu_video : recipe->video);
        if (gpu)
            args << QStringLiteral("-preset") << QStringLiteral("p5")
                 << QStringLiteral("-rc") << QStringLiteral("vbr")
                 << QStringLiteral("-cq") << QStringLiteral("21");
        else if (QLatin1String(recipe->video) == QLatin1String("prores_ks"))
            args << QStringLiteral("-profile:v") << QStringLiteral("3");
        else
            args << QStringLiteral("-crf") << QStringLiteral("21")
                 << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    }

    if (recipe->audio[0] == '\0')
        args << QStringLiteral("-c:a") << QStringLiteral("copy");
    else
        args << QStringLiteral("-c:a") << QString::fromLatin1(recipe->audio);

    if (QLatin1String(recipe->suffix) == QLatin1String("mp4"))
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");

    // The container is named rather than inferred: the file being written ends in `.part`,
    // which ffmpeg has never heard of, so without this nothing is written at all.
    args << QStringLiteral("-f")
         << (QLatin1String(recipe->suffix) == QLatin1String("mkv")
                 ? QStringLiteral("matroska")
                 : QString::fromLatin1(recipe->suffix))
         << QStringLiteral("-progress") << QStringLiteral("pipe:1")
         << QStringLiteral("-nostats") << running_partial_;

    running_ = std::make_unique<QProcess>();
    running_->setProcessChannelMode(QProcess::ForwardedErrorChannel);

    connect(running_.get(), &QProcess::readyReadStandardOutput, this, [this] {
        // `-progress pipe:1` prints `key=value` lines; `out_time_ms` is the one that moves.
        const QString text = QString::fromUtf8(running_->readAllStandardOutput());
        for (const auto& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (!line.startsWith(QLatin1String("out_time_ms=")))
                continue;
            const double seconds = line.mid(12).trimmed().toDouble() / 1'000'000.0;
            if (running_duration_ <= 0.0)
                continue;
            for (ExportJob& job : queue_) {
                if (job.id != running_id_)
                    continue;
                job.progress = std::clamp(seconds / running_duration_, 0.0, 1.0);
                emit queueChanged();
            }
        }
    });

    connect(running_.get(), &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) {
                if (status == QProcess::NormalExit && code == 0) {
                    finishJob(QStringLiteral("Done"), {});
                } else {
                    // A cancel terminated it, which is not a failure to report as one.
                    const bool cancelled = status == QProcess::CrashExit;
                    finishJob(cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Failed"),
                              cancelled ? QString{}
                                        : tr("ffmpeg exited with code %1").arg(code));
                }
            });

    connect(running_.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finishJob(QStringLiteral("Failed"), tr("ffmpeg could not be started"));
    });

    emit queueChanged();
    running_->start(QStringLiteral("ffmpeg"), args);
}

void Media::finishJob(const QString& state, const QString& error) {
    if (!running_)
        return;
    // Renamed into place only on success, so a failed or cancelled export never leaves a file
    // that looks like a finished one.
    if (state == QLatin1String("Done")) {
        QFile::remove(running_destination_);
        if (!QFile::rename(running_partial_, running_destination_)) {
            for (ExportJob& job : queue_) {
                if (job.id == running_id_) {
                    job.state = QStringLiteral("Failed");
                    job.error = tr("the finished file could not be moved into Exports");
                }
            }
            QFile::remove(running_partial_);
            running_.reset();
            emit queueChanged();
            startNextJob();
            return;
        }
    } else {
        QFile::remove(running_partial_);
    }

    for (ExportJob& job : queue_) {
        if (job.id != running_id_)
            continue;
        job.state = state;
        job.error = error;
        job.progress = state == QLatin1String("Done") ? 1.0 : job.progress;
    }
    if (!error.isEmpty())
        emit notice(tr("Export failed: %1").arg(error));

    running_.reset();
    running_id_.clear();
    emit queueChanged();
    // A finished export lands in Exports; if that is the folder on screen, show it.
    if (!folder_.isEmpty() && QFileInfo(running_destination_).absolutePath() == folder_)
        scan();
    startNextJob();
}

} // namespace munkicast::ui

#include "media.moc"

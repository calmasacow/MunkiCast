// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "assetcache.hpp"

#include "preferences.hpp"

#include "lcore/ulid.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <optional>
#include <variant>

namespace libertas::ui {
namespace {

AssetCache* g_instance = nullptr;

/// The setting that names this type's file, and whether it holds a list. One table, so a type
/// can never be cacheable while nothing knows which of its settings is the path.
struct PathKey {
    const char* key;
    bool list;
};

std::optional<PathKey> path_key_for(std::string_view type) {
    if (type == "source.media")
        return PathKey{"localFile", false};
    if (type == "source.image")
        return PathKey{"file", false};
    if (type == "source.slideshow" || type == "source.playlist")
        return PathKey{"files", true};
    return std::nullopt;
}

/// A settings value as a string, or empty when the key is absent or holds something else.
QString setting_string(const ldoc::ParamMap& settings, std::string_view key) {
    const auto it = settings.find(key);
    if (it == settings.end())
        return {};
    const auto* text = std::get_if<std::string>(&it->second);
    return text ? QString::fromStdString(*text) : QString{};
}

/// A cap on the index, the same shape as the one on the encoder preset library. A file bigger
/// than this is not an index this build wrote.
constexpr qint64 kIndexMaxBytes = 8 * 1024 * 1024;

/// Copied in chunks rather than with `QFile::copy`, because the per-item progress bar needs to
/// be told how far along it is, and because cancelling has to be able to stop between chunks
/// rather than at the end of 384 MiB. 4 MiB measured no slower than the whole-file copy on the
/// reference mount, which reads at 194 MB/s.
constexpr qint64 kCopyChunk = 4 * 1024 * 1024;

/// Bumped when a recipe starts producing a different file for the same input, which makes
/// every entry written by the older one stale rather than silently kept.
constexpr int kRecipeVersion = 1;

/// Even dimensions: H.264 chroma is subsampled, and an odd width is either refused or
/// silently rounded depending on the encoder.
QSize even(QSize size) {
    return {size.width() - (size.width() % 2), size.height() - (size.height() % 2)};
}

} // namespace

Probe probe_asset(const QString& path) {
    // ffprobe rather than opening the file here: the UI has no demuxer, and the one thing this
    // needs to know -- what the stored dimensions are -- is exactly what ffprobe prints.
    QProcess probe;
    probe.start(QStringLiteral("ffprobe"),
                {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                 QStringLiteral("v:0"), QStringLiteral("-show_entries"),
                 QStringLiteral("stream=codec_name,width,height,nb_frames"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("format=duration"), QStringLiteral("-of"),
                 QStringLiteral("default=nw=1"), path});
    if (!probe.waitForFinished(15000) || probe.exitStatus() != QProcess::NormalExit ||
        probe.exitCode() != 0)
        return {};
    Probe found;
    int width = 0;
    int height = 0;
    for (const auto& line : QString::fromUtf8(probe.readAllStandardOutput())
                                .split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto split = line.indexOf(QLatin1Char('='));
        if (split < 0)
            continue;
        const auto key = line.left(split);
        const auto value = line.mid(split + 1).trimmed();
        if (key == QLatin1String("width"))
            width = value.toInt();
        else if (key == QLatin1String("height"))
            height = value.toInt();
        else if (key == QLatin1String("codec_name"))
            found.codec = value;
        else if (key == QLatin1String("duration"))
            found.duration = value.toDouble();
        else if (key == QLatin1String("nb_frames"))
            found.frames = value.toLongLong();
    }
    // Reported as found, including a still's dimensions. Whether that makes it something to
    // re-encode is `recipe_for`'s question, and it asks about the duration.
    found.size = QSize(width, height);
    return found;
}

Recipe recipe_for(const Probe& probe, QSize canvas) {
    const auto copy = Recipe{QStringLiteral("copy"), QSize{}};
    // What separates a video from a still, in the three ways a still can present itself.
    //
    // Duration alone is not enough, which a real collection caught: a 1080x1920 JPEG probes as
    // `mjpeg` with `duration=0.040000` -- one frame at 25fps -- and was re-encoded into a
    // single-frame MP4 that an image source cannot open at all. A still that is merely copied
    // is correct; a still turned into a video is a broken asset.
    static const QStringList kStillCodecs = {
        QStringLiteral("mjpeg"), QStringLiteral("png"),  QStringLiteral("bmp"),
        QStringLiteral("webp"),  QStringLiteral("tiff"), QStringLiteral("gif"),
        QStringLiteral("jpeg2000"), QStringLiteral("ppm"), QStringLiteral("targa"),
    };
    if (probe.duration <= 0.0 || probe.frames == 1 || kStillCodecs.contains(probe.codec))
        return copy;
    if (!probe.size.isValid() || probe.size.isEmpty() || !canvas.isValid() || canvas.isEmpty())
        return copy;
    if (probe.size.width() <= canvas.width() && probe.size.height() <= canvas.height())
        return copy;
    // Scaled to fit inside the canvas, keeping the aspect ratio: a 4:3 clip on a 16:9 canvas
    // is letterboxed by the compositor, and encoding it at the canvas's own aspect would bake
    // in a stretch that no later setting could undo.
    const auto target = even(probe.size.scaled(canvas, Qt::KeepAspectRatio));
    if (target.isEmpty() || target == probe.size)
        return copy;
    return {QStringLiteral("downscale"), target};
}

bool source_type_cacheable(std::string_view type) { return path_key_for(type).has_value(); }

bool source_uses_cache(const ldoc::Source& source) {
    const auto it = source.settings.find("useCache");
    if (it == source.settings.end())
        return true;
    const auto* flag = std::get_if<bool>(&it->second);
    return flag ? *flag : true;
}

QStringList source_asset_paths(const ldoc::Source& source) {
    const auto key = path_key_for(source.type);
    if (!key)
        return {};
    const auto value = setting_string(source.settings, key->key);
    if (value.isEmpty())
        return {};
    if (!key->list)
        return {value};
    // Blank lines dropped, whitespace trimmed: the field is typed into by hand and a trailing
    // newline is not an asset called "".
    QStringList paths;
    for (const QString& line : value.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            paths.push_back(trimmed);
    }
    return paths;
}

void substitute_source_paths(ldoc::Source& source, const AssetCache& cache) {
    const auto key = path_key_for(source.type);
    if (!key || !source_uses_cache(source))
        return;
    const auto paths = source_asset_paths(source);
    if (paths.isEmpty())
        return;
    if (!key->list) {
        const auto prepared = cache.preparedPath(paths.front());
        if (!prepared.isEmpty())
            source.settings.insert_or_assign(std::string{key->key}, prepared.toStdString());
        return;
    }
    // Rewritten line by line, keeping order and count: a playlist is an ordered thing, and an
    // asset with no prepared copy keeps its own path in place rather than being dropped.
    QStringList substituted;
    bool any = false;
    for (const QString& path : paths) {
        const auto prepared = cache.preparedPath(path);
        any = any || !prepared.isEmpty();
        substituted.push_back(prepared.isEmpty() ? path : prepared);
    }
    if (any)
        source.settings.insert_or_assign(
            std::string{key->key},
            substituted.join(QLatin1Char('\n')).toStdString());
}

AssetCache::AssetCache(QObject* parent) : QObject(parent) {
    g_instance = this;
}

AssetCache::~AssetCache() {
    // request_stop before the members are destroyed, so a worker mid-copy stops at its next
    // chunk rather than being joined for the length of one.
    worker_.request_stop();
    sweeper_.request_stop();
    if (g_instance == this)
        g_instance = nullptr;
}

AssetCache* AssetCache::create(QQmlEngine*, QJSEngine*) { return new AssetCache(nullptr); }

AssetCache* AssetCache::instance() { return g_instance; }

// Asked of Preferences rather than mirrored, so there is one stored flag and no way for the
// two to disagree after a failed write.
bool AssetCache::enabled() const { return caching_enabled(); }

QString AssetCache::dir() const { return cache_dir(); }

QString AssetCache::indexPath() const {
    const auto base = dir();
    return base.isEmpty() ? QString{} : base + QStringLiteral("/index.json");
}

QString AssetCache::absolute(const Entry& e) const {
    const auto base = dir();
    return base.isEmpty() || e.prepared.isEmpty() ? QString{}
                                                  : base + QLatin1Char('/') + e.prepared;
}

double AssetCache::overallProgress() const {
    if (items_total_ <= 0)
        return 0.0;
    const double done = static_cast<double>(items_done_) + item_progress_;
    return std::clamp(done / static_cast<double>(items_total_), 0.0, 1.0);
}

qint64 AssetCache::bytesHeld() const {
    qint64 total = 0;
    for (const auto& [original, entry] : index_)
        total += entry.preparedSize;
    return total;
}

void AssetCache::reload() {
    index_.clear();
    stamps_.clear();
    const auto path = indexPath();
    QFile file(path);
    if (!path.isEmpty() && file.open(QIODevice::ReadOnly) && file.size() <= kIndexMaxBytes) {
        const auto json = QJsonDocument::fromJson(file.readAll()).object();
        if (json.value(QStringLiteral("version")).toInt() == 1) {
            const auto entries = json.value(QStringLiteral("entries")).toObject();
            for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
                const auto object = it.value().toObject();
                Entry entry;
                entry.prepared = object.value(QStringLiteral("prepared")).toString();
                entry.recipe = object.value(QStringLiteral("recipe")).toString();
                entry.recipeVersion = object.value(QStringLiteral("recipeVersion")).toInt(1);
                entry.originalMtimeMs =
                    static_cast<qint64>(object.value(QStringLiteral("mtimeMs")).toDouble());
                entry.originalSize =
                    static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
                entry.preparedSize =
                    static_cast<qint64>(object.value(QStringLiteral("preparedSize")).toDouble());
                entry.preparedFor = QSize(object.value(QStringLiteral("forWidth")).toInt(),
                                          object.value(QStringLiteral("forHeight")).toInt());
                entry.error = object.value(QStringLiteral("error")).toString();
                // A name, never a path: an index that could name `../../etc` would make a
                // delete of the cache a delete of something else.
                if (!entry.prepared.contains(QLatin1Char('/')) && !it.key().isEmpty())
                    index_.insert_or_assign(it.key(), std::move(entry));
            }
        }
    }
    resweep();
    emit changed();
}

bool AssetCache::writeIndex() {
    const auto base = dir();
    if (base.isEmpty() || !QDir().mkpath(base))
        return false;
    QJsonObject entries;
    for (const auto& [original, entry] : index_) {
        entries.insert(original,
                       QJsonObject{{"prepared", entry.prepared},
                                   {"recipe", entry.recipe},
                                   {"recipeVersion", entry.recipeVersion},
                                   {"mtimeMs", static_cast<double>(entry.originalMtimeMs)},
                                   {"size", static_cast<double>(entry.originalSize)},
                                   {"preparedSize", static_cast<double>(entry.preparedSize)},
                                   {"forWidth", entry.preparedFor.width()},
                                   {"forHeight", entry.preparedFor.height()},
                                   {"error", entry.error}});
    }
    // QSaveFile, as the encoder preset library does: an index half-written over a power cut
    // would describe files that are not there, which reads as a corrupt cache rather than an
    // absent one.
    QSaveFile file(indexPath());
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 1}, {"entries", entries}}).toJson();
    return bytes.size() <= kIndexMaxBytes && file.open(QIODevice::WriteOnly) &&
           file.write(bytes) == bytes.size() && file.commit();
}

void AssetCache::resweep() {
    // Asked to stop before the assignment below, which joins it. The loop checks the token
    // between files, so the join costs at most one outstanding stat -- 10-14 ms on the mount
    // these files live on, once, rather than the whole sweep.
    sweeper_.request_stop();
    QStringList paths;
    for (const auto& [original, entry] : index_) {
        paths.push_back(original);
        if (const auto prepared = absolute(entry); !prepared.isEmpty())
            paths.push_back(prepared);
    }
    for (const QString& original : wanted_)
        if (!index_.contains(original))
            paths.push_back(original);
    if (paths.isEmpty()) {
        stamps_.clear();
        return;
    }
    // Off-thread for the reason commit fac75f6 exists: a stat on the gvfs/SMB mount these
    // files live on intermittently took 10-14 ms, which stalled a frame. One sweep, one
    // posted answer, so a 27-asset show costs one queued call rather than 54.
    sweeper_ = std::jthread([this, paths](std::stop_token stop) {
        QVariantMap found;
        for (const QString& path : paths) {
            if (stop.stop_requested())
                return;
            const QFileInfo info(path);
            if (info.exists() && info.isFile())
                found.insert(path, QVariantList{info.lastModified().toMSecsSinceEpoch(),
                                                static_cast<qint64>(info.size())});
            else
                found.insert(path, QVariantList{});
        }
        if (stop.stop_requested())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, found] {
                for (auto it = found.constBegin(); it != found.constEnd(); ++it) {
                    const auto pair = it.value().toList();
                    Stamp stamp;
                    stamp.exists = pair.size() == 2;
                    if (stamp.exists) {
                        stamp.mtimeMs = pair.at(0).toLongLong();
                        stamp.size = pair.at(1).toLongLong();
                    }
                    stamps_.insert_or_assign(it.key(), stamp);
                }
                emit changed();
            },
            Qt::QueuedConnection);
    });
}

QString AssetCache::preparedPath(const QString& original) const {
    if (!enabled())
        return {};
    const auto it = index_.find(original);
    if (it == index_.end() || !it->second.error.isEmpty())
        return {};
    const auto prepared = absolute(it->second);
    if (prepared.isEmpty())
        return {};
    // A prepared file the sweep has not reached yet is used: the index says it was written, and
    // refusing it until the stat lands would send the mount's path for the first frames of a
    // show. A missing one is caught by the sweep a moment later, and by the renderer's own
    // open failing, which is what it would do for a missing original too.
    const auto stamp = stamps_.find(prepared);
    if (stamp != stamps_.end() && !stamp->second.exists)
        return {};
    return prepared;
}

QVariantMap AssetCache::stateOf(const QString& original) const {
    const auto make = [](const char* state, const QString& reason, const QString& remedy) {
        return QVariantMap{{"state", QString::fromLatin1(state)},
                           {"reason", reason},
                           {"remedy", remedy}};
    };
    if (!enabled())
        return make("off", tr("Caching is off, so this source reads its original path."),
                    tr("Turn it on in the Caching menu, then build the cache."));
    const auto it = index_.find(original);
    if (it == index_.end())
        return make("error", tr("This file has no prepared copy yet."),
                    tr("Caching > Build Cache prepares it. Until then the file is read from "
                       "where it lives."));
    if (!it->second.error.isEmpty())
        return make("error", tr("Preparing this file failed: %1").arg(it->second.error),
                    tr("The source is reading its original path. Caching > Rebuild Cache tries "
                       "again."));
    const auto prepared = absolute(it->second);
    const auto preparedStamp = stamps_.find(prepared);
    if (preparedStamp != stamps_.end() && !preparedStamp->second.exists)
        return make("error", tr("The prepared copy is gone from the cache directory."),
                    tr("Caching > Rebuild Cache writes it again. The source is reading its "
                       "original path meanwhile."));
    const auto originalStamp = stamps_.find(original);
    if (originalStamp != stamps_.end() && originalStamp->second.exists &&
        (originalStamp->second.mtimeMs != it->second.originalMtimeMs ||
         originalStamp->second.size != it->second.originalSize))
        return make("stale",
                    tr("The original has changed since this copy was prepared, so the show is "
                       "playing the older version."),
                    tr("Caching > Rebuild Cache prepares it again. A show in progress keeps "
                       "the copy it started with rather than switching mid-show."));
    // A downscaled copy is made for a particular number of pixels. Resize the canvas and it is
    // the wrong clip -- too small, which the compositor would have to upscale. A byte copy has
    // no such tie, and `preparedFor` is left invalid for one.
    if (it->second.recipeVersion != kRecipeVersion)
        return make("stale",
                    tr("This copy was prepared by an older version of the preparation step."),
                    tr("Caching > Rebuild Cache prepares it the current way."));
    if (it->second.preparedFor.isValid() && !it->second.preparedFor.isEmpty() &&
        canvas_.isValid() && !canvas_.isEmpty() && it->second.preparedFor != canvas_)
        return make("stale",
                    tr("This copy was prepared for a %1x%2 canvas, and the show is now %3x%4.")
                        .arg(it->second.preparedFor.width())
                        .arg(it->second.preparedFor.height())
                        .arg(canvas_.width())
                        .arg(canvas_.height()),
                    tr("Caching > Rebuild Cache prepares it at the size this show needs."));
    return make("ok", tr("A prepared copy is in use, read from local disk."), QString{});
}

QVariantMap AssetCache::worstStateOf(const QStringList& originals) const {
    if (originals.isEmpty())
        return stateOf(QString{});
    // Severity order, so one broken clip in a playlist of twelve is what the row reports. A
    // row that showed "ok" because eleven were fine is a row that hides the one that matters.
    static const std::array<QString, 4> kOrder = {QStringLiteral("error"),
                                                  QStringLiteral("stale"),
                                                  QStringLiteral("off"), QStringLiteral("ok")};
    QVariantMap worst;
    std::size_t worstRank = kOrder.size();
    for (const QString& original : originals) {
        if (original.isEmpty())
            continue;
        auto state = stateOf(original);
        const auto name = state.value(QStringLiteral("state")).toString();
        const auto found = std::ranges::find(kOrder, name);
        const auto rank = static_cast<std::size_t>(std::distance(kOrder.begin(), found));
        if (rank < worstRank) {
            worstRank = rank;
            worst = std::move(state);
        }
    }
    return worst.isEmpty() ? stateOf(QString{}) : worst;
}

void AssetCache::setWanted(QStringList originals, QSize canvas) {
    originals.removeDuplicates();
    if (originals == wanted_ && canvas == canvas_)
        return;
    const bool pathsChanged = originals != wanted_;
    wanted_ = std::move(originals);
    canvas_ = canvas;
    // Only the paths need re-statting. A canvas resize changes what `stateOf` concludes from
    // stamps it already has, so sweeping again for it would be 27 stats on a slow mount for an
    // answer that is already in memory.
    if (pathsChanged)
        resweep();
    emit changed();
}

int AssetCache::pendingCount() const {
    int pending = 0;
    for (const QString& original : wanted_) {
        const auto state = stateOf(original).value(QStringLiteral("state")).toString();
        if (state == QLatin1String("error") || state == QLatin1String("stale"))
            ++pending;
    }
    return pending;
}

bool AssetCache::build(bool rebuild) {
    return build(rebuild, wanted_);
}

bool AssetCache::build(bool rebuild, QStringList originals) {
    if (building())
        return false;
    const auto base = dir();
    if (base.isEmpty() || !QDir().mkpath(base)) {
        emit notice(tr("The cache directory could not be created: %1").arg(base));
        return false;
    }

    // Decided here, on this thread, from the index and the stamps the sweep already took. The
    // worker is handed a finished list so it never asks the document anything.
    struct Job {
        QString original;
        /// The ULID stem. The extension is decided in the worker, once the probe has said
        /// whether this is a copy (keep the original's) or a re-encode (.mp4).
        QString stem;
    };
    std::vector<Job> jobs;
    originals.removeDuplicates();
    for (const QString& original : originals) {
        if (original.isEmpty())
            continue;
        const auto state = stateOf(original).value(QStringLiteral("state")).toString();
        const bool needed = state != QLatin1String("ok");
        if (!needed && !rebuild)
            continue;
        // A fresh name on every preparation, rather than overwriting in place: the old copy
        // stays readable until the new one is committed, so a rebuild during a show never
        // leaves a source pointed at a half-written file.
        jobs.push_back({original, QString::fromStdString(lc::Ulid::generate().to_string())});
    }
    if (jobs.empty()) {
        emit notice(tr("Every asset already has a prepared copy."));
        return false;
    }

    items_total_ = static_cast<int>(jobs.size());
    items_done_ = 0;
    item_progress_ = 0.0;
    current_item_.clear();
    emit progressed();

    worker_ = std::jthread([this, base, jobs, canvas = canvas_](std::stop_token stop) {
        int prepared = 0;
        int failed = 0;
        // Said once per item, from the worker, so the bar moves without the caller having to
        // know which of the two paths below is running.
        const auto say_progress = [this](double fraction) {
            QMetaObject::invokeMethod(
                this,
                [this, fraction] {
                    item_progress_ = fraction;
                    emit progressed();
                },
                Qt::QueuedConnection);
        };
        for (const Job& job : jobs) {
            if (stop.stop_requested())
                break;
            const QFileInfo source(job.original);
            QMetaObject::invokeMethod(
                this,
                [this, name = source.fileName()] {
                    current_item_ = name;
                    item_progress_ = 0.0;
                    emit progressed();
                },
                Qt::QueuedConnection);

            // Probed per asset rather than once up front: a probe is a process launch, and
            // doing them all first would stall the build before the bar had moved.
            const auto probe = probe_asset(job.original);
            const auto recipe = recipe_for(probe, canvas);
            const bool transcoding = recipe.name == QLatin1String("downscale");

            // Written to `.part` and renamed, so an interrupted preparation is never mistaken
            // for a finished one by the next run's index read.
            const QString name =
                job.stem + QLatin1Char('.') +
                (transcoding ? QStringLiteral("mp4") : source.suffix());
            const QString target = base + QLatin1Char('/') + name;
            const QString partial = target + QStringLiteral(".part");
            QString error;
            qint64 writtenBytes = 0;

            if (transcoding) {
                // Twice, if the first one fails: on the GPU, then on the CPU.
                //
                // The GPU pass keeps decode and scale in VRAM, which is what made the measured
                // 104 s 4K clip take 31 s rather than minutes. But NVDEC decodes a fixed list
                // of codecs, and an oversized asset in something it does not know -- ffv1,
                // ProRes, anything an editor exported -- fails there and succeeds in software.
                // Falling back is slower and still worth it: the gain being bought is in the
                // *playback* conversion cost, which does not care how the file was made.
                for (int attempt = 0; attempt < 2 && !stop.stop_requested(); ++attempt) {
                    const bool gpu = attempt == 0;
                    error.clear();
                    QStringList args{QStringLiteral("-nostdin"), QStringLiteral("-y"),
                                     QStringLiteral("-v"), QStringLiteral("error")};
                    if (gpu)
                        args << QStringLiteral("-hwaccel") << QStringLiteral("cuda")
                             << QStringLiteral("-hwaccel_output_format") << QStringLiteral("cuda");
                    // The pixel format is set inside `scale_cuda` on the GPU path and with
                    // `-pix_fmt` on the CPU one. They are not interchangeable: frames leaving
                    // `scale_cuda` are CUDA frames, and a `-pix_fmt yuv420p` against those is
                    // refused with "Function not implemented", which leaves NVENC unable to
                    // open and the output file empty.
                    args << QStringLiteral("-i") << job.original << QStringLiteral("-vf")
                         << (gpu ? QStringLiteral("scale_cuda=%1:%2:format=yuv420p")
                                       .arg(recipe.target.width())
                                       .arg(recipe.target.height())
                                 : QStringLiteral("scale=%1:%2")
                                       .arg(recipe.target.width())
                                       .arg(recipe.target.height()))
                         // H.264 rather than HEVC: NVDEC and NVENC handle both, but H.264 is
                         // what Qt's FFmpeg backend opens most reliably, and at canvas sizes
                         // the bitrate saving does not pay for the risk (ADR-0069).
                         << QStringLiteral("-c:v")
                         << (gpu ? QStringLiteral("h264_nvenc") : QStringLiteral("libx264"))
                         << QStringLiteral("-preset") << (gpu ? QStringLiteral("p5")
                                                              : QStringLiteral("veryfast"));
                    if (gpu)
                        args << QStringLiteral("-rc") << QStringLiteral("vbr")
                             << QStringLiteral("-cq") << QStringLiteral("21");
                    else
                        // yuv420p explicitly: a 10-bit or 4:4:4 source would otherwise produce
                        // a profile Qt's decoder opens but the compositor's YUV path does not
                        // expect.
                        args << QStringLiteral("-crf") << QStringLiteral("21")
                             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
                    args
                        // Audio copied rather than re-encoded: nothing about it is oversized,
                        // and re-encoding would be a second quality loss for no gain.
                        << QStringLiteral("-c:a") << QStringLiteral("copy")
                        << QStringLiteral("-movflags") << QStringLiteral("+faststart")
                        << QStringLiteral("-progress") << QStringLiteral("pipe:1")
                        << QStringLiteral("-nostats")
                        // Named, not guessed. The output is written to `<ulid>.mp4.part`, and
                        // ffmpeg infers the container from the extension -- which is `.part`,
                        // which it has never heard of, so without this nothing is written at
                        // all and the failure reads as an encoder problem.
                        << QStringLiteral("-f") << QStringLiteral("mp4") << partial;

                    QProcess ffmpeg;
                    ffmpeg.setProcessChannelMode(QProcess::ForwardedErrorChannel);
                    ffmpeg.start(QStringLiteral("ffmpeg"), args);
                    if (!ffmpeg.waitForStarted(10000)) {
                        error = tr("ffmpeg could not be started");
                        break;
                    }
                    while (ffmpeg.state() != QProcess::NotRunning) {
                        if (stop.stop_requested()) {
                            // terminate first, kill only if it will not go: an NVENC session
                            // killed outright can leave the encoder busy for the next asset.
                            ffmpeg.terminate();
                            if (!ffmpeg.waitForFinished(3000))
                                ffmpeg.kill();
                            ffmpeg.waitForFinished(2000);
                            break;
                        }
                        if (!ffmpeg.waitForReadyRead(250))
                            continue;
                        // `-progress pipe:1` prints `key=value` lines; `out_time_us` against
                        // the probed duration is the only one this needs.
                        for (const auto& line :
                             QString::fromUtf8(ffmpeg.readAllStandardOutput())
                                 .split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                            if (!line.startsWith(QLatin1String("out_time_us=")) ||
                                probe.duration <= 0.0)
                                continue;
                            const auto micros = line.mid(12).trimmed().toDouble();
                            say_progress(std::clamp(micros / 1e6 / probe.duration, 0.0, 1.0));
                        }
                    }
                    if (stop.stop_requested())
                        break;
                    ffmpeg.waitForFinished(10000);
                    if (ffmpeg.exitStatus() == QProcess::NormalExit && ffmpeg.exitCode() == 0) {
                        writtenBytes = QFileInfo(partial).size();
                        break;
                    }
                    QFile::remove(partial);
                    // Only the second attempt's failure is final. The GPU's is expected for a
                    // codec NVDEC does not decode, and for a target below NVENC's minimum
                    // frame size -- saying so would report a failure the next pass recovers
                    // from, in software, which is why there is a next pass.
                    error = gpu ? tr("the GPU could not decode it; retrying in software")
                                : tr("ffmpeg could not re-encode it at canvas size");
                    say_progress(0.0);
                }
            } else {
                QFile in(job.original);
                QFile out(partial);
                if (!in.open(QIODevice::ReadOnly))
                    error = tr("cannot read the original");
                else if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    error = tr("cannot write into the cache directory");
                else {
                    const qint64 total = in.size();
                    QByteArray chunk;
                    while (error.isEmpty() && !in.atEnd()) {
                        if (stop.stop_requested())
                            break;
                        chunk = in.read(kCopyChunk);
                        if (chunk.isEmpty() && !in.atEnd()) {
                            error = tr("the original stopped being readable part way through");
                            break;
                        }
                        if (out.write(chunk) != chunk.size()) {
                            error = tr("the cache directory ran out of room");
                            break;
                        }
                        writtenBytes += chunk.size();
                        say_progress(total > 0 ? static_cast<double>(writtenBytes) /
                                                     static_cast<double>(total)
                                               : 1.0);
                    }
                    if (error.isEmpty() && !out.flush())
                        error = tr("the copy could not be flushed to disk");
                }
            }

            const bool cancelled = stop.stop_requested();
            if (cancelled || !error.isEmpty()) {
                QFile::remove(partial);
                if (cancelled)
                    break;
            } else if (!QFile::rename(partial, target)) {
                QFile::remove(partial);
                error = tr("the finished copy could not be put in place");
            }

            if (error.isEmpty())
                ++prepared;
            else
                ++failed;

            QMetaObject::invokeMethod(
                this,
                [this, original = job.original, name, error, writtenBytes,
                 recipe, mtime = source.lastModified().toMSecsSinceEpoch(),
                 size = static_cast<qint64>(source.size())] {
                    // A failure is recorded, not dropped. The entry is what makes the row red
                    // and the state dialog able to say why; without it the row would say
                    // "never built", which is a different problem with a different remedy.
                    Entry entry;
                    entry.prepared = error.isEmpty() ? name : QString{};
                    entry.recipe = recipe.name;
                    entry.recipeVersion = kRecipeVersion;
                    entry.originalMtimeMs = mtime;
                    entry.originalSize = size;
                    entry.preparedSize = error.isEmpty() ? writtenBytes : 0;
                    entry.preparedFor = recipe.target;
                    entry.error = error;
                    index_.insert_or_assign(original, std::move(entry));
                    ++items_done_;
                    item_progress_ = 0.0;
                    emit progressed();
                },
                Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(
            this,
            [this, cancelled = stop.stop_requested(), prepared, failed] {
                finishWorker(cancelled, prepared, failed);
            },
            Qt::QueuedConnection);
    });
    emit progressed();
    return true;
}

void AssetCache::finishWorker(bool cancelled, int prepared, int failed) {
    // Joined, not detached. This is a queued call, so it runs on this object's thread rather
    // than the worker's -- and the worker posted it as its last statement, so the join returns
    // at once. Detaching instead would leave a thread nothing could wait for at shutdown.
    if (worker_.joinable())
        worker_.join();
    worker_ = {};
    current_item_.clear();
    item_progress_ = 0.0;
    if (!writeIndex())
        emit notice(tr("The cache index could not be written, so this build will not be "
                       "remembered."));
    else if (cancelled)
        emit notice(tr("Cache build cancelled. %n file(s) were prepared before stopping.", "",
                       prepared));
    else if (failed > 0)
        emit notice(tr("Cache build finished: %1 prepared, %2 could not be.")
                        .arg(prepared)
                        .arg(failed));
    else
        emit notice(tr("Cache build finished: %n file(s) prepared.", "", prepared));
    resweep();
    emit progressed();
    emit changed();
}

void AssetCache::cancel() {
    if (building())
        worker_.request_stop();
}

bool AssetCache::clear() {
    if (building())
        return false;
    bool whole = true;
    const auto base = dir();
    // Only files this index named, plus the index. Never the directory itself: the operator
    // may have pointed it at something that holds more than this -- and a recursive delete of
    // a configurable path is the one mistake in this feature that cannot be undone.
    for (const auto& [original, entry] : index_) {
        const auto prepared = absolute(entry);
        if (!prepared.isEmpty() && QFile::exists(prepared) && !QFile::remove(prepared))
            whole = false;
    }
    if (!base.isEmpty()) {
        for (const QFileInfo& leftover :
             QDir(base).entryInfoList({QStringLiteral("*.part")}, QDir::Files))
            QFile::remove(leftover.absoluteFilePath());
    }
    const auto path = indexPath();
    if (!path.isEmpty() && QFile::exists(path) && !QFile::remove(path))
        whole = false;
    index_.clear();
    items_total_ = 0;
    items_done_ = 0;
    resweep();
    emit progressed();
    emit changed();
    if (!whole)
        emit notice(tr("Some prepared files could not be deleted; check the cache directory."));
    return whole;
}

} // namespace libertas::ui

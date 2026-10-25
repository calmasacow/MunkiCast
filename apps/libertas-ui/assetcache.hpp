// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

class AssetCache;

/// Whether a source type keeps its content in files that are worth copying locally.
///
/// One answer, in one place, for the three callers that must not drift: the schema tables
/// (which offer the "Use caching" checkbox), the row builder (which draws the indicator) and
/// `Preview::publish` (which substitutes the path).
///
/// `source.v4l2` is deliberately absent, and so is every other live type: its device key is a
/// device node, not a file, and copying `/dev/video0` is not a thing that finishes.
[[nodiscard]] bool source_type_cacheable(std::string_view type);

/// Whether this source's own "Use caching" setting is on. True by default, matching the
/// schema: a source added before this build existed has no such key, and the operator who
/// turned caching on meant all of it.
[[nodiscard]] bool source_uses_cache(const ldoc::Source& source);

/// Every file this source reads, absolute paths, in settings order. Empty for a type that is
/// not cacheable. A playlist or slideshow returns each line of its list.
[[nodiscard]] QStringList source_asset_paths(const ldoc::Source& source);

/// What a prepared copy of this file should be: a byte copy, or a re-encode at canvas size.
///
/// A free function, and probing is its own step, so the decision can be tested without a
/// build: the answer for a given file and canvas is a fact about them, not about the cache.
struct Recipe {
    /// "copy" or "downscale".
    QString name;
    /// The size to encode at. Invalid for a copy.
    QSize target;

    friend bool operator==(const Recipe&, const Recipe&) = default;
};

/// What ffprobe found.
struct Probe {
    QSize size;
    QString codec;
    /// Seconds, for turning ffmpeg's progress into a fraction. 0 when unknown.
    double duration = 0.0;
    /// Frames in the stream, 0 when the container does not say. A still is 1.
    qint64 frames = 0;

    friend bool operator==(const Probe&, const Probe&) = default;
};

/// Runs ffprobe. Blocking, and called on the build thread only.
[[nodiscard]] Probe probe_asset(const QString& path);

/// Downscale when the asset is a video larger than the canvas in either direction; copy
/// otherwise.
///
/// Measured on the reference collection: the 4K trailer at 3840x2160 against a 2560x1440
/// canvas is a 33.2 MB RGBA frame against 14.7 MB, which is 2.25x the per-frame conversion
/// work for pixels the canvas cannot show. Downscaling it took 31 s for a 104 s clip. Nothing
/// at or under canvas size is touched: NVDEC already handles those, and re-encoding them would
/// spend quality on no gain.
[[nodiscard]] Recipe recipe_for(const Probe& probe, QSize canvas);

/// Points this source's path settings at their prepared copies, where one is usable.
///
/// Called on the copy of the project that goes to the renderer, never on the document, which
/// is what keeps the saved project full of the operator's own paths and makes turning caching
/// off a flag rather than a migration. A path with no usable copy is left exactly as it was,
/// so one failed asset costs that asset and nothing else.
void substitute_source_paths(ldoc::Source& source, const AssetCache& cache);

/// Local prepared copies of a show's file-backed assets, so a show runs off local disk rather
/// than off the mount the files were authored on.
///
/// Why this exists: the reference collection is 5.92 GiB read live from a gvfs/SMB mount, and
/// a stat on that mount intermittently took 10-14 ms. Commit fac75f6 moved the slow stats off
/// the frame; the reads were still on the mount. This moves the bytes.
///
/// Substitution happens in one place only -- the copy of the project `Preview::publish` sends
/// to the renderer -- so the saved project keeps the operator's own paths and turning caching
/// off is a flag, not a migration. `libertas-render` and `libertas-source` need no new path
/// logic: they receive the prepared path as if it had been typed into the settings field.
class AssetCache : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// Whether prepared copies are used at all. Mirrors Preferences, which owns the stored flag.
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    /// True while a build is running; the menu entries that would disturb it are disabled.
    Q_PROPERTY(bool building READ building NOTIFY progressed)
    /// 0..1 across the whole build, and 0..1 through the current item.
    Q_PROPERTY(double overallProgress READ overallProgress NOTIFY progressed)
    Q_PROPERTY(double itemProgress READ itemProgress NOTIFY progressed)
    /// The file being prepared right now, for the line of text under the bars.
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressed)
    Q_PROPERTY(int itemsDone READ itemsDone NOTIFY progressed)
    Q_PROPERTY(int itemsTotal READ itemsTotal NOTIFY progressed)
    /// Bytes the cache directory holds, as the index accounts for it.
    Q_PROPERTY(qint64 bytesHeld READ bytesHeld NOTIFY changed)

  public:
    explicit AssetCache(QObject* parent = nullptr);
    ~AssetCache() override;

    static AssetCache* create(QQmlEngine*, QJSEngine*);
    /// The live singleton, for the two C++ callers that are not QML. Null before the engine
    /// has instantiated it.
    static AssetCache* instance();

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool building() const { return worker_.joinable(); }
    [[nodiscard]] double overallProgress() const;
    [[nodiscard]] double itemProgress() const { return item_progress_; }
    [[nodiscard]] QString currentItem() const { return current_item_; }
    [[nodiscard]] int itemsDone() const { return items_done_; }
    [[nodiscard]] int itemsTotal() const { return items_total_; }
    [[nodiscard]] qint64 bytesHeld() const;

    /// Re-reads the index from the configured directory. Called at startup and whenever
    /// Preferences changes the directory or the flag.
    void reload();

    /// The prepared file for an original, or empty when there is none to use -- caching off,
    /// never built, failed, or the entry's own file has gone. Never stats: the answer comes
    /// from the index and the memoised stamps, because this is called once per asset inside
    /// `Preview::publish`, which runs on a document change.
    [[nodiscard]] QString preparedPath(const QString& original) const;

    /// What the indicator should say for one original path: `state` is one of "off", "ok",
    /// "stale", "error"; `reason` and `remedy` are the sentences the state dialog shows.
    Q_INVOKABLE [[nodiscard]] QVariantMap stateOf(const QString& original) const;
    /// The worst state across a list of originals, for a playlist or slideshow row. Order of
    /// severity: error, stale, ok, off.
    Q_INVOKABLE [[nodiscard]] QVariantMap worstStateOf(const QStringList& originals) const;

    /// The originals a build should prepare, and the size to prepare them for. Set from the
    /// document on every change, so the menu can start a build without the cache knowing what
    /// a project is.
    ///
    /// The size is the largest canvas in the project, not the one being looked at: a clip
    /// prepared for a 1080p canvas is the wrong clip for the 1440p canvas beside it, and
    /// preparing for the larger of the two is the choice that never upscales.
    void setWanted(QStringList originals, QSize canvas);
    [[nodiscard]] const QStringList& wanted() const { return wanted_; }

    /// Prepares every wanted asset. `rebuild` re-prepares entries that are already ok;
    /// otherwise only missing, stale and failed ones are done. False when nothing needs doing
    /// or a build is already running.
    Q_INVOKABLE bool build(bool rebuild);
    /// Prepare only these files. An empty list never expands to the whole project.
    Q_INVOKABLE bool build(bool rebuild, QStringList originals);
    /// Asks the running build to stop. The asset in flight is abandoned and its `.part` file
    /// removed; assets already prepared stay prepared.
    Q_INVOKABLE void cancel();
    /// Removes every prepared file and the index. False when the directory cannot be written.
    Q_INVOKABLE bool clear();

    /// How many wanted assets are not currently usable, for the menu's enabled state.
    Q_INVOKABLE [[nodiscard]] int pendingCount() const;

  signals:
    void changed();
    void progressed();
    /// Said out loud in the notice log: finished, cancelled, or could not start.
    void notice(const QString& text);

  private:
    struct Entry {
        QString prepared; ///< File name inside the cache directory, not a path.
        /// "copy" or "downscale". What was done, so a change of mind about what a file needs
        /// shows up as staleness rather than as a file that silently stays wrong.
        QString recipe = "copy";
        int recipeVersion = 1;
        qint64 originalMtimeMs = 0;
        qint64 originalSize = 0;
        qint64 preparedSize = 0;
        /// The canvas this was prepared for, 0x0 for a copy. A resized canvas makes a
        /// downscaled copy stale: it is the wrong number of pixels for the show it is in.
        QSize preparedFor;
        QString error;
    };
    /// What the last sweep found on disk, for originals and prepared files alike, so that
    /// `stateOf` and `preparedPath` never stat. Both are asked once per asset on every
    /// document change, and a stat on the mount these files live on took 10-14 ms.
    struct Stamp {
        qint64 mtimeMs = 0;
        qint64 size = 0;
        bool exists = false;
    };

    [[nodiscard]] QString dir() const;
    [[nodiscard]] QString indexPath() const;
    [[nodiscard]] QString absolute(const Entry& e) const;
    bool writeIndex();
    /// Restarts the off-thread stat sweep over the indexed originals and prepared files.
    /// Posts its answer back as one map, rather than one signal per file.
    void resweep();
    /// Joins the finished build thread and says how it went. Posted from the worker's own end,
    /// because a jthread cannot join itself.
    void finishWorker(bool cancelled, int prepared, int failed);

    std::map<QString, Entry> index_;
    /// Keyed by absolute path: the originals the index knows, and the prepared files beside them.
    std::map<QString, Stamp> stamps_;
    QStringList wanted_;
    QSize canvas_;

    std::jthread worker_;
    std::jthread sweeper_;
    double item_progress_ = 0.0;
    QString current_item_;
    int items_done_ = 0;
    int items_total_ = 0;
};

} // namespace libertas::ui

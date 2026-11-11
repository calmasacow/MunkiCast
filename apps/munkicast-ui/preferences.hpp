// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <filesystem>

class QJSEngine;
class QQmlEngine;

namespace munkicast::ui {

/// The two cache preferences, readable without the singleton.
///
/// `AssetCache` needs them on its own thread and before QML has instantiated anything, and the
/// Preferences constructor is private because Qt owns the instance. These read the same stored
/// values the properties do -- there is one copy of each, not a mirror.
[[nodiscard]] QString cache_dir();
[[nodiscard]] bool caching_enabled();

/// Machine-wide settings from Edit > Preferences, kept in preferences.json beside the other
/// configuration. They belong to this computer, not to a collection.
class Preferences : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// GPU memory the compositor may use, in MiB. 0 means automatic: half the GPU memory the
    /// driver reports free at renderer start.
    Q_PROPERTY(int gpuBudgetMib READ gpuBudgetMib NOTIFY changed)

    /// Where prepared copies of show assets are kept. Empty means the default under
    /// $XDG_CACHE_HOME. See `assetcache.hpp` for why they exist at all.
    Q_PROPERTY(QString cacheDir READ cacheDir NOTIFY changed)
    /// Whether prepared copies are used. Off means every source reads the path in its settings.
    Q_PROPERTY(bool cachingEnabled READ cachingEnabled NOTIFY changed)
    Q_PROPERTY(bool showOutputMissedFrames READ showOutputMissedFrames NOTIFY changed)

  public:
    /// Where preferences live. Call before the QML engine instantiates the singleton; this
    /// also exports them to the environment the render process inherits.
    static void configure(std::filesystem::path path);
    static Preferences* create(QQmlEngine*, QJSEngine*);

    [[nodiscard]] int gpuBudgetMib() const;
    /// The configured directory, or the default when none was configured. Always absolute.
    [[nodiscard]] QString cacheDir() const;
    [[nodiscard]] bool cachingEnabled() const;
    /// Saves, then restarts the render process so it takes effect. False when out of range
    /// (0, or 256 MiB to 65536 MiB) or the file cannot be written.
    Q_INVOKABLE bool setGpuBudgetMib(int mib);
    /// Saves. An empty path restores the default. False when the path is not absolute, is
    /// absurdly long, or the preferences file cannot be written. The renderer is *not*
    /// restarted: prepared paths travel inside the published document, so nothing the render
    /// process read at startup depends on this.
    Q_INVOKABLE bool setCacheDir(const QString& path);
    Q_INVOKABLE bool setCachingEnabled(bool on);
    [[nodiscard]] bool showOutputMissedFrames() const;
    Q_INVOKABLE bool setShowOutputMissedFrames(bool on);

  signals:
    void changed();
    /// Asks the preview to restart the render process.
    void restartRenderer();

  private:
    using QObject::QObject;
};

} // namespace munkicast::ui

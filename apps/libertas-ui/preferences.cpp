// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preferences.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace libertas::ui {
namespace {
QString g_path;
int g_gpu_budget_mib = 0;
/// Empty means "the default", which is resolved on read rather than stored. Storing the
/// resolved default would freeze a path that XDG is allowed to move.
QString g_cache_dir;
bool g_caching_enabled = false;
bool g_show_output_missed_frames = false;

QString default_cache_dir() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return base.isEmpty() ? QString{} : base + QStringLiteral("/libertas/cache");
}

/// Absolute, not absurd, and no embedded null -- the same three questions the source settings
/// ask of a path. Not checked for existence: the directory is created when a build runs, and
/// refusing a path because it is not there yet would refuse a fresh external disk.
bool valid_cache_dir(const QString& path) {
    return path.isEmpty() ||
           (path.startsWith(QLatin1Char('/')) && path.size() <= 4096 &&
            !path.contains(QChar::Null));
}

void export_budget() {
    if (g_gpu_budget_mib > 0)
        qputenv("LIBERTAS_GPU_BUDGET_MIB", QByteArray::number(g_gpu_budget_mib));
    else
        qunsetenv("LIBERTAS_GPU_BUDGET_MIB");
}

bool valid_budget(int mib) { return mib == 0 || (mib >= 256 && mib <= 65536); }

/// One writer for the whole file, so adding a key cannot drop the others -- which is what a
/// second `setX` writing only its own key would have done.
bool save() {
    if (g_path.isEmpty() || !QDir().mkpath(QFileInfo(g_path).absolutePath()))
        return false;
    QSaveFile file(g_path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{{"version", 1},
                                         {"gpuBudgetMib", g_gpu_budget_mib},
                                         {"cacheDir", g_cache_dir},
                                         {"cachingEnabled", g_caching_enabled},
                                         {"showOutputMissedFrames", g_show_output_missed_frames}})
                   .toJson());
    return file.commit();
}
} // namespace

void Preferences::configure(std::filesystem::path path) {
    g_path = QString::fromStdString(path.string());
    g_show_output_missed_frames = false;
    QFile file(g_path);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 64 * 1024) {
        const auto json = QJsonDocument::fromJson(file.readAll()).object();
        const int mib = json.value("gpuBudgetMib").toInt(0);
        g_gpu_budget_mib = valid_budget(mib) ? mib : 0;
        const auto dir = json.value("cacheDir").toString();
        g_cache_dir = valid_cache_dir(dir) ? dir : QString{};
        g_caching_enabled = json.value("cachingEnabled").toBool(false);
        g_show_output_missed_frames = json.value("showOutputMissedFrames").toBool(false);
    }
    export_budget();
}

Preferences* Preferences::create(QQmlEngine*, QJSEngine*) { return new Preferences(nullptr); }

int Preferences::gpuBudgetMib() const { return g_gpu_budget_mib; }

QString cache_dir() { return g_cache_dir.isEmpty() ? default_cache_dir() : g_cache_dir; }

bool caching_enabled() { return g_caching_enabled; }

QString Preferences::cacheDir() const { return cache_dir(); }

bool Preferences::cachingEnabled() const { return caching_enabled(); }

bool Preferences::setGpuBudgetMib(int mib) {
    if (!valid_budget(mib))
        return false;
    if (mib == g_gpu_budget_mib)
        return true;
    const int previous = g_gpu_budget_mib;
    g_gpu_budget_mib = mib;
    if (!save()) {
        g_gpu_budget_mib = previous;
        return false;
    }
    export_budget();
    emit changed();
    emit restartRenderer();
    return true;
}

bool Preferences::setCacheDir(const QString& path) {
    const auto trimmed = path.trimmed();
    if (!valid_cache_dir(trimmed))
        return false;
    if (trimmed == g_cache_dir)
        return true;
    const auto previous = g_cache_dir;
    g_cache_dir = trimmed;
    if (!save()) {
        g_cache_dir = previous;
        return false;
    }
    emit changed();
    return true;
}

bool Preferences::setCachingEnabled(bool on) {
    if (on == g_caching_enabled)
        return true;
    g_caching_enabled = on;
    if (!save()) {
        g_caching_enabled = !on;
        return false;
    }
    emit changed();
    return true;
}

bool Preferences::showOutputMissedFrames() const { return g_show_output_missed_frames; }

bool Preferences::setShowOutputMissedFrames(bool on) {
    if (on == g_show_output_missed_frames)
        return true;
    g_show_output_missed_frames = on;
    if (!save()) {
        g_show_output_missed_frames = !on;
        return false;
    }
    emit changed();
    return true;
}

} // namespace libertas::ui

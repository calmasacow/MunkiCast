// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Projects: the outer construct, and what the shell does when one is made or opened.
//
// A project is *not* a scene collection. A collection is one show -- its scenes, sources and
// outputs. A project is the directory that work lives in: the collections, the timelines cut
// from them, the graphics authored for them, the recordings made, the media cached for
// playback, and the settings in force. Opening a project points the collection catalogue,
// the media cache and the sub-document workbench at that directory, so everything a session
// writes lands inside the one folder the operator can copy, hand over or back up.

#include "document.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <ldoc/project.hpp>

namespace munkicast::ui {
namespace {

namespace fs = std::filesystem;

/// Where the list of projects this machine has opened lives.
///
/// Beside the collection catalogue, and in the same shape, because it answers the same kind
/// of question: what have I been working on. The projects themselves are wherever the
/// operator put them; this only remembers the way back.
QString projectsCatalogPath() {
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return base.isEmpty() ? QString{} : QDir(base).filePath(QStringLiteral("projects.json"));
}

QVariantList readProjects() {
    const QString path = projectsCatalogPath();
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return {};
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject() || doc.object().value("version").toInt() != 1)
        return {};

    QVariantList out;
    for (const auto& entry : doc.object()["projects"].toArray()) {
        const auto item = entry.toObject();
        const QString dir = item["path"].toString();
        // A project the operator has since deleted or unplugged is dropped from the list
        // rather than offered: a menu row that cannot work is worse than one that is absent.
        if (dir.isEmpty() || !QFileInfo(dir).isAbsolute())
            continue;
        if (!ldoc::is_project(fs::path(dir.toStdString())))
            continue;
        out.append(QVariantMap{{QStringLiteral("name"), item["name"].toString()},
                               {QStringLiteral("path"), dir}});
    }
    return out;
}

void writeProjects(const QVariantList& projects) {
    const QString path = projectsCatalogPath();
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 1},
                                  {"projects", QJsonArray::fromVariantList(projects)}})
            .toJson();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size()) {
        return;
    }
    static_cast<void>(file.commit());
}

/// Put a project at the front of the recent list, keeping it unique by path.
QVariantList remember(QVariantList projects, const QString& name, const QString& path) {
    QVariantList out{QVariantMap{{QStringLiteral("name"), name},
                                 {QStringLiteral("path"), path}}};
    for (const QVariant& entry : projects) {
        if (entry.toMap().value(QStringLiteral("path")).toString() != path)
            out.append(entry);
    }
    // Enough to find your way back, not a history: a list long enough to scroll is one
    // nobody reads to the end of.
    while (out.size() > 12)
        out.removeLast();
    return out;
}

} // namespace

QString Document::openProjectName() const { return project_name_; }

QString Document::openProjectPath() const { return project_dir_; }

bool Document::hasProject() const { return !project_dir_.isEmpty(); }

QVariantList Document::recentProjects() const { return readProjects(); }

bool Document::createProject(const QString& path, const QString& name) {
    if (path.isEmpty()) {
        emit notice(QStringLiteral("Choose a folder for the project."));
        return false;
    }
    const auto made = ldoc::create_project_tree(fs::path(path.toStdString()),
                                                name.trimmed().toStdString());
    if (!made) {
        emit notice(QString::fromStdString(made.error()));
        return false;
    }
    return openProject(path);
}

bool Document::openProject(const QString& path) {
    const fs::path dir{path.toStdString()};
    if (!ldoc::is_project(dir)) {
        emit notice(QStringLiteral("%1 is not a project.").arg(path));
        return false;
    }
    const auto manifest = ldoc::load_manifest(dir);
    if (!manifest) {
        emit notice(QString::fromStdString(manifest.error()));
        return false;
    }

    // Whatever is open now belongs to the project being left.
    if (hasProject() && !saveCurrentCollection())
        return false;

    // Folders first: a project made by an older build, or one whose folders were moved
    // between sessions, gains them by being opened.
    for (const std::string& problem : ldoc::ensure_project_folders(dir))
        emit notice(QString::fromStdString(problem));

    project_dir_ = path;
    project_name_ = QString::fromStdString(manifest->name);
    // The collections of *this* project, not a machine-wide pile. This is the line that
    // makes a project the thing that holds collections rather than a synonym for one.
    useCollectionRoot(ldoc::collections_dir(dir));

    writeProjects(remember(readProjects(), project_name_, path));
    emit projectChanged();

    // Open the collection the project was left on, or the first one it has. A project with
    // no collections is legal and opens empty -- making the first show is then the next
    // thing the operator does.
    const auto collections = ldoc::project_collections(dir);
    QString wanted;
    if (!manifest->open_collection.empty()) {
        const fs::path named = ldoc::collections_dir(dir) / manifest->open_collection;
        if (std::find(collections.begin(), collections.end(), named) != collections.end())
            wanted = QString::fromStdString(named.string());
    }
    if (wanted.isEmpty() && !collections.empty())
        wanted = QString::fromStdString(collections.front().string());

    if (wanted.isEmpty()) {
        emit notice(QStringLiteral("Opened '%1'. It has no scene collections yet.")
                        .arg(project_name_));
        return true;
    }
    return openCollection(wanted);
}

void Document::noteOpenCollection() {
    if (!hasProject())
        return;
    const fs::path dir{project_dir_.toStdString()};
    auto manifest = ldoc::load_manifest(dir);
    if (!manifest)
        return;
    const fs::path open = session_->path();
    manifest->open_collection = open.empty() ? std::string{} : open.filename().string();
    static_cast<void>(ldoc::save_manifest(dir, *manifest));
}

bool Document::showOpenProjectFolder() {
    if (!hasProject()) {
        emit notice(QStringLiteral("No project is open."));
        return false;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(project_dir_))) {
        emit notice(QStringLiteral("Could not open %1.").arg(project_dir_));
        return false;
    }
    return true;
}

void Document::forgetProject(const QString& path) {
    QVariantList kept;
    for (const QVariant& entry : readProjects()) {
        if (entry.toMap().value(QStringLiteral("path")).toString() != path)
            kept.append(entry);
    }
    writeProjects(kept);
    emit projectChanged();
}

} // namespace munkicast::ui

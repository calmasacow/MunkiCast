// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <expected>
#include <ldoc/command.hpp>
#include <ldoc/project_io.hpp>
#include <limport/obs.hpp>

namespace munkicast::ui {
namespace {
namespace fs = std::filesystem;
fs::path collectionRoot;
QString catalogPath;

std::expected<QJsonObject, QString> readCatalog() {
    QFile file(catalogPath);
    if (!file.exists())
        return QJsonObject{{"version", 1}, {"collections", QJsonArray{}}};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return std::unexpected(QStringLiteral("Cannot read collection catalog."));
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() ||
        doc.object().value("version").toInt() != 1 || !doc.object()["collections"].isArray())
        return std::unexpected(
            QStringLiteral("Collection catalog is invalid; it was not changed."));
    for (const auto& entry : doc.object()["collections"].toArray()) {
        const auto item = entry.toObject();
        if (!item["name"].isString() || !item["path"].isString() ||
            !QFileInfo(item["path"].toString()).isAbsolute())
            return std::unexpected(QStringLiteral("Invalid collection catalog entry."));
    }
    return doc.object();
}

bool validName(const QString& name) {
    return !name.trimmed().isEmpty() && name.size() <= 128 && !name.contains(QChar::Null) &&
           !name.contains('\n') && !name.contains('\r');
}

fs::path freshPath() {
    return collectionRoot / (ldoc::Id::generate().to_string() + ".munkicast");
}

// The project writer owns model files. Companion files retain their relative layout; session
// state must never be copied into a second project. No source directory is modified.
std::expected<void, std::string> copyCollection(const ldoc::Project& project,
                                                const fs::path& source, const fs::path& target) {
    if (target.empty() || !target.is_absolute())
        return std::unexpected("Choose an absolute destination path.");
    std::error_code ec;
    if (fs::exists(target, ec) || ec)
        return std::unexpected("Destination already exists or cannot be checked.");
    if (!source.empty()) {
        const auto from = fs::weakly_canonical(source, ec);
        if (ec)
            return std::unexpected(ec.message());
        const auto to = fs::weakly_canonical(target, ec);
        if (ec)
            return std::unexpected(ec.message());
        const auto relative = to.lexically_relative(from);
        if (!relative.empty() && *relative.begin() != "..")
            return std::unexpected("Destination cannot be inside the current collection.");
    }
    fs::create_directories(target.parent_path(), ec);
    if (ec || !fs::create_directory(target, ec))
        return std::unexpected("Cannot create new destination: " + ec.message());
    const auto created = ldoc::create_project(project, target);
    if (!created) {
        fs::remove_all(target, ec);
        return std::unexpected(created.error());
    }
    if (!source.empty()) {
        fs::directory_iterator entries(source, ec);
        if (!ec) {
            const fs::directory_iterator end;
            while (entries != end && !ec) {
                const auto name = entries->path().filename();
                if (name != ".munkicast" && name != "project.json" && name != "assets.json" &&
                    name != "scenes")
                    fs::copy(entries->path(), target / name,
                             fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
                if (!ec)
                    entries.increment(ec);
            }
        }
    }
    if (ec) {
        const auto why = ec.message();
        fs::remove_all(target, ec);
        return std::unexpected("Could not copy collection files: " + why);
    }
    return {};
}
} // namespace

void Document::configureCollections(fs::path root, fs::path catalog) {
    collectionRoot = std::move(root);
    catalogPath = QString::fromStdString(catalog.string());
}

fs::path Document::lastCollection() {
    const auto catalog = readCatalog();
    if (!catalog)
        return {};
    const fs::path path = catalog->value("last").toString().toStdString();
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path / "project.json", ec) && !ec ? path
                                                                                  : fs::path{};
}

void Document::initCollections() {
    if (catalogPath.isEmpty())
        return;
    const auto catalog = readCatalog();
    if (!catalog) {
        collection_catalog_error_ = catalog.error();
        QMetaObject::invokeMethod(
            this, [this] { emit notice(collection_catalog_error_); }, Qt::QueuedConnection);
        return;
    }
    collection_items_ = catalog->value("collections").toArray().toVariantList();
    if (!session_->path().empty())
        rememberCollection();
}

bool Document::rememberCollection() {
    if (!collection_catalog_error_.isEmpty()) {
        emit notice(collection_catalog_error_);
        return false;
    }
    if (catalogPath.isEmpty())
        return true;
    auto items = collection_items_;
    const auto path = QString::fromStdString(session_->path().string());
    bool found = false;
    for (auto& entry : items) {
        auto item = entry.toMap();
        if (item["path"] == path) {
            item["name"] = projectName();
            entry = item;
            found = true;
        }
    }
    if (!path.isEmpty() && !found)
        items.append(QVariantMap{{"name", projectName()}, {"path", path}});
    QSaveFile file(catalogPath);
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 1},
                                  {"last", path},
                                  {"collections", QJsonArray::fromVariantList(items)}})
            .toJson();
    if (!QDir().mkpath(QFileInfo(catalogPath).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        emit notice(QStringLiteral(
            "Could not save collection catalog. Current project remains available."));
        return false;
    }
    collection_items_ = items;
    emit collectionsChanged();
    return true;
}

void Document::installSession(std::unique_ptr<lsession::Session> session) {
    autosave_.stop();
    if (session_)
        session_->wait_for_save();
    session_ = std::move(session);
    selected_scene_.clear();
    selected_node_.clear();
    also_selected_.clear();
    selection_anchor_.clear();
    current_canvas_.clear();
    copied_nodes_.clear();
    copied_scene_.reset();
    copied_transform_.reset();
    session_->set_notice_handler([this](ldoc::Severity, const std::string& text) {
        emit notice(QString::fromStdString(text));
    });
    session_->subscribe([this](const lsession::Change&) { rebuild(); });
    rebuild();
    emit canvasChanged();
    rememberCollection();
}

bool Document::saveCurrentCollection() {
    if (live_) {
        emit notice(QStringLiteral("Stop recording and streaming before switching collections."));
        return false;
    }
    if (!collection_catalog_error_.isEmpty()) {
        emit notice(collection_catalog_error_);
        return false;
    }
    if (session_->path().empty()) {
        const auto path = freshPath();
        const auto copied = copyCollection(session_->project(), {}, path);
        if (!copied) {
            emit notice(QString::fromStdString(copied.error()));
            return false;
        }
        auto created = lsession::LocalSession::open(path);
        if (!created) {
            emit notice(QString::fromStdString(created.error()));
            return false;
        }
        installSession(std::move(*created));
    } else {
        const auto saved = session_->save();
        if (!saved) {
            emit notice(QString::fromStdString(saved.error()));
            return false;
        }
    }
    return rememberCollection();
}

bool Document::openCollection(const QString& path) {
    const fs::path target = path.toStdString();
    std::error_code ec;
    if (!session_->path().empty() && fs::equivalent(target, session_->path(), ec) && !ec)
        return true;
    auto opened = lsession::LocalSession::open(target);
    if (!opened) {
        emit notice(QString::fromStdString(opened.error()));
        return false;
    }
    if (!saveCurrentCollection())
        return false;
    installSession(std::move(*opened));
    collection_report_.clear();
    emit collectionsChanged();
    return true;
}

bool Document::createCollection(const QString& name, bool duplicate) {
    const auto clean = name.trimmed();
    if (!validName(clean)) {
        emit notice(QStringLiteral("Enter a collection name (1–128 characters)."));
        return false;
    }
    if (!saveCurrentCollection())
        return false;
    auto project = duplicate ? session_->project() : studioProject(clean.toStdString());
    project.name = clean.toStdString();
    project.id = ldoc::Id::generate();
    const auto target = freshPath();
    const auto copied = copyCollection(project, duplicate ? session_->path() : fs::path{}, target);
    if (!copied) {
        emit notice(QString::fromStdString(copied.error()));
        return false;
    }
    return openCollection(QString::fromStdString(target.string()));
}

bool Document::renameCollection(const QString& name) {
    const auto clean = name.trimmed();
    if (!validName(clean)) {
        emit notice(QStringLiteral("Enter a collection name (1–128 characters)."));
        return false;
    }
    // An unsaved collection gets its folder first: saving it installs a new session, which
    // would drop the rename from the undo history if the rename came first.
    if (session_->path().empty() && !saveCurrentCollection())
        return false;
    run(ldoc::SetProjectName{clean.toStdString()});
    return saveCurrentCollection();
}

bool Document::removeCollection() {
    if (!saveCurrentCollection())
        return false;
    const auto path = QString::fromStdString(session_->path().string());
    QString trashed;
    if (!QFile::moveToTrash(path, &trashed)) {
        emit notice(QStringLiteral("Could not move collection to Trash; collection remains open."));
        return false;
    }
    for (qsizetype i = collection_items_.size(); i-- > 0;)
        if (collection_items_[i].toMap().value("path") == path)
            collection_items_.removeAt(i);
    // Reuse a remaining collection so switching cannot save a new Untitled replacement.
    std::unique_ptr<lsession::Session> next;
    for (const auto& entry : collection_items_) {
        auto opened = lsession::LocalSession::open(entry.toMap().value("path").toString().toStdString());
        if (opened) {
            next = std::move(*opened);
            break;
        }
    }
    installSession(next ? std::move(next)
                        : lsession::LocalSession::in_memory(studioProject("Untitled")));
    emit notice(QStringLiteral("Collection moved to Trash: %1").arg(trashed));
    return true;
}

bool Document::importCollection(const QString& path, bool obs, const QString& name, int width,
                                int height, int fpsNum, int fpsDen) {
    const auto clean = name.trimmed();
    if (!validName(clean)) {
        emit notice(QStringLiteral("Enter a collection name (1–128 characters)."));
        return false;
    }
    if (!saveCurrentCollection())
        return false;
    const auto target = freshPath();
    QString report;
    if (obs) {
        const auto imported =
            limport::import_obs(path.toStdString(), target, {width, height, {fpsNum, fpsDen}});
        if (!imported) {
            emit notice(QString::fromStdString(imported.error()));
            return false;
        }
        QFile file(QString::fromStdString((target / "obs-import-report.md").string()));
        if (file.open(QIODevice::ReadOnly))
            report = QString::fromUtf8(file.readAll());
    } else {
        const fs::path source = path.toStdString();
        std::error_code ec;
        if (!session_->path().empty() && fs::equivalent(source, session_->path(), ec) && !ec) {
            if (!createCollection(clean, true))
                return false;
            return true;
        }
        auto opened = lsession::LocalSession::open(source);
        if (!opened) {
            emit notice(QString::fromStdString(opened.error()));
            return false;
        }
        auto project = (*opened)->project();
        project.id = ldoc::Id::generate();
        project.name = clean.toStdString();
        const auto copied = copyCollection(project, source, target);
        if (!copied) {
            emit notice(QString::fromStdString(copied.error()));
            return false;
        }
    }
    if (!openCollection(QString::fromStdString(target.string())) || !renameCollection(clean))
        return false;
    collection_report_ = report.isEmpty() ? QStringLiteral("Native collection imported.") : report;
    emit collectionsChanged();
    return true;
}

bool Document::exportCollection(const QString& path, bool obs) {
    if (!saveCurrentCollection())
        return false;
    if (obs) {
        const auto result =
            limport::export_obs(session_->project(), session_->path(), path.toStdString());
        if (!result) {
            emit notice(QString::fromStdString(result.error()));
            return false;
        }
        collection_report_ = QString::fromStdString(*result);
    } else {
        const auto result =
            copyCollection(session_->project(), session_->path(), path.toStdString());
        if (!result) {
            emit notice(QString::fromStdString(result.error()));
            return false;
        }
        collection_report_ =
            QStringLiteral(
                "Native collection exported to %1.\nCompanion files copied; absolute external "
                "media references remain external. Credentials stay in the secret store.")
                .arg(path);
    }
    emit collectionsChanged();
    emit notice(QStringLiteral("Collection exported to %1").arg(path));
    return true;
}
} // namespace munkicast::ui

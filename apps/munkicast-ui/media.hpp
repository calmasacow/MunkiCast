// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QString>
#include <QThread>
#include <QVariantList>

#include <memory>

namespace munkicast::ui {

/// One row of the media table.
struct MediaItem {
    QString path;
    QString name;
    /// Which of the project's folders it was found in: "Recordings", "SourceMedia", "Exports".
    QString folder;
    qint64 bytes = 0;
    qint64 modified = 0; // seconds since the epoch

    // Filled in by the prober, which runs behind the listing rather than inside it.
    bool probed = false;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    QString codec;
    QString audioCodec;
    double frameRate = 0.0;
};

/// A file waiting to be written, being written, or finished.
struct ExportJob {
    QString id;
    QString source;
    QString destination;
    QString preset;
    /// Queued, Running, Done, Failed, Cancelled -- as a string, because that is what the panel
    /// draws and there is nothing else that reads it.
    QString state;
    double progress = 0.0;
    QString error;
};

/// The Media workspace's model: what is in the project's folders, and what is being made.
///
/// Scanning is shallow and on demand rather than recursive and eager. A project's Recordings
/// folder is read when the operator looks at it; walking an entire media drive on startup is how
/// an application earns a reputation for hanging, and the answer would be stale by the time it
/// was shown anyway.
///
/// Probing is the expensive part -- it starts a process per file -- so it happens on a worker
/// thread, one file at a time, with the results filled into rows that are already on screen. The
/// table is usable immediately and gains detail as it arrives, rather than being blank until
/// every file has been opened.
class Media : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// The folder being listed, as an absolute path.
    Q_PROPERTY(QString folder READ folder NOTIFY folderChanged)
    /// Its name within the project ("Recordings"), or the full path for a folder outside it.
    Q_PROPERTY(QString folderLabel READ folderLabel NOTIFY folderChanged)
    /// The project's own folders, for the tree on the left. Empty when no project is open.
    Q_PROPERTY(QVariantList projectFolders READ projectFolders NOTIFY folderChanged)
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY folderChanged)
    /// The row the player and the details panel follow, or -1.
    Q_PROPERTY(int selected READ selected WRITE setSelected NOTIFY selectedChanged)
    /// Everything about the selected file, for the details panel.
    Q_PROPERTY(QVariantMap selectedItem READ selectedItem NOTIFY selectedChanged)
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY queueChanged)
    /// What the presets are called, in the order the menu shows them.
    Q_PROPERTY(QStringList presets READ presets CONSTANT)
    /// How many rows are listed. `rowCount` is not a property, and a QML binding on a model's
    /// length has to have something to attach to.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /// Show only files whose name contains this, case-insensitively. Empty shows everything.
    ///
    /// Applied in the scan rather than by a proxy model: the list is one directory, so
    /// re-reading it is cheap, and a second model to keep in step is not.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        NameRole,
        FolderRole,
        SizeRole,
        ModifiedRole,
        DurationRole,
        ResolutionRole,
        CodecRole,
        ProbedRole,
    };

    /// Created by the QML engine, which is where it finds `Document` to follow the open
    /// project from.
    static Media* create(QQmlEngine* engine, QJSEngine*);

    ~Media() override;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QString folder() const { return folder_; }
    [[nodiscard]] QString folderLabel() const;
    [[nodiscard]] QVariantList projectFolders() const;
    [[nodiscard]] bool hasProject() const;
    [[nodiscard]] int selected() const { return selected_; }
    void setSelected(int row);
    [[nodiscard]] QVariantMap selectedItem() const;
    [[nodiscard]] QVariantList queue() const;
    [[nodiscard]] bool exporting() const { return running_ != nullptr; }
    [[nodiscard]] QStringList presets() const;
    [[nodiscard]] int count() const { return static_cast<int>(items_.size()); }
    [[nodiscard]] QString filter() const { return filter_; }
    void setFilter(const QString& text);

    /// Open one of the project's folders by name, or any absolute path.
    Q_INVOKABLE void openFolder(const QString& pathOrName);
    /// List it again. Cheap: it is one directory read.
    Q_INVOKABLE void refresh();
    /// A file:// URL for the player, or empty when nothing is selected.
    Q_INVOKABLE QString selectedUrl() const;

    /// Queue a job. `preset` is one of `presets()`; the destination is derived from the source
    /// name and the preset, inside the project's Exports folder.
    Q_INVOKABLE void enqueue(const QString& sourcePath, const QString& preset);
    /// Queue every file currently listed.
    Q_INVOKABLE void enqueueAll(const QString& preset);
    Q_INVOKABLE void cancelJob(const QString& id);
    /// Drop everything that has finished, failed or been cancelled.
    Q_INVOKABLE void clearFinished();

    /// Called by Document when a project is opened or closed.
    void setProjectDirectory(const QString& dir);

signals:
    void folderChanged();
    void countChanged();
    void filterChanged();
    void selectedChanged();
    void queueChanged();
    void notice(const QString& text);

private:
    /// Private so that `create` is the only way in.
    ///
    /// A QML singleton with a public default constructor gets default-constructed, and the
    /// `create` overload -- which is where it is wired to `Document` -- is never called. That
    /// fails quietly: the workspace comes up with no project and refuses every edit, saying
    /// only "open a project first" about one that is already open.
    explicit Media(QObject* parent = nullptr);

    void scan();
    void probeNext();
    void applyProbe(const QString& path, const MediaItem& found);
    void startNextJob();
    void finishJob(const QString& state, const QString& error);

    QString projectDir_;
    QString folder_;
    QVector<MediaItem> items_;
    QString filter_;
    int selected_ = -1;

    /// The prober lives here and is talked to by queued signal, so a file that takes seconds to
    /// open cannot hold up the table it is filling in.
    QThread prober_;
    class Prober;
    Prober* prober_object_ = nullptr;
    int probing_ = -1;
    bool probe_in_flight_ = false;

    QVector<ExportJob> queue_;
    std::unique_ptr<QProcess> running_;
    QString running_id_;
    double running_duration_ = 0.0;
    /// Written to `<name>.part` and renamed on success, so a cancelled or failed export never
    /// leaves a file that looks finished.
    QString running_partial_;
    QString running_destination_;

    QFileSystemWatcher watcher_;
};

} // namespace munkicast::ui

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSet>
#include <QString>
#include <QThread>

namespace munkicast::ui {

/// Filmstrips and waveforms for the clips on a timeline.
///
/// A timeline of blue rectangles is a list of filenames with extra steps. What makes one
/// readable at a glance is that each clip looks like what it holds -- so a cut can be placed
/// against a shot change or the start of a word, without scrubbing to find them.
///
/// Both are expensive and neither is needed to *edit*, so they are built behind the timeline
/// rather than in front of it: the panel draws as soon as the document is open and the pictures
/// arrive over the next moment. Results are cached by media path, because the same file appears
/// on a timeline many times and a trim does not change what the file looks like.
class ClipArt : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static ClipArt* create(QQmlEngine* engine, QJSEngine*);
    ~ClipArt() override;


    /// Ask for a file's artwork. Returns immediately; `ready` fires when it has been built.
    ///
    /// Safe to call from paint: asking twice for the same file is free, and the panel has no
    /// other moment at which it knows which files are on screen.
    Q_INVOKABLE void request(const QString& path, bool audio);

    /// The image provider's source for a path, or empty when nothing has been built yet.
    Q_INVOKABLE [[nodiscard]] QString source(const QString& path, bool audio) const;

    /// How long the file is, in seconds, once it has been looked at. Zero until then.
    ///
    /// Kept here because the artwork covers the whole file: to place a filmstrip under a clip
    /// that has been trimmed, the panel has to know how much media the picture spans.
    Q_INVOKABLE [[nodiscard]] double duration(const QString& path) const;

    /// What the provider hands back. Null until built.
    [[nodiscard]] QImage image(const QString& key) const;

signals:
    void ready();

private:
    explicit ClipArt(QObject* parent = nullptr);

    void store(const QString& key, const QImage& image, const QString& path, double seconds);

    class Worker;
    QThread thread_;
    Worker* worker_ = nullptr;
    QHash<QString, QImage> cache_;
    /// Media length in seconds, by path, learned while building artwork.
    QHash<QString, double> durations_;
    /// Asked for and not yet answered, so a repaint does not queue the same file sixty times.
    QSet<QString> pending_;
};

/// Serves `image://clipart/<key>`.
class ClipArtProvider : public QQuickImageProvider {
public:
    explicit ClipArtProvider(ClipArt* art)
        : QQuickImageProvider(QQuickImageProvider::Image), art_(art) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;

private:
    ClipArt* art_ = nullptr;
};

} // namespace munkicast::ui

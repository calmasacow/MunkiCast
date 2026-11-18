// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipart.hpp"

#include "assetcache.hpp"

#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace munkicast::ui {
namespace {

/// How tall the artwork is built. Clips are drawn at whatever height the track is, and the
/// image is stretched; building at the tallest a track is likely to be keeps one copy usable
/// everywhere rather than one per track height.
constexpr int kStripHeight = 64;
constexpr int kWaveHeight = 64;

/// How many frames go into a filmstrip. Enough that a long clip zoomed in still shows motion,
/// few enough that building one is seconds rather than a minute.
constexpr int kStripFrames = 24;

/// The width of a waveform image. A clip wider than this on screen is drawn from a stretched
/// copy, which is honest: the shape is right even where the samples are not.
constexpr int kWaveWidth = 1600;

QString key_for(const QString& path, bool audio) {
    return (audio ? QStringLiteral("a:") : QStringLiteral("v:")) + path;
}

/// Wait for a child process, giving up promptly when the thread is being shut down.
///
/// `waitForFinished(60000)` blocks the worker's event loop for up to a minute, so a `quit()`
/// sent while ffmpeg is running is not seen until it finishes -- and `~QThread` aborts the
/// whole application when its `wait()` then times out. That is not hypothetical: it is what
/// closing the window during a build did, with a core dump naming this.
///
/// Polling in slices instead means shutdown costs a fraction of a second, and the child is
/// killed rather than left running after the window has gone.
bool wait_or_abandon(QProcess& process, int timeout_ms) {
    constexpr int kSlice = 100;
    for (int waited = 0; waited < timeout_ms; waited += kSlice) {
        if (process.waitForFinished(kSlice))
            return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (QThread::currentThread()->isInterruptionRequested())
            break;
    }
    process.kill();
    process.waitForFinished(1000);
    return false;
}

} // namespace

/// Builds artwork off the UI thread, one file at a time.
class ClipArt::Worker : public QObject {
    Q_OBJECT

public slots:
    void build(const QString& path, bool audio) {
        // Probed here rather than on the UI thread: it is the same process the artwork needs
        // anyway, and the panel cannot place a filmstrip without it.
        const double seconds = probe_asset(path).duration;
        QImage out = audio ? waveform(path) : filmstrip(path);
        emit built(key_for(path, audio), out, path, seconds);
    }

signals:
    void built(const QString& key, const QImage& image, const QString& path, double seconds);

private:
    /// A row of frames taken across the file.
    ///
    /// One input seek per frame, not a single `fps=` pass over the whole file. The sequential
    /// pass was tried first and is the obvious shape, but `fps=` makes ffmpeg *decode* every
    /// frame to throw almost all of them away: a five-minute recording took 34 seconds, and
    /// the cost grows with the file. `-ss` before `-i` seeks to the nearest keyframe and
    /// decodes one picture, so the same file takes 10 seconds and the cost grows with the
    /// number of thumbnails instead -- a constant.
    static QImage filmstrip(const QString& path) {
        const double seconds = probe_asset(path).duration;
        if (seconds <= 0.0)
            return {};

        QTemporaryDir dir;
        if (!dir.isValid())
            return {};

        QList<QImage> frames;
        for (int i = 0; i < kStripFrames; ++i) {
            if (QThread::currentThread()->isInterruptionRequested())
                break;
            const double at = seconds * i / kStripFrames;
            const QString out = dir.filePath(QStringLiteral("%1.jpg").arg(i));
            QProcess ffmpeg;
            ffmpeg.start(QStringLiteral("ffmpeg"),
                         {QStringLiteral("-hide_banner"), QStringLiteral("-v"),
                          QStringLiteral("error"), QStringLiteral("-ss"),
                          QString::number(at, 'f', 3), QStringLiteral("-i"), path,
                          QStringLiteral("-vf"),
                          QStringLiteral("scale=-1:%1").arg(kStripHeight),
                          QStringLiteral("-frames:v"), QStringLiteral("1"),
                          QStringLiteral("-y"), out});
            // One picture from a keyframe. Ten seconds is far more than that needs and
            // still bounds a file that turns out to be unreadable.
            if (!wait_or_abandon(ffmpeg, 10000))
                break;
            QImage frame(out);
            if (frame.isNull())
                break;
            frames.append(frame);
        }
        if (frames.isEmpty())
            return {};

        int width = 0;
        for (const QImage& f : frames)
            width += f.width();
        QImage strip(width, kStripHeight, QImage::Format_RGB32);
        strip.fill(Qt::black);
        QPainter painter(&strip);
        int x = 0;
        for (const QImage& f : frames) {
            painter.drawImage(x, 0, f);
            x += f.width();
        }
        return strip;
    }

    /// The envelope of the audio, drawn symmetrically about the centre line.
    ///
    /// Decoded to raw mono 8-bit through ffmpeg rather than parsed here: the UI has no decoder,
    /// and a peak envelope is the one thing that has to be right for an editor to cut on sound.
    static QImage waveform(const QString& path) {
        QProcess ffmpeg;
        ffmpeg.start(QStringLiteral("ffmpeg"),
                     {QStringLiteral("-hide_banner"), QStringLiteral("-v"),
                      QStringLiteral("error"), QStringLiteral("-i"), path,
                      QStringLiteral("-ac"), QStringLiteral("1"),
                      // 8 kHz is far below the audio's own rate and far above what a few
                      // thousand pixels of envelope can show.
                      QStringLiteral("-ar"), QStringLiteral("8000"),
                      QStringLiteral("-f"), QStringLiteral("u8"),
                      QStringLiteral("-")});
        if (!wait_or_abandon(ffmpeg, 60000))
            return {};
        const QByteArray pcm = ffmpeg.readAllStandardOutput();
        if (pcm.isEmpty())
            return {};

        QImage wave(kWaveWidth, kWaveHeight, QImage::Format_ARGB32_Premultiplied);
        wave.fill(Qt::transparent);
        QPainter painter(&wave);
        painter.setPen(QColor(210, 232, 255));

        const auto samples = static_cast<double>(pcm.size());
        const double per = samples / kWaveWidth;
        const double mid = kWaveHeight / 2.0;
        for (int x = 0; x < kWaveWidth; ++x) {
            const auto from = static_cast<qsizetype>(x * per);
            const auto to = std::min<qsizetype>(static_cast<qsizetype>((x + 1) * per),
                                                pcm.size());
            // The peak over the column, not the mean: a mean envelope flattens transients,
            // which are exactly the marks an editor cuts against.
            int peak = 0;
            for (qsizetype i = from; i < to; ++i)
                peak = std::max(peak, std::abs(static_cast<int>(static_cast<std::uint8_t>(
                                                   pcm[i])) - 128));
            const double height = (peak / 128.0) * mid;
            painter.drawLine(QPointF(x, mid - height), QPointF(x, mid + height));
        }
        return wave;
    }
};

ClipArt* ClipArt::create(QQmlEngine* engine, QJSEngine*) {
    auto* art = new ClipArt;
    if (engine)
        engine->addImageProvider(QLatin1String("clipart"), new ClipArtProvider(art));
    return art;
}

ClipArt::ClipArt(QObject* parent) : QObject(parent) {
    worker_ = new Worker;
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &Worker::built, this, &ClipArt::store);
    thread_.start();
}

ClipArt::~ClipArt() {
    // Interrupt first: the worker may be inside an ffmpeg run, and `quit()` alone is not seen
    // until that returns. `~QThread` aborts the process if its wait times out, so this order
    // is what keeps closing the window from being a crash.
    thread_.requestInterruption();
    thread_.quit();
    if (!thread_.wait(5000))
        thread_.terminate();
    thread_.wait(1000);
}

void ClipArt::request(const QString& path, bool audio) {
    if (path.isEmpty())
        return;
    const QString key = key_for(path, audio);
    // Called from paint, so asking again has to be free: a repaint during a scrub would
    // otherwise queue the same file once per frame.
    if (cache_.contains(key) || pending_.contains(key))
        return;
    pending_.insert(key);
    QMetaObject::invokeMethod(worker_, "build", Qt::QueuedConnection, Q_ARG(QString, path),
                              Q_ARG(bool, audio));
}

void ClipArt::store(const QString& key, const QImage& image, const QString& path,
                    double seconds) {
    pending_.remove(key);
    if (seconds > 0.0)
        durations_.insert(path, seconds);
    // A file that could not be read is cached as a null image rather than retried: without
    // that, every repaint starts another ffmpeg against a file that will fail again.
    cache_.insert(key, image);
    emit ready();
}

QString ClipArt::source(const QString& path, bool audio) const {
    const QString key = key_for(path, audio);
    const auto it = cache_.constFind(key);
    if (it == cache_.constEnd() || it->isNull())
        return {};
    // No generation in the URL. One was there to defeat Qt's own caching, but an answer is
    // only ever handed out once the picture is final -- `source` returns nothing until the
    // image is cached, and a cached image is never rebuilt -- so there is nothing to defeat.
    // A changing URL was actively wrong: `Canvas.loadImage` is asynchronous, so every new
    // clip's artwork renamed every other clip's image out from under a load that had not
    // finished, and none of them ever drew.
    return QStringLiteral("image://clipart/%1").arg(QString::fromUtf8(
        key.toUtf8().toBase64(QByteArray::Base64UrlEncoding)));
}

double ClipArt::duration(const QString& path) const {
    return durations_.value(path, 0.0);
}

QImage ClipArt::image(const QString& key) const {
    return cache_.value(key);
}

QImage ClipArtProvider::requestImage(const QString& id, QSize* size, const QSize&) {
    // "<generation>/<base64 key>": the generation is only there to defeat Qt's own caching.
    const auto slash = id.indexOf(QLatin1Char('/'));
    const QString encoded = slash < 0 ? id : id.mid(slash + 1);
    const QString key = QString::fromUtf8(
        QByteArray::fromBase64(encoded.toUtf8(), QByteArray::Base64UrlEncoding));

    QImage out = art_ ? art_->image(key) : QImage{};
    if (size)
        *size = out.size();
    return out;
}

} // namespace munkicast::ui

#include "clipart.moc"

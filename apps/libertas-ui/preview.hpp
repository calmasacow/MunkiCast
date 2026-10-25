// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

class ProgramImageProvider;

/// The program picture, as published by `libertas-render`.
///
/// This process does not composite. It sends the document when the session says it changed and
/// keeps the latest frame of each canvas for the panel to draw. A remote session will do the
/// same send; the renderer does not know which parent it has (ADR-0040).
class Preview : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// Bumped when a commit's frames have been copied. The image URL carries it so Qt reloads.
    Q_PROPERTY(int generation READ generation NOTIFY changed)
    Q_PROPERTY(bool rendererAttached READ rendererAttached NOTIFY changed)
    /// Whether the renderer is writing a file right now. Reported by the renderer rather than
    /// assumed from the request: the encoder opens over there, and it can fail to.
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(bool recordingPending READ recordingPending NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingPath READ recordingPath NOTIFY recordingChanged)

  public:
    /// Not default-constructible. Qt's singleton registration prefers a default constructor
    /// over `create()`, and `create()` is the only one of the two that is handed the engine
    /// the image provider has to be installed on.
    explicit Preview(QObject* parent);
    ~Preview() override;

    /// The renderer to spawn. Call before the QML engine instantiates the singleton.
    static void configure(std::filesystem::path renderer);
    static Preview* create(QQmlEngine* engine, QJSEngine*);

    [[nodiscard]] int generation() const;
    [[nodiscard]] bool rendererAttached() const;
    Q_INVOKABLE [[nodiscard]] QVariantMap renderStats(const QString& canvasId) const;
    Q_INVOKABLE void resetStats();
    /// Stops the render process and starts a fresh one with the latest document, so settings
    /// read at its start (Edit > Preferences) take effect.
    void restart();
    Q_INVOKABLE [[nodiscard]] QVariantMap audioLevel(const QString& nodeId) const;

    /// Empty until a frame has arrived for this canvas, which is when the panel keeps its own
    /// "no pipeline attached" wording. An empty id is the first canvas (ADR-0027).
    Q_INVOKABLE [[nodiscard]] QString caption(const QString& canvasId) const;
    Q_INVOKABLE [[nodiscard]] bool hasFrame(const QString& canvasId) const;
    /// The id the image URL is keyed by. Empty until a document has named a canvas.
    Q_INVOKABLE [[nodiscard]] QString canvasKey(const QString& canvasId) const;
    Q_INVOKABLE void requestSize(QObject* owner, int edge);
    Q_INVOKABLE QString setFilterTarget(const QString& owner);

    /// The latest document. Called from the session's change notification, never from the
    /// return of the command that caused it (ADR-0024). A snapshot already in flight is not
    /// sent twice: the newest one goes when the renderer acknowledges.
    void publish(const ldoc::Project& project);

    /// One source setting as it stands in the project that was last published, which is not
    /// necessarily what the document holds: this is where the asset cache's path substitution
    /// has happened. For the self-check, which has no other way to see what the renderer was
    /// actually handed. Empty when the source or the key is not there.
    [[nodiscard]] std::string publishedSourceSetting(const QString& sourceId,
                                                     std::string_view key) const;

    /// Keeps the picture moving while something on it is live.
    ///
    /// The preview is otherwise published only when the document changes (ADR-0024), which is
    /// right for a scene made of colours and stills: nothing moves until someone moves it. A
    /// screen source breaks that assumption -- the picture changes because the desktop changed,
    /// not because the project did -- so a canvas holding one republishes on a timer.
    ///
    /// The rate is the canvas's own `fps`, not the monitor's and not a constant. A 30fps show
    /// previewed at 60 would be showing something the programme will never be, and the canvas
    /// already carries the exact rational to use (ADR-0013 on why it is never a double).
    void setLive(bool live, ldoc::Fps rate);

    friend class ProgramImageProvider;

    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] bool recordingPending() const { return recording_pending_; }
    [[nodiscard]] QString recordingPath() const { return recording_path_; }
    /// Start writing `encodeId` to `path`, or stop whatever is being written. The answer
    /// arrives as `recordingChanged`, or as a notice when the encoder would not open.
    Q_INVOKABLE void startRecording(const QString& encodeId, const QString& path);
    Q_INVOKABLE void stopRecording(const QString& encodeId);

  signals:
    void changed();
    void recordingChanged();
    void notice(const QString& text);

  private:
    struct Held;
    struct State;
    struct Link;

    void start();
    void tick();
    void try_send();
    void update_size();
    void on_readable();
    void on_writable();
    void lose(const QString& why);
    void reap(bool patient);
    void report(const QString& text);
    void send_record(const QString& encodeId, const QString& path, bool start);
    [[nodiscard]] QString resolve(const QString& canvasId) const;

    std::filesystem::path renderer_;
    std::shared_ptr<State> state_;
    QString first_canvas_;
    std::unique_ptr<Link> link_;
    QTimer* live_ = nullptr;
    bool program_live_ = false;
    bool recording_ = false;
    bool recording_pending_ = false;
    QString recording_path_;
    ldoc::Fps recording_rate_;
    ldoc::Project latest_;
    QString filter_target_;
    ldoc::Id filter_canvas_ = ldoc::Id::generate();
    ldoc::Id filter_scene_ = ldoc::Id::generate();
    ldoc::Id filter_node_ = ldoc::Id::generate();
    ldoc::Id filter_deck_ = ldoc::Id::generate();
    std::map<ldoc::Id, ldoc::Id> filter_ids_;
    std::map<QObject*, int> preview_sizes_;
    std::uint32_t preview_edge_ = 0;
    struct RenderBaseline { std::uint64_t ns = 0, frames = 0, missed = 0; };
    std::map<QString, RenderBaseline> render_baselines_;
    std::map<QString, QVariantMap> audio_levels_;
};

} // namespace libertas::ui

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"
#include "lcomp/preview.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

class QJSEngine;
class QProcess;
class QQmlEngine;

namespace munkicast::ui {

class ProgramImageProvider;

/// The program picture, as published by `munkicast-render`.
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
    Q_PROPERTY(bool replayActive READ replayActive NOTIFY recordingChanged)
    Q_PROPERTY(bool replaySaving READ replaySaving NOTIFY recordingChanged)
    Q_PROPERTY(qulonglong replayBytes READ replayBytes NOTIFY recordingChanged)
    Q_PROPERTY(qulonglong replayDurationMs READ replayDurationMs NOTIFY recordingChanged)
    Q_PROPERTY(QString lastReplayPath READ lastReplayPath NOTIFY recordingChanged)
    Q_PROPERTY(bool outputPending READ outputPending NOTIFY recordingChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY recordingChanged)
    Q_PROPERTY(bool streamingPending READ streamingPending NOTIFY recordingChanged)
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
    [[nodiscard]] QImage snapshot(const QString& canvasId) const;
    Q_INVOKABLE [[nodiscard]] QVariantMap renderStats(const QString& canvasId) const;
    Q_INVOKABLE void resetStats();
    Q_INVOKABLE [[nodiscard]] QVariantMap outputStats() const;
    Q_INVOKABLE [[nodiscard]] QVariantList captureStats() const;
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
    Q_INVOKABLE QString studioPreviewKey() const;
    /// -1 releases the manual transition; [0, 1] mixes Main and its linked destinations.
    Q_INVOKABLE void setManualTransition(double progress);
    Q_INVOKABLE QString setFilterTarget(const QString& owner);

    /// Projectors use renderer-owned GPU windows, independent of dock resolution/UI activity.
    Q_INVOKABLE QVariantList projectorScreens() const;
    Q_INVOKABLE QVariantList projectors() const { return projectors_; }
    Q_INVOKABLE bool openProjector(const QString& canvasId, bool preview, bool fullscreen,
                                  const QString& screen);
    Q_INVOKABLE void closeProjector(int id);
    Q_INVOKABLE void closeProjectors();

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

    [[nodiscard]] bool recordingPending() const { return recording_pending_ && !streaming_mode_; }
    [[nodiscard]] QString recordingPath() const { return recording_path_; }
    /// Start writing `encodeId` to `path`, or stop the recording destination. The answer
    /// arrives as `recordingChanged`, or as a notice when the encoder would not open.
    Q_INVOKABLE void startRecording(const QString& encodeId, const QString& path);
    Q_INVOKABLE void stopRecording(const QString& encodeId);

    [[nodiscard]] bool streaming() const { return streaming_; }

    [[nodiscard]] bool streamingPending() const { return recording_pending_ && streaming_mode_; }

    Q_INVOKABLE void startStreaming(const QString& outputId);
    Q_INVOKABLE void stopStreaming();

    bool replayActive() const { return replay_; }
    bool replaySaving() const { return replay_saving_; }
    bool outputPending() const { return recording_pending_; }
    qulonglong replayBytes() const { return replay_bytes_; }
    qulonglong replayDurationMs() const { return replay_duration_ms_; }
    QString lastReplayPath() const { return replay_path_; }
    Q_INVOKABLE void startReplay();
    Q_INVOKABLE void stopReplay();
    Q_INVOKABLE void saveReplay(const QString& path);

  signals:
    void changed();
    void projectorsChanged();
    void recordingChanged();
    void replaySaved(const QString& path, const QString& error);
    void notice(const QString& text);

  private:
    struct Held;
    struct State;
    struct Link;

    void start();
    void queueProjector(const QByteArray& request);
    void flushProjectors();
    void tick();
    void schedule_tick();
    void try_send();
    void update_size();
    struct Received;
    void on_received(Received& item);
    void stop_reader();
    void on_writable();
    void lose(const QString& why);
    void reap(bool patient);
    void report(const QString& text);
    void send_record(const QString& encodeId, const QString& path, bool start,
                     const ldoc::Id& output = ldoc::Id::nil(),
                     lcomp::RecordOperation operation = lcomp::RecordOperation::Output);
    [[nodiscard]] QString resolve(const QString& canvasId) const;

    std::filesystem::path renderer_;
    std::shared_ptr<State> state_;
    QString first_canvas_;
    std::unique_ptr<Link> link_;
    QTimer* live_ = nullptr;
    QTimer* projector_poll_ = nullptr;
    QVariantList projectors_;
    QString projector_error_;
    unsigned next_projector_ = 1;
    std::chrono::nanoseconds live_period_{};
    std::chrono::steady_clock::time_point live_deadline_{};
    bool output_stats_available_ = false;
    std::uint64_t output_frames_ = 0, output_missed_ = 0, output_lag_ = 0;
    std::uint64_t base_output_frames_ = 0, base_output_missed_ = 0, base_output_lag_ = 0;
    bool program_live_ = false;
    std::chrono::steady_clock::time_point transition_until_{};
    QProcess* key_lookup_ = nullptr;
    void syncReplay();
    bool replay_ = false, replay_saving_ = false;
    std::uint64_t replay_bytes_ = 0, replay_duration_ms_ = 0;
    QString replay_path_;
    bool streaming_mode_ = false; // Identifies the pending control operation.
    bool streaming_ = false;
    ldoc::Id streaming_output_ = ldoc::Id::nil();
    ldoc::Id recording_encode_ = ldoc::Id::nil();
    bool recording_ = false;
    bool recording_pending_ = false;
    QString recording_path_;
    ldoc::Fps recording_rate_;
    ldoc::Project latest_;
    ldoc::Id studio_canvas_ = ldoc::Id::generate();
    ldoc::Id studio_scene_ = ldoc::Id::generate();
    ldoc::Id studio_node_ = ldoc::Id::generate();
    ldoc::Id studio_deck_ = ldoc::Id::generate();
    ldoc::Id manual_project_;
    double manual_progress_ = -1;
    std::map<ldoc::Id, std::pair<std::optional<ldoc::Id>, ldoc::Id>> manual_targets_;
    std::map<ldoc::Id, std::optional<ldoc::Id>> immediate_targets_;
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
    std::vector<lcomp::CaptureState> capture_stats_;
};

} // namespace munkicast::ui

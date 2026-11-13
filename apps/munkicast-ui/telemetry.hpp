// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace munkicast::ui {

/// Machine telemetry for the broadcast statistics panel.
///
/// Everything here is measured on this machine right now: process CPU and memory from `/proc`,
/// free space from `statvfs`, and GPU load, VRAM and *encoder* load from NVML, which is opened
/// with `dlopen` so that it is not a build dependency and its absence is not an error.
///
/// Deliberately absent: frame rate, render times, dropped and skipped frames, and anything
/// per-output. Those are the renderer's figures, they do not exist until the renderer does, and
/// the panel shows them as unavailable rather than as zero. A zero there would read as "nothing
/// is being dropped", which is a different and much more dangerous claim than "nothing is
/// running". ADR-0020 is why they will arrive per-canvas and per-output rather than as one
/// number: a hitch in one canvas must not be hidden inside another's average.
class Telemetry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// Of one core, like every other tool reports it; this process only, not the machine.
    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY sampled)
    Q_PROPERTY(bool cpuAvailable READ cpuAvailable NOTIFY sampled)
    /// The whole machine across all cores, 0-100, which is what an operator means by "CPU" and
    /// what a system monitor beside us shows. Kept alongside the per-process figure rather than
    /// replacing it, so a gauge can draw both: our own share against everything running.
    Q_PROPERTY(double systemCpuPercent READ systemCpuPercent NOTIFY sampled)
    Q_PROPERTY(bool systemCpuAvailable READ systemCpuAvailable NOTIFY sampled)
    Q_PROPERTY(double memoryBytes READ memoryBytes NOTIFY sampled)
    Q_PROPERTY(bool memoryAvailable READ memoryAvailable NOTIFY sampled)

    /// Free space where recordings would land.
    Q_PROPERTY(double diskFreeBytes READ diskFreeBytes NOTIFY sampled)
    Q_PROPERTY(bool diskAvailable READ diskAvailable NOTIFY sampled)
    Q_PROPERTY(QString diskPath READ diskPath NOTIFY sampled)

    Q_PROPERTY(bool gpuAvailable READ gpuAvailable NOTIFY sampled)
    Q_PROPERTY(QString gpuName READ gpuName NOTIFY sampled)
    Q_PROPERTY(QVariantList encodingGpus READ encodingGpus CONSTANT)
    Q_PROPERTY(double gpuPercent READ gpuPercent NOTIFY sampled)
    /// The video encode engine specifically, which is the one that matters when it saturates:
    /// a GPU at 40% whose encoder is at 100% drops frames while looking idle.
    Q_PROPERTY(double encoderPercent READ encoderPercent NOTIFY sampled)
    Q_PROPERTY(bool encoderAvailable READ encoderAvailable NOTIFY sampled)
    Q_PROPERTY(double vramUsedBytes READ vramUsedBytes NOTIFY sampled)
    Q_PROPERTY(double vramTotalBytes READ vramTotalBytes NOTIFY sampled)
    /// Why the GPU figures are missing, when they are. Shown rather than swallowed.
    Q_PROPERTY(QString gpuNote READ gpuNote NOTIFY sampled)

    /// False until a render process reports in. Everything the renderer owns reads as
    /// unavailable while this is false.
    Q_PROPERTY(bool rendererAttached READ rendererAttached NOTIFY sampled)

public:
    explicit Telemetry(QObject* parent = nullptr);
    ~Telemetry() override;

    static Telemetry* create(QQmlEngine*, QJSEngine*);
    /// Where to measure free space. The project directory when there is one.
    static void configure(QString disk_path);

    [[nodiscard]] double cpuPercent() const { return cpu_percent_; }
    [[nodiscard]] double systemCpuPercent() const { return system_cpu_percent_; }
    [[nodiscard]] bool systemCpuAvailable() const { return system_cpu_ok_; }
    [[nodiscard]] bool cpuAvailable() const { return cpu_ok_; }
    [[nodiscard]] double memoryBytes() const { return static_cast<double>(memory_bytes_); }
    [[nodiscard]] bool memoryAvailable() const { return memory_ok_; }
    [[nodiscard]] double diskFreeBytes() const { return static_cast<double>(disk_free_); }
    [[nodiscard]] bool diskAvailable() const { return disk_ok_; }
    [[nodiscard]] QString diskPath() const { return disk_path_; }
    [[nodiscard]] bool gpuAvailable() const { return gpu_ok_; }
    [[nodiscard]] QString gpuName() const { return gpu_name_; }
    [[nodiscard]] QVariantList encodingGpus() const;
    [[nodiscard]] double gpuPercent() const { return gpu_percent_; }
    [[nodiscard]] double encoderPercent() const { return encoder_percent_; }
    [[nodiscard]] bool encoderAvailable() const { return encoder_ok_; }
    [[nodiscard]] double vramUsedBytes() const { return static_cast<double>(vram_used_); }
    [[nodiscard]] double vramTotalBytes() const { return static_cast<double>(vram_total_); }
    [[nodiscard]] QString gpuNote() const { return gpu_note_; }
    [[nodiscard]] bool rendererAttached() const { return false; }

    /// Take a reading now, rather than waiting for the timer. Used by the self-check.
    Q_INVOKABLE void sample();
    Q_INVOKABLE void setDiskPath(const QString& path);

signals:
    void sampled();

private:
    void read_cpu();
    void read_system_cpu();
    void read_memory();
    void read_disk();
    void read_gpu();

    QTimer timer_;

    double cpu_percent_ = 0.0;
    bool cpu_ok_ = false;
    std::uint64_t last_cpu_ticks_ = 0;
    std::int64_t last_cpu_wall_ms_ = 0;
    double system_cpu_percent_ = 0.0;
    bool system_cpu_ok_ = false;
    std::uint64_t last_system_busy_ = 0;
    std::uint64_t last_system_total_ = 0;

    std::uint64_t memory_bytes_ = 0;
    bool memory_ok_ = false;

    std::uint64_t disk_free_ = 0;
    bool disk_ok_ = false;
    QString disk_path_;

    bool gpu_ok_ = false;
    bool encoder_ok_ = false;
    QString gpu_name_;
    QString gpu_note_;
    double gpu_percent_ = 0.0;
    double encoder_percent_ = 0.0;
    std::uint64_t vram_used_ = 0;
    std::uint64_t vram_total_ = 0;

    struct Nvml;
    std::unique_ptr<Nvml> nvml_;
};

} // namespace munkicast::ui

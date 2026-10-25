// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "telemetry.hpp"

#include <QDateTime>
#include <QFile>
#include <QQmlEngine>

#include <array>

#include <dlfcn.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace libertas::ui {

QVariantList Telemetry::encodingGpus() const {
    // NVENC's GPU indices are CUDA ordinals, which can differ from NVML/Vulkan ordering.
    // ponytail: enumerate once per process; refresh this list if live GPU hotplug is needed.
    static const QVariantList devices = [] {
        QVariantList result;
        void* library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            return result;
        const auto init = reinterpret_cast<int (*)(unsigned)>(dlsym(library, "cuInit"));
        const auto count = reinterpret_cast<int (*)(int*)>(dlsym(library, "cuDeviceGetCount"));
        const auto get = reinterpret_cast<int (*)(int*, int)>(dlsym(library, "cuDeviceGet"));
        const auto name = reinterpret_cast<int (*)(char*, int, int)>(dlsym(library, "cuDeviceGetName"));
        int total = 0;
        if (init && count && get && name && init(0) == 0 && count(&total) == 0) {
            for (int i = 0; i < total; ++i) {
                std::array<char, 256> label{};
                int device = 0;
                if (get(&device, i) == 0 && name(label.data(), static_cast<int>(label.size()), device) == 0)
                    result.append(QVariantMap{{"index", i}, {"name", QString::fromUtf8(label.data())}});
            }
        }
        dlclose(library);
        return result;
    }();
    return devices;
}

namespace {

QString g_disk_path;

/// NVML's own return code for success. Everything non-zero is a failure of some kind, and the
/// codes are not worth enumerating here: the panel says the figure is unavailable either way.
constexpr int kNvmlSuccess = 0;

struct NvmlUtilization {
    unsigned int gpu = 0;
    unsigned int memory = 0;
};

struct NvmlMemory {
    unsigned long long total = 0;
    unsigned long long free = 0;
    unsigned long long used = 0;
};

} // namespace

/// NVML, opened at run time.
///
/// `dlopen` rather than linking: a machine with no NVIDIA driver must still build and run this
/// program, and on such a machine the library simply is not there. Binding the handful of
/// entry points we use by name also means a driver that is too old to have one of them costs
/// that one figure rather than the whole panel.
///
/// The `_v2` suffixes are NVML's ABI versioning. They are the current entry points; the
/// unsuffixed names still exist but are the older ABI with different struct layouts, and mixing
/// them is how this kind of code ends up reading garbage.
struct Telemetry::Nvml {
    void* handle = nullptr;
    void* device = nullptr;
    bool ready = false;

    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*device_count)(unsigned int*) = nullptr;
    int (*device_by_index)(unsigned int, void**) = nullptr;
    int (*get_name)(void*, char*, unsigned int) = nullptr;
    int (*get_utilization)(void*, NvmlUtilization*) = nullptr;
    int (*get_memory)(void*, NvmlMemory*) = nullptr;
    int (*get_encoder_utilization)(void*, unsigned int*, unsigned int*) = nullptr;

    template <typename Fn> void bind(Fn& slot, const char* name) {
        slot = reinterpret_cast<Fn>(::dlsym(handle, name));
    }

    ~Nvml() {
        if (ready && shutdown)
            shutdown();
        if (handle)
            ::dlclose(handle);
    }
};

Telemetry::Telemetry(QObject* parent) : QObject(parent) {
    disk_path_ = g_disk_path;

    nvml_ = std::make_unique<Nvml>();
    nvml_->handle = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!nvml_->handle) {
        gpu_note_ = QStringLiteral("No NVIDIA management library on this machine.");
    } else {
        nvml_->bind(nvml_->init, "nvmlInit_v2");
        nvml_->bind(nvml_->shutdown, "nvmlShutdown");
        nvml_->bind(nvml_->device_count, "nvmlDeviceGetCount_v2");
        nvml_->bind(nvml_->device_by_index, "nvmlDeviceGetHandleByIndex_v2");
        nvml_->bind(nvml_->get_name, "nvmlDeviceGetName");
        nvml_->bind(nvml_->get_utilization, "nvmlDeviceGetUtilizationRates");
        nvml_->bind(nvml_->get_memory, "nvmlDeviceGetMemoryInfo");
        nvml_->bind(nvml_->get_encoder_utilization, "nvmlDeviceGetEncoderUtilization");

        if (!nvml_->init || nvml_->init() != kNvmlSuccess) {
            gpu_note_ = QStringLiteral("The NVIDIA management library would not initialise.");
        } else {
            nvml_->ready = true;
            unsigned int count = 0;
            if (nvml_->device_count && nvml_->device_count(&count) == kNvmlSuccess &&
                count > 0 && nvml_->device_by_index &&
                nvml_->device_by_index(0, &nvml_->device) == kNvmlSuccess) {
                std::array<char, 96> name{};
                if (nvml_->get_name &&
                    nvml_->get_name(nvml_->device, name.data(),
                                    static_cast<unsigned int>(name.size())) == kNvmlSuccess)
                    gpu_name_ = QString::fromLatin1(name.data());
                gpu_ok_ = true;
            } else {
                gpu_note_ = QStringLiteral("The management library reported no devices.");
            }
        }
    }

    // One second, which is what every other tool that shows these numbers uses, and slow enough
    // that reading /proc does not itself show up in the CPU figure.
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &Telemetry::sample);
    timer_.start();
    sample();
}

Telemetry::~Telemetry() = default;

void Telemetry::configure(QString disk_path) {
    g_disk_path = std::move(disk_path);
}

Telemetry* Telemetry::create(QQmlEngine*, QJSEngine*) {
    return new Telemetry;
}

void Telemetry::setDiskPath(const QString& path) {
    if (disk_path_ == path)
        return;
    disk_path_ = path;
    sample();
}

void Telemetry::sample() {
    read_cpu();
    read_memory();
    read_disk();
    read_gpu();
    emit sampled();
}

void Telemetry::read_cpu() {
    QFile stat{QStringLiteral("/proc/self/stat")};
    if (!stat.open(QIODevice::ReadOnly))
        return;
    const QByteArray line = stat.readAll();
    // The second field is the executable name in parentheses and may itself contain spaces and
    // brackets, so the fields after it are counted from the *last* ')' rather than by splitting
    // the whole line. Getting this wrong reads the wrong numbers on any binary with a space in
    // its name.
    const qsizetype close = line.lastIndexOf(')');
    if (close < 0)
        return;
    const QList<QByteArray> fields =
        line.mid(static_cast<int>(close) + 2).simplified().split(' ');
    // After the ')' the fields are state, ppid, pgrp, session, tty, tpgid, flags, minflt,
    // cminflt, majflt, cmajflt, utime, stime -- utime is index 11 counting from state at 0.
    if (fields.size() < 13)
        return;
    bool ok_u = false;
    bool ok_s = false;
    const std::uint64_t utime = fields[11].toULongLong(&ok_u);
    const std::uint64_t stime = fields[12].toULongLong(&ok_s);
    if (!ok_u || !ok_s)
        return;

    const std::uint64_t ticks = utime + stime;
    const std::int64_t now_ms = QDateTime::currentMSecsSinceEpoch();
    const long hz = ::sysconf(_SC_CLK_TCK);

    if (last_cpu_wall_ms_ != 0 && now_ms > last_cpu_wall_ms_ && hz > 0) {
        const double elapsed_s = static_cast<double>(now_ms - last_cpu_wall_ms_) / 1000.0;
        const double used_s = static_cast<double>(ticks - last_cpu_ticks_) / static_cast<double>(hz);
        cpu_percent_ = (used_s / elapsed_s) * 100.0;
        cpu_ok_ = true;
    }
    last_cpu_ticks_ = ticks;
    last_cpu_wall_ms_ = now_ms;
}

void Telemetry::read_memory() {
    QFile status{QStringLiteral("/proc/self/status")};
    if (!status.open(QIODevice::ReadOnly))
        return;
    // readAll() and split, rather than readLine() in a loop over atEnd().
    //
    // A file in /proc reports a size of zero, and QFile::atEnd() believes it -- so the obvious
    // loop exits before reading a single line and the figure silently never appears. read_cpu()
    // above happened to be written with readAll() and worked; this one was not, and did not.
    for (const QByteArray& line : status.readAll().split('\n')) {
        if (!line.startsWith("VmRSS:"))
            continue;
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 2)
            return;
        bool ok = false;
        const std::uint64_t kib = parts[1].toULongLong(&ok);
        if (ok) {
            memory_bytes_ = kib * 1024;
            memory_ok_ = true;
        }
        return;
    }
}

void Telemetry::read_disk() {
    const QString where = disk_path_.isEmpty() ? QStringLiteral(".") : disk_path_;
    struct statvfs vfs {};
    if (::statvfs(where.toUtf8().constData(), &vfs) != 0) {
        disk_ok_ = false;
        return;
    }
    // f_bavail, not f_bfree: the blocks this user may actually use, which is the number that
    // decides whether a recording survives.
    disk_free_ = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    disk_ok_ = true;
}

void Telemetry::read_gpu() {
    if (!gpu_ok_ || !nvml_ || !nvml_->device)
        return;

    NvmlUtilization util{};
    if (nvml_->get_utilization &&
        nvml_->get_utilization(nvml_->device, &util) == kNvmlSuccess)
        gpu_percent_ = util.gpu;

    NvmlMemory mem{};
    if (nvml_->get_memory && nvml_->get_memory(nvml_->device, &mem) == kNvmlSuccess) {
        vram_used_ = mem.used;
        vram_total_ = mem.total;
    }

    unsigned int encoder = 0;
    unsigned int sampling_us = 0;
    if (nvml_->get_encoder_utilization &&
        nvml_->get_encoder_utilization(nvml_->device, &encoder, &sampling_us) == kNvmlSuccess) {
        encoder_percent_ = encoder;
        encoder_ok_ = true;
    }
}

} // namespace libertas::ui

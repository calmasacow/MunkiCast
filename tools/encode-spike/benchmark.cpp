// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcore/atomic_file.hpp>
#include <ldoc/json.hpp>
#include <lencode/encoder.hpp>

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {
double cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("getrusage failed");
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double p95(std::vector<double> values) {
    std::ranges::sort(values);
    return values[(values.size() - 1) * 95 / 100];
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 3)
        throw std::runtime_error("usage: munkicast-encode-bench PROJECT.json OUTPUT_DIRECTORY");
    const auto text = lc::read_file(argv[1]);
    if (!text)
        throw std::runtime_error(text.error());
    const auto parsed = ldoc::project_from_json(*text);
    if (!parsed || parsed->project.encodes.empty())
        throw std::runtime_error(parsed ? "project has no encodes" : parsed.error());
    const auto& project = parsed->project;
    // ponytail: first encode only; accept an encode ID if multi-output comparisons are needed.
    const auto& video = project.encodes.front();
    const auto* canvas = project.canvas(video.canvas);
    if (!canvas || canvas->width <= 0 || canvas->height <= 0 || canvas->width > 8192 ||
        canvas->height > 8192 || !canvas->fps.valid())
        throw std::runtime_error("invalid benchmark canvas");
    const auto* audio = project.audio_encodes.empty() ? nullptr : &project.audio_encodes.front();
    for (const auto& output : project.outputs)
        if (output.encode == video.id && output.audio_encode)
            audio = project.audio_encode(*output.audio_encode);
    if (!audio)
        throw std::runtime_error("benchmark needs an audio encode");
    const std::filesystem::path directory{argv[2]};
    std::filesystem::create_directories(directory);
    auto cfg = munkicast::encode::runtime_config(video, *audio, *canvas, "");
    cfg.source_rgba = true;
    const auto width = static_cast<std::size_t>(canvas->width);
    const auto height = static_cast<std::size_t>(canvas->height);
    // Fixed moving texture, prepared before timing; both builds receive identical RGBA bytes.
    std::vector<std::vector<std::uint8_t>> frames(16,
                                                  std::vector<std::uint8_t>(width * height * 4));
    for (std::size_t phase = 0; phase < frames.size(); ++phase)
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x) {
                auto* pixel = &frames[phase][(y * width + x) * 4];
                pixel[0] = static_cast<std::uint8_t>((x + phase * 13) ^ (y / 4));
                pixel[1] = static_cast<std::uint8_t>((y + phase * 7) ^ (x / 8));
                pixel[2] = static_cast<std::uint8_t>((x / 16 + y / 16 + phase) * 17);
                pixel[3] = 255;
            }
    std::vector<float> tone(48000 * 4);
    for (std::size_t i = 0; i < tone.size() / 2; ++i)
        tone[i * 2] = tone[i * 2 + 1] =
            static_cast<float>(0.1 * std::sin(static_cast<double>(i) * 0.1308996938995747));
    std::puts("ffmpeg,trial,frames,width,height,fps,wall_s,cpu_s,throughput_fps,convert_mean_ms,"
              "convert_p95_ms,encode_mean_ms,encode_p95_ms,frame_p95_ms,over_budget,video_bytes");
    for (int trial = 0; trial <= 3; ++trial) {
        cfg.path = (directory / ("trial-" + std::to_string(trial) + ".mkv")).string();
        std::string error;
        auto encoder = munkicast::encode::encoder_open(cfg, error);
        if (!encoder)
            throw std::runtime_error(error);
        const int count = trial == 0 ? 120 : 600;
        std::vector<double> frame_ms;
        const auto started = std::chrono::steady_clock::now();
        const auto cpu = cpu_seconds();
        for (int frame = 0; frame < count; ++frame) {
            const auto t = std::chrono::steady_clock::now();
            const auto pts =
                static_cast<std::int64_t>(frame) * 1000000000 * cfg.fps_den / cfg.fps_num;
            const auto first =
                static_cast<std::int64_t>(frame) * cfg.audio_rate * cfg.fps_den / cfg.fps_num;
            const auto next =
                static_cast<std::int64_t>(frame + 1) * cfg.audio_rate * cfg.fps_den / cfg.fps_num;
            if (next - first > 48000 ||
                !munkicast::encode::encoder_video(
                    *encoder, frames[static_cast<std::size_t>(frame) % frames.size()].data(),
                    width * 4, pts, error) ||
                !munkicast::encode::encoder_audio(
                    *encoder, &tone[static_cast<std::size_t>(first % 48000) * 2],
                    static_cast<std::uint32_t>(next - first), pts, error))
                throw std::runtime_error(error.empty() ? "invalid audio block" : error);
            frame_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t)
                    .count());
        }
        if (!munkicast::encode::encoder_finish(*encoder, error))
            throw std::runtime_error(error);
        const auto wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto used_cpu = cpu_seconds() - cpu;
        const auto& stats = munkicast::encode::encoder_stats(*encoder);
        if (stats.video_frames != static_cast<std::uint64_t>(count) ||
            stats.video_encoder != cfg.force_tier)
            throw std::runtime_error("frame count or selected encoder changed");
        if (trial != 0) {
            const double fps = static_cast<double>(cfg.fps_num) / cfg.fps_den;
            const auto late =
                std::ranges::count_if(frame_ms, [&](double ms) { return ms > 1000 / fps; });
            std::printf("%s,%d,%d,%zu,%zu,%.6f,%.6f,%.6f,%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%td,%llu\n",
                        av_version_info(), trial, count, width, height, fps, wall, used_cpu,
                        count / wall, mean(stats.convert_ms), p95(stats.convert_ms),
                        mean(stats.video_encode_ms), p95(stats.video_encode_ms), p95(frame_ms),
                        late, static_cast<unsigned long long>(stats.video_bytes));
            std::fflush(stdout);
        }
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}

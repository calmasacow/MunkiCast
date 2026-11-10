// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Drives the real libertas-render binary. A test that called composite_canvas in-process would
// pass with the process boundary broken, which is the boundary this preview exists to prove.

#include "lcomp/preview.hpp"
#include "ltest.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <lcapture/audio.hpp>
#include <ldoc/project_io.hpp>
#include <ldoc/json.hpp>
#include <mutex>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#ifndef LIBERTAS_RENDER
#error "LIBERTAS_RENDER must name the libertas-render binary"
#endif

namespace {

int count_fds() {
    int n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++n;
    }
    return n;
}

/// Wait for the child to exit. A hang is a failed test, not a leaked process: kill it and say so.
int reap(pid_t pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        const pid_t got = ::waitpid(pid, &status, WNOHANG);
        if (got == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (got < 0)
            return -1;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

ldoc::Source colour_source(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Colour";
    source.type = "source.color";
    source.settings["color"] = ldoc::Rgba::from_srgb8(r, g, b);
    source.settings["width"] = static_cast<std::int64_t>(4);
    source.settings["height"] = static_cast<std::int64_t>(4);
    return source;
}

ldoc::Node bound_to(const ldoc::Source& source) {
    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, source.name);
    std::get<ldoc::SourceData>(node.data).source = source.id;
    return node;
}

ldoc::Project show(ldoc::Scene scene, std::vector<ldoc::Source> sources) {
    ldoc::Project project = ldoc::make_default_project("show");
    project.canvases[0].width = 4;
    project.canvases[0].height = 4;
    scene.canvas = project.canvases[0].id;
    project.decks[0].program_scene = scene.id;
    project.scenes = {std::move(scene)};
    project.sources = std::move(sources);
    return project;
}

std::tuple<int, int, int, int> pixel(const lcomp::PreviewFrame& frame, int x, int y) {
    const std::size_t i =
        static_cast<std::size_t>(y) * frame.stride + static_cast<std::size_t>(x) * 4u;
    return {frame.pixels[i], frame.pixels[i + 1], frame.pixels[i + 2], frame.pixels[i + 3]};
}

/// One commit, and the one frame it produced. Fails the case rather than returning a blank.
lcomp::PreviewFrame commit(lcomp::PreviewLink& link, const ldoc::Project& project,
                           lcomp::RecordState* output_stats = nullptr) {
    const auto sent = link.send_project(project);
    REQUIRE_MSG(sent.has_value(), "{}", sent.error());
    lcomp::PreviewFrame frame;
    bool have = false;
    for (int i = 0; i < 8; ++i) {
        const auto event = link.receive(2000);
        REQUIRE_MSG(event.has_value(), "{}", event.error());
        REQUIRE_MSG(event->has_value(), "the renderer went quiet");
        const lcomp::PreviewEvent& got = **event;
        if (got.kind == lcomp::PreviewEvent::Kind::Fault)
            REQUIRE_MSG(false, "{}", got.fault);
        if (got.kind == lcomp::PreviewEvent::Kind::PeerGone)
            REQUIRE_MSG(false, "the renderer exited during a commit");
        if (got.kind == lcomp::PreviewEvent::Kind::Record && output_stats)
            *output_stats = got.record;
        if (got.kind == lcomp::PreviewEvent::Kind::Frame) {
            frame = got.frame;
            have = true;
        }
        if (got.kind == lcomp::PreviewEvent::Kind::Ack) {
            REQUIRE(have);
            return frame;
        }
    }
    REQUIRE_MSG(false, "the renderer never acknowledged the commit");
    return frame;
}

} // namespace

LTEST_CASE(browser_scene_switch_preserves_page_unless_shutdown_is_enabled) {
    for (bool shutdown : {false, true}) {
        auto ends = lipc::Channel::pair();
        REQUIRE(ends);
        auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
        REQUIRE(child);
        lcomp::PreviewLink link{std::move(ends->first)};
        REQUIRE(link.take_hello(5000));
        auto browser = colour_source(0, 0, 0);
        browser.type = "source.browser";
        browser.settings.erase("color");
        browser.settings["width"] = std::int64_t{64};
        browser.settings["height"] = std::int64_t{32};
        browser.settings["ignoreAudio"] = true;
        browser.settings["url"] = std::string("file://") + LIBERTAS_IMAGE_FIXTURES +
                                  "/browser-lifecycle.html";
        // Absence must preserve the page for existing projects as well as new sources.
        if (shutdown) browser.settings["shutdownWhenHidden"] = true;
        ldoc::Scene scene;
        scene.id = ldoc::Id::generate();
        scene.roots = {bound_to(browser)};
        scene.roots[0].layout.size = {64, 32};
        auto project = show(scene, {browser});
        project.canvases[0].width = 64;
        project.canvases[0].height = 32;
        const auto colour = [&] {
            const auto frame = commit(link, project);
            if (frame.status != lcomp::PictureStatus::Complete || frame.pixels.size() < 4)
                return 0;
            const auto [b, g, r, a] = pixel(frame, 2, 2);
            (void)b; (void)a;
            return g > 240 ? 2 : r > 240 ? 1 : 0;
        };
        bool loaded = false;
        for (int i = 0; i < 200 && !loaded; ++i) {
            loaded = colour() == 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        const auto worker_pid = [&] {
            // Children belong to the worker thread that spawned them, not the main thread.
            for (const auto& task : std::filesystem::directory_iterator(
                     "/proc/" + std::to_string(*child) + "/task")) {
                std::ifstream children(task.path() / "children");
                pid_t pid = 0;
                if (children >> pid) return pid;
            }
            return pid_t{0};
        };
        const auto initial_worker = worker_pid();
        project.scenes[0].roots.clear();
        commit(link, project);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        commit(link, project);
        const auto hidden_worker = worker_pid();
        project.scenes[0].roots = scene.roots;
        int returned = 0;
        for (int i = 0; i < 200 && returned == 0; ++i) {
            returned = colour();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        const auto returned_worker = worker_pid();
        link.quit();
        CHECK_EQ(reap(*child, 5000), 0);
        REQUIRE(loaded);
        REQUIRE(initial_worker > 0);
        CHECK_EQ(hidden_worker, shutdown ? 0 : initial_worker);
        CHECK(returned_worker > 0);
        CHECK_EQ(returned_worker == initial_worker, !shutdown);
        // A restarted Chromium page may finish its timer before producing its first frame.
        CHECK(shutdown ? returned != 0 : returned == 2);
    }
}

static void output_writes_playable_video_and_routed_audio(bool streaming, int shared = 0) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(5000));

    auto red = colour_source(255, 0, 0);
    red.settings["width"] = std::int64_t{64};
    red.settings["height"] = std::int64_t{64};
    auto tone = colour_source(0, 0, 0);
    tone.type = "source.media";
    tone.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-tone.webm";
    tone.settings["loop"] = true;
    auto background = bound_to(red);
    background.layout.fit = ldoc::FitMode::Stretch;
    background.layout.size = {64, 64};
    auto sound = bound_to(tone);
    sound.layout.size = {8, 8};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.roots = {background, sound};
    auto project = show(scene, {red, tone});
    project.canvases[0].width = project.canvases[0].height = 64;
    project.canvases[0].fps = {30, 1};
    std::get<ldoc::SourceData>(project.scenes[0].roots[1].data).audio.bus = project.buses[0].id;
    ldoc::Encode video;
    video.id = ldoc::Id::generate();
    video.canvas = project.canvases[0].id;
    video.encoder.implementation = "x264";
    video.rescale = ldoc::Vec2{32, 32};
    project.encodes = {video};
    ldoc::AudioEncode audio;
    audio.id = ldoc::Id::generate();
    audio.bus = project.buses[0].id;
    project.audio_encodes = {audio};
    auto stream_audio = audio;
    stream_audio.id = ldoc::Id::generate();
    stream_audio.codec = "opus";
    project.audio_encodes.push_back(stream_audio);
    ldoc::Output stream;
    stream.id = ldoc::Id::generate();
    stream.encode = video.id;
    stream.audio_encode = stream_audio.id;
    ldoc::Output output;
    output.id = ldoc::Id::generate();
    output.encode = video.id;
    output.audio_encode = audio.id;
    output.destination.kind = ldoc::DestinationKind::File;
    // The file must use its own AAC route, even with an Opus stream output listed first.
    project.outputs = {stream, output};
    if (streaming) {
        // Stream audio must come from the selected destination, not the file sharing its video.
        project.outputs[0].audio_encode = audio.id;
        project.outputs[1].audio_encode = stream_audio.id;
    }
    if (shared) project.outputs[1].audio_encode = audio.id;
    const auto path = std::filesystem::temp_directory_path() /
                      ("libertas-record-" + video.id.to_string() + ".mkv");
    const auto simultaneous = path.string() + ".local.mkv";

    struct Listener {
        pid_t pid = -1;

        ~Listener() {
            if (pid > 0) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
        }
    } listener;

    std::string address;
    if (streaming) {
        const int socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(socket >= 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
        socklen_t size = sizeof(local);
        REQUIRE(::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size) == 0);
        ::close(socket);
        address = "rtmp://127.0.0.1:" + std::to_string(ntohs(local.sin_port)) + "/live/test-key";
        project.outputs[0].enabled = true;
        project.outputs[0].destination.url = address;
        listener.pid = ::fork();
        REQUIRE(listener.pid >= 0);
        if (listener.pid == 0) {
            ::execl(LIBERTAS_TEST_FFMPEG, LIBERTAS_TEST_FFMPEG, "-nostdin", "-v", "error",
                    "-listen", "1", "-timeout", "10", "-i", address.c_str(), "-c", "copy", "-f",
                    "matroska", path.c_str(), nullptr);
            ::_exit(127);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    bool ready = false;
    for (int i = 0; i < 200 && !ready; ++i) {
        ready = commit(link, project).status == lcomp::PictureStatus::Complete;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    REQUIRE(ready);
    bool file_running = false, stream_running = false;
    const auto file_control = [&](bool start) {
        REQUIRE(link.send_record({start, video.id, start ? simultaneous : std::string{}}));
        bool replied = false;
        for (int i = 0; i < 30 && !replied; ++i) {
            auto event = link.receive(2000);
            REQUIRE(event && *event);
            if ((**event).kind != lcomp::PreviewEvent::Kind::Record ||
                !(**event).record.control_reply) continue;
            const auto& state = (**event).record;
            CHECK_MSG(state.error.empty(), "{}", state.error);
            CHECK_EQ(state.recording, start);
            CHECK_EQ(state.streaming, stream_running);
            file_running = start;
            replied = true;
        }
        REQUIRE(replied);
    };
    const auto control = [&](bool start) {
        REQUIRE(link.send_record({start, start ? video.id : ldoc::Id::nil(),
                                  streaming ? address : path.string(),
                                  streaming ? stream.id : ldoc::Id::nil()}));
        bool replied = false;
        for (int i = 0; i < 20 && !replied; ++i) {
            auto event = link.receive(2000);
            REQUIRE(event && *event);
            if ((**event).kind == lcomp::PreviewEvent::Kind::Record &&
                (**event).record.control_reply) {
                CHECK_MSG((**event).record.error.empty(), "{}", (**event).record.error);
                CHECK_EQ((**event).record.running, start || file_running);
                if (streaming) {
                    stream_running = start;
                    CHECK_EQ((**event).record.streaming, start);
                    CHECK_EQ((**event).record.recording, file_running);
                }
                replied = true;
            }
        }
        REQUIRE(replied);
    };
    if (shared == 1) file_control(true);
    control(true);
    if (shared == 2) file_control(true);
    for (int i = 0; i < (shared ? 90 : 60); ++i) {
        commit(link, project);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    lcomp::RecordState before_pause, after_pause;
    commit(link, project, &before_pause);
    CHECK(before_pause.running);
    CHECK(before_pause.frames > 40);
    // A screenshot/modal UI pause must not stop either the file writer or RTMP publisher.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    commit(link, project, &after_pause);
    CHECK(after_pause.frames >= before_pause.frames + 18);
    CHECK(after_pause.missed <= before_pause.missed + 2);
    CHECK(after_pause.encode_lag <= after_pause.missed);
    if (shared == 1) {
        file_control(false);
        lcomp::RecordState state;
        commit(link, project, &state);
        CHECK(state.streaming && !state.recording);
        CHECK(state.frames >= after_pause.frames);
    }
    control(false);
    if (shared == 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        lcomp::RecordState state;
        commit(link, project, &state);
        CHECK(state.recording && !state.streaming);
        CHECK(state.frames > after_pause.frames);
        file_control(false);
    }
    if (shared) {
        AVFormatContext* recording = nullptr;
        REQUIRE(avformat_open_input(&recording, simultaneous.c_str(), nullptr, nullptr) >= 0);
        REQUIRE(avformat_find_stream_info(recording, nullptr) >= 0);
        CHECK(av_find_best_stream(recording, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) >= 0);
        CHECK(av_find_best_stream(recording, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0);
        avformat_close_input(&recording);
        std::filesystem::remove(simultaneous);
    }
    if (streaming) {
        CHECK_EQ(reap(listener.pid, 5000), 0);
        listener.pid = -1;
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 5000), 0);

    AVFormatContext* file = nullptr;
    REQUIRE(avformat_open_input(&file, path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(file, nullptr) >= 0);
    const int vi = av_find_best_stream(file, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(file, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    REQUIRE(vi >= 0 && ai >= 0);
    CHECK_EQ(file->streams[ai]->codecpar->codec_id, AV_CODEC_ID_AAC);
    CHECK_EQ(file->streams[vi]->codecpar->width, 32);
    CHECK_EQ(file->streams[vi]->codecpar->height, 32);
    AVCodecContext* decoder = avcodec_alloc_context3(nullptr);
    REQUIRE(decoder);
    REQUIRE(avcodec_parameters_to_context(decoder, file->streams[vi]->codecpar) >= 0);
    REQUIRE(avcodec_open2(decoder, avcodec_find_decoder(decoder->codec_id), nullptr) >= 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    REQUIRE(packet && frame);
    bool decoded = false;
    while (av_read_frame(file, packet) >= 0) {
        if (packet->stream_index == vi && avcodec_send_packet(decoder, packet) >= 0 &&
            avcodec_receive_frame(decoder, frame) >= 0) {
            decoded = true;
            av_packet_unref(packet);
            break;
        }
        av_packet_unref(packet);
    }
    REQUIRE(decoded);
    auto* converter = sws_getContext(32, 32, static_cast<AVPixelFormat>(frame->format), 32, 32,
                                     AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
    REQUIRE(converter);
    std::vector<std::uint8_t> rgba(32 * 32 * 4);
    std::uint8_t* planes[] = {rgba.data(), nullptr, nullptr, nullptr};
    const int strides[] = {32 * 4, 0, 0, 0};
    CHECK_EQ(sws_scale(converter, frame->data, frame->linesize, 0, 32, planes, strides), 32);
    const auto pixel_offset = (24 * 32 + 24) * 4;
    CHECK_MSG(rgba[pixel_offset] > 200 && rgba[pixel_offset + 2] < 30,
              "decoded red pixel is {}, {}, {}", rgba[pixel_offset], rgba[pixel_offset + 1],
              rgba[pixel_offset + 2]);
    sws_freeContext(converter);
    int video_packets = 1;
    std::int64_t previous_dts = AV_NOPTS_VALUE;
    double largest_gap = 0;
    while (av_read_frame(file, packet) >= 0) {
        if (packet->stream_index == vi) {
            ++video_packets;
            if (packet->dts != AV_NOPTS_VALUE) {
                if (previous_dts != AV_NOPTS_VALUE)
                    largest_gap =
                        std::max(largest_gap, static_cast<double>(packet->dts - previous_dts) *
                                                  av_q2d(file->streams[vi]->time_base));
                previous_dts = packet->dts;
            }
        }
        av_packet_unref(packet);
    }
    CHECK_MSG(largest_gap < 0.12, "UI pause left a {} second gap in encoded video", largest_gap);
    CHECK_MSG(video_packets >= 50, "early preview ticks retained only {} video frames",
              video_packets);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&file);
    std::filesystem::remove(path);
}

LTEST_CASE(record_writes_playable_rescaled_video_with_correct_colours_and_routed_audio) {
    output_writes_playable_video_and_routed_audio(false);
}

LTEST_CASE(rtmp_publishes_playable_rescaled_video_and_destination_audio) {
    output_writes_playable_video_and_routed_audio(true);
}

LTEST_CASE(shared_encode_can_start_recording_then_stream_and_stop_recording_independently) {
    output_writes_playable_video_and_routed_audio(true, 1);
}

LTEST_CASE(shared_encode_can_start_stream_then_record_and_stop_stream_independently) {
    output_writes_playable_video_and_routed_audio(true, 2);
}

LTEST_CASE(recording_failure_reaches_the_client_without_killing_preview) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(5000));
    auto project = ldoc::make_default_project("record failure");
    project.canvases.front().width = project.canvases.front().height = 64;
    ldoc::Encode video;
    video.id = ldoc::Id::generate();
    video.canvas = project.canvases.front().id;
    video.encoder.implementation = "x264";
    video.encoder.preset = "ultrafast";
    project.encodes.push_back(video);
    const auto path = std::filesystem::temp_directory_path() /
                      ("libertas-failure-" + video.id.to_string() + ".mkv");
    commit(link, project);
    REQUIRE(link.send_record({true, video.id, path.string()}));
    bool started = false;
    for (int i = 0; i < 8 && !started; ++i) {
        auto event = link.receive(2000);
        REQUIRE(event && *event);
        if ((**event).kind == lcomp::PreviewEvent::Kind::Record) {
            CHECK((**event).record.error.empty());
            started = (**event).record.running;
        }
    }
    REQUIRE(started);
    for (int i = 0; i < 12; ++i) commit(link, project);
    project.canvases.front().width = 32;
    REQUIRE(link.send_project(project));
    bool stopped = false;
    for (int i = 0; i < 12 && !stopped; ++i) {
        auto event = link.receive(2000);
        REQUIRE(event && *event);
        if ((**event).kind == lcomp::PreviewEvent::Kind::Record) {
            CHECK(!(**event).record.running);
            CHECK((**event).record.error.contains("size changed"));
            stopped = true;
        }
    }
    CHECK(stopped);
    while (auto event = link.receive(100)) {
        if (!*event || (**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
    }
    CHECK(commit(link, project).width == 32);
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 5000), 0);
    std::filesystem::remove(path);
}

LTEST_CASE(recording_receives_full_size_and_preview_from_one_composition) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    int render_calls = 0, output_calls = 0;
    bool server_ok = false, correct_output = false, timestamp_before_render = false;
    std::chrono::steady_clock::time_point render_started, sampled_time;
    std::jthread server([&, channel = std::move(ends->second)]() mutable {
        lcomp::RecordHooks record;
        record.wants = [](const ldoc::Id&) { return true; };
        record.frame = [&](const ldoc::Id&, const lcomp::Picture& full,
                           std::chrono::steady_clock::time_point frame_time) {
            timestamp_before_render = frame_time <= render_started && frame_time == sampled_time;
            ++output_calls;
            correct_output =
                full.width == 64 && full.height == 32 && full.pixels.size() == 64 * 32 * 4;
            return std::optional<lcomp::RecordState>{};
        };
        server_ok = lcomp::serve_preview(
                        std::move(channel), {}, {}, {},
                        [&](const ldoc::Project& project, const ldoc::Id& canvas,
                            std::uint32_t edge, lcomp::Picture* full, bool, auto frame_time) {
                            ++render_calls;
                            sampled_time = frame_time;
                            render_started = std::chrono::steady_clock::now();
                            auto picture = lcomp::composite_canvas(project, canvas);
                            if (full)
                                *full = picture;
                            return lcomp::preview_size(std::move(picture), edge);
                        },
                        {}, {}, std::move(record))
                        .has_value();
    });
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto project = ldoc::make_default_project("shared output render");
    project.canvases.front().width = 64;
    project.canvases.front().height = 32;
    REQUIRE(link.send_project(project, 16));
    bool acknowledged = false;
    for (int i = 0; i < 8 && !acknowledged; ++i) {
        auto event = link.receive(2000);
        REQUIRE(event && *event);
        if ((**event).kind == lcomp::PreviewEvent::Kind::Frame) {
            CHECK_EQ((**event).frame.width, 16);
            CHECK_EQ((**event).frame.height, 8);
        }
        acknowledged = (**event).kind == lcomp::PreviewEvent::Kind::Ack;
    }
    REQUIRE(acknowledged);
    link.quit();
    link.close();
    server.join();
    CHECK(server_ok);
    CHECK(correct_output);
    CHECK(timestamp_before_render);
    CHECK_EQ(render_calls, 1);
    CHECK_EQ(output_calls, 1);
}

LTEST_CASE(projector_clock_survives_paused_ui_without_changing_recording_cadence) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    std::atomic<unsigned> output_frames = 0, display_frames = 0, unrelated_frames = 0,
                          recorded_display_frames = 0;
    bool server_ok = false;
    auto project = ldoc::make_default_project("independent display clock");
    project.canvases[0].width = project.canvases[0].height = 4;
    project.canvases[0].fps = {30, 1};
    auto unrelated = project.canvases.front();
    unrelated.id = ldoc::Id::generate();
    project.canvases.push_back(unrelated);
    auto display_canvas = unrelated;
    display_canvas.id = ldoc::Id::generate();
    display_canvas.fps = {60, 1};
    project.canvases.push_back(display_canvas);
    const auto main = project.canvases.front().id;
    std::jthread server([&, channel = std::move(ends->second)]() mutable {
        bool recording = false, projecting = false;
        lcomp::RecordHooks record;
        record.control = [&](const auto& r, const auto&) {
            recording = r.start;
            return lcomp::RecordState{recording, {}};
        };
        record.wants = [&](const auto& id) { return recording && id == main; };
        record.frame = [&](const auto&, const auto&, auto) {
            ++output_frames;
            return std::optional<lcomp::RecordState>{};
        };
        lcomp::ProjectorHooks display;
        display.control = [&](const auto& command, const auto&) {
            projecting = command == "open";
            return std::string{"{}"};
        };
        display.pump = [&](const auto&) -> std::optional<std::chrono::nanoseconds> {
            if (projecting)
                return std::chrono::nanoseconds(1000000000 / 60);
            return {};
        };
        display.wants = [&](const auto& id) {
            return projecting && (id == main || id == display_canvas.id);
        };
        server_ok = lcomp::serve_preview(
                        std::move(channel), {}, {}, {},
                        [&](const auto& p, const auto& id, std::uint32_t edge, lcomp::Picture* full,
                            bool display_output, auto) {
                            if (id == unrelated.id)
                                ++unrelated_frames;
                            if (display_output && id == display_canvas.id)
                                ++display_frames;
                            if (display_output && recording && id == main)
                                ++recorded_display_frames;
                            auto picture = lcomp::composite_canvas(p, id);
                            if (full)
                                *full = picture;
                            return lcomp::preview_size(std::move(picture), edge);
                        },
                        {}, {}, std::move(record), {}, std::move(display))
                        .has_value();
    });
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    commit(link, project);
    REQUIRE(!link.send_projector(std::string(4097, 'x')));
    REQUIRE(link.send_projector("open"));
    auto reply = link.receive(2000);
    REQUIRE(reply && *reply && (**reply).kind == lcomp::PreviewEvent::Kind::Projector);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(display_frames.load() >= 15);
    CHECK_EQ(unrelated_frames.load(), 1u);
    REQUIRE(link.send_record({true, ldoc::Id::generate(), "clock"}));
    reply = link.receive(2000);
    REQUIRE(reply && *reply && (**reply).kind == lcomp::PreviewEvent::Kind::Record);
    const auto before = display_frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    CHECK_MSG(output_frames.load() >= 16 && output_frames.load() <= 23,
              "30fps recording produced {} frames", output_frames.load());
    CHECK(display_frames.load() >= before + 30);
    CHECK_EQ(unrelated_frames.load(), 1u);
    const auto queued = link.poll();
    REQUIRE(queued);
    CHECK(!queued->has_value());
    REQUIRE(link.send_record({false, {}, {}}));
    reply = link.receive(2000);
    REQUIRE(reply && *reply && (**reply).kind == lcomp::PreviewEvent::Kind::Record);
    CHECK_EQ(recorded_display_frames.load(), output_frames.load());
    const auto stopped = output_frames.load(), display_before = display_frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK_EQ(output_frames.load(), stopped);
    CHECK(display_frames.load() > display_before);
    REQUIRE(link.send_projector("close"));
    reply = link.receive(2000);
    REQUIRE(reply && *reply && (**reply).kind == lcomp::PreviewEvent::Kind::Projector);
    const auto closed = display_frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    CHECK_EQ(display_frames.load(), closed);
    link.quit();
    link.close();
    server.join();
    CHECK(server_ok);
}

// Explicit opt-in: opens a short-lived window on the current desktop, never an operator show.
LTEST_CASE(native_projector_presents_gpu_frames_while_ui_is_paused) {
    if (!std::getenv("LIBERTAS_TEST_PROJECTORS"))
        return;
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);

    struct Child {
        pid_t pid;

        ~Child() {
            if (pid > 0) {
                kill(pid, SIGTERM);
                (void)reap(pid, 3000);
            }
        }
    } guard{*child};

    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(5000));
    auto red = colour_source(255, 0, 0);
    ldoc::Effect scroll;
    scroll.id = ldoc::Id::generate();
    scroll.type = "filter.scroll";
    scroll.params["speed_x"] = 4.0;
    red.effects = {scroll};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.roots = {bound_to(red)};
    auto project = show(scene, {red});
    project.canvases[0].width = 1920;
    project.canvases[0].height = 1080;
    project.canvases[0].fps = {60, 1};
    commit(link, project);
    const auto command = [&](const std::string& request) {
        REQUIRE(link.send_projector(request));
        auto event = link.receive(5000);
        REQUIRE(event && *event && (**event).kind == lcomp::PreviewEvent::Kind::Projector);
        return (**event).projector;
    };
    auto status = command(std::format(R"({{"id":1,"canvas":"{}","fullscreen":false,"screen":""}})",
                                      project.canvases[0].id.to_string()));
    const auto started = std::chrono::steady_clock::now();
    while (status.find(R"("ready":true)") == std::string::npos &&
           std::chrono::steady_clock::now() - started < std::chrono::seconds(6)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        status = command(R"({"status":true})");
    }
    REQUIRE_MSG(status.find(R"("ready":true)") != std::string::npos, "{}", status);
    const auto counter = [](const std::string& json, const std::string& name) {
        const auto at = json.find("\"" + name + "\":");
        return at == std::string::npos ? 0ull : std::stoull(json.substr(at + name.size() + 3));
    };
    const auto first = counter(status, "submitted");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    auto queued = link.poll();
    REQUIRE(queued);
    CHECK(!queued->has_value());
    status = command(R"({"status":true})");
    CHECK_MSG(counter(status, "submitted") > first + 10, "{}", status);
    std::fprintf(stderr, "PROJECTOR_NATIVE %s\n", status.c_str());
    status = command(R"({"closeAll":true})");
    CHECK(status.find(R"("windows":[])") != std::string::npos);
    if (const char* monitor = std::getenv("LIBERTAS_TEST_PROJECTOR_FULLSCREEN")) {
        // Explicit second opt-in: the selected monitor briefly shows this test's fullscreen.
        command(std::format(R"({{"id":3,"canvas":"{}","fullscreen":true,"screen":"{}"}})",
                            project.canvases[0].id.to_string(), monitor));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            status = command(R"({"status":true})");
        } while (status.find(R"("ready":true)") == std::string::npos &&
                 std::chrono::steady_clock::now() < deadline);
        CHECK_MSG(status.find(R"("ready":true)") != std::string::npos &&
                      status.find(R"("fullscreen":true)") != std::string::npos,
                  "{}", status);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        status = command(R"({"status":true})");
        if (status.find(R"("presentationFeedback":true)") != std::string::npos)
            CHECK_MSG(counter(status, "presented") > 0, "{}", status);
        std::fprintf(stderr, "PROJECTOR_FULLSCREEN %s\n", status.c_str());
        command(R"({"closeAll":true})");
    }
    // A missing monitor must report a fault without taking down the output process.
    command(std::format(R"({{"id":2,"canvas":"{}","fullscreen":true,"screen":"not-a-monitor"}})",
                        project.canvases[0].id.to_string()));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    status = command(R"({"status":true})");
    CHECK_MSG(status.find("monitor was not found") != std::string::npos, "{}", status);
    command(R"({"closeAll":true})");
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
    guard.pid = -1;
}

LTEST_CASE(output_clock_survives_ui_pause_and_partial_project_transfer) {
    for (const ldoc::Fps rate : {ldoc::Fps{60, 1}, ldoc::Fps{60000, 1001}}) {
        auto ends = lipc::Channel::pair();
        REQUIRE(ends);
        std::atomic<unsigned> frames = 0, blue_frames = 0, invalid_frames = 0;
        bool server_ok = false, timestamps_aligned = true;
        std::optional<std::chrono::steady_clock::time_point> first_frame;
        std::jthread server([&, channel = std::move(ends->second)]() mutable {
            bool running = false;
            lcomp::RecordHooks record;
            record.control = [&](const lcomp::RecordRequest& request, const ldoc::Project&) {
                running = request.start;
                return lcomp::RecordState{running, {}};
            };
            record.wants = [&](const ldoc::Id&) { return running; };
            record.frame = [&](const ldoc::Id&, const lcomp::Picture& picture,
                               std::chrono::steady_clock::time_point frame_time) {
                if (!first_frame)
                    first_frame = frame_time;
                const auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(frame_time - *first_frame)
                        .count();
                const auto slot = std::llround(static_cast<double>(ns) * rate.hz() / 1e9);
                const double scheduled_ns = static_cast<double>(slot) * 1e9 / rate.hz();
                timestamps_aligned =
                    timestamps_aligned && std::abs(static_cast<double>(ns) - scheduled_ns) < 1000.0;
                ++frames;
                const auto* pixels = picture.read_from();
                if (pixels && pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 255)
                    ++blue_frames;
                else if (!pixels || pixels[0] != 255 || pixels[1] != 0 || pixels[2] != 0)
                    ++invalid_frames;
                return std::optional<lcomp::RecordState>{};
            };
            server_ok =
                lcomp::serve_preview(std::move(channel), {}, {}, {}, {}, {}, {}, std::move(record))
                    .has_value();
        });
        // An extra writer lets the test pause between Project and Commit on the real channel.
        lipc::Channel writer{lipc::Fd{::dup(ends->first.poll_fd())}};
        REQUIRE(writer.valid());
        lcomp::PreviewLink link{std::move(ends->first)};
        REQUIRE(link.take_hello(2000));
        auto red = colour_source(255, 0, 0);
        ldoc::Scene scene;
        scene.id = ldoc::Id::generate();
        scene.roots = {bound_to(red)};
        auto project = show(scene, {red});
        project.canvases.front().fps = rate;
        commit(link, project);
        REQUIRE(link.send_record({true, ldoc::Id::generate(), "clock-test.mkv"}));
        auto state = link.receive(2000);
        REQUIRE(state && *state && (**state).kind == lcomp::PreviewEvent::Kind::Record);
        REQUIRE((**state).record.running);

        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK_MSG(frames.load() >= 38, "only {} frames while UI was paused", frames.load());
        const auto queued = link.poll();
        REQUIRE(queued);
        CHECK(!queued->has_value()); // No unrequested preview traffic or growing socket backlog.

        project.sources.front().settings["color"] = ldoc::Rgba::from_srgb8(0, 0, 255);
        const auto bytes = ldoc::project_to_json(project);
        auto memory = lipc::make_memfd("incomplete-project", bytes.size());
        REQUIRE(memory);
        REQUIRE(::pwrite(memory->get(), bytes.data(), bytes.size(), 0) ==
                static_cast<ssize_t>(bytes.size()));
        std::vector<std::byte> payload{
            std::byte{2}}; // Project; scenes/commit deliberately withheld.
        for (unsigned shift = 0; shift < 32; shift += 8)
            payload.push_back(static_cast<std::byte>((bytes.size() >> shift) & 255u));
        const int fd = memory->get();
        REQUIRE(writer.send(payload, std::span<const int>(&fd, 1)));
        const auto before = frames.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(frames.load() >= before + 6);
        CHECK_EQ(blue_frames.load(), 0u);
        CHECK_EQ(invalid_frames.load(), 0u);
        commit(link, project);
        CHECK(blue_frames.load() > 0);

        // A malformed replacement must not corrupt the last committed live project either.
        REQUIRE(writer.send(payload)); // Missing memfd.
        REQUIRE(link.refresh());
        auto fault = link.receive(2000);
        REQUIRE(fault && *fault && (**fault).kind == lcomp::PreviewEvent::Kind::Fault);
        auto ack = link.receive(2000);
        REQUIRE(ack && *ack && (**ack).kind == lcomp::PreviewEvent::Kind::Ack);
        const auto blue_before = blue_frames.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(blue_frames.load() >= blue_before + 3);
        CHECK_EQ(invalid_frames.load(), 0u);

        REQUIRE(link.send_record({false, {}, {}}));
        state = link.receive(2000);
        REQUIRE(state && *state && (**state).kind == lcomp::PreviewEvent::Kind::Record);
        CHECK(!(**state).record.running);
        const auto stopped_at = frames.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
        CHECK_EQ(frames.load(), stopped_at);
        link.quit();
        link.close();
        server.join();
        CHECK(server_ok);
        CHECK(timestamps_aligned);
    }
}

LTEST_CASE(capture_statistics_report_actual_rates_errors_and_clear_removed_sources) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    const auto id = ldoc::Id::generate();
    bool server_ok = false;
    std::jthread server([&, channel = std::move(ends->second)]() mutable {
        int requests = 0;
        server_ok = lcomp::serve_preview(std::move(channel), {}, {}, {}, {}, {}, {}, {}, [&] {
            if (++requests == 1)
                return std::vector<lcomp::CaptureState>{{id, 60000, 59940, ""},
                    {ldoc::Id::nil(), 0, 0, "device does not support requested frame rate"}};
            return std::vector<lcomp::CaptureState>{};
        }).has_value();
    });
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto project = ldoc::make_default_project("capture statistics");
    project.canvases[0].width = project.canvases[0].height = 4;
    REQUIRE(link.send_project(project));
    for (int round = 0; round < 2; ++round) {
        auto event = link.receive(2000);
        REQUIRE(event && *event);
        REQUIRE((**event).kind == lcomp::PreviewEvent::Kind::Capture);
        const auto& states = (**event).capture;
        if (round == 0) {
            REQUIRE(states.size() == 2);
            CHECK_EQ(states[0].source, id);
            CHECK_EQ(states[0].negotiated, 60000u);
            CHECK_EQ(states[0].received, 59940u);
            CHECK(states[0].error.empty());
            CHECK_EQ(states[1].error, "device does not support requested frame rate");
        } else CHECK(states.empty());
        do {
            event = link.receive(2000);
            REQUIRE(event && *event);
        } while ((**event).kind != lcomp::PreviewEvent::Kind::Ack);
        if (round == 0) REQUIRE(link.refresh());
    }
    link.quit();
    server.join();
    CHECK(server_ok);
}

LTEST_CASE(render_lag_counts_composite_budgets_not_delayed_acknowledgments) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    bool server_ok = false;
    std::jthread server([channel = std::move(ends->second), &server_ok]() mutable {
        server_ok = lcomp::serve_preview(
                        std::move(channel), {},
                        [] {
                            // Delayed release/transport must not become another missed render
                            // frame.
                            std::this_thread::sleep_for(std::chrono::milliseconds(120));
                        },
                        {},
                        [](const ldoc::Project& project, const ldoc::Id& canvas, std::uint32_t,
                           lcomp::Picture*, bool, auto) {
                            if (project.canvas(canvas)->name == "Slow")
                                std::this_thread::sleep_for(std::chrono::milliseconds(45));
                            return lcomp::composite_canvas(project, canvas);
                        })
                        .has_value();
    });
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto project = ldoc::make_default_project("render lag check");
    auto& canvas = project.canvases.front();
    canvas.width = canvas.height = 4;
    canvas.name = "Slow";
    canvas.fps = {1, 1};
    const auto first = commit(link, project);
    CHECK(first.render_ns >= 45000000u);
    CHECK_EQ(first.render_lag_frames, 0u);

    canvas.fps = {60000, 1001};
    const auto second = commit(link, project);
    const auto elapsed = second.render_ns - first.render_ns;
    const auto missed = static_cast<std::uint64_t>(
        std::ceil(static_cast<long double>(elapsed) * 60000 / 1001000000000.0L)) - 1;
    CHECK(missed >= 2u);
    CHECK_EQ(second.render_lag_frames, missed);
    CHECK_EQ(second.rendered_frames, 2u);

    // A separate canvas does not inherit another canvas's overruns.
    auto independent = ldoc::make_default_project("independent canvas");
    independent.canvases.front().width = independent.canvases.front().height = 4;
    independent.canvases.front().fps = {1, 1};
    const auto third = commit(link, independent);
    CHECK_EQ(third.rendered_frames, 1u);
    CHECK_EQ(third.render_lag_frames, 0u);
    link.quit();
    link.close();
    server.join();
    CHECK(server_ok);
}

LTEST_CASE(a_picture_crosses_the_process_and_the_renderer_exits_when_the_client_closes) {
    const int fds_before = count_fds();
    int status = -1;
    {
        auto ends = lipc::Channel::pair();
        REQUIRE_MSG(ends.has_value(), "{}", ends.error());
        auto pid = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
        REQUIRE_MSG(pid.has_value(), "{}", pid.error());

        lcomp::PreviewLink link{std::move(ends->first)};
        const auto hello = link.take_hello(2000);
        REQUIRE_MSG(hello.has_value(), "{}", hello.error());

        ldoc::Source red = colour_source(255, 0, 0);
        ldoc::Scene scene;
        scene.id = ldoc::Id::generate();
        scene.name = "Scene";
        scene.roots = {bound_to(red)};
        ldoc::Project project = show(scene, {red});

        const lcomp::PreviewFrame first = commit(link, project);
        CHECK_EQ(first.status, lcomp::PictureStatus::Complete);
        CHECK_EQ(first.gaps, 0u);
        CHECK_EQ(first.rendered_frames, 1u);
        CHECK(first.render_ns > 0);
        CHECK_EQ(first.fps_num, 60u);
        CHECK_EQ(first.fps_den, 1u);
        CHECK_EQ(pixel(first, 0, 0), std::tuple(255, 0, 0, 255));

        // The document changed. The next frame has to be the new picture, not the one still
        // sitting in the memfd from the commit above.
        red.settings["color"] = ldoc::Rgba::from_srgb8(0, 0, 255);
        project.sources = {red};
        const lcomp::PreviewFrame second = commit(link, project);
        CHECK_EQ(second.status, lcomp::PictureStatus::Complete);
        CHECK_EQ(second.rendered_frames, 2u);
        CHECK(second.render_ns >= first.render_ns);
        CHECK_EQ(pixel(second, 1, 1), std::tuple(0, 0, 255, 255));

        // A camera is not drawn. The colour underneath still is, and the gap count is what the
        // panel will say. Roots are topmost first, so the camera is in front.
        ldoc::Source camera;
        camera.id = ldoc::Id::generate();
        camera.name = "Cam";
        camera.type = "source.v4l2";
        ldoc::Node cam_node = bound_to(camera);
        scene.roots.insert(scene.roots.begin(), std::move(cam_node));
        project.scenes = {scene};
        project.sources = {red, camera};
        const lcomp::PreviewFrame gapped = commit(link, project);
        CHECK_EQ(gapped.status, lcomp::PictureStatus::Partial);
        CHECK_EQ(gapped.gaps, 1u);
        CHECK_EQ(pixel(gapped, 2, 2), std::tuple(0, 0, 255, 255));

        // A refresh uses the assembled document and a new size budget. Read the actual memfd
        // through the real process, including its resize, instead of testing a sizing helper.
        REQUIRE(link.refresh(2).has_value());
        auto capture = link.receive(2000);
        REQUIRE(capture && *capture);
        CHECK_EQ((**capture).kind, lcomp::PreviewEvent::Kind::Capture);
        auto meters = link.receive(2000);
        REQUIRE(meters && meters->has_value());
        CHECK_EQ((**meters).kind, lcomp::PreviewEvent::Kind::Audio);
        auto resized = link.receive(2000);
        REQUIRE(resized && resized->has_value());
        REQUIRE((**resized).kind == lcomp::PreviewEvent::Kind::Frame);
        CHECK_EQ((**resized).frame.width, 2);
        CHECK_EQ((**resized).frame.height, 2);
        CHECK_EQ(pixel((**resized).frame, 1, 1), std::tuple(0, 0, 255, 255));
        auto ack = link.receive(2000);
        REQUIRE(ack && ack->has_value());
        CHECK_EQ((**ack).kind, lcomp::PreviewEvent::Kind::Ack);
        REQUIRE(link.refresh(2).has_value());
        capture = link.receive(2000);
        REQUIRE(capture && *capture);
        CHECK_EQ((**capture).kind, lcomp::PreviewEvent::Kind::Capture);
        meters = link.receive(2000);
        REQUIRE(meters && meters->has_value());
        CHECK_EQ((**meters).kind, lcomp::PreviewEvent::Kind::Audio);
        auto unchanged = link.receive(2000);
        REQUIRE(unchanged && unchanged->has_value());
        CHECK_EQ((**unchanged).kind, lcomp::PreviewEvent::Kind::Ack);

        // A transparent precomp over a blue source must preserve the background and blend
        // its red child once. Both selected backends execute this through the process.
        ldoc::Source overlay = colour_source(255, 0, 0);
        ldoc::Scene inner;
        inner.id = ldoc::Id::generate();
        inner.canvas = project.canvases[0].id;
        auto child = bound_to(overlay);
        child.layout.size = {2, 2};
        child.layout.fit = ldoc::FitMode::Stretch;
        inner.roots = {child};
        auto nested = ldoc::make_node(ldoc::NodeKind::Precomp, "nested");
        std::get<ldoc::PrecompData>(nested.data).scene = inner.id;
        nested.blend.opacity = 0.5;
        scene.roots = {nested, bound_to(red)};
        project.sources = {red, overlay};
        project.scenes = {scene, inner};
        const auto blended = commit(link, project);
        CHECK_EQ(blended.status, lcomp::PictureStatus::Complete);
        CHECK_EQ(pixel(blended, 0, 0), std::tuple(188, 0, 188, 255));
        CHECK_EQ(pixel(blended, 3, 3), std::tuple(0, 0, 255, 255));

        // Closing is the peer going away. Quit is the polite path; this is the one a crashed
        // client actually takes, and the renderer has to exit rather than wait.
        link.close();
        status = reap(*pid, 2000);
    }
    CHECK_EQ(status, 0);
    CHECK_EQ(count_fds(), fds_before);
}

LTEST_CASE(text_source_crosses_worker_and_renderer_and_obeys_placement) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child.has_value());
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto source = colour_source(1, 1, 1);
    source.type = "source.text";
    source.settings["text"] = std::string("Hi");
    source.settings["size"] = std::int64_t{20};
    source.settings["width"] = std::int64_t{32};
    source.settings["height"] = std::int64_t{32};
    auto node = bound_to(source);
    node.layout.fit = ldoc::FitMode::Stretch;
    node.layout.size = {32, 32};
    node.transform.position = {32, 0};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.roots = {node};
    auto project = show(scene, {source});
    project.canvases[0].width = 64;
    project.canvases[0].height = 32;
    bool ink = false;
    for (int attempt = 0; attempt < 100 && !ink; ++attempt) {
        const auto frame = commit(link, project);
        if (frame.status == lcomp::PictureStatus::Complete) {
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 64; ++x) {
                    if (x < 32) CHECK_EQ(pixel(frame, x, y), std::make_tuple(0, 0, 0, 255));
                    else ink = ink || std::get<0>(pixel(frame, x, y)) > 0;
                }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 2000), 0);
    CHECK(ink);
}

LTEST_CASE(native_alpha_media_crosses_worker_and_keeps_authored_box_and_letterboxing) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto source = colour_source(0, 0, 0);
    source.type = "source.media";
    source.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha.webm";
    source.settings["width"] = std::int64_t{32};
    source.settings["height"] = std::int64_t{32};
    auto node = bound_to(source);
    node.transform.position = {32, 0};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.roots = {node};
    auto project = show(scene, {source});
    project.canvases[0].width = 64;
    project.canvases[0].height = 32;
    bool ink = false;
    for (int attempt = 0; attempt < 100 && !ink; ++attempt) {
        const auto frame = commit(link, project);
        if (frame.status == lcomp::PictureStatus::Complete) {
            CHECK_EQ(pixel(frame, 10, 10), std::make_tuple(0, 0, 0, 255));
            CHECK_EQ(pixel(frame, 42, 0), std::make_tuple(0, 0, 0, 255));
            const auto centre = pixel(frame, 42, 10);
            ink = std::get<0>(centre) > 0 || std::get<1>(centre) > 0 || std::get<2>(centre) > 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 2000), 0);
    CHECK(ink);
}

LTEST_CASE(manual_fades_render_intermediate_frames_and_can_be_reversed) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto blue = colour_source(0, 0, 255), green = colour_source(0, 255, 0);
    ldoc::Scene before;
    before.id = ldoc::Id::generate(); before.roots = {bound_to(blue)};
    auto project = show(before, {blue, green});
    project.canvases.front().width = 4; project.canvases.front().height = 4;
    auto after = project.scenes.front();
    after.id = ldoc::Id::generate(); after.roots = {bound_to(green)};
    project.scenes.push_back(after);
    const auto before_id = project.decks.front().program_scene;
    commit(link, project);
    auto& deck = project.decks.front();
    deck.preview_scene = before_id;
    deck.program_scene = after.id;
    deck.transition = "transition.fade";
    const auto key = "ui.manual." + deck.id.to_string();
    for (double progress : {0.0, 0.5, 1.0, 0.5, 0.0}) {
        project.settings.extra[key] = progress;
        const auto frame = commit(link, project);
        const auto [r, g, b, a] = pixel(frame, 0, 0);
        CHECK(r == 0 && a == 255);
        if (progress == 0) CHECK(g == 0 && b == 255);
        else if (progress == 1) CHECK(g == 255 && b == 0);
        else CHECK(g > 100 && g < 230 && b > 100 && b < 230);
    }
    project.settings.extra[key] = 0.5;
    commit(link, project);
    project.settings.extra.erase(key);
    deck.transition_ms = 100;
    CHECK_EQ(pixel(commit(link, project), 0, 0), std::make_tuple(0, 0, 255, 255));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(pixel(commit(link, project), 0, 0), std::make_tuple(0, 255, 0, 255));
    deck.transition = "transition.fade-to-black";
    project.settings.extra[key] = 0.5;
    CHECK_EQ(pixel(commit(link, project), 0, 0), std::make_tuple(0, 0, 0, 255));
    project.settings.extra.erase(key);
    project.settings.extra["ui.immediate." + deck.id.to_string()] = true;
    deck.program_scene = before_id;
    CHECK_EQ(pixel(commit(link, project), 0, 0), std::make_tuple(0, 0, 255, 255));
    project.settings.extra.erase("ui.immediate." + deck.id.to_string());
    deck.transition = "transition.fade";
    deck.transition_ms = 600;
    deck.program_scene = after.id;
    CHECK_EQ(pixel(commit(link, project), 0, 0), std::make_tuple(0, 0, 255, 255));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto middle = pixel(commit(link, project), 0, 0);
    CHECK(std::get<1>(middle) > 30 && std::get<2>(middle) > 30);
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    CHECK_EQ(pixel(commit(link, project), 0, 0), std::make_tuple(0, 255, 0, 255));
    link.quit(); link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

LTEST_CASE(manual_fades_scale_audio_and_preview_decks_stay_silent) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto tone = colour_source(0, 0, 0);
    tone.type = "source.media";
    tone.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-tone.webm";
    tone.settings["loop"] = true;
    ldoc::Scene audible;
    audible.id = ldoc::Id::generate(); audible.roots = {bound_to(tone)};
    auto project = show(audible, {tone});
    const auto tone_node = project.scenes.front().roots.front().id;
    std::get<ldoc::SourceData>(project.scenes.front().roots.front().data).audio.bus = project.buses.front().id;
    ldoc::Scene quiet;
    quiet.id = ldoc::Id::generate(); quiet.canvas = project.canvases.front().id;
    project.scenes.push_back(quiet);
    const auto peak = [&](int milliseconds) {
        float loudest = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        do {
            REQUIRE(link.send_project(project));
            for (;;) {
                const auto event = link.receive(2000);
                REQUIRE(event && event->has_value());
                REQUIRE((**event).kind != lcomp::PreviewEvent::Kind::Fault);
                if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
                    for (const auto& level : (**event).audio)
                        if (level.available && level.node == tone_node) loudest = std::max({loudest, level.left, level.right});
                if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < until);
        return loudest;
    };
    const float full = peak(3000);
    REQUIRE(full > 0.01f);
    auto& deck = project.decks.front();
    deck.preview_scene = deck.program_scene;
    deck.program_scene = quiet.id;
    project.settings.extra["ui.manual." + deck.id.to_string()] = 0.5;
    (void)peak(200);
    const float half = peak(400);
    CHECK(half > full * 0.4f && half < full * 0.6f);
    project.settings.extra.erase("ui.manual." + deck.id.to_string());
    ldoc::Deck preview = deck;
    preview.id = ldoc::Id::generate();
    preview.program_scene = audible.id;
    project.settings.extra["ui.studio-preview-deck"] = preview.id.to_string();
    project.decks.push_back(preview);
    (void)peak(200);
    CHECK(peak(300) < 0.001f);
    link.quit(); link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

LTEST_CASE(stinger_overlays_alpha_switches_at_cut_and_routes_audio) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto blue = colour_source(0, 0, 255), green = colour_source(0, 255, 0);
    for (auto* source : {&blue, &green}) {
        source->settings["width"] = std::int64_t{32};
        source->settings["height"] = std::int64_t{24};
    }
    auto stinger = blue;
    stinger.id = ldoc::Id::generate();
    stinger.type = "source.media";
    stinger.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/alpha-tone.webm";
    stinger.settings["transitionStinger"] = true;
    stinger.settings["loop"] = false;
    stinger.settings["stingerCutMs"] = std::int64_t{500};
    stinger.settings["stingerDurationMs"] = std::int64_t{1000};
    ldoc::Scene before;
    before.id = ldoc::Id::generate(); before.roots = {bound_to(blue)};
    auto project = show(before, {blue, green, stinger});
    project.canvases.front().width = 32; project.canvases.front().height = 24;
    auto after = project.scenes.front();
    after.id = ldoc::Id::generate(); after.roots = {bound_to(green)};
    project.scenes.push_back(after);
    project.decks.front().transition = "transition.stinger:" + stinger.id.to_string();
    commit(link, project);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    project.decks.front().program_scene = after.id;
    const auto first = commit(link, project);
    CHECK_EQ(pixel(first, 0, 0), std::make_tuple(0, 0, 255, 255));
    const auto overlay = pixel(first, 12, 8);
    CHECK(std::get<0>(overlay) > 50 && std::get<2>(overlay) > 50);
    const auto start = std::chrono::steady_clock::now();
    bool audio = false, switched = false;
    lcomp::PreviewFrame last;
    for (int tick = 0; tick < 50; ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        REQUIRE(link.send_project(project));
        for (;;) {
            const auto event = link.receive(2000);
            REQUIRE(event && event->has_value());
            const auto& value = **event;
            REQUIRE(value.kind != lcomp::PreviewEvent::Kind::Fault);
            if (value.kind == lcomp::PreviewEvent::Kind::Frame) last = value.frame;
            if (value.kind == lcomp::PreviewEvent::Kind::Audio)
                for (const auto& level : value.audio)
                    audio = audio || (level.available && std::max(level.left, level.right) > 0.001f);
            if (value.kind == lcomp::PreviewEvent::Kind::Ack) break;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < std::chrono::milliseconds(300))
            CHECK_EQ(pixel(last, 0, 0), std::make_tuple(0, 0, 255, 255));
        if (elapsed > std::chrono::milliseconds(650))
            switched = pixel(last, 0, 0) == std::make_tuple(0, 255, 0, 255);
    }
    CHECK(switched);
    CHECK(audio);
    CHECK_EQ(pixel(last, 12, 8), std::make_tuple(0, 255, 0, 255));
    link.quit(); link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

LTEST_CASE(inactive_media_is_preloaded_for_first_scene_cut) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto source = colour_source(0, 0, 0);
    source.type = "source.media";
    const auto* actual = std::getenv("LIBERTAS_BENCH_MEDIA");
    source.settings["localFile"] = actual ? std::string(actual)
                                           : std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion.mkv";
    source.settings["loop"] = true;
    source.settings["restartOnActivate"] = false;
    ldoc::Scene media;
    media.id = ldoc::Id::generate();
    media.roots = {bound_to(source)};
    auto project = show(media, {source});
    auto blank = project.scenes.front();
    blank.id = ldoc::Id::generate();
    blank.roots.clear();
    project.scenes.push_back(blank);
    project.decks.front().program_scene = blank.id;
    commit(link, project);
    std::this_thread::sleep_for(std::chrono::seconds(actual ? 6 : 4));
    for (int cut = 0; cut < 3; ++cut) {
        project.decks.front().program_scene = media.id;
        const auto start = std::chrono::steady_clock::now();
        const auto frame = commit(link, project);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "preloaded media cut %d: %.2f ms, status %d\n", cut, elapsed,
                     static_cast<int>(frame.status));
        CHECK(frame.status == lcomp::PictureStatus::Complete);
        CHECK(elapsed < 250);
        project.decks.front().program_scene = blank.id;
        commit(link, project);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    project.sources.clear();
    project.scenes.front().roots.clear();
    CHECK(commit(link, project).gaps == 0);
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

LTEST_CASE(media_audio_crosses_worker_mixer_and_meter_transport) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child.has_value());
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto source = colour_source(0, 0, 0);
    source.type = "source.media";
    source.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion-tone.mkv";
    source.settings["width"] = std::int64_t{32};
    source.settings["height"] = std::int64_t{24};
    auto node = bound_to(source);
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    std::get<ldoc::SourceData>(node.data).audio.bus = project.buses.front().id;
    project.scenes[0].roots = {node};
    bool source_signal = false, bus_signal = false;
    float source_peak = 0;
    REQUIRE(link.send_project(project).has_value());
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < until && !(source_signal && bus_signal)) {
        auto event = link.receive(1000);
        REQUIRE(event && event->has_value());
        if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
            for (const auto& level : (**event).audio) {
                if (level.node == node.id && level.available && level.left > 0.01f) {
                    source_signal = true;
                    source_peak = level.left;
                }
                if (level.node == project.buses.front().id && level.available && level.left > 0.01f) bus_signal = true;
            }
        if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            REQUIRE(link.refresh(64).has_value());
        }
    }
    project.buses.front().mute = true;
    bool muted = false;
    // Drain the in-flight refresh before sending an edited project.
    for (int i = 0; i < 8; ++i) {
        auto event = link.receive(1000);
        if (event && event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
    }
    REQUIRE(link.send_project(project).has_value());
    const auto mute_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < mute_until && !muted) {
        auto event = link.receive(1000);
        REQUIRE(event && event->has_value());
        if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
            for (const auto& level : (**event).audio)
                if (level.node == project.buses.front().id && level.available && level.left == 0) muted = true;
        if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            REQUIRE(link.refresh(64).has_value());
        }
    }
    for (int i = 0; i < 8; ++i) {
        auto event = link.receive(1000);
        if (event && event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
    }
    for (const double db : {-20.0, -60.0, 0.0}) {
        auto& routing = std::get<ldoc::SourceData>(project.scenes[0].roots[0].data).audio;
        routing.gain_db = db;
        routing.mute = db == 0;
        REQUIRE(link.send_project(project));
        const float expected = routing.mute ? 0
                              : source_peak * static_cast<float>(std::pow(10.0, db / 20));
        bool followed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline && !followed) {
            const auto event = link.receive(1000);
            REQUIRE(event && event->has_value());
            if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
                for (const auto& level : (**event).audio)
                    if (level.node == node.id && level.available)
                        followed = expected == 0 ? level.left == 0 && level.right == 0
                                   : level.left > expected * 0.5f && level.left < expected * 1.5f;
            if ((**event).kind == lcomp::PreviewEvent::Kind::Ack && !followed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                REQUIRE(link.refresh(64));
            }
        }
        CHECK_MSG(followed, "source meter follows gain {} and mute {}", db, routing.mute);
        for (int i = 0; i < 8; ++i) {
            const auto event = link.receive(1000);
            if (event && event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack)
                break;
        }
    }
    link.quit(); link.close();
    CHECK_EQ(reap(*child, 3000), 0);
    CHECK(source_signal);
    CHECK(bus_signal);
    CHECK(muted);
}

LTEST_CASE(source_clone_audio_uses_original_worker_and_independent_route) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child.has_value());
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto source = colour_source(0, 0, 0);
    source.type = "source.media";
    source.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion-tone.mkv";
    source.settings["width"] = std::int64_t{32};
    source.settings["height"] = std::int64_t{24};
    auto clone = source;
    clone.id = ldoc::Id::generate();
    clone.type = "source.clone";
    clone.settings.clear();
    clone.settings["target"] = source.id.to_string();
    clone.settings["audio"] = true;
    auto node = bound_to(clone);
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source, clone});
    std::get<ldoc::SourceData>(node.data).audio.bus = project.buses.front().id;
    project.scenes[0].roots = {node};
    bool source_signal = false, bus_signal = false;
    REQUIRE(link.send_project(project).has_value());
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < until && !(source_signal && bus_signal)) {
        auto event = link.receive(1000);
        REQUIRE(event && event->has_value());
        if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
            for (const auto& level : (**event).audio) {
                if (level.node == node.id && level.available && level.left > 0.01f) source_signal = true;
                if (level.node == project.buses.front().id && level.available && level.left > 0.01f) bus_signal = true;
            }
        if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            REQUIRE(link.refresh(64).has_value());
        }
    }
    std::get<ldoc::SourceData>(project.scenes[0].roots[0].data).audio.mute = true;
    bool muted = false;
    // Drain the in-flight refresh before sending an edited project.
    for (int i = 0; i < 8; ++i) {
        auto event = link.receive(1000);
        if (event && event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
    }
    REQUIRE(link.send_project(project).has_value());
    const auto mute_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < mute_until && !muted) {
        auto event = link.receive(1000);
        REQUIRE(event && event->has_value());
        if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
            for (const auto& level : (**event).audio)
                if (level.node == project.buses.front().id && level.available && level.left == 0) muted = true;
        if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            REQUIRE(link.refresh(64).has_value());
        }
    }
    for (int i = 0; i < 8; ++i) {
        auto event = link.receive(1000);
        if (event && event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
    }
    link.quit(); link.close();
    CHECK_EQ(reap(*child, 3000), 0);
    CHECK(source_signal);
    CHECK(bus_signal);
    CHECK(muted);
}

LTEST_CASE(static_filter_preview_does_not_stop_program_scroll_animation) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto source = colour_source(255, 0, 0);
    ldoc::Effect scroll;
    scroll.id = ldoc::Id::generate();
    scroll.type = "filter.scroll";
    scroll.params["speed_x"] = 4.0;
    source.effects = {scroll};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.roots = {bound_to(source)};
    auto project = show(scene, {source});
    auto canvas = project.canvases.front();
    canvas.id = ldoc::Id::generate();
    project.canvases.push_back(canvas);
    commit(link, project);
    REQUIRE(link.refresh(4));
    bool programFrame = false, ack = false;
    for (int i = 0; i < 20 && !ack; ++i) {
        const auto event = link.receive(500);
        REQUIRE(event);
        if (!event->has_value())
            continue;
        if ((**event).kind == lcomp::PreviewEvent::Kind::Frame)
            programFrame = programFrame || (**event).frame.canvas == project.canvases.front().id;
        ack = (**event).kind == lcomp::PreviewEvent::Kind::Ack;
    }
    CHECK(ack && programFrame);
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

LTEST_CASE(monitoring_routes_real_renderer_audio_to_selected_sink_and_none) {
    if (!std::getenv("LIBERTAS_TEST_MONITOR"))
        return;
    std::string error;
    const auto base = "libertas_monitor_test_" + ldoc::Id::generate().to_string();
    auto sink1 = libertas::capture::AudioSink::open(base + "_1", error);
    auto sink2 = libertas::capture::AudioSink::open(base + "_2", error);
    REQUIRE_MSG(sink1 && sink2, "{}", error);

    struct Signal {
        std::mutex mutex;
        float peak = 0;

        void add(std::span<const float> samples) {
            std::lock_guard lock(mutex);
            for (const auto sample : samples)
                peak = std::max(peak, std::abs(sample));
        }

        float take() {
            std::lock_guard lock(mutex);
            return std::exchange(peak, 0);
        }
    } first, second;

    auto capture1 = libertas::capture::AudioCapture::open(
        base + "_1.monitor", true, [&](auto samples) { first.add(samples); }, error);
    auto capture2 = libertas::capture::AudioCapture::open(
        base + "_2.monitor", true, [&](auto samples) { second.add(samples); }, error);
    REQUIRE_MSG(capture1 && capture2, "{}", error);
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto source = colour_source(0, 0, 0);
    source.type = "source.media";
    source.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion-tone.mkv";
    source.settings["loop"] = true;
    auto node = bound_to(source);
    std::get<ldoc::SourceData>(node.data).audio.monitor = true;
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    project.scenes[0].roots = {node};
    for (const int selected : {-1, 1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}) {
        project.sources.front().effects.clear();
        project.scenes.front().effects.clear();
        if (selected >= 3 && selected < 16) {
            ldoc::Effect effect;
            effect.id = ldoc::Id::generate();
            effect.type = selected >= 7 ? "filter.gain" : "unknown.audio-filter";
            if (selected >= 7)
                effect.params["db"] = -60.0;
            effect.enabled = selected % 2 == 1;
            if (selected >= 11) {
                effect.type = "filter.vst";
                effect.params.clear();
                effect.params["path"] = std::string(LIBERTAS_VST_FIXTURE);
                effect.params["parameter.0"] = selected == 14 ? 1.0 : selected == 13 ? 0.5 : 0.0;
                effect.enabled = selected != 12 && selected != 15;
            }
            auto& stack = selected <= 4 || selected == 7 || selected == 8
                              ? project.sources.front().effects
                              : project.scenes.front().effects;
            stack.push_back(effect);
        }
        if (selected == 16)
            project.settings.extra["ui.filter-preview-deck"] = project.decks.front().id.to_string();
        else
            project.settings.extra.erase("ui.filter-preview-deck");
        if (selected < 0)
            project.settings.extra.erase("audio.monitor.output");
        else
            project.settings.extra["audio.monitor.output"] =
                selected == 0 ? std::string("none")
                              : "sink:" + base + "_" + std::to_string(selected >= 3 ? 1 : selected);
        REQUIRE(link.send_project(project));
        const auto start = std::chrono::steady_clock::now();
        const auto end = start + std::chrono::milliseconds(1400);
        bool pending = true, cleared = false;
        while (std::chrono::steady_clock::now() < end) {
            if (!cleared &&
                std::chrono::steady_clock::now() - start > std::chrono::milliseconds(500)) {
                first.take();
                second.take();
                cleared = true;
            }
            auto event = link.receive(50);
            REQUIRE(event);
            if (event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) {
                pending = false;
                if (std::chrono::steady_clock::now() + std::chrono::milliseconds(100) < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    REQUIRE(link.refresh(64));
                    pending = true;
                }
            }
        }
        for (int i = 0; pending && i < 20; ++i) {
            auto event = link.receive(100);
            REQUIRE(event);
            if (event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack)
                pending = false;
        }
        REQUIRE(!pending);
        CHECK_EQ(first.take() > 0.01f, selected == 1 || selected == 4 || selected == 6 ||
                                           selected == 8 || selected == 10 || selected == 12 ||
                                           selected == 13 || selected == 15 || selected == 17);
        CHECK_EQ(second.take() > 0.01f, selected == 2);
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
    CHECK(!libertas::capture::AudioMonitor::open(error, base + "_missing"));
}

LTEST_CASE(video_capture_audio_uses_one_source_and_respects_none_and_deletion) {
    if (!std::getenv("LIBERTAS_TEST_MONITOR"))
        return;
    std::string error;
    const auto name = "libertas_video_audio_" + ldoc::Id::generate().to_string();
    auto input = libertas::capture::AudioSink::open(name, error);
    auto other_input = libertas::capture::AudioSink::open(name + "_other", error);
    auto output = libertas::capture::AudioSink::open(name + "_out", error);
    REQUIRE_MSG(input && other_input && output, "{}", error);
    auto tone = libertas::capture::AudioMonitor::open(error, name);
    auto other_tone = libertas::capture::AudioMonitor::open(error, name + "_other");
    REQUIRE_MSG(tone && other_tone, "{}", error);
    std::jthread feed([&](std::stop_token stop) {
        std::vector<float> block(960, 0.1f);
        std::vector<float> other_block(960, 0.2f);
        while (!stop.stop_requested()) {
            tone->write(block);
            other_tone->write(other_block);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::mutex mutex;
    float peak = 0;
    auto received = libertas::capture::AudioCapture::open(
        name + "_out.monitor", true,
        [&](auto samples) {
            std::lock_guard lock(mutex);
            for (float sample : samples)
                peak = std::max(peak, std::abs(sample));
        },
        error);
    REQUIRE(received);
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto source = colour_source(0, 0, 0);
    source.type = "source.v4l2";
    source.settings["device"] = std::string("/dev/libertas-missing-test-video");
    source.settings["audioDevice"] = name + ".monitor";
    auto node = bound_to(source);
    std::get<ldoc::SourceData>(node.data).audio.monitor = true;
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    std::get<ldoc::SourceData>(node.data).audio.bus = project.buses[0].id;
    project.settings.extra["audio.monitor.output"] = "sink:" + name + "_out";
    for (int step = 0; step < 9; ++step) {
        project.sources = {source};
        project.scenes[0].roots = {node};
        if (step == 1)
            project.sources[0].settings["audioDevice"] = std::string("none");
        if (step == 2)
            project.sources[0].settings["audioDevice"] = name + "_missing";
        if (step == 3)
            project.sources[0].settings["audioDevice"] = std::string("default");
        if (step == 4)
            project.scenes[0].roots[0].visible = false;
        if (step == 6) {
            project.sources.clear();
            project.scenes[0].roots.clear();
        }
        if (step == 7)
            project.sources[0].settings.erase("audioDevice");
        if (step == 8)
            project.sources[0].settings["audioDevice"] = name + "_other.monitor";
        REQUIRE(link.send_project(project));
        const auto start = std::chrono::steady_clock::now();
        const auto end = start + std::chrono::milliseconds(1400);
        bool pending = true, cleared = false, metered = false;
        while (std::chrono::steady_clock::now() < end) {
            if (!cleared &&
                std::chrono::steady_clock::now() - start > std::chrono::milliseconds(600)) {
                std::lock_guard lock(mutex);
                peak = 0;
                metered = false;
                cleared = true;
            }
            auto event = link.receive(50);
            REQUIRE(event);
            if (!event->has_value())
                continue;
            if ((**event).kind == lcomp::PreviewEvent::Kind::Audio && cleared)
                for (const auto& level : (**event).audio)
                    if (level.node == node.id && level.available && level.left > 0.01f)
                        metered = true;
            if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
                pending = false;
                if (std::chrono::steady_clock::now() + std::chrono::milliseconds(100) < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    REQUIRE(link.refresh(64));
                    pending = true;
                }
            }
        }
        REQUIRE(!pending);
        const bool audible = step == 0 || step == 5 || step == 8;
        CHECK_EQ(metered, audible);
        std::lock_guard lock(mutex);
        CHECK_EQ(peak > 0.01f, audible);
        if (step == 8) CHECK(peak > 0.15f);
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

LTEST_CASE(video_audio_default_matches_physical_capture_card) {
    const char* video = std::getenv("LIBERTAS_TEST_VIDEO_AUDIO");
    if (!video)
        return;
    std::string error;
    const auto input = libertas::capture::video_audio_device(video, error);
    REQUIRE_MSG(!input.empty(), "{}", error);
    const char* expected = std::getenv("LIBERTAS_TEST_VIDEO_AUDIO_EXPECTED");
    REQUIRE(expected);
    CHECK_EQ(input, std::string(expected));
}

LTEST_CASE(capture_reuses_one_snapshot_across_direct_and_nested_placements) {
    const char* connector = std::getenv("LIBERTAS_TEST_CAPTURE");
    const char* video = std::getenv("LIBERTAS_TEST_VIDEO");
    const char* window = std::getenv("LIBERTAS_TEST_WINDOW");
    if (!connector && !video && !window)
        return;
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child.has_value());
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto source = colour_source(0, 0, 0);
    source.type = window ? "source.window" : video ? "source.v4l2" : "source.screen";
    if (window) source.settings["window"] = std::string(window);
    else if (video) {
        source.settings["device"] = std::string(video);
        source.settings["resolution"] = std::string("2560x1440");
        source.settings["frameRate"] = std::string("60");
    } else source.settings["output"] = std::string(connector);
    auto node = bound_to(source);
    node.layout.size = {64, 72};
    node.layout.fit = ldoc::FitMode::Stretch;
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    project.canvases[0].width = 128;
    project.canvases[0].height = 72;
    ldoc::Scene inner;
    inner.id = ldoc::Id::generate();
    inner.canvas = project.canvases[0].id;
    auto duplicate = node;
    duplicate.id = ldoc::Id::generate();
    inner.roots = {node, duplicate};
    auto nested = ldoc::make_node(ldoc::NodeKind::Precomp, "same screen again");
    auto& pre = std::get<ldoc::PrecompData>(nested.data);
    pre.scene = inner.id;
    pre.size = {64, 72};
    nested.transform.position = {64, 0};
    project.scenes[0].roots = {node, nested};
    project.scenes.push_back(inner);
    if (video && !window) {
        auto hidden = project;
        hidden.decks.front().program_scene.reset();
        commit(link, hidden);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        CHECK(commit(link, project).gaps == 0);
    }
    int complete = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (complete < 20 && std::chrono::steady_clock::now() < deadline) {
        const auto frame = commit(link, project);
        if (frame.status == lcomp::PictureStatus::Complete) {
            ++complete;
            for (int y = 0; y < 72; ++y)
                for (int x = 0; x < 64; ++x)
                    CHECK_EQ(pixel(frame, x, y), pixel(frame, x + 64, y));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (video && !window) {
        const auto device = std::filesystem::canonical(video);
        const auto device_open = [&] {
            std::error_code ec;
            for (const auto& fd : std::filesystem::directory_iterator(
                     "/proc/" + std::to_string(*child) + "/fd", ec)) {
                if (std::filesystem::canonical(fd.path(), ec) == device && !ec) return true;
                ec.clear();
            }
            return false;
        };
        const auto active = project.scenes.front().id;
        ldoc::Scene blank;
        blank.id = ldoc::Id::generate();
        blank.canvas = project.canvases.front().id;
        project.scenes.push_back(blank);
        CHECK(device_open());
        for (int i = 0; i < 3; ++i) {
            project.decks.front().program_scene = blank.id;
            commit(link, project);
            CHECK(device_open());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            project.decks.front().program_scene = active;
            CHECK(commit(link, project).gaps == 0);
        }
        // The setting itself must not restart a visible device.
        project.sources.front().settings["deactivateWhenHidden"] = true;
        CHECK(commit(link, project).gaps == 0);
        project.decks.front().program_scene = blank.id;
        commit(link, project);
        CHECK(!device_open());
        project.sources.front().settings["deactivateWhenHidden"] = false;
        project.decks.front().program_scene = active;
        commit(link, project);
        CHECK(device_open());
        project.decks.front().program_scene = blank.id;
        commit(link, project);
        CHECK(device_open());
        project.sources.clear();
        commit(link, project);
        CHECK(!device_open());
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 2000), 0);
    CHECK_EQ(complete, 20);
}

LTEST_CASE(measure_saved_scene_through_real_renderer) {
    const auto* path = std::getenv("LIBERTAS_BENCH_PROJECT");
    if (!path)
        return;
    auto opened = ldoc::open_project(path);
    REQUIRE_MSG(opened, "{}", opened.error());
    auto& project = opened->project;
    const auto* name = std::getenv("LIBERTAS_BENCH_SCENE");
    const auto scene = std::ranges::find_if(project.scenes, [&](const auto& entry) {
        return entry.name == (name ? name : "FS.W.CAM");
    });
    REQUIRE(scene != project.scenes.end());
    for (auto& deck : project.decks)
        if (deck.canvas == scene->canvas)
            deck.program_scene = scene->id;
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    REQUIRE(link.send_project(project, 960));
    lcomp::PreviewFrame baseline, last, checkpoint;
    const auto* switch_name = std::getenv("LIBERTAS_BENCH_SWITCH");
    const auto* warmup = std::getenv("LIBERTAS_BENCH_WARMUP");
    const int cut_at = warmup ? std::clamp(std::atoi(warmup), 120, 35000) : 360;
    auto switch_started = std::chrono::steady_clock::time_point{};
    const auto* requested = std::getenv("LIBERTAS_BENCH_FRAMES");
    const int iterations = requested ? std::clamp(std::atoi(requested), 240, 36000) : 1920;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        bool ack = false;
        while (!ack) {
            const auto event = link.receive(3000);
            REQUIRE(event && event->has_value());
            REQUIRE((**event).kind != lcomp::PreviewEvent::Kind::Fault);
            REQUIRE((**event).kind != lcomp::PreviewEvent::Kind::PeerGone);
            if ((**event).kind == lcomp::PreviewEvent::Kind::Frame &&
                (**event).frame.canvas == scene->canvas)
                last = (**event).frame;
            ack = (**event).kind == lcomp::PreviewEvent::Kind::Ack;
        }
        if (iteration == cut_at && switch_name)
            std::fprintf(stderr, "scene cut to %s: %.3f ms to first frame/ack, status %d, %u gaps\n",
                         switch_name, std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - switch_started).count(),
                         static_cast<int>(last.status), last.gaps);
        if (iteration == 119)
            baseline = checkpoint = last;
        if (iteration > 119 && (iteration - 119) % 600 == 0) {
            const auto frames = last.rendered_frames - checkpoint.rendered_frames;
            std::fprintf(stderr, "window %d: %.3f ms/frame, %llu rendered, %llu missed\n",
                         iteration,
                         frames ? static_cast<double>(last.render_ns - checkpoint.render_ns) /
                                      static_cast<double>(frames) / 1e6
                                : 0.0,
                         static_cast<unsigned long long>(frames),
                         static_cast<unsigned long long>(last.render_lag_frames -
                                                         checkpoint.render_lag_frames));
            checkpoint = last;
        }
        std::this_thread::sleep_until(start + std::chrono::microseconds(16667));
        if (iteration == cut_at - 1 && switch_name) {
            const auto target = std::ranges::find(project.scenes, std::string(switch_name), &ldoc::Scene::name);
            REQUIRE(target != project.scenes.end());
            for (auto& deck : project.decks)
                if (deck.canvas == target->canvas) deck.program_scene = target->id;
            switch_started = std::chrono::steady_clock::now();
            REQUIRE(link.send_project(project, 960));
        } else if (iteration + 1 < iterations)
            REQUIRE(link.refresh(960));
    }
    REQUIRE(last.rendered_frames > baseline.rendered_frames);
    if (const auto* frame_path = std::getenv("LIBERTAS_BENCH_FRAME")) {
        std::ofstream image(frame_path, std::ios::binary);
        image << "P6\n" << last.width << ' ' << last.height << "\n255\n";
        for (int y = 0; y < last.height; ++y)
            for (int x = 0; x < last.width; ++x) {
                const auto i = static_cast<std::size_t>(y) * last.stride + static_cast<std::size_t>(x) * 4;
                for (const auto channel : {2u, 1u, 0u}) image.put(static_cast<char>(last.bytes()[i + channel]));
            }
        REQUIRE(image.good());
        std::fprintf(stderr, "saved frame: %s, status %d, %u gaps\n", frame_path, int(last.status), last.gaps);
    }
    const auto count = last.rendered_frames - baseline.rendered_frames;
    std::fprintf(
        stderr, "%s live: %.3f ms/frame, %llu rendered, %llu missed after warmup\n",
        scene->name.c_str(),
        static_cast<double>(last.render_ns - baseline.render_ns) / static_cast<double>(count) / 1e6,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(last.render_lag_frames - baseline.render_lag_frames));
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

int main() {
    if (std::getenv("LIBERTAS_TEST_PROJECTORS")) {
        setenv("LIBERTAS_RENDER_BACKEND", "gpu", 1);
        std::erase_if(ltest::registry(), [](const auto& test) {
            return test.name != "native_projector_presents_gpu_frames_while_ui_is_paused";
        });
    }
    if (std::getenv("LIBERTAS_TEST_STUDIO"))
        std::erase_if(ltest::registry(), [](const auto& test) {
            return !test.name.starts_with("manual_fades_");
        });
    if (std::getenv("LIBERTAS_BENCH_PROJECT"))
        std::erase_if(ltest::registry(), [](const auto& test) {
            return test.name != "measure_saved_scene_through_real_renderer";
        });
    if (!std::getenv("LIBERTAS_TEST_CAPTURE") && !std::getenv("LIBERTAS_TEST_VIDEO") && !std::getenv("LIBERTAS_TEST_WINDOW"))
        std::erase_if(ltest::registry(), [](const auto& test) {
            return test.name == "capture_reuses_one_snapshot_across_direct_and_nested_placements";
        });
    return ltest::run_all("preview");
}

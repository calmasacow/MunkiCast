// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_worker.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <lcore/log.hpp>
#include <semaphore>
#include <sys/wait.h>

namespace {
std::counting_semaphore<2> preload_slots{2};
std::atomic<int> priority_pending{0};

bool reserve_priority(const ldoc::Source& source, bool active) {
    const auto it = source.settings.find("restartOnActivate");
    const auto* restart = it == source.settings.end() ? nullptr : std::get_if<bool>(&it->second);
    const bool priority = !active &&
                          (source.type == "source.media" || source.type == "source.playlist") &&
                          restart && !*restart;
    if (priority)
        priority_pending.fetch_add(1);
    return priority;
}

struct PriorityReady {
    bool pending;

    void release() {
        if (pending) {
            priority_pending.fetch_sub(1);
            pending = false;
        }
    }

    ~PriorityReady() { release(); }
};

struct PreloadSlot {
    bool held = false;

    void release() {
        if (held) {
            preload_slots.release();
            held = false;
        }
    }

    ~PreloadSlot() { release(); }
};
} // namespace

SourceWorker::SourceWorker(const ldoc::Source& source, Callback callback,
                           std::function<void(std::span<const float>)> audio, bool initial_active)
    : active_(initial_active), priority_(reserve_priority(source, initial_active)),
      thread_([this, source, callback = std::move(callback),
               audio = std::move(audio)](std::stop_token stop) {
          PriorityReady priority{priority_};
          while (!stop.stop_requested()) {
              pid_t child = -1;
              const auto opened = std::chrono::steady_clock::now();
              bool first_frame = true;
              try {
                  PreloadSlot preload;
                  auto ends = lipc::Channel::pair();
                  if (!ends)
                      throw std::runtime_error(ends.error());
                  auto spawned = lcomp::spawn_renderer(MUNKICAST_SOURCE_EXE,
                                                       std::move(ends->second).take_socket());
                  if (!spawned)
                      throw std::runtime_error(spawned.error());
                  child = *spawned;
                  lcomp::PreviewLink link(std::move(ends->first), true);
                  auto hello = link.take_hello(5000);
                  if (!hello)
                      throw std::runtime_error(hello.error());
                  auto project = ldoc::make_default_project("Source worker");
                  project.sources = {source};
                  bool active = active_.load();
                  bool allow_player = active;
                  bool needs_player = false;
                  project.sources.front().settings["worker.allowPlayer"] = allow_player;
                  auto restart = restart_.load();
                  project.sources.front().settings["worker.restart"] = restart;
                  project.sources.front().settings["worker.active"] = active;
                  auto sent = link.send_project(project);
                  if (!sent)
                      throw std::runtime_error(sent.error());
                  // Concurrent media opens probe codecs/devices before their first ack.
                  // Keep the shorter watchdog once configuration has completed.
                  auto next_refresh = std::chrono::steady_clock::now();
                  auto deadline = next_refresh + std::chrono::seconds(30);
                  auto preload_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                  while (!stop.stop_requested()) {
                      if (std::chrono::steady_clock::now() >= preload_until) {
                          preload.release();
                          priority.release();
                      }
                      auto event = link.receive(100);
                      if (!event)
                          throw std::runtime_error(event.error());
                      if (!*event) {
                          if (std::chrono::steady_clock::now() > deadline)
                              throw std::runtime_error("source worker timed out");
                          continue;
                      }
                      auto& value = **event;
                      if (value.kind == lcomp::PreviewEvent::Kind::Pcm) {
                          preload.release();
                          priority.release();
                          if (audio)
                              audio(value.pcm.samples);
                      }
                      if (value.kind == lcomp::PreviewEvent::Kind::PeerGone)
                          throw std::runtime_error("source worker exited");
                      if (value.kind == lcomp::PreviewEvent::Kind::Fault)
                          throw std::runtime_error(value.fault);
                      if (value.kind == lcomp::PreviewEvent::Kind::Frame) {
                          // A worker defers QMediaPlayer construction until a startup slot is
                          // available.
                          needs_player = value.frame.status == lcomp::PictureStatus::NoProgram;
                          if (value.frame.status == lcomp::PictureStatus::Complete) {
                              preload.release();
                              priority.release();
                              if (first_frame && std::getenv("MUNKICAST_BENCHMARK"))
                                  lc::info("source ready {} in {:.1f} ms", source.name,
                                           std::chrono::duration<double, std::milli>(
                                               std::chrono::steady_clock::now() - opened)
                                               .count());
                              first_frame = false;
                              callback(
                                  std::make_shared<lcomp::PreviewFrame>(std::move(value.frame)));
                          } else
                              callback({});
                      }
                      if (value.kind == lcomp::PreviewEvent::Kind::Ack) {
                          next_refresh = std::max(next_refresh + std::chrono::milliseconds(16),
                                                  std::chrono::steady_clock::now());
                          std::this_thread::sleep_until(next_refresh);
                          const bool next_active = active_.load();
                          const bool old_allow_player = allow_player;
                          if (needs_player && !allow_player) {
                              if (next_active)
                                  allow_player = true;
                              else if ((priority_ || priority_pending.load() == 0) &&
                                       preload_slots.try_acquire()) {
                                  preload.held = true;
                                  allow_player = true;
                              }
                              if (allow_player)
                                  preload_until =
                                      std::chrono::steady_clock::now() + std::chrono::seconds(5);
                          }
                          project.sources.front().settings["worker.allowPlayer"] = allow_player;
                          project.sources.front().settings["worker.active"] = next_active;
                          const auto next_restart = restart_.load();
                          project.sources.front().settings["worker.restart"] = next_restart;
                          auto refresh = next_active == active && next_restart == restart &&
                                                 old_allow_player == allow_player
                                             ? link.refresh()
                                             : link.send_project(project);
                          restart = next_restart;
                          active = next_active;
                          if (!refresh)
                              throw std::runtime_error(refresh.error());
                          deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                      }
                  }
                  link.quit();
                  link.close();
              } catch (const std::exception& e) {
                  priority.release();
                  lc::warn("source {}: {}", source.name, e.what());
              }
              callback({});
              if (child > 0) {
                  int status = 0;
                  bool reaped = false;
                  for (int i = 0; i < 50; ++i) {
                      if (::waitpid(child, &status, WNOHANG) != 0) {
                          reaped = true;
                          break;
                      }
                      std::this_thread::sleep_for(std::chrono::milliseconds(10));
                  }
                  if (!reaped) {
                      ::kill(child, SIGKILL);
                      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
                      }
                  }
              }
              for (int i = 0; i < 30 && !stop.stop_requested(); ++i)
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
      }) {}

SourceWorker::~SourceWorker() = default;

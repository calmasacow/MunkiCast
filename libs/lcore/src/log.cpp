// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcore/log.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

namespace lc {
namespace {

Level g_level = Level::Info;
std::mutex g_mutex;

bool stderr_is_tty() {
    static const bool tty = ::isatty(STDERR_FILENO) == 1;
    return tty;
}

struct Style {
    std::string_view tag;
    std::string_view colour;
};

Style style_for(Level lvl) {
    switch (lvl) {
    case Level::Trace: return {"trace", "\033[2;37m"};
    case Level::Debug: return {"debug", "\033[0;36m"};
    case Level::Info:  return {"info ", "\033[0;32m"};
    case Level::Warn:  return {"warn ", "\033[0;33m"};
    case Level::Error: return {"error", "\033[1;31m"};
    }
    return {"?????", ""};
}

} // namespace

void set_level(Level lvl) { g_level = lvl; }
Level level() { return g_level; }

void log_raw(Level lvl, std::string_view msg) {
    const Style s = style_for(lvl);
    std::lock_guard lock(g_mutex);
    if (stderr_is_tty())
        std::fprintf(stderr, "%s%s\033[0m %.*s\n", s.colour.data(), s.tag.data(),
                     static_cast<int>(msg.size()), msg.data());
    else
        std::fprintf(stderr, "%s %.*s\n", s.tag.data(), static_cast<int>(msg.size()), msg.data());
}

void init_from_env() {
    const char* env = std::getenv("MUNKICAST_LOG");
    if (env == nullptr)
        return;
    const std::string v(env);
    if (v == "trace") g_level = Level::Trace;
    else if (v == "debug") g_level = Level::Debug;
    else if (v == "info")  g_level = Level::Info;
    else if (v == "warn")  g_level = Level::Warn;
    else if (v == "error") g_level = Level::Error;
}

} // namespace lc

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace lc {

enum class Level { Trace, Debug, Info, Warn, Error };

/// Minimum level that will be emitted. Defaults to Info; MUNKICAST_LOG=debug etc. overrides.
void set_level(Level lvl);
Level level();

/// Emit a pre-formatted line. Prefer the templated helpers below.
void log_raw(Level lvl, std::string_view msg);

template <typename... Args>
void log(Level lvl, std::format_string<Args...> fmt, Args&&... args) {
    if (lvl < level())
        return;
    log_raw(lvl, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void trace(std::format_string<Args...> f, Args&&... a) {
    log(Level::Trace, f, std::forward<Args>(a)...);
}
template <typename... Args> void debug(std::format_string<Args...> f, Args&&... a) {
    log(Level::Debug, f, std::forward<Args>(a)...);
}
template <typename... Args> void info(std::format_string<Args...> f, Args&&... a) {
    log(Level::Info, f, std::forward<Args>(a)...);
}
template <typename... Args> void warn(std::format_string<Args...> f, Args&&... a) {
    log(Level::Warn, f, std::forward<Args>(a)...);
}
template <typename... Args> void error(std::format_string<Args...> f, Args&&... a) {
    log(Level::Error, f, std::forward<Args>(a)...);
}

/// Reads MUNKICAST_LOG from the environment. Call once at startup.
void init_from_env();

} // namespace lc

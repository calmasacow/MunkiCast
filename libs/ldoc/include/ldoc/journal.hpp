// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/command.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace ldoc {

/// An append-only log of commands, one JSON object per line.
///
/// It is what turns "the application closed without saving" from lost work into a question the
/// operator gets asked. Every command is appended and fsynced as it is applied, tagged with the
/// generation of the saved project it was applied *to*; on open, the entries tagged with the
/// generation that was just loaded are replayed. Saving commits a new generation and clears it.
///
/// JSON Lines rather than one JSON document, because a document has to be closed to be valid and
/// a log that is only valid once it has been closed is useless for a crash. A torn final line --
/// a process that died mid-write -- costs that one entry and nothing before it.
class Journal {
public:
    Journal() = default;
    ~Journal();
    Journal(Journal&& other) noexcept;
    Journal& operator=(Journal&& other) noexcept;
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    [[nodiscard]] static std::expected<Journal, std::string> open(
        const std::filesystem::path& path);

    [[nodiscard]] bool is_open() const { return fd_ >= 0; }
    void close();

    /// Append one command, fsynced before returning.
    ///
    /// `base_generation` is the saved generation the command was applied to. Replaying only the
    /// entries that match the generation actually loaded is what stops a stale journal -- one
    /// left behind by a crash *after* a save -- from being applied twice.
    std::expected<void, std::string> record(const Command& command,
                                            std::uint64_t base_generation);

    /// Empty the log. Called once a save has committed, since everything in it is now in the
    /// project.
    std::expected<void, std::string> clear();

    struct Entry {
        std::uint64_t base_generation = 0;
        Command command;
    };

    struct Replay {
        std::vector<Entry> entries;
        /// True when the last line was incomplete, which is what a crash mid-write looks like.
        bool truncated_tail = false;
        /// Lines that parsed as JSON but were not commands this build knows.
        std::size_t unreadable = 0;
    };

    /// Read a journal without opening it for writing.
    [[nodiscard]] static std::expected<Replay, std::string> read(
        const std::filesystem::path& path);

private:
    explicit Journal(int fd, std::filesystem::path path) : fd_(fd), path_(std::move(path)) {}

    int fd_ = -1;
    std::filesystem::path path_;
};

} // namespace ldoc

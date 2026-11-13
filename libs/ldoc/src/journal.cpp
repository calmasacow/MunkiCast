// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/journal.hpp"

#include "lcore/atomic_file.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <sstream>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace ldoc {
namespace {

std::string why(std::string_view what, const std::filesystem::path& p) {
    return std::format("{} {}: {}", what, p.string(), std::strerror(errno));
}

/// One line of JSON with no newlines inside it, so the log stays line-oriented and a partial
/// write can only ever damage the line it was writing.
std::string one_line(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\n' || c == '\r')
            continue;
        out += c;
    }
    return out;
}

} // namespace

Journal::~Journal() { close(); }

Journal::Journal(Journal&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

Journal& Journal::operator=(Journal&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

void Journal::close() {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

std::expected<Journal, std::string> Journal::open(const std::filesystem::path& path) {
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(why("open", path));
    return Journal{fd, path};
}

std::expected<void, std::string> Journal::record(const Command& command,
                                                 std::uint64_t base_generation) {
    if (fd_ < 0)
        return std::unexpected("the journal is not open");

    // The command's own JSON, with the generation folded in. Written compactly: a journal is read
    // by a program and by a human debugging a crash, in that order.
    std::string text = one_line(command_to_json(command));
    if (text.empty() || text.front() != '{')
        return std::unexpected("a command did not serialise to an object");
    text.insert(1, std::format(" \"gen\" : {},", base_generation));
    text += '\n';

    const char* p = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(why("write", path_));
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    // Fsynced per command. A journal that is only durable at some later point is not a journal;
    // the one command that matters is the last one before the crash.
    if (::fsync(fd_) != 0)
        return std::unexpected(why("fsync", path_));
    return {};
}

std::expected<void, std::string> Journal::rotate(const std::filesystem::path& retired) {
    if (fd_ < 0)
        return std::unexpected("the journal is not open");
    std::error_code ec;
    // A previous background save that failed left its entries in `retired`. Keep them by
    // appending the current file after them, rather than overwriting.
    if (std::filesystem::exists(retired, ec)) {
        const auto current = lc::read_file(path_);
        if (current && !current->empty()) {
            const int out = ::open(retired.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
            if (out < 0)
                return std::unexpected(why("open", retired));
            const char* p = current->data();
            std::size_t left = current->size();
            while (left > 0) {
                const ssize_t n = ::write(out, p, left);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { ::close(out); return std::unexpected(why("write", retired)); }
                p += n;
                left -= static_cast<std::size_t>(n);
            }
            const bool synced = ::fsync(out) == 0;
            ::close(out);
            if (!synced)
                return std::unexpected(why("fsync", retired));
        }
        if (::ftruncate(fd_, 0) != 0 || ::fsync(fd_) != 0)
            return std::unexpected(why("truncate", path_));
        return {};
    }
    if (::rename(path_.c_str(), retired.c_str()) != 0)
        return std::unexpected(why("rename", path_));
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(why("open", path_));
    ::close(fd_);
    fd_ = fd;
    if (const int dir = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        dir >= 0) {
        (void)::fsync(dir);
        ::close(dir);
    }
    return {};
}

std::expected<void, std::string> Journal::clear() {
    if (fd_ < 0)
        return std::unexpected("the journal is not open");
    if (::ftruncate(fd_, 0) != 0)
        return std::unexpected(why("truncate", path_));
    if (::fsync(fd_) != 0)
        return std::unexpected(why("fsync", path_));
    return {};
}

std::expected<Journal::Replay, std::string> Journal::read(const std::filesystem::path& path) {
    Replay out;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return out;

    const auto text = lc::read_file(path);
    if (!text)
        return std::unexpected(text.error());

    std::istringstream in{*text};
    std::string line;
    bool last_line_complete = text->empty() || text->back() == '\n';
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        auto command = command_from_json(line);
        if (!command) {
            // A line that will not parse at the *end* of the file is a torn write, which is
            // expected after a crash and costs one command. Anywhere else it is corruption, and
            // the entries before it are still good.
            if (in.eof() && !last_line_complete)
                out.truncated_tail = true;
            else
                ++out.unreadable;
            continue;
        }

        std::uint64_t gen = 0;
        if (const auto at = line.find("\"gen\""); at != std::string::npos) {
            const auto colon = line.find(':', at);
            if (colon != std::string::npos)
                gen = std::strtoull(line.c_str() + colon + 1, nullptr, 10);
        }
        out.entries.push_back(Entry{gen, std::move(*command)});
    }
    return out;
}

} // namespace ldoc

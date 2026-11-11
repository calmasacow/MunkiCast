// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcore/atomic_file.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lc {
namespace {

std::string why(const char* what, const std::filesystem::path& p) {
    return std::format("{} {}: {}", what, p.string(), std::strerror(errno));
}

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    void reset() {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }
    int release() {
        const int f = fd_;
        fd_ = -1;
        return f;
    }

private:
    int fd_ = -1;
};

} // namespace

std::expected<void, std::string> write_atomic(const std::filesystem::path& path,
                                             std::string_view data) {
    std::error_code ec;
    const std::filesystem::path dir = path.has_parent_path() ? path.parent_path() : ".";
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return std::unexpected(std::format("create {}: {}", dir.string(), ec.message()));

    std::filesystem::path tmp = path;
    tmp += ".tmp-XXXXXX";
    std::string tmp_str = tmp.string();
    std::vector<char> tmpl(tmp_str.begin(), tmp_str.end());
    tmpl.push_back('\0');

    Fd fd{::mkstemp(tmpl.data())};
    if (!fd.valid())
        return std::unexpected(why("mkstemp", tmp));
    const std::filesystem::path tmp_path{tmpl.data()};

    // From here on, every failure path removes the temporary. A stray .tmp-* beside a layout file
    // is a small thing, but it is also exactly the kind of litter that makes a config directory
    // untrustworthy.
    auto fail = [&tmp_path](std::string msg) {
        std::error_code rm;
        std::filesystem::remove(tmp_path, rm);
        return std::unexpected(std::move(msg));
    };

    if (::fchmod(fd.get(), 0644) != 0)
        return fail(why("fchmod", tmp_path));

    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd.get(), p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return fail(why("write", tmp_path));
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }

    if (::fsync(fd.get()) != 0)
        return fail(why("fsync", tmp_path));
    if (::close(fd.release()) != 0)
        return fail(why("close", tmp_path));

    std::filesystem::rename(tmp_path, path, ec);
    if (ec)
        return fail(std::format("rename {} -> {}: {}", tmp_path.string(), path.string(),
                                ec.message()));

    // The rename is only durable once the directory entry is on disk. Skipping this is the
    // classic way to lose a file that was reported as saved.
    Fd dfd{::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (dfd.valid())
        (void)::fsync(dfd.get());

    return {};
}

std::expected<std::string, std::string> read_file(const std::filesystem::path& path) {
    Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!fd.valid())
        return std::unexpected(why("open", path));

    std::string out;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(why("read", path));
        }
        if (n == 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

} // namespace lc

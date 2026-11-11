// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <expected>
#include <string>
#include <utility>

namespace lipc {

/// An owned file descriptor.
///
/// Every descriptor in this library is owned by one of these, because the thing that goes wrong
/// with descriptor passing is not the passing -- it is the leaking. A render process that leaks
/// one descriptor per frame runs for four minutes. Receiving is the dangerous direction: the
/// kernel installs the descriptors into the process *before* the receiving code has looked at the
/// message, so a message that is rejected, truncated or malformed has already cost descriptors
/// that something must close.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~Fd() { close(); }

    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    [[nodiscard]] int get() const { return fd_; }
    /// Hand ownership to the caller.
    [[nodiscard]] int release() { return std::exchange(fd_, -1); }
    void close();

    /// A second descriptor for the same open file, close-on-exec. Used when a descriptor has to
    /// be sent while the sender keeps using it.
    [[nodiscard]] std::expected<Fd, std::string> duplicate() const;

    friend bool operator==(const Fd& a, const Fd& b) { return a.fd_ == b.fd_; }

private:
    int fd_ = -1;
};

/// Anonymous memory that can be passed to another process, sealed against resizing.
///
/// `name` appears in /proc/self/fd and in `lsof`, so it is worth being honest in: a frame ring
/// that shows up as "memfd:frames-program" is one that can be identified while a show is running.
[[nodiscard]] std::expected<Fd, std::string> make_memfd(const char* name, std::size_t bytes);

} // namespace lipc

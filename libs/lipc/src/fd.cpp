// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lipc/fd.hpp"

#include <cerrno>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace lipc {
namespace {

std::string why(const char* what) {
    return std::format("{}: {}", what, std::strerror(errno));
}

} // namespace

void Fd::close() {
    if (fd_ < 0)
        return;
    // Retrying close() on EINTR is wrong on Linux: the descriptor is already gone, and a retry
    // would close whatever number has since been handed out. Close once and drop it.
    ::close(fd_);
    fd_ = -1;
}

std::expected<Fd, std::string> Fd::duplicate() const {
    if (!valid())
        return std::unexpected(std::string{"cannot duplicate a closed descriptor"});
    const int copy = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
    if (copy < 0)
        return std::unexpected(why("fcntl(F_DUPFD_CLOEXEC)"));
    return Fd{copy};
}

std::expected<Fd, std::string> make_memfd(const char* name, std::size_t bytes) {
    const int raw = ::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (raw < 0)
        return std::unexpected(why("memfd_create"));
    Fd fd{raw};

    if (::ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0)
        return std::unexpected(why("ftruncate"));

    // Seal the size. A receiver maps this and then indexes into it; if the sender could shrink
    // it afterwards, every one of those accesses would become a SIGBUS the receiver cannot
    // defend against by checking anything. F_SEAL_SHRINK and F_SEAL_GROW together mean the
    // mapping a receiver makes stays valid for as long as it holds the descriptor.
    //
    // Deliberately *not* F_SEAL_WRITE: this is shared memory meant to be written.
    if (::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0)
        return std::unexpected(why("fcntl(F_ADD_SEALS)"));

    return fd;
}

} // namespace lipc

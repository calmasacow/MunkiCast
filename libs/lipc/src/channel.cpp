// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lipc/channel.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>

#include <poll.h>
#include <sys/socket.h>

namespace lipc {
namespace {

std::string why(const char* what) {
    return std::format("{}: {}", what, std::strerror(errno));
}

/// Space for the control message carrying the descriptors. A fixed size, large enough for
/// `kMaxFds`: sizing it from anything the peer said would let the peer decide how much this
/// process allocates.
constexpr std::size_t kControlBytes = CMSG_SPACE(sizeof(int) * Channel::kMaxFds);

/// Close descriptors the kernel has already installed.
///
/// Called on every path that abandons a received message. By the time `recvmsg` returns, the
/// descriptors exist in this process whether or not the message is wanted -- so "reject the
/// message" has to mean "and close what arrived with it", or a peer sending malformed messages
/// in a loop exhausts the descriptor table.
void close_installed(msghdr& msg) {
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        const std::size_t payload = c->cmsg_len - CMSG_LEN(0);
        const std::size_t count = payload / sizeof(int);
        std::array<int, Channel::kMaxFds> fds{};
        const std::size_t take = std::min(count, fds.size());
        std::memcpy(fds.data(), CMSG_DATA(c), take * sizeof(int));
        for (std::size_t i = 0; i < take; ++i)
            Fd{fds[i]}.close();
    }
}

} // namespace

std::expected<std::pair<Channel, Channel>, std::string> Channel::pair() {
    int fds[2] = {-1, -1};
    // SOCK_NONBLOCK here rather than a later fcntl: a channel that is briefly blocking is a
    // channel that can briefly hang the render loop.
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) != 0)
        return std::unexpected(why("socketpair"));
    return std::pair<Channel, Channel>{Channel{Fd{fds[0]}}, Channel{Fd{fds[1]}}};
}

std::expected<void, std::string> Channel::send(std::span<const std::byte> payload,
                                               std::span<const int> fds) {
    if (!socket_.valid())
        return std::unexpected(std::string{"the channel is closed"});
    // A zero-length message is refused, and this is not arbitrary. On Linux an AF_UNIX
    // SOCK_SEQPACKET socket delivers a genuine empty packet as `recvmsg` returning 0 with
    // msg_flags 0 -- which is byte-for-byte what it reports when the peer has closed. Measured,
    // not assumed: MSG_EOR is not set in either case. So the two cannot be told apart, and the
    // choice is between a transport that sometimes reads a keepalive as the renderer dying and
    // one where every message carries at least one byte. Any real protocol has a message-kind
    // byte anyway, so this costs nothing and removes the ambiguity entirely.
    if (payload.empty())
        return std::unexpected(std::string{
            "an empty message cannot be sent: it is indistinguishable from the peer closing"});
    if (payload.size() > kMaxPayload)
        return std::unexpected(std::format(
            "a {}-byte message is larger than the {}-byte limit", payload.size(), kMaxPayload));
    if (fds.size() > kMaxFds)
        return std::unexpected(
            std::format("{} descriptors is more than the {} a message may carry", fds.size(),
                        kMaxFds));
    for (const int fd : fds) {
        if (fd < 0)
            return std::unexpected(std::string{"a closed descriptor cannot be sent"});
    }

    iovec iov{};
    iov.iov_base = const_cast<void*>(static_cast<const void*>(payload.data()));
    iov.iov_len = payload.size();

    alignas(cmsghdr) std::array<unsigned char, kControlBytes> control{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (!fds.empty()) {
        msg.msg_control = control.data();
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
    }

    ssize_t sent = 0;
    do {
        sent = ::sendmsg(socket_.get(), &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    if (sent < 0)
        return std::unexpected(why("sendmsg"));
    // On a sequenced-packet socket a short send cannot happen: the message is delivered whole or
    // not at all. Checking anyway, because silently losing the tail of a message is the failure
    // this socket type was chosen to make impossible, and an assertion in the right place is
    // cheaper than finding out during a show.
    if (static_cast<std::size_t>(sent) != payload.size())
        return std::unexpected(std::format("sendmsg delivered {} of {} bytes", sent,
                                           payload.size()));
    return {};
}

std::expected<Message, Empty> Channel::receive() {
    if (!socket_.valid())
        return std::unexpected(Empty::PeerGone);

    Message out;
    out.payload.resize(kMaxPayload);

    iovec iov{};
    iov.iov_base = out.payload.data();
    iov.iov_len = out.payload.size();

    alignas(cmsghdr) std::array<unsigned char, kControlBytes> control{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();

    ssize_t got = 0;
    do {
        // MSG_CMSG_CLOEXEC is the important flag here. Without it a received descriptor survives
        // an exec, so spawning a worker would hand it every frame buffer this process had been
        // sent -- descriptors nobody asked for and nobody closes.
        got = ::recvmsg(socket_.get(), &msg, MSG_CMSG_CLOEXEC);
    } while (got < 0 && errno == EINTR);

    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return std::unexpected(Empty::WouldBlock);
        // Anything else on a socketpair means the peer is gone or the socket is unusable;
        // either way nothing more will arrive on it.
        return std::unexpected(Empty::PeerGone);
    }
    // Zero bytes is the peer closing. It is also what a genuine empty packet looks like, with
    // identical msg_flags -- which is why `send()` refuses to produce one. See the note there.
    if (got == 0) {
        close_installed(msg);
        return std::unexpected(Empty::PeerGone);
    }

    // A truncated message is not a message. Both of these mean the peer sent more than this
    // build is prepared to receive, and in both cases the descriptors have already been
    // installed, so they are closed before the message is dropped.
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        close_installed(msg);
        return std::unexpected(Empty::PeerGone);
    }

    out.payload.resize(static_cast<std::size_t>(got));
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        std::array<int, kMaxFds> fds{};
        const std::size_t take = std::min(count, fds.size());
        std::memcpy(fds.data(), CMSG_DATA(c), take * sizeof(int));
        for (std::size_t i = 0; i < take; ++i)
            out.fds.emplace_back(fds[i]);
    }
    return out;
}

std::expected<bool, std::string> Channel::wait(int timeout_ms) const {
    if (!socket_.valid())
        return std::unexpected(std::string{"the channel is closed"});
    pollfd p{};
    p.fd = socket_.get();
    p.events = POLLIN;
    int n = 0;
    do {
        n = ::poll(&p, 1, timeout_ms);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return std::unexpected(why("poll"));
    // POLLHUP counts as readable: the peer closing is news, and `receive()` is what turns it
    // into `PeerGone`. A caller that only woke on POLLIN would never learn the renderer died.
    return n > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

} // namespace lipc

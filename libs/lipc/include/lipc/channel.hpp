// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lipc/fd.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lipc {

/// A message and the descriptors that travelled with it.
struct Message {
    std::vector<std::byte> payload;
    std::vector<Fd> fds;
};

/// Why a receive produced nothing.
enum class Empty : std::uint8_t {
    /// Nothing is waiting. The peer is still there; poll and come back.
    WouldBlock,
    /// The peer closed its end. Nothing more will ever arrive.
    PeerGone,
};

/// One end of a connected pair of sockets, carrying whole messages and file descriptors.
///
/// `SOCK_SEQPACKET`, not `SOCK_STREAM`. A stream socket would make every reader responsible for
/// finding message boundaries in a byte soup, and a framing bug there is the kind that only
/// appears under load -- which, for this program, means only during a show. Sequenced packets
/// make the boundary the kernel's problem: one `send` is one `receive`, or an error.
///
/// Not `SOCK_DGRAM` either: a datagram socket cannot tell the difference between a quiet peer
/// and a dead one, and knowing that the renderer has gone is the entire point of ADR-0025's
/// safe state.
///
/// This is deliberately a transport and nothing more. It carries bytes and descriptors; what the
/// bytes mean belongs to whoever is talking, so that the protocol can change without this file
/// changing with it.
class Channel {
public:
    Channel() = default;
    explicit Channel(Fd socket) : socket_(std::move(socket)) {}

    /// Two connected ends. Both are close-on-exec: a child that needs one is given it
    /// explicitly, rather than inheriting every descriptor that happened to be open.
    [[nodiscard]] static std::expected<std::pair<Channel, Channel>, std::string> pair();

    [[nodiscard]] bool valid() const { return socket_.valid(); }
    /// For poll/epoll. The channel keeps ownership.
    [[nodiscard]] int poll_fd() const { return socket_.get(); }

    /// Send one message, with up to `kMaxFds` descriptors alongside it.
    ///
    /// The descriptors are *copied* into the message by the kernel; this does not take ownership
    /// of them, so a caller sending something it still uses does not have to duplicate it first.
    ///
    /// A payload larger than `kMaxPayload` is refused here rather than truncated in transit,
    /// which is what a sequenced-packet socket does to an oversized message. An *empty* payload
    /// is refused too: the kernel reports a genuine empty packet exactly as it reports the peer
    /// closing, so allowing one would make "the renderer is quiet" and "the renderer is dead"
    /// the same answer. Every message carries at least one byte, which a message-kind tag
    /// supplies for free.
    [[nodiscard]] std::expected<void, std::string> send(std::span<const std::byte> payload,
                                                        std::span<const int> fds = {});

    /// Receive one message, or say why there is none. Never blocks.
    [[nodiscard]] std::expected<Message, Empty> receive();

    /// Wait until a message is readable or `timeout_ms` passes. A negative timeout waits
    /// forever. Returns true if `receive()` is now worth calling.
    [[nodiscard]] std::expected<bool, std::string> wait(int timeout_ms) const;

    /// Close this end. The peer's next receive reports `PeerGone`.
    void close() { socket_.close(); }

    /// Hand the descriptor to a child process. The channel is empty afterwards.
    [[nodiscard]] Fd take_socket() { return std::move(socket_); }

    /// As many descriptors as one message may carry. Chosen to be far more than the protocol
    /// needs -- a frame ring and its fences -- while keeping the control buffer a fixed size, so
    /// a receive never has to allocate based on what the peer claimed.
    static constexpr std::size_t kMaxFds = 16;
    /// The largest payload one message may carry. Control messages are tens of bytes; this is
    /// generous for a document command and still well under the default socket buffer, so a
    /// send that fits here does not depend on the peer having drained.
    static constexpr std::size_t kMaxPayload = 64 * 1024;

private:
    Fd socket_;
};

} // namespace lipc

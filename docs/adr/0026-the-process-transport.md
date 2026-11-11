<!--
SPDX-FileCopyrightText: MunkiCast contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0026 — The transport between our processes carries bytes and descriptors, and nothing else

- **Status:** accepted, implemented
- **Date:** 2026-10-13
- **Depends on:** ADR-0005 (multi-process isolation), ADR-0025 (the safe state)

## Context

ADR-0005 splits this program into a session that owns the document, a render process that owns
the GPU and the encoders, and pooled workers for anything that can fault. None of those exist
yet. The first of them cannot be written until there is something for them to talk over.

The thing they have to pass is not only messages. A render process hands a composited frame to an
encoder as a DMA-BUF, a worker hands back a decoded buffer, and a frame ring is a `memfd` that
two processes map. All of those are file descriptors, so the transport has to carry descriptors
as a first-class thing rather than as an afterthought bolted onto a message bus.

## Decision

**`libs/lipc` is a transport and nothing more: whole messages, file descriptors, and no opinion
about what either means.**

The protocol lives with the processes that speak it. A transport that knows the message types has
to change every time the protocol does, and the protocol is the part most likely to change in the
next year.

### `SOCK_SEQPACKET`, not `SOCK_STREAM` and not `SOCK_DGRAM`

- **Not a stream.** A stream socket makes every reader responsible for finding message
  boundaries in a byte soup. A framing bug there appears under load, and for this program load
  means a show. Sequenced packets make the boundary the kernel's problem: one send is one
  receive, or an error.
- **Not a datagram.** A datagram socket cannot distinguish a quiet peer from a dead one. Knowing
  that the renderer has died is the whole mechanism of ADR-0025's safe state, so a transport that
  cannot report it is disqualified.

### A message may not be empty, which was measured rather than assumed

The first version of this allowed a zero-length message, for a bare signal whose whole content is
the descriptor it carries. The test written for it failed, and the reason is worth recording.

On Linux, an AF_UNIX `SOCK_SEQPACKET` socket delivers a genuine empty packet as `recvmsg`
returning `0` with `msg_flags == 0`. That is **byte-for-byte identical** to what it reports when
the peer has closed. `MSG_EOR` is set in neither case:

```
sendmsg(0 bytes) = 0
  empty packet: got=0 flags=0x0 EOR=0
  peer closed : got=0 flags=0x0 EOR=0
```

So the two are not distinguishable at the socket layer, and the choice is between a transport that
sometimes reads a keepalive as the renderer having died, and one where every message carries at
least one byte. Any protocol has a message-kind tag anyway, so the second costs nothing.

`send()` refuses an empty payload and says why. `receive()` treats zero bytes as the peer closing,
without ambiguity.

### Receiving a descriptor is the dangerous direction

A render process that passes one descriptor per frame and leaks one per frame runs for about four
minutes. The leak does not come from the passing; it comes from the paths that *reject* a message:

- By the time `recvmsg` returns, the kernel has already installed the descriptors into this
  process. A message that is truncated, malformed or unwanted has **already cost descriptors**.
  Every such path closes what arrived before dropping the message.
- `MSG_CMSG_CLOEXEC` on every receive. Without it, a received descriptor survives `exec`, so
  spawning a worker would hand it every buffer this process had ever been sent.
- The control buffer is a fixed size holding at most `kMaxFds`. Sizing it from anything the peer
  claimed would let the peer decide how much this process allocates.
- `MSG_TRUNC` or `MSG_CTRUNC` means the peer sent more than this build is prepared to receive.
  The message is dropped rather than half-read, and its descriptors closed.

Two tests count the entries in `/proc/self/fd` across two hundred round trips and across twenty
channels destroyed with messages still in flight, because an fd leak is invisible to the leak
sanitizer and invisible in testing right up until the table fills.

### Other things that are decisions rather than details

- **Non-blocking from creation**, via `SOCK_NONBLOCK` on the `socketpair`, not a later `fcntl`. A
  channel that is briefly blocking is a channel that can briefly hang the render loop.
- **`MSG_NOSIGNAL` on every send.** Without it, writing to a dead peer raises `SIGPIPE` and takes
  the process down — so a renderer that died would take the UI with it on the next write.
- **`wait()` wakes on `POLLHUP`, not only `POLLIN`.** A supervisor that woke only for messages
  would sit in `wait()` forever while the show ran on a dead process.
- **A `memfd` is sealed with `F_SEAL_SHRINK | F_SEAL_GROW`** when created here. A receiver maps it
  and then indexes into it; if the sender could shrink it afterwards, those accesses would become
  a `SIGBUS` that the receiver cannot prevent by validating anything it was sent. Deliberately not
  `F_SEAL_WRITE` — it is shared memory meant to be written.
- **Every descriptor is owned by an RAII `Fd`.** `send()` does not take ownership, so a sender
  passing something it still uses need not duplicate it first.

## Consequences

- The frame ring, the protocol and the process supervisor are all still to be designed, and none
  of them require this file to change. That is the point of keeping it this thin.
- `kMaxPayload` is 64 KiB and `kMaxFds` is 16. Both are generous for a control message and well
  inside the default socket buffer, so a send that fits does not depend on the peer having
  drained. A protocol that wants to move more than 64 KiB should be moving it through shared
  memory and sending the descriptor, which is what this is for.
- This is tested without a renderer, a GPU or a display: fifteen cases, all of them headless, and
  included in the TSan preset.
- The honest limitation: nothing here is a security boundary. A peer on the other end of the
  socket is trusted to the extent that it is one of our own processes, and the validation here is
  against bugs and crashes rather than against an attacker. Sandboxing the workers is a separate
  decision that has not been made.

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ltest.hpp"

#include "lipc/channel.hpp"
#include "lipc/fd.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

using namespace lipc;

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string text_of(const std::vector<std::byte>& b) {
    return std::string{reinterpret_cast<const char*>(b.data()), b.size()};
}

/// How many descriptors this process holds. The only reliable way to prove that a path which
/// receives descriptors and then throws the message away does not leak them.
std::size_t open_fds() {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        (void)e;
        ++n;
    }
    return n;
}

} // namespace

LTEST_CASE(a_pair_carries_whole_messages_in_order) {
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    REQUIRE(a.send(bytes_of("first")).has_value());
    REQUIRE(a.send(bytes_of("second")).has_value());
    REQUIRE(a.send(bytes_of("third")).has_value());

    // Each send is one receive. This is the property SOCK_SEQPACKET was chosen for: a stream
    // socket would deliver "firstsecondthird" and leave the boundaries to the reader.
    for (const char* expected : {"first", "second", "third"}) {
        const auto got = b.receive();
        REQUIRE_MSG(got.has_value(), "nothing arrived where '{}' was expected", expected);
        CHECK_EQ(text_of(got->payload), std::string{expected});
        CHECK(got->fds.empty());
    }
    CHECK(!b.receive().has_value());
}

LTEST_CASE(nothing_waiting_and_a_dead_peer_are_different_answers) {
    // The distinction ADR-0025 rests on: a renderer that is quiet is not a renderer that has
    // died, and only one of the two means cut to the safe state.
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    auto quiet = b.receive();
    REQUIRE(!quiet.has_value());
    CHECK_EQ(quiet.error() == Empty::WouldBlock, true);

    a.close();
    auto gone = b.receive();
    REQUIRE(!gone.has_value());
    CHECK_MSG(gone.error() == Empty::PeerGone, "a closed peer was reported as merely quiet");

    // And it keeps saying so rather than reverting to WouldBlock on the next call.
    auto again = b.receive();
    REQUIRE(!again.has_value());
    CHECK(again.error() == Empty::PeerGone);
}

LTEST_CASE(a_message_already_sent_survives_the_sender_closing) {
    // A renderer that sends its last frame and exits must not lose the frame. The kernel holds
    // queued messages until they are read, and PeerGone is only reported once the queue is dry.
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    REQUIRE(a.send(bytes_of("last words")).has_value());
    a.close();

    const auto got = b.receive();
    REQUIRE_MSG(got.has_value(), "a queued message was lost when the sender closed");
    CHECK_EQ(text_of(got->payload), std::string{"last words"});
    const auto after = b.receive();
    REQUIRE(!after.has_value());
    CHECK(after.error() == Empty::PeerGone);
}

LTEST_CASE(an_empty_message_is_refused_because_it_would_look_like_a_dead_peer) {
    // This case started out asserting the opposite -- that a bare signal with no payload would
    // arrive as an empty message -- and failed. Measured on this kernel: a genuine empty packet
    // on an AF_UNIX SOCK_SEQPACKET socket arrives as recvmsg returning 0 with msg_flags 0,
    // which is exactly what the peer closing produces. MSG_EOR is set in neither case, so there
    // is nothing to tell them apart by.
    //
    // So an empty message is refused at the sending end instead. The alternative would be a
    // transport that sometimes reads a keepalive as the renderer having died, which is the one
    // mistake ADR-0025's safe state must not be built on.
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    const auto refused = a.send(std::span<const std::byte>{});
    REQUIRE_MSG(!refused.has_value(), "an empty message was accepted");
    CHECK(refused.error().contains("indistinguishable"));

    const auto nothing = b.receive();
    REQUIRE(!nothing.has_value());
    CHECK_MSG(nothing.error() == Empty::WouldBlock,
              "a refused send put something on the wire anyway");

    // One byte is enough, and a message kind is one byte.
    REQUIRE(a.send(bytes_of("\x01")).has_value());
    const auto got = b.receive();
    REQUIRE(got.has_value());
    CHECK_EQ(got->payload.size(), std::size_t{1});
}

LTEST_CASE(a_descriptor_arrives_pointing_at_the_same_memory) {
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    auto mem = make_memfd("munkicast-test-ring", 4096);
    REQUIRE_MSG(mem.has_value(), "{}", mem ? std::string{} : mem.error());

    void* mine = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem->get(), 0);
    REQUIRE(mine != MAP_FAILED);
    std::memcpy(mine, "written by the sender", 22);

    const int raw = mem->get();
    REQUIRE(a.send(bytes_of("here is the ring"), std::span<const int>{&raw, 1}).has_value());

    const auto got = b.receive();
    REQUIRE(got.has_value());
    CHECK_EQ(text_of(got->payload), std::string{"here is the ring"});
    REQUIRE(got->fds.size() == 1);
    // A different number in this process, the same open file underneath.
    CHECK(got->fds[0].get() != raw);

    void* theirs = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                          got->fds[0].get(), 0);
    REQUIRE(theirs != MAP_FAILED);
    CHECK_EQ(std::string{static_cast<const char*>(theirs)}, std::string{"written by the sender"});

    // And it is shared, not copied: a write through one mapping is visible through the other.
    std::memcpy(theirs, "written by the receiver", 24);
    CHECK_EQ(std::string{static_cast<const char*>(mine)}, std::string{"written by the receiver"});

    ::munmap(mine, 4096);
    ::munmap(theirs, 4096);
}

LTEST_CASE(a_sealed_memfd_cannot_be_shrunk_under_a_receiver) {
    // Why the seal is there: a receiver maps the whole thing and then indexes into it. If the
    // sender could shrink it afterwards, those accesses would become SIGBUS -- a crash the
    // receiver cannot prevent by validating anything it was sent.
    auto mem = make_memfd("munkicast-test-sealed", 8192);
    REQUIRE(mem.has_value());
    CHECK_MSG(::ftruncate(mem->get(), 1024) != 0, "a sealed memfd was shrunk");
    CHECK_MSG(::ftruncate(mem->get(), 16384) != 0, "a sealed memfd was grown");
    // Still writable, which is the whole point of it.
    void* p = ::mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mem->get(), 0);
    REQUIRE(p != MAP_FAILED);
    std::memset(p, 0x5a, 8192);
    ::munmap(p, 8192);
}

LTEST_CASE(several_descriptors_travel_together_and_none_are_lost) {
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    std::vector<Fd> mine;
    std::vector<int> raw;
    for (std::size_t i = 0; i < Channel::kMaxFds; ++i) {
        auto m = make_memfd("munkicast-test-many", 64);
        REQUIRE(m.has_value());
        // A different byte in each, so a mix-up is visible rather than merely possible.
        void* p = ::mmap(nullptr, 64, PROT_READ | PROT_WRITE, MAP_SHARED, m->get(), 0);
        REQUIRE(p != MAP_FAILED);
        std::memset(p, static_cast<int>('A' + i), 1);
        ::munmap(p, 64);
        raw.push_back(m->get());
        mine.push_back(std::move(*m));
    }

    REQUIRE(a.send(bytes_of("all of them"), raw).has_value());
    const auto got = b.receive();
    REQUIRE(got.has_value());
    REQUIRE_MSG(got->fds.size() == Channel::kMaxFds, "{} of {} descriptors arrived",
                got->fds.size(), Channel::kMaxFds);
    for (std::size_t i = 0; i < got->fds.size(); ++i) {
        void* p = ::mmap(nullptr, 64, PROT_READ, MAP_SHARED, got->fds[i].get(), 0);
        REQUIRE(p != MAP_FAILED);
        CHECK_MSG(*static_cast<const char*>(p) == static_cast<char>('A' + i),
                  "descriptor {} arrived out of order", i);
        ::munmap(p, 64);
    }
}

LTEST_CASE(sending_more_than_a_message_may_carry_is_refused_not_truncated) {
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    const std::vector<std::byte> huge(Channel::kMaxPayload + 1, std::byte{0x7f});
    CHECK(!a.send(huge).has_value());
    // Nothing was sent, so nothing arrives: a refused send must not leave half a message in the
    // queue for the peer to misread.
    const auto nothing = b.receive();
    REQUIRE(!nothing.has_value());
    CHECK(nothing.error() == Empty::WouldBlock);

    // The largest legal message does go through whole.
    const std::vector<std::byte> big(Channel::kMaxPayload, std::byte{0x41});
    REQUIRE(a.send(big).has_value());
    const auto got = b.receive();
    REQUIRE(got.has_value());
    CHECK_EQ(got->payload.size(), Channel::kMaxPayload);
    CHECK(got->payload.front() == std::byte{0x41} && got->payload.back() == std::byte{0x41});

    std::vector<int> too_many(Channel::kMaxFds + 1, 0);
    CHECK(!a.send(bytes_of("x"), too_many).has_value());
    const int closed = -1;
    CHECK_MSG(!a.send(bytes_of("x"), std::span<const int>{&closed, 1}).has_value(),
              "a closed descriptor was accepted for sending");
}

LTEST_CASE(receiving_does_not_leak_a_descriptor_however_it_ends) {
    // The failure this library exists to prevent. A render process passing a buffer per frame
    // leaks a descriptor per frame if any path forgets one, and runs for about four minutes.
    const std::size_t before = open_fds();
    {
        auto made = Channel::pair();
        REQUIRE(made.has_value());
        auto& [a, b] = *made;

        for (int i = 0; i < 200; ++i) {
            auto m = make_memfd("munkicast-test-churn", 64);
            REQUIRE(m.has_value());
            const int raw = m->get();
            REQUIRE(a.send(bytes_of("frame"), std::span<const int>{&raw, 1}).has_value());
            const auto got = b.receive();
            REQUIRE(got.has_value());
            REQUIRE(got->fds.size() == 1);
            // `got` goes out of scope here, which must close what arrived with it.
        }
    }
    const std::size_t after = open_fds();
    CHECK_MSG(after <= before, "{} descriptors leaked over 200 round trips", after - before);
}

LTEST_CASE(a_message_dropped_without_being_read_still_closes_its_descriptors) {
    // Messages left in the queue when a channel is destroyed. The kernel closes those, and this
    // asserts it rather than assuming it -- a queue holding fifty frame buffers that leaked
    // would be invisible until the table filled.
    const std::size_t before = open_fds();
    for (int round = 0; round < 20; ++round) {
        auto made = Channel::pair();
        REQUIRE(made.has_value());
        auto& [a, b] = *made;
        for (int i = 0; i < 5; ++i) {
            auto m = make_memfd("munkicast-test-dropped", 64);
            REQUIRE(m.has_value());
            const int raw = m->get();
            REQUIRE(a.send(bytes_of("never read"), std::span<const int>{&raw, 1}).has_value());
        }
        // Both ends close with five messages, each holding a descriptor, still in flight.
        (void)b;
    }
    const std::size_t after = open_fds();
    CHECK_MSG(after <= before, "{} descriptors leaked from undelivered messages", after - before);
}

LTEST_CASE(a_closed_channel_refuses_to_send_and_reports_a_gone_peer) {
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;
    a.close();
    CHECK(!a.valid());
    CHECK(!a.send(bytes_of("into the void")).has_value());
    const auto got = a.receive();
    REQUIRE(!got.has_value());
    CHECK(got.error() == Empty::PeerGone);
    CHECK(!a.wait(0).has_value());
    (void)b;
}

LTEST_CASE(sending_to_a_closed_peer_fails_rather_than_raising_a_signal) {
    // Without MSG_NOSIGNAL this raises SIGPIPE and takes the process down. A renderer that dies
    // must not take the UI with it on the next write to it.
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;
    b.close();
    const auto sent = a.send(bytes_of("anyone there?"));
    CHECK_MSG(!sent.has_value(), "a send to a closed peer appeared to succeed");
}

LTEST_CASE(wait_wakes_for_a_message_and_for_the_peer_closing) {
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    const auto idle = b.wait(0);
    REQUIRE(idle.has_value());
    CHECK_MSG(!*idle, "wait() claimed a message was ready on an idle channel");

    REQUIRE(a.send(bytes_of("wake up")).has_value());
    const auto ready = b.wait(0);
    REQUIRE(ready.has_value());
    CHECK(*ready);
    (void)b.receive();

    // A closed peer has to wake a waiter too, or nothing ever learns the renderer died: a
    // supervisor blocked in wait() would sit there forever while the show ran on a dead
    // process.
    a.close();
    const auto hung_up = b.wait(50);
    REQUIRE(hung_up.has_value());
    CHECK_MSG(*hung_up, "wait() slept through the peer closing");
}

LTEST_CASE(a_channel_survives_being_used_from_two_threads_in_turn) {
    // Not concurrent access -- one owner at a time -- but a handoff, which is what a renderer
    // and its supervisor do. Here so that the TSan preset has something in this library to say
    // about it.
    auto made = Channel::pair();
    REQUIRE(made.has_value());
    auto& [a, b] = *made;

    std::thread writer([&a] {
        for (int i = 0; i < 100; ++i)
            (void)a.send(bytes_of("tick"));
    });
    writer.join();

    int seen = 0;
    while (true) {
        const auto got = b.receive();
        if (!got)
            break;
        ++seen;
    }
    CHECK_EQ(seen, 100);
}

LTEST_CASE(a_moved_descriptor_leaves_nothing_behind) {
    const std::size_t before = open_fds();
    {
        auto m = make_memfd("munkicast-test-move", 64);
        REQUIRE(m.has_value());
        const int raw = m->get();
        Fd moved = std::move(*m);
        CHECK(!m->valid());
        CHECK_EQ(moved.get(), raw);

        auto copy = moved.duplicate();
        REQUIRE(copy.has_value());
        CHECK(copy->get() != raw);

        const int released = copy->release();
        CHECK(!copy->valid());
        Fd{released}.close();
    }
    CHECK_EQ(open_fds(), before);
}

LTEST_MAIN("ipc")

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcore/ulid.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>

namespace lc {
namespace {

/// Crockford base32: the digits plus the alphabet without I, L, O and U -- the four that get
/// misread as 1, 1, 0 and V when a human copies an id out of a log line.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// Decoding table for all 128 ASCII values; -1 for anything that is not a symbol. Crockford's
/// aliases are honoured: i/I/l/L read as 1, o/O as 0, and case is ignored.
constexpr std::array<signed char, 128> make_decode_table() {
    std::array<signed char, 128> t{};
    for (auto& v : t)
        v = -1;
    for (signed char i = 0; i < 32; ++i) {
        const char c = kAlphabet[i];
        t[static_cast<std::size_t>(c)] = i;
        if (c >= 'A' && c <= 'Z')
            t[static_cast<std::size_t>(c - 'A' + 'a')] = i;
    }
    for (const char c : {'i', 'I', 'l', 'L'})
        t[static_cast<std::size_t>(c)] = 1;
    for (const char c : {'o', 'O'})
        t[static_cast<std::size_t>(c)] = 0;
    return t;
}

constexpr std::array<signed char, 128> kDecode = make_decode_table();

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

/// Generation state. A mutex rather than thread_local counters: two threads minting ids in the
/// same millisecond must not be able to produce the same one, and per-thread state only avoids
/// that by relying on the randomness never colliding, which is a weaker promise than it looks
/// when the random field is also being incremented.
std::mutex g_mutex;
std::uint64_t g_last_ms = 0;
std::array<std::uint8_t, 10> g_last_random{};

void draw_random(std::array<std::uint8_t, 10>& out) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t a = rng();
    std::uint64_t b = rng();
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(a & 0xff);
        a >>= 8;
    }
    out[8] = static_cast<std::uint8_t>(b & 0xff);
    out[9] = static_cast<std::uint8_t>((b >> 8) & 0xff);
}

/// Add one to an 80-bit big-endian counter. Returns false on overflow, which would need 2^80 ids
/// inside one millisecond and is handled only so the code has no silent wrap.
bool increment(std::array<std::uint8_t, 10>& r) {
    for (int i = 9; i >= 0; --i) {
        if (++r[static_cast<std::size_t>(i)] != 0)
            return true;
    }
    return false;
}

} // namespace

Ulid Ulid::generate() {
    std::array<std::uint8_t, 16> out{};
    {
        const std::lock_guard<std::mutex> lock{g_mutex};
        std::uint64_t ms = now_ms();
        if (ms < g_last_ms) {
            // The wall clock went backwards -- NTP, a suspend, a VM restore. Ids must keep
            // sorting forwards, so hold the last timestamp and keep counting within it rather
            // than minting ids that sort before ones already written to disk.
            ms = g_last_ms;
        }
        if (ms == g_last_ms) {
            if (!increment(g_last_random)) {
                ++g_last_ms;
                ms = g_last_ms;
                draw_random(g_last_random);
            }
        } else {
            g_last_ms = ms;
            draw_random(g_last_random);
        }

        for (int i = 0; i < 6; ++i)
            out[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((ms >> (8 * (5 - i))) & 0xff);
        std::copy(g_last_random.begin(), g_last_random.end(), out.begin() + 6);
    }
    return from_bytes(out);
}

Ulid Ulid::from_bytes(const std::array<std::uint8_t, 16>& b) {
    Ulid u;
    u.bytes_ = b;
    return u;
}

bool Ulid::is_nil() const {
    return std::all_of(bytes_.begin(), bytes_.end(), [](std::uint8_t b) { return b == 0; });
}

std::uint64_t Ulid::timestamp_ms() const {
    std::uint64_t ms = 0;
    for (int i = 0; i < 6; ++i)
        ms = (ms << 8) | bytes_[static_cast<std::size_t>(i)];
    return ms;
}

std::string Ulid::to_string() const {
    // 128 bits into 26 base32 characters, most significant first. The leading character carries
    // only the top two bits, so a valid ULID never starts above '7'.
    std::string out(kUlidLength, '0');
    for (std::size_t i = 0; i < kUlidLength; ++i) {
        const std::size_t bit = i * 5;
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 5; ++k) {
            const std::size_t b = bit + k;
            // The first character is padded with two leading zero bits.
            const int shifted = static_cast<int>(b) - 2;
            std::uint32_t v = 0;
            if (shifted >= 0) {
                const auto idx = static_cast<std::size_t>(shifted);
                // Everything unsigned and explicit: a uint8_t promotes to int, and letting the
                // result drift back to unsigned is a sign conversion the CI preset rejects.
                const auto byte = static_cast<std::uint32_t>(bytes_[idx / 8]);
                const auto shift = static_cast<std::uint32_t>(7 - (idx % 8));
                v = (byte >> shift) & 1u;
            }
            value = (value << 1) | v;
        }
        out[i] = kAlphabet[value];
    }
    return out;
}

std::optional<Ulid> Ulid::parse(std::string_view text) {
    if (text.size() != kUlidLength)
        return std::nullopt;

    std::array<std::uint8_t, 16> out{};
    std::size_t bit = 0;
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc >= kDecode.size())
            return std::nullopt;
        const signed char v = kDecode[uc];
        if (v < 0)
            return std::nullopt;
        for (int k = 4; k >= 0; --k) {
            const std::uint32_t b = (static_cast<std::uint32_t>(v) >> k) & 1u;
            const int shifted = static_cast<int>(bit) - 2;
            if (shifted < 0) {
                // The two padding bits at the front must be zero, or the text encodes a value
                // larger than 128 bits and is not a ULID however well-formed it looks.
                if (b != 0)
                    return std::nullopt;
            } else {
                const auto idx = static_cast<std::size_t>(shifted);
                const auto shift = static_cast<std::uint32_t>(7 - (idx % 8));
                out[idx / 8] = static_cast<std::uint8_t>(
                    static_cast<std::uint32_t>(out[idx / 8]) | (b << shift));
            }
            ++bit;
        }
    }
    return from_bytes(out);
}

} // namespace lc

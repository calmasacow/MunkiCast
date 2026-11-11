// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace lc {

/// A ULID: 48 bits of millisecond timestamp followed by 80 bits of randomness, rendered as 26
/// Crockford base32 characters.
///
/// Every node, effect, bus and scene in a project carries one, and every reference is by id.
/// That is what makes rename free, undo precise, Stream Deck bindings stable across edits, and
/// merging two projects possible at all.
///
/// Chosen over a UUID for two properties that matter here. Ids sort by creation time as plain
/// text, so a directory of `scenes/<ulid>.json` lists in the order the scenes were made and a
/// diff of a project file reads chronologically. And they are shorter and case-insensitive,
/// which matters because these ids end up in filenames, log lines and bug reports people retype.
class Ulid {
public:
    /// The all-zero id. Valid to hold, never valid to reference.
    constexpr Ulid() = default;

    /// A fresh id. Monotonic: two ids minted in the same millisecond still sort in the order they
    /// were created, because the random field is incremented rather than redrawn. Without that,
    /// the sort order of things created in the same tick is arbitrary and changes between runs,
    /// which makes golden fixtures and "what changed in this diff" both unreliable.
    [[nodiscard]] static Ulid generate();

    [[nodiscard]] static std::optional<Ulid> parse(std::string_view text);
    [[nodiscard]] static constexpr Ulid nil() { return Ulid{}; }

    /// 26 characters, Crockford base32, uppercase.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool is_nil() const;
    [[nodiscard]] std::uint64_t timestamp_ms() const;

    [[nodiscard]] const std::array<std::uint8_t, 16>& bytes() const { return bytes_; }
    [[nodiscard]] static Ulid from_bytes(const std::array<std::uint8_t, 16>& b);

    friend auto operator<=>(const Ulid&, const Ulid&) = default;
    friend bool operator==(const Ulid&, const Ulid&) = default;

private:
    std::array<std::uint8_t, 16> bytes_{};
};

/// Text length of a rendered ULID.
inline constexpr std::size_t kUlidLength = 26;

} // namespace lc

template <> struct std::hash<lc::Ulid> {
    std::size_t operator()(const lc::Ulid& u) const noexcept {
        const auto& b = u.bytes();
        std::uint64_t hi = 0;
        std::uint64_t lo = 0;
        for (int i = 0; i < 8; ++i) {
            hi = (hi << 8) | b[static_cast<std::size_t>(i)];
            lo = (lo << 8) | b[static_cast<std::size_t>(i) + 8];
        }
        return static_cast<std::size_t>(hi ^ (lo * 0x9e3779b97f4a7c15ULL));
    }
};

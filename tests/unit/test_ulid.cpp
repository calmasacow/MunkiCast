// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcore/ulid.hpp"

#include "ltest.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using lc::Ulid;

LTEST_CASE(a_generated_id_looks_like_a_ulid) {
    const Ulid u = Ulid::generate();
    const std::string s = u.to_string();
    CHECK_EQ(s.size(), lc::kUlidLength);
    CHECK(!u.is_nil());
    // Crockford base32, uppercase, and never the four ambiguous letters.
    for (const char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
        CHECK_MSG(ok, "'{}' is not a base32 symbol", c);
        CHECK_MSG(c != 'I' && c != 'L' && c != 'O' && c != 'U',
                  "'{}' is excluded from Crockford base32", c);
    }
    // The leading character carries only two bits, so it can never exceed '7'.
    CHECK(s[0] <= '7');
}

LTEST_CASE(text_round_trips_exactly) {
    for (int i = 0; i < 2000; ++i) {
        const Ulid u = Ulid::generate();
        const auto back = Ulid::parse(u.to_string());
        REQUIRE(back.has_value());
        CHECK_MSG(*back == u, "{} did not survive a round trip", u.to_string());
        CHECK_EQ(back->to_string(), u.to_string());
    }
}

LTEST_CASE(the_nil_id_is_distinct_and_round_trips) {
    const Ulid nil = Ulid::nil();
    CHECK(nil.is_nil());
    CHECK_EQ(nil.to_string(), std::string(lc::kUlidLength, '0'));
    const auto back = Ulid::parse(nil.to_string());
    REQUIRE(back.has_value());
    CHECK(back->is_nil());
    CHECK(nil != Ulid::generate());
}

LTEST_CASE(crockford_aliases_and_case_are_accepted_on_input) {
    // People retype these out of log lines and bug reports; Crockford's whole point is that the
    // characters that get misread decode to what the reader meant.
    const Ulid u = Ulid::generate();
    std::string s = u.to_string();

    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto from_lower = Ulid::parse(lower);
    REQUIRE(from_lower.has_value());
    CHECK(*from_lower == u);

    // 'I' and 'L' read as 1, 'O' reads as 0 -- wherever those digits appear.
    std::string typo = s;
    bool substituted = false;
    for (char& c : typo) {
        if (c == '1') {
            c = 'I';
            substituted = true;
            break;
        }
    }
    if (substituted) {
        const auto from_typo = Ulid::parse(typo);
        REQUIRE(from_typo.has_value());
        CHECK(*from_typo == u);
    }
}

LTEST_CASE(malformed_text_is_rejected) {
    const std::string good = Ulid::generate().to_string();
    for (const std::string& bad : {
             std::string{},
             good.substr(0, 25),
             good + "0",
             std::string(26, 'U'),                       // excluded letter
             std::string(26, '~'),                       // not a symbol
             "8" + good.substr(1),                       // overflows 128 bits
             "Z" + good.substr(1),                       // likewise, maximally
             good.substr(0, 25) + " ",
         }) {
        CHECK_MSG(!Ulid::parse(bad).has_value(), "accepted '{}'", bad);
    }
}

LTEST_CASE(ids_sort_by_creation_order_as_plain_text) {
    // This is the property that makes scenes/<ulid>.json list chronologically and a project diff
    // read in the order things were made. It has to hold for the *text*, not just the value.
    std::vector<Ulid> ids;
    std::vector<std::string> text;
    for (int i = 0; i < 5000; ++i) {
        ids.push_back(Ulid::generate());
        text.push_back(ids.back().to_string());
    }
    for (std::size_t i = 1; i < ids.size(); ++i) {
        CHECK_MSG(ids[i - 1] < ids[i], "id {} does not sort before {}", i - 1, i);
        CHECK_MSG(text[i - 1] < text[i], "text {} does not sort before {}", i - 1, i);
    }
}

LTEST_CASE(ids_are_unique_including_within_one_millisecond) {
    // The generator is deliberately monotonic rather than redrawing the random field, so a burst
    // inside a single tick is the interesting case, not the easy one.
    std::unordered_set<std::string> seen;
    std::vector<Ulid> ids;
    ids.reserve(50000);
    for (int i = 0; i < 50000; ++i)
        ids.push_back(Ulid::generate());

    int same_ms = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK_MSG(seen.insert(ids[i].to_string()).second, "duplicate id at {}", i);
        if (i > 0 && ids[i].timestamp_ms() == ids[i - 1].timestamp_ms())
            ++same_ms;
    }
    CHECK_MSG(same_ms > 1000, "only {} ids shared a millisecond; the burst case is untested",
              same_ms);
}

LTEST_CASE(the_timestamp_is_readable_and_recent) {
    const auto before = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const Ulid u = Ulid::generate();
    const std::uint64_t ts = u.timestamp_ms();
    CHECK(ts >= before - 1000);
    CHECK(ts <= before + 1000);
    // And it survives the text encoding, which is where an off-by-two bit error would show.
    CHECK_EQ(Ulid::parse(u.to_string())->timestamp_ms(), ts);
}

LTEST_CASE(generation_is_safe_from_several_threads) {
    // Two threads minting inside one millisecond must not be able to produce the same id.
    constexpr int kThreads = 8;
    constexpr int kEach = 20000;
    std::vector<std::vector<Ulid>> per_thread(kThreads);
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&per_thread, t] {
            per_thread[static_cast<std::size_t>(t)].reserve(kEach);
            for (int i = 0; i < kEach; ++i)
                per_thread[static_cast<std::size_t>(t)].push_back(Ulid::generate());
        });
    }
    for (auto& w : workers)
        w.join();

    std::set<Ulid> all;
    for (const auto& v : per_thread)
        for (const Ulid& u : v)
            all.insert(u);
    CHECK_EQ(all.size(), static_cast<std::size_t>(kThreads) * kEach);
}

LTEST_CASE(ids_work_as_keys) {
    std::unordered_set<Ulid> set;
    std::set<Ulid> ordered;
    for (int i = 0; i < 1000; ++i) {
        const Ulid u = Ulid::generate();
        CHECK(set.insert(u).second);
        CHECK(ordered.insert(u).second);
    }
    CHECK_EQ(set.size(), std::size_t{1000});
    CHECK(std::is_sorted(ordered.begin(), ordered.end()));
}

LTEST_MAIN("ulid")

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// A test harness in one header.
///
/// Deliberately not a framework. All this needs to do is register cases, compare values, print a
/// legible failure and return a non-zero exit code for ctest. A dependency would have to be
/// present on every machine the project builds on -- including `argus`, the reference target,
/// which has neither ninja nor passwordless sudo -- in exchange for assertions we can write in
/// sixty lines.

#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ltest {

/// Thrown by REQUIRE to abandon the current case without killing the run.
struct Abort {};

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& case_failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(std::string name, void (*fn)()) {
        registry().push_back(Case{std::move(name), fn});
    }
};

template <typename T> struct is_vector : std::false_type {};
template <typename T, typename A> struct is_vector<std::vector<T, A>> : std::true_type {};

template <typename T> std::string show(const T& v);

template <typename T> std::string show(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return std::format("{}", static_cast<long long>(v));
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::format("{}", v);
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        return std::format("\"{}\"", std::string_view(v));
    } else if constexpr (is_vector<T>::value) {
        std::string s = "[";
        for (std::size_t i = 0; i < v.size(); ++i)
            s += (i ? ", " : "") + show(v[i]);
        return s + "]";
    } else {
        return "<value>";
    }
}

inline void report(const char* file, int line, const std::string& msg) {
    std::fprintf(stderr, "    %s:%d\n      %s\n", file, line, msg.c_str());
    ++case_failures();
}

inline int run_all(const char* suite) {
    int failed = 0;
    for (const Case& c : registry()) {
        case_failures() = 0;
        try {
            c.fn();
        } catch (const Abort&) {
            // REQUIRE already reported it.
        } catch (const std::exception& e) {
            report(__FILE__, __LINE__, std::format("threw std::exception: {}", e.what()));
        } catch (...) {
            report(__FILE__, __LINE__, "threw an unknown exception");
        }
        if (case_failures() == 0) {
            std::fprintf(stderr, "  ok   %s\n", c.name.c_str());
        } else {
            std::fprintf(stderr, "  FAIL %s (%d failure(s))\n", c.name.c_str(), case_failures());
            ++failed;
        }
    }
    std::fprintf(stderr, "\n%s: %zu case(s), %d failed\n", suite, registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

} // namespace ltest

#define LTEST_CASE(NAME)                                                                           \
    static void NAME();                                                                            \
    static const ::ltest::Registrar ltest_registrar_##NAME{#NAME, &NAME};                          \
    static void NAME()

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::ltest::report(__FILE__, __LINE__, std::format("CHECK failed: {}", #expr));           \
    } while (0)

#define CHECK_MSG(expr, ...)                                                                       \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::ltest::report(__FILE__, __LINE__,                                                    \
                            std::format("CHECK failed: {} -- {}", #expr,                           \
                                        std::format(__VA_ARGS__)));                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        /* By value, not by reference: `CHECK_EQ(opt.value(), x)` binds a reference into a      \
           temporary optional that dies at the end of the declaration, which GCC rightly calls   \
           a dangling reference. Test values are small and copyable; correctness is not. */       \
        const auto ltest_a = (a);                                                                  \
        const auto ltest_b = (b);                                                                  \
        if (!(ltest_a == ltest_b))                                                                 \
            ::ltest::report(__FILE__, __LINE__,                                                    \
                            std::format("CHECK_EQ failed: {} == {}\n        left  = {}\n   "       \
                                        "     right = {}",                                         \
                                        #a, #b, ::ltest::show(ltest_a), ::ltest::show(ltest_b)));  \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        const double ltest_a = static_cast<double>(a);                                             \
        const double ltest_b = static_cast<double>(b);                                             \
        if (!(std::abs(ltest_a - ltest_b) <= (tol)))                                               \
            ::ltest::report(__FILE__, __LINE__,                                                    \
                            std::format("CHECK_NEAR failed: {} ~ {} (tol {})\n        left  = "    \
                                        "{}\n        right = {}",                                  \
                                        #a, #b, static_cast<double>(tol), ltest_a, ltest_b));      \
    } while (0)

/// Like CHECK_MSG, but the rest of the case cannot run meaningfully, so abandon it.
#define REQUIRE_MSG(expr, ...)                                                                     \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::ltest::report(__FILE__, __LINE__,                                                    \
                            std::format("REQUIRE failed: {} -- {}", #expr,                         \
                                        std::format(__VA_ARGS__)));                                \
            throw ::ltest::Abort{};                                                                \
        }                                                                                          \
    } while (0)

/// Like CHECK, but the rest of the case cannot run meaningfully, so abandon it.
#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::ltest::report(__FILE__, __LINE__, std::format("REQUIRE failed: {}", #expr));         \
            throw ::ltest::Abort{};                                                                \
        }                                                                                          \
    } while (0)

#define LTEST_MAIN(SUITE)                                                                          \
    int main() { return ::ltest::run_all(SUITE); }

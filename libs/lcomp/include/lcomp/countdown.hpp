// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <charconv>
#include <format>
#include <ldoc/document.hpp>
#include <string_view>
#include <vector>

namespace lcomp {
inline std::int64_t timer_number(const ldoc::ParamMap& settings, const char* key,
                                 std::int64_t fallback = 0) {
    const auto it = settings.find(key);
    const auto* value = it == settings.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
    return value ? *value : fallback;
}

inline bool timer_flag(const ldoc::ParamMap& settings, const char* key, bool fallback = false) {
    const auto it = settings.find(key);
    const auto* value = it == settings.end() ? nullptr : std::get_if<bool>(&it->second);
    return value ? *value : fallback;
}

inline std::string timer_text(const ldoc::ParamMap& settings, const char* key,
                              std::string fallback = {}) {
    const auto it = settings.find(key);
    const auto* value = it == settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    return value ? *value : fallback;
}

/// A year, in milliseconds. Every duration in here is clamped to it, so a settings file with an
/// absurd number in it is a wrong timer rather than an overflow.
inline constexpr std::int64_t kCountdownLimit = 366ll * 86400 * 1000;
/// 2100-01-01, the ceiling on an absolute wall-clock time.
inline constexpr std::int64_t kCountdownEpochLimit = 4102444800000ll;

/// The spin-box ranges the OBS plugin uses, and what a period field is clamped to here.
inline constexpr std::int64_t kCountdownMaxDays = 999;

/// The period, stored the way the plugin stores it: four numbers rather than one duration.
///
/// `countdown.durationMs` is still read when none of the four are present, so a project written
/// before this and an OBS import that only carried a duration both still open. Anything that
/// writes a period writes both, so the two can never disagree.
inline std::int64_t countdown_period_ms(const ldoc::ParamMap& settings) {
    const bool per_unit =
        settings.contains("countdown.periodDays") || settings.contains("countdown.periodHours") ||
        settings.contains("countdown.periodMinutes") ||
        settings.contains("countdown.periodSeconds");
    if (!per_unit)
        return std::clamp(timer_number(settings, "countdown.durationMs", 900000),
                          std::int64_t{0}, kCountdownLimit);
    const auto days = std::clamp(timer_number(settings, "countdown.periodDays"), std::int64_t{0},
                                 kCountdownMaxDays);
    const auto hours =
        std::clamp(timer_number(settings, "countdown.periodHours"), std::int64_t{0}, std::int64_t{23});
    const auto minutes = std::clamp(timer_number(settings, "countdown.periodMinutes"),
                                    std::int64_t{0}, std::int64_t{59});
    const auto seconds = std::clamp(timer_number(settings, "countdown.periodSeconds"),
                                    std::int64_t{0}, std::int64_t{59});
    return std::clamp(((days * 24 + hours) * 60 + minutes) * 60 + seconds, std::int64_t{0},
                      kCountdownLimit / 1000) *
           1000;
}

/// "period" counts a duration; "datetime" counts to a wall-clock time. The plugin's two tabs.
///
/// Older projects stored one `countdown.mode` that conflated this with the count-up flag, which
/// is why both are read through here rather than directly.
inline std::string countdown_type(const ldoc::ParamMap& settings) {
    const auto explicit_type = timer_text(settings, "countdown.type");
    if (explicit_type == "period" || explicit_type == "datetime")
        return explicit_type;
    return timer_text(settings, "countdown.mode", "remaining") == "target" ? "datetime" : "period";
}

inline bool countdown_counts_up(const ldoc::ParamMap& settings) {
    const auto it = settings.find("countdown.countUp");
    if (it != settings.end())
        if (const auto* value = std::get_if<bool>(&it->second))
            return *value;
    return timer_text(settings, "countdown.mode", "remaining") == "elapsed";
}

/// One `%token%` substitution pass over a format string.
///
/// A single left-to-right scan rather than a sequence of replacements, so a value that happens
/// to contain a `%` cannot be re-read as another token. An unknown token is left exactly as it
/// was typed, which is what makes a literal "100% done" survive.
inline std::string countdown_apply_template(std::string_view format, std::string_view time,
                                            std::int64_t millis) {
    const auto total = std::max<std::int64_t>(0, millis) / 1000;
    const std::int64_t parts[4] = {total / 86400, total % 86400 / 3600, total % 3600 / 60,
                                   total % 60};
    static constexpr std::string_view names[4] = {"days", "hours", "minutes", "seconds"};
    std::string out;
    out.reserve(format.size() + time.size());
    for (std::size_t at = 0; at < format.size();) {
        const auto end = format[at] == '%' ? format.find('%', at + 1) : std::string_view::npos;
        if (end == std::string_view::npos) {
            out += format[at++];
            continue;
        }
        const auto token = format.substr(at + 1, end - at - 1);
        bool matched = token == "time";
        if (matched)
            out += time;
        for (std::size_t i = 0; i < 4 && !matched; ++i) {
            if (token == names[i]) {
                out += std::to_string(parts[i]);
                matched = true;
            } else if (token.size() == names[i].size() + 1 && token.back() == '2' &&
                       token.substr(0, names[i].size()) == names[i]) {
                out += std::format("{:02}", parts[i]);
                matched = true;
            }
        }
        if (matched)
            at = end + 1;
        else
            out += format[at++];
    }
    return out;
}

/// One "between these two times, use this colour" rule.
struct CountdownColourRule {
    std::int64_t min_ms = 0, max_ms = 0;
    ldoc::Rgba colour{};
};

/// Colour rules, stored as `min,max,r,g,b,a` per rule with `;` between them.
///
/// A flat string because `ldoc::ParamValue` has no list type, and six integers need no parser
/// worth the name. A malformed rule is skipped rather than failing the whole list: one bad
/// entry in a settings file should cost that entry.
inline std::vector<CountdownColourRule> countdown_colour_rules(const ldoc::ParamMap& settings) {
    std::vector<CountdownColourRule> out;
    const std::string stored = timer_text(settings, "countdown.colourRules");
    std::string_view rest{stored};
    while (!rest.empty() && out.size() < 64) {
        const auto cut = rest.find(';');
        std::string_view rule = rest.substr(0, cut);
        rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
        std::int64_t value[6] = {};
        bool ok = true;
        for (std::size_t i = 0; i < 6 && ok; ++i) {
            const auto comma = rule.find(',');
            const auto field = rule.substr(0, comma);
            const auto parsed =
                std::from_chars(field.data(), field.data() + field.size(), value[i]);
            ok = !field.empty() && parsed.ec == std::errc{} &&
                 parsed.ptr == field.data() + field.size() &&
                 (i == 5 || comma != std::string_view::npos);
            rule = comma == std::string_view::npos ? std::string_view{} : rule.substr(comma + 1);
        }
        if (!ok || !rule.empty())
            continue;
        const auto channel = [](std::int64_t raw) {
            return static_cast<std::uint8_t>(std::clamp(raw, std::int64_t{0}, std::int64_t{255}));
        };
        out.push_back(CountdownColourRule{
            std::clamp(value[0], std::int64_t{0}, kCountdownLimit),
            std::clamp(value[1], std::int64_t{0}, kCountdownLimit),
            ldoc::Rgba::from_srgb8(channel(value[2]), channel(value[3]), channel(value[4]),
                                   channel(value[5]))});
    }
    return out;
}

struct CountdownValue {
    std::int64_t elapsed = 0, remaining = 0;
    /// What is on screen: the remaining time, or the elapsed one when counting up.
    std::int64_t shown = 0;
    bool ended = false;
    std::string text;
    /// Set when text colouring is on and a rule or the main colour applies.
    bool has_colour = false;
    ldoc::Rgba colour{};
};

inline CountdownValue countdown(const ldoc::ParamMap& settings, std::int64_t now) {
    constexpr std::int64_t limit = kCountdownLimit;
    const auto duration = countdown_period_ms(settings);
    const auto offset =
        std::clamp(timer_number(settings, "countdown.elapsedMs"), std::int64_t{0}, limit);
    const auto type = countdown_type(settings);
    const bool counts_up = countdown_counts_up(settings);
    const bool running = timer_flag(settings, "countdown.running");
    const auto start = std::clamp(timer_number(settings, "countdown.startedAtMs", now),
                                  std::int64_t{0}, kCountdownEpochLimit);
    CountdownValue out;
    out.elapsed =
        std::clamp(offset + (running ? std::clamp(now - start, std::int64_t{0}, limit) : 0),
                   std::int64_t{0}, limit);
    out.remaining = std::max<std::int64_t>(0, duration - out.elapsed);
    if (type == "datetime") {
        const auto target = std::clamp(timer_number(settings, "countdown.targetAtMs", now),
                                       std::int64_t{0}, kCountdownEpochLimit);
        out.remaining =
            running ? std::clamp(target - now, std::int64_t{0}, limit)
                    : std::clamp(timer_number(settings, "countdown.targetRemainingMs", duration),
                                 std::int64_t{0}, limit);
    }
    out.ended = counts_up ? duration > 0 && out.elapsed >= duration : out.remaining == 0;
    // Counting up shows how far it has come and stops at the period; counting down shows what is
    // left, rounded up so a timer reading 00:01 still has a second to run rather than none.
    out.shown = counts_up ? std::min(out.elapsed, duration ? duration : limit) : out.remaining;
    const auto seconds = counts_up ? out.shown / 1000 : (out.shown + 999) / 1000;

    const bool days = timer_flag(settings, "countdown.showDays");
    const bool hours = timer_flag(settings, "countdown.showHours");
    const bool minutes = timer_flag(settings, "countdown.showMinutes", true);
    const bool zero = timer_flag(settings, "countdown.leadingZero", true);
    std::string time;
    const auto append = [&](std::int64_t value) {
        const bool first = time.empty();
        if (!first)
            time += ':';
        time += first && !zero ? std::to_string(value) : std::format("{:02}", value);
    };
    if (days)
        append(seconds / 86400);
    if (hours)
        append(seconds / 3600 % 24);
    if (minutes)
        append(seconds / 60 % 60);
    if (timer_flag(settings, "countdown.showSeconds", true))
        append(seconds % 60);
    // Every unit unchecked is a misconfiguration, not an empty caption. The plugin says so on the
    // card rather than publishing a blank text source, and so does this.
    if (time.empty())
        time = "Nothing selected!";

    // A format string only applies when the plugin's "Format Output" box is ticked. Projects
    // written before that flag existed carry a format and no flag, so a stored format still
    // counts as having asked for one.
    const auto format = timer_text(settings, "countdown.format", "%time%");
    out.text = timer_flag(settings, "countdown.useFormattedOutput", !format.empty())
                   ? countdown_apply_template(format, time, out.shown)
                   : time;
    if (out.ended && timer_flag(settings, "countdown.showEndMessage", true))
        out.text = countdown_apply_template(timer_text(settings, "countdown.endMessage", "SOON"),
                                            time, out.shown);

    if (timer_flag(settings, "countdown.useTextColour")) {
        // Whole seconds, so a rule written as "0 to 10 seconds" covers the second that reads 10.
        const auto compare = out.shown / 1000 * 1000;
        for (const auto& rule : countdown_colour_rules(settings)) {
            if (rule.max_ms < rule.min_ms || compare < rule.min_ms || compare > rule.max_ms)
                continue;
            out.colour = rule.colour;
            out.has_colour = true;
            break;
        }
        if (!out.has_colour) {
            const auto it = settings.find("countdown.mainTextColour");
            if (it != settings.end())
                if (const auto* colour = std::get_if<ldoc::Rgba>(&it->second)) {
                    out.colour = *colour;
                    out.has_colour = true;
                }
        }
    }
    return out;
}
} // namespace lcomp

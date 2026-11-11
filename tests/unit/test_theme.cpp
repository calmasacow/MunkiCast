// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Theme tokens and Omarchy theme following.

#include "ltheme/omarchy.hpp"
#include "ltheme/palette.hpp"

#include "lcore/atomic_file.hpp"
#include "ltest.hpp"

#include <algorithm>
#include <filesystem>
#include <unistd.h>

using namespace ltheme;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        path_ = std::filesystem::temp_directory_path() /
                std::format("munkicast-ltheme-{}", ::getpid());
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_ / "current" / "theme", ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] std::filesystem::path theme() const { return path_ / "current" / "theme"; }
    [[nodiscard]] std::filesystem::path root() const { return path_; }

private:
    std::filesystem::path path_;
};

/// The real Omarchy token set, as this machine's tokyo-night theme writes it.
const char* kRealColorsToml = R"(mode = "dark"

accent = "#7aa2f7"
selection = "#292e42"
muted = "#414868"

background = "#1a1b26"
dark_background = "#13141c"
darker_background = "#0e0e14"
lighter_background = "#24283b"

foreground = "#a9b1d6"
dark_foreground = "#565f89"
light_foreground = "#b4bee6"
bright_foreground = "#c0caf5"

red = "#f7768e"
yellow = "#e0af68"
orange = "#eb927b"
green = "#9ece6a"
cyan = "#449dab"
blue = "#7aa2f7"
magenta = "#ad8ee6"
brown = "#75493d"
)";

} // namespace

LTEST_CASE(colours_parse_in_every_form_a_theme_might_use) {
    const Color want{0xaa, 0xbb, 0xcc, 0xff};
    CHECK_EQ(parse_color("#aabbcc").value(), want);
    CHECK_EQ(parse_color("#AABBCC").value(), want);
    CHECK_EQ(parse_color("0xaabbcc").value(), want);
    CHECK_EQ(parse_color("  #aabbcc  ").value(), want);
    CHECK_EQ(parse_color("#abc").value(), (Color{0xaa, 0xbb, 0xcc, 0xff}));
    CHECK_EQ(parse_color("#aabbcc80").value(), (Color{0xaa, 0xbb, 0xcc, 0x80}));
    CHECK_EQ(parse_color("#abcd").value(), (Color{0xaa, 0xbb, 0xcc, 0xdd}));
}

LTEST_CASE(a_malformed_colour_is_nothing_rather_than_black) {
    // The distinction matters: silently turning a typo into black produces a theme that looks
    // deliberately broken, which is much harder to diagnose than a reported missing token.
    for (std::string_view bad : {"", "aabbcc", "#", "#ab", "#abcde", "#aabbccd", "#gggggg",
                                 "rgb(1,2,3)", "#aabbccddee", "red"})
        CHECK_MSG(!parse_color(bad).has_value(), "accepted '{}'", bad);
}

LTEST_CASE(colour_formatting_round_trips) {
    const Color c{0x12, 0x34, 0x56, 0x78};
    CHECK_EQ(c.argb_hex(), std::string("#78123456"));
    CHECK_EQ(c.css_hex(), std::string("#12345678"));
    CHECK_EQ(parse_color(c.css_hex()).value(), c);
}

LTEST_CASE(alpha_and_mixing_behave) {
    const Color c{0x40, 0x80, 0xc0, 0xff};
    CHECK_EQ(c.with_alpha(0.5).a, std::uint8_t{128});
    CHECK_EQ(c.with_alpha(0.0).a, std::uint8_t{0});
    CHECK_EQ(c.with_alpha(1.0).a, std::uint8_t{255});
    CHECK_EQ(c.with_alpha(5.0).a, std::uint8_t{255}); // clamped, not wrapped
    CHECK_EQ(c.with_alpha(-1.0).a, std::uint8_t{0});

    const Color black{0, 0, 0, 255};
    CHECK_EQ(c.mix(black, 0.0), c);
    CHECK_EQ(c.mix(black, 1.0), black);
    CHECK_EQ(c.mix(black, 0.5).r, std::uint8_t{0x20});
}

LTEST_CASE(every_role_has_a_name_and_the_fallback_defines_all_of_them) {
    const Palette p = Palette::fallback();
    for (std::size_t i = 0; i < role_count(); ++i) {
        const Role r = static_cast<Role>(i);
        const std::string_view name = role_name(r);
        CHECK_MSG(name != "unknown", "role {} has no name", i);
        CHECK_EQ(role_from_name(name).value(), r);

        const Color c = p.get(r);
        // Every role must be *something*. A role left at the default-constructed value would be
        // opaque black, which is the one value that is indistinguishable from "forgot to set it".
        const bool is_default_black = (c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255);
        CHECK_MSG(!is_default_black, "role '{}' is still default-constructed", name);
    }
    CHECK(p.dark());
    CHECK_EQ(p.source(), std::string("built-in"));
    CHECK(!role_from_name("not-a-role").has_value());
}

LTEST_CASE(the_drop_overlay_is_translucent_and_the_rest_is_not) {
    // ADR-0016: translucent fill plus a clear border, because the overlay is drawn over live
    // panel content including the program canvas.
    const Palette p = Palette::fallback();
    CHECK(p.get(Role::DropFill).a < 255);
    CHECK(p.get(Role::DropFill).a > 0);
    CHECK_EQ(p.get(Role::DropBorder).a, std::uint8_t{255});
    CHECK_EQ(p.get(Role::PanelBackground).a, std::uint8_t{255});
}

LTEST_CASE(an_empty_token_set_leaves_the_built_in_palette_alone) {
    std::vector<std::string> missing;
    const Palette p = palette_from_tokens({}, true, &missing);
    CHECK(p == Palette::fallback());
    CHECK_MSG(missing.size() >= 10, "only {} tokens reported missing", missing.size());
}

LTEST_CASE(a_sparse_theme_gets_what_it_asked_for_and_falls_back_for_the_rest) {
    std::vector<std::string> missing;
    const Palette p = palette_from_tokens({{"background", Color{0x11, 0x22, 0x33, 0xff}},
                                           {"foreground", Color{0xee, 0xee, 0xee, 0xff}}},
                                          true, &missing);
    CHECK_EQ(p.get(Role::PanelBackground), (Color{0x11, 0x22, 0x33, 0xff}));
    CHECK_EQ(p.get(Role::Text), (Color{0xee, 0xee, 0xee, 0xff}));
    // No accent and no muted colour, so those roles keep the built-in values.
    CHECK_EQ(p.get(Role::Accent), Palette::fallback().get(Role::Accent));
    CHECK_EQ(p.get(Role::Border), Palette::fallback().get(Role::Border));
    CHECK(std::find(missing.begin(), missing.end(), "accent") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "background") == missing.end());
}

LTEST_CASE(a_theme_without_an_accent_falls_back_through_blue) {
    std::vector<std::string> missing;
    const Palette p =
        palette_from_tokens({{"blue", Color{0x00, 0x66, 0xff, 0xff}}}, true, &missing);
    CHECK_EQ(p.get(Role::Accent), (Color{0x00, 0x66, 0xff, 0xff}));
    CHECK_EQ(p.get(Role::DropBorder), p.get(Role::Accent));
    CHECK_EQ(p.get(Role::DividerHover), p.get(Role::Accent));
    CHECK(std::find(missing.begin(), missing.end(), "accent") == missing.end());
}

LTEST_CASE(the_accent_drives_the_whole_drag_overlay) {
    // One token changing has to carry the highlight with it, or a theme switch leaves the drop
    // zones looking like they belong to the previous theme.
    std::vector<std::string> missing;
    const Palette p =
        palette_from_tokens({{"accent", Color{0x7a, 0xa2, 0xf7, 0xff}}}, true, &missing);
    const Color accent = p.get(Role::Accent);
    CHECK_EQ(accent, (Color{0x7a, 0xa2, 0xf7, 0xff}));
    CHECK_EQ(p.get(Role::DropBorder), accent);
    CHECK_EQ(p.get(Role::DividerHover), accent);
    CHECK_EQ(p.get(Role::DropFill).r, accent.r);
    CHECK_EQ(p.get(Role::DropFill).g, accent.g);
    CHECK_EQ(p.get(Role::DropFill).b, accent.b);
    CHECK(p.get(Role::DropFill).a < 255);
}

LTEST_CASE(the_real_omarchy_token_set_maps_completely) {
    const ScratchDir sd;
    REQUIRE(lc::write_atomic(sd.theme() / "colors.toml", kRealColorsToml).has_value());
    REQUIRE(lc::write_atomic(sd.root() / "current" / "theme.name", "tokyo-night\n").has_value());

    const auto t = read_omarchy(sd.theme());
    REQUIRE_MSG(t.has_value(), "{}", t.error());
    CHECK_EQ(t->name, std::string("tokyo-night"));
    CHECK(t->palette.dark());
    CHECK_EQ(t->palette.source(), std::string("omarchy:tokyo-night"));
    CHECK_MSG(t->missing_tokens.empty(), "{} token(s) unmapped, first '{}'",
              t->missing_tokens.size(),
              t->missing_tokens.empty() ? std::string{} : t->missing_tokens.front());

    CHECK_EQ(t->palette.get(Role::PanelBackground), parse_color("#1a1b26").value());
    CHECK_EQ(t->palette.get(Role::Window), parse_color("#0e0e14").value());
    CHECK_EQ(t->palette.get(Role::PanelHeader), parse_color("#13141c").value());
    CHECK_EQ(t->palette.get(Role::PanelHeaderActive), parse_color("#24283b").value());
    CHECK_EQ(t->palette.get(Role::Text), parse_color("#a9b1d6").value());
    CHECK_EQ(t->palette.get(Role::TextMuted), parse_color("#565f89").value());
    CHECK_EQ(t->palette.get(Role::Border), parse_color("#414868").value());
    CHECK_EQ(t->palette.get(Role::Accent), parse_color("#7aa2f7").value());
    CHECK_EQ(t->palette.get(Role::Error), parse_color("#f7768e").value());
    CHECK_EQ(t->palette.get(Role::Warning), parse_color("#e0af68").value());
    CHECK_EQ(t->palette.get(Role::Good), parse_color("#9ece6a").value());
}

LTEST_CASE(light_mode_is_detected_from_the_file_and_from_the_marker) {
    const ScratchDir sd;

    REQUIRE(lc::write_atomic(sd.theme() / "colors.toml",
                             "mode = \"light\"\nbackground = \"#ffffff\"\n")
                .has_value());
    const auto light = read_omarchy(sd.theme());
    REQUIRE_MSG(light.has_value(), "{}", light.error());
    CHECK(!light->palette.dark());

    // Omarchy's other convention: a `light.mode` marker file and no `mode` key.
    REQUIRE(lc::write_atomic(sd.theme() / "colors.toml", "background = \"#ffffff\"\n")
                .has_value());
    REQUIRE(lc::write_atomic(sd.theme() / "light.mode", "").has_value());
    const auto marker = read_omarchy(sd.theme());
    REQUIRE_MSG(marker.has_value(), "{}", marker.error());
    CHECK(!marker->palette.dark());
}

LTEST_CASE(an_unreadable_theme_is_an_error_not_a_silently_wrong_palette) {
    const ScratchDir sd;
    CHECK(!read_omarchy(sd.root() / "nowhere").has_value());
    CHECK(!read_omarchy(sd.theme()).has_value()); // the directory exists, colors.toml does not

    REQUIRE(lc::write_atomic(sd.theme() / "colors.toml", "this is not [ toml").has_value());
    const auto broken = read_omarchy(sd.theme());
    CHECK(!broken.has_value());
    CHECK(!broken.error().empty());

    REQUIRE(lc::write_atomic(sd.theme() / "colors.toml", "mode = \"dark\"\ncount = 3\n")
                .has_value());
    const auto colourless = read_omarchy(sd.theme());
    CHECK_MSG(!colourless.has_value(), "accepted a theme with no colours in it");
}

LTEST_CASE(unparsable_tokens_are_reported_rather_than_dropped) {
    const ScratchDir sd;
    REQUIRE(lc::write_atomic(sd.theme() / "colors.toml",
                             "background = \"#1a1b26\"\naccent = \"not a colour\"\n")
                .has_value());
    const auto t = read_omarchy(sd.theme());
    REQUIRE_MSG(t.has_value(), "{}", t.error());
    CHECK(std::find(t->missing_tokens.begin(), t->missing_tokens.end(), "accent") !=
          t->missing_tokens.end());
    CHECK_EQ(t->palette.get(Role::Accent), Palette::fallback().get(Role::Accent));
}

LTEST_CASE(the_watch_list_covers_the_directory_that_gets_replaced) {
    // A theme switch rewrites the whole theme directory, so a watcher on colors.toml alone loses
    // its subject at the exact moment it is needed.
    const auto paths = watch_paths(std::filesystem::path("/tmp/x/current/theme"));
    bool has_file = false;
    bool has_dir = false;
    bool has_parent = false;
    for (const auto& p : paths) {
        has_file = has_file || p.filename() == "colors.toml";
        has_dir = has_dir || p == std::filesystem::path("/tmp/x/current/theme");
        has_parent = has_parent || p == std::filesystem::path("/tmp/x/current");
    }
    CHECK(has_file);
    CHECK(has_dir);
    CHECK(has_parent);
    CHECK(!watch_paths().empty());
}

LTEST_CASE(this_machines_actual_theme_loads_if_it_is_an_omarchy_machine) {
    // Omarchy is the bedrock platform, so when the tests run on one, read the live theme rather
    // than only a fixture. A failure here means the real file no longer looks like the fixture.
    if (!omarchy_present()) {
        std::fprintf(stderr, "       (not an Omarchy machine; skipped)\n");
        return;
    }
    const auto t = read_omarchy();
    REQUIRE_MSG(t.has_value(), "{}", t.error());
    CHECK(!t->name.empty());
    CHECK_MSG(t->missing_tokens.empty(), "live theme '{}' left {} token(s) unmapped, first '{}'",
              t->name, t->missing_tokens.size(),
              t->missing_tokens.empty() ? std::string{} : t->missing_tokens.front());
    CHECK(t->palette.get(Role::Accent) != Color{});
    std::fprintf(stderr, "       (live theme '%s', accent %s, %s)\n", t->name.c_str(),
                 t->palette.get(Role::Accent).css_hex().c_str(),
                 t->palette.dark() ? "dark" : "light");
}

LTEST_MAIN("theme")

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a capture test can honestly assert without a screen.
//
// The suite runs headless, over SSH, on a box with no RTX card. Real capture needs a live Wayland
// compositor with ext-image-copy-capture-v1 bound, so nothing here claims a frame arrived -- that
// is `capture-spike`'s job, on a machine with a display, and it reports numbers rather than a
// green tick. ADR-0013 names the trap directly: a capture check that cannot tell a still screen
// from a broken path reports success on both.
//
// What is ours, and is covered here, is choosing which screen a source means (ADR-0041).

#include "lcapture/wlcap.hpp"

#include "ltest.hpp"

#include <string>
#include <vector>
#include <cstdlib>

namespace {

std::vector<munkicast::capture::WlOutputInfo> three_screens() {
    return {
        {"DP-1", "Samsung G5", 2560, 1440, 165000},
        {"DP-2", "Dell U2412M", 1920, 1200, 59950},
        {"HDMI-A-1", "Living room TV", 3840, 2160, 60000},
    };
}

} // namespace

LTEST_CASE(live_window_list_has_capture_dimensions) {
    if (!std::getenv("MUNKICAST_TEST_WINDOWS")) return;
    std::string error;
    auto windows = munkicast::capture::list_windows(error);
    REQUIRE_MSG(!windows.empty(), "{}", error);
    for (const auto& window : windows)
        std::fprintf(stderr, "window %s %s [%s]\n", window.id.c_str(), window.title.c_str(), window.app_id.c_str());
    int width = 0, height = 0;
    REQUIRE_MSG(munkicast::capture::window_size(windows.front().id, width, height, error), "{}", error);
    CHECK(width > 0);
    CHECK(height > 0);
}

LTEST_CASE(a_stored_connector_picks_that_screen_wherever_it_sits_in_the_list) {
    std::string error = "untouched";
    const auto outputs = three_screens();

    const auto first = munkicast::capture::wlcap_select_output(outputs, "DP-1", error);
    REQUIRE(first.has_value());
    CHECK_EQ(*first, std::size_t{0});

    // The one in the middle, because an implementation that returns 0 for everything passes a
    // test that only ever asks for the first.
    const auto middle = munkicast::capture::wlcap_select_output(outputs, "DP-2", error);
    REQUIRE(middle.has_value());
    CHECK_EQ(*middle, std::size_t{1});

    const auto last = munkicast::capture::wlcap_select_output(outputs, "HDMI-A-1", error);
    REQUIRE(last.has_value());
    CHECK_EQ(*last, std::size_t{2});

    // Untouched on success: a caller that logs the error string regardless should print nothing.
    CHECK_EQ(error, std::string{"untouched"});
}

LTEST_CASE(an_empty_connector_means_the_first_screen_not_a_failure) {
    // This is the one-screen machine, and the source nobody has pointed anywhere yet. ADR-0041
    // makes it capture something rather than refuse, which is also what wlcap_begin_session did
    // before the choice was split out of it.
    std::string error;
    const auto outputs = three_screens();
    const auto chosen = munkicast::capture::wlcap_select_output(outputs, "", error);
    REQUIRE(chosen.has_value());
    CHECK_EQ(*chosen, std::size_t{0});
}

LTEST_CASE(a_connector_that_is_not_there_fails_and_says_what_is) {
    // Not a fallback. Capturing a different screen than the project names is the failure an
    // operator cannot see -- it looks like a working show of the wrong thing.
    std::string error;
    const auto outputs = three_screens();
    const auto chosen = munkicast::capture::wlcap_select_output(outputs, "DP-9", error);
    CHECK(!chosen.has_value());

    // The message has to carry both halves: an operator reading it has a monitor plugged into
    // something, and the connector they expected is what they need to compare against.
    CHECK(error.find("DP-9") != std::string::npos);
    CHECK(error.find("DP-1") != std::string::npos);
    CHECK(error.find("HDMI-A-1") != std::string::npos);
}

LTEST_CASE(no_outputs_is_distinguishable_from_a_name_that_did_not_match) {
    // The headless case, and the real one: a compositor that advertised nothing. It must not read
    // as "your connector is wrong", because nothing is wrong with the connector.
    std::string error;
    const std::vector<munkicast::capture::WlOutputInfo> none;

    const auto named = munkicast::capture::wlcap_select_output(none, "DP-1", error);
    CHECK(!named.has_value());
    CHECK_EQ(error, std::string{"no outputs"});

    std::string empty_error;
    const auto unnamed = munkicast::capture::wlcap_select_output(none, "", empty_error);
    CHECK(!unnamed.has_value());
    CHECK_EQ(empty_error, std::string{"no outputs"});
}

LTEST_CASE(the_idle_wording_is_one_sentence_wherever_it_is_printed) {
    // ADR-0013: a wait that ran out on a still screen is not a fault, and the point of a shared
    // wording is that the cause reads the same in every tool that can hit it. The duration is
    // printed in seconds, because that is the unit the operator is waiting in.
    const std::string reason = munkicast::capture::wlcap_idle_reason(1000);
    CHECK(reason.find("1.0 s") != std::string::npos);
    // It has to say the screen being still is expected, or the message is just a timeout and the
    // next person goes looking for a fault that is not there.
    CHECK(reason.find("damage-driven") != std::string::npos);
    CHECK(reason.find("not a fault") != std::string::npos);
}

LTEST_MAIN("capture")

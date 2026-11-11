// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "connectors.hpp"
#include "ltest.hpp"

#include <QCoreApplication>
#include <QSet>

using namespace munkicast::ui;

LTEST_CASE(oauth_callback_requires_the_expected_state_and_decodes_the_code) {
    const QByteArray request =
        "GET /callback?state=random-state&code=4%2Fexample%2Bcode HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n\r\n";
    const auto code = oauth_callback_code(request, "random-state");
    REQUIRE(code.has_value());
    CHECK(*code == "4/example+code");
    CHECK(!oauth_callback_code(request, "wrong-state"));
    CHECK(!oauth_callback_code(request, ""));
    CHECK(!oauth_callback_code("GET /callback?code=secret HTTP/1.1\r\n\r\n", "random-state"));
    CHECK(!oauth_callback_code("GET /callback?state=random-state HTTP/1.1\r\n", "random-state"));
    CHECK(
        !oauth_callback_code("POST /callback?state=random-state HTTP/1.1\r\n\r\n", "random-state"));
    CHECK(!oauth_callback_code("GET /other?state=random-state HTTP/1.1\r\n\r\n", "random-state"));
    CHECK(!oauth_callback_code(
        "GET https://example.com/callback?state=random-state HTTP/1.1\r\n\r\n", "random-state"));
    CHECK(!oauth_callback_code(QByteArray(17000, 'x'), "random-state"));
    const auto declined = oauth_callback_code(
        "GET /callback?state=random-state&error=access_denied HTTP/1.1\r\n\r\n", "random-state");
    REQUIRE(declined.has_value());
    CHECK(declined->isEmpty());
}

LTEST_CASE(catalog_has_unique_destinations_and_usable_ingest_servers) {
    const auto catalog = Connectors::catalog();
    REQUIRE(catalog.size() > 70);
    QSet<QString> ids;
    for (const auto& value : catalog) {
        const auto entry = value.toMap();
        const auto id = entry.value("id").toString();
        CHECK(!id.isEmpty());
        CHECK(!ids.contains(id));
        ids.insert(id);
        for (const auto& item : entry.value("servers").toList()) {
            const QUrl url(item.toMap().value("id").toString());
            CHECK(url.isValid());
            CHECK(url.scheme() == "rtmp" || url.scheme() == "rtmps");
            CHECK(!url.host().isEmpty());
        }
    }
    CHECK(ids.contains("rtmp"));
    CHECK(ids.contains("twitch"));
    CHECK(ids.contains("youtube"));
    CHECK(!ids.contains("obs:YouTube - HLS"));
    CHECK(!ids.contains("obs:Dacast"));
    CHECK(!ids.contains("obs:SHOWROOM"));
    CHECK(Connectors::service("twitch").value("oauth").toBool());
    CHECK(Connectors::service("youtube").value("oauth").toBool());
    CHECK(Connectors::service("missing").isEmpty());
}

LTEST_CASE(kick_and_rumble_offer_stream_key_destinations) {
    const auto kick = Connectors::service("obs:Kick");
    const auto rumble = Connectors::service("obs:Rumble");
    for (const auto& service : {kick, rumble}) {
        REQUIRE(!service.isEmpty());
        CHECK(service.value("kind").toString() == "rtmp");
        CHECK(!service.value("oauth").toBool());
        REQUIRE(service.value("servers").toList().size() == 1);
    }
    CHECK(kick.value("title").toString() == "Kick");
    CHECK(kick.value("keyLink").toString() == "https://kick.com/dashboard/settings/stream");
    CHECK(kick.value("servers").toList().first().toMap().value("id").toString() ==
          "rtmps://fa723fc1b171.global-contribute.live-video.net");
    CHECK(rumble.value("title").toString() == "Rumble");
    CHECK(rumble.value("keyLink").toString() == "https://rumble.com/account/static-stream-key");
    CHECK(rumble.value("servers").toList().first().toMap().value("id").toString() ==
          "rtmp://rtmp.rumble.com/live");
}

LTEST_CASE(unsupported_oauth_and_cancel_never_authorize_an_output) {
    int argc = 1;
    char name[] = "test_connectors";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    Connectors connectors;
    bool authorized = false;
    QObject::connect(&connectors, &Connectors::authorized, &app, [&] { authorized = true; });
    connectors.start("output", "unsupported", "project");
    CHECK(!connectors.busy());
    CHECK(connectors.clientId("unsupported").isEmpty());
    connectors.chooseStream("missing");
    connectors.cancel();
    CHECK(!authorized);
    CHECK(connectors.streams().isEmpty());
    CHECK(connectors.status().isEmpty());
}

LTEST_MAIN("connectors")

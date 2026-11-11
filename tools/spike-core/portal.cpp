// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// org.freedesktop.portal.ScreenCast handshake.
//
// The portal is request/response over D-Bus: each method returns a Request object path, and the
// real answer arrives later as a Response signal on that path. The token must therefore be chosen
// by us and the signal subscribed to *before* the call is made, or the reply can race ahead of the
// subscription. Reference reading (never copied): obs-studio plugins/linux-pipewire/portal.c.

#include "portal.hpp"

#include <lcore/log.hpp>

#include <atomic>
#include <cstdio>
#include <random>
#include <string>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

namespace munkicast::spike {
namespace {

constexpr const char* kBus = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRequestIface = "org.freedesktop.portal.Request";

std::string random_token() {
    static std::mt19937_64 rng{std::random_device{}()};
    return std::format("munkicast_{:016x}", rng());
}

/// The portal derives the Request object path from our unique bus name and token, so we can
/// predict it and subscribe before issuing the call.
std::string request_path(GDBusConnection* bus, const std::string& token) {
    const char* unique = g_dbus_connection_get_unique_name(bus);
    std::string sender = (unique != nullptr) ? unique : "";
    if (!sender.empty() && sender.front() == ':')
        sender.erase(0, 1);
    for (char& c : sender)
        if (c == '.')
            c = '_';
    return std::format("/org/freedesktop/portal/desktop/request/{}/{}", sender, token);
}

/// State shared between the call chain and the Response signal handlers.
struct Ctx {
    GDBusConnection* bus = nullptr;
    GDBusProxy* proxy = nullptr;
    GMainLoop* loop = nullptr;

    std::uint32_t response = 0; // 0 ok, 1 user cancelled, 2 other
    GVariant* results = nullptr; // owned on success
    std::string error;
    bool got_reply = false;
};

void on_response(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                 GVariant* params, gpointer user_data) {
    auto* ctx = static_cast<Ctx*>(user_data);
    std::uint32_t code = 2;
    GVariant* results = nullptr;
    g_variant_get(params, "(u@a{sv})", &code, &results);
    ctx->response = code;
    ctx->results = results; // transfer
    ctx->got_reply = true;
    if (ctx->loop != nullptr)
        g_main_loop_quit(ctx->loop);
}

/// Issues one portal method that follows the Request/Response pattern and waits for the response.
/// `build_args` receives the token that must be embedded in the options dict.
bool portal_call(Ctx& ctx, const char* method,
                 GVariant* (*build_args)(const std::string& token, void* ud), void* ud,
                 GVariant** out_results) {
    const std::string token = random_token();
    const std::string path = request_path(ctx.bus, token);

    ctx.response = 2;
    ctx.results = nullptr;
    ctx.got_reply = false;

    const guint sub = g_dbus_connection_signal_subscribe(
        ctx.bus, kBus, kRequestIface, "Response", path.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response, &ctx, nullptr);

    GError* err = nullptr;
    GVariant* args = build_args(token, ud);
    GVariant* reply = g_dbus_proxy_call_sync(ctx.proxy, method, args, G_DBUS_CALL_FLAGS_NONE,
                                             10000, nullptr, &err);
    if (reply == nullptr) {
        ctx.error = std::format("{} failed: {}", method,
                                (err != nullptr && err->message != nullptr) ? err->message : "?");
        if (err != nullptr)
            g_error_free(err);
        g_dbus_connection_signal_unsubscribe(ctx.bus, sub);
        return false;
    }
    g_variant_unref(reply);

    // Wait for the Response signal.
    ctx.loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(ctx.loop);
    g_main_loop_unref(ctx.loop);
    ctx.loop = nullptr;
    g_dbus_connection_signal_unsubscribe(ctx.bus, sub);

    if (!ctx.got_reply) {
        ctx.error = std::format("{}: no Response signal", method);
        return false;
    }
    if (ctx.response != 0) {
        ctx.error = std::format("{}: response code {} ({})", method, ctx.response,
                                ctx.response == 1 ? "user cancelled" : "failed");
        if (ctx.results != nullptr)
            g_variant_unref(ctx.results);
        ctx.results = nullptr;
        return false;
    }
    *out_results = ctx.results;
    ctx.results = nullptr;
    return true;
}

// ---- argument builders -------------------------------------------------------------------

GVariant* build_create_session(const std::string& token, void*) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    g_variant_builder_add(&b, "{sv}", "session_handle_token",
                          g_variant_new_string(random_token().c_str()));
    return g_variant_new("(a{sv})", &b);
}

struct SelectArgs {
    const PortalRequest* req;
    const char* session;
};

GVariant* build_select_sources(const std::string& token, void* ud) {
    auto* a = static_cast<SelectArgs*>(ud);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(a->req->source_types));
    g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(a->req->multiple));
    g_variant_builder_add(&b, "{sv}", "cursor_mode",
                          g_variant_new_uint32(static_cast<std::uint32_t>(a->req->cursor_mode)));
    g_variant_builder_add(&b, "{sv}", "persist_mode",
                          g_variant_new_uint32(a->req->persist_mode));
    if (!a->req->restore_token.empty())
        g_variant_builder_add(&b, "{sv}", "restore_token",
                              g_variant_new_string(a->req->restore_token.c_str()));
    return g_variant_new("(oa{sv})", a->session, &b);
}

GVariant* build_start(const std::string& token, void* ud) {
    const char* session = static_cast<const char*>(ud);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    return g_variant_new("(osa{sv})", session, "", &b);
}

} // namespace

void close_screencast_session(const std::string& session_handle) {
    if (session_handle.empty())
        return;

    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (bus == nullptr) {
        if (err != nullptr)
            g_error_free(err);
        return;
    }

    GVariant* reply = g_dbus_connection_call_sync(
        bus, kBus, session_handle.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
        nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &err);
    if (reply != nullptr)
        g_variant_unref(reply);
    if (err != nullptr) {
        lc::debug("session Close failed (likely already gone): {}",
                  err->message != nullptr ? err->message : "?");
        g_error_free(err);
    } else {
        lc::debug("closed portal session {}", session_handle);
    }
    g_object_unref(bus);
}

bool negotiate_screencast(const PortalRequest& req, PortalSession& out, std::string& error) {
    Ctx ctx;
    GError* err = nullptr;

    ctx.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (ctx.bus == nullptr) {
        error = std::format("no session bus: {}",
                            (err != nullptr && err->message != nullptr) ? err->message : "?");
        if (err != nullptr) g_error_free(err);
        return false;
    }

    ctx.proxy = g_dbus_proxy_new_sync(ctx.bus, G_DBUS_PROXY_FLAGS_NONE, nullptr, kBus, kPath,
                                      kScreenCast, nullptr, &err);
    if (ctx.proxy == nullptr) {
        error = std::format("no ScreenCast portal: {}",
                            (err != nullptr && err->message != nullptr) ? err->message : "?");
        if (err != nullptr) g_error_free(err);
        g_object_unref(ctx.bus);
        return false;
    }

    // Report the portal's advertised capabilities; useful diagnostics and it validates the proxy.
    if (GVariant* v = g_dbus_proxy_get_cached_property(ctx.proxy, "version"); v != nullptr) {
        lc::info("portal ScreenCast version {}", g_variant_get_uint32(v));
        g_variant_unref(v);
    }
    if (GVariant* v = g_dbus_proxy_get_cached_property(ctx.proxy, "AvailableSourceTypes");
        v != nullptr) {
        lc::info("portal AvailableSourceTypes 0x{:x}", g_variant_get_uint32(v));
        g_variant_unref(v);
    }
    if (GVariant* v = g_dbus_proxy_get_cached_property(ctx.proxy, "AvailableCursorModes");
        v != nullptr) {
        lc::info("portal AvailableCursorModes 0x{:x}", g_variant_get_uint32(v));
        g_variant_unref(v);
    }

    auto cleanup = [&] {
        g_object_unref(ctx.proxy);
        g_object_unref(ctx.bus);
    };

    // 1. CreateSession
    GVariant* results = nullptr;
    if (!portal_call(ctx, "CreateSession", build_create_session, nullptr, &results)) {
        error = ctx.error;
        cleanup();
        return false;
    }
    const char* session_handle = nullptr;
    g_variant_lookup(results, "session_handle", "&s", &session_handle);
    if (session_handle == nullptr) {
        error = "CreateSession returned no session_handle";
        g_variant_unref(results);
        cleanup();
        return false;
    }
    out.session_handle = session_handle;
    lc::info("session {}", out.session_handle);
    g_variant_unref(results);

    // 2. SelectSources
    SelectArgs sa{&req, out.session_handle.c_str()};
    results = nullptr;
    if (!portal_call(ctx, "SelectSources", build_select_sources, &sa, &results)) {
        error = ctx.error;
        cleanup();
        return false;
    }
    g_variant_unref(results);

    // 3. Start  (this is where the picker appears, unless restored)
    lc::info("calling Start - a share picker may appear now");
    results = nullptr;
    if (!portal_call(ctx, "Start", build_start,
                     const_cast<void*>(static_cast<const void*>(out.session_handle.c_str())),
                     &results)) {
        error = ctx.error;
        cleanup();
        return false;
    }

    if (const char* tok = nullptr; g_variant_lookup(results, "restore_token", "&s", &tok)) {
        if (tok != nullptr)
            out.restore_token = tok;
    }

    GVariant* streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (streams == nullptr) {
        error = "Start returned no streams";
        g_variant_unref(results);
        cleanup();
        return false;
    }

    GVariantIter it;
    g_variant_iter_init(&it, streams);
    std::uint32_t node = 0;
    GVariant* props = nullptr;
    while (g_variant_iter_next(&it, "(u@a{sv})", &node, &props)) {
        PortalStream s;
        s.node_id = node;
        g_variant_lookup(props, "source_type", "u", &s.source_type);
        if (GVariant* sz = g_variant_lookup_value(props, "size", G_VARIANT_TYPE("(ii)"));
            sz != nullptr) {
            g_variant_get(sz, "(ii)", &s.width, &s.height);
            g_variant_unref(sz);
        }
        if (GVariant* p = g_variant_lookup_value(props, "position", G_VARIANT_TYPE("(ii)"));
            p != nullptr) {
            g_variant_get(p, "(ii)", &s.pos_x, &s.pos_y);
            g_variant_unref(p);
        }
        if (const char* mid = nullptr; g_variant_lookup(props, "mapping_id", "&s", &mid)) {
            if (mid != nullptr)
                s.mapping_id = mid;
        }
        out.streams.push_back(s);
        g_variant_unref(props);
    }
    g_variant_unref(streams);
    g_variant_unref(results);

    if (out.streams.empty()) {
        error = "Start returned an empty stream list";
        cleanup();
        return false;
    }

    // 4. OpenPipeWireRemote - returns a PipeWire connection fd over the D-Bus fd list.
    GVariantBuilder ob;
    g_variant_builder_init(&ob, G_VARIANT_TYPE_VARDICT);
    GUnixFDList* fds = nullptr;
    err = nullptr;
    GVariant* fd_reply = g_dbus_proxy_call_with_unix_fd_list_sync(
        ctx.proxy, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", out.session_handle.c_str(), &ob), G_DBUS_CALL_FLAGS_NONE, 10000,
        nullptr, &fds, nullptr, &err);

    if (fd_reply == nullptr) {
        error = std::format("OpenPipeWireRemote failed: {}",
                            (err != nullptr && err->message != nullptr) ? err->message : "?");
        if (err != nullptr) g_error_free(err);
        cleanup();
        return false;
    }

    gint32 idx = -1;
    g_variant_get(fd_reply, "(h)", &idx);
    out.pipewire_fd = (fds != nullptr && idx >= 0) ? g_unix_fd_list_get(fds, idx, nullptr) : -1;
    g_variant_unref(fd_reply);
    if (fds != nullptr)
        g_object_unref(fds);

    if (out.pipewire_fd < 0) {
        error = "OpenPipeWireRemote returned no usable fd";
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

} // namespace munkicast::spike

// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace munkicast::spike {

/// One stream handed back by org.freedesktop.portal.ScreenCast.Start.
struct PortalStream {
    std::uint32_t node_id = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t pos_x = 0;
    std::int32_t pos_y = 0;
    std::uint32_t source_type = 0; // 1 = monitor, 2 = window, 4 = virtual
    std::string mapping_id;
};

/// A negotiated ScreenCast session: a PipeWire remote fd plus the streams on it.
struct PortalSession {
    int pipewire_fd = -1; ///< owned; close() when done
    std::vector<PortalStream> streams;
    std::string session_handle;
    std::string restore_token; ///< persist this to skip the picker next time
};

enum class CursorMode : std::uint32_t { Hidden = 1, Embedded = 2, Metadata = 4 };
enum class SourceType : std::uint32_t { Monitor = 1, Window = 2, Virtual = 4 };

struct PortalRequest {
    std::uint32_t source_types = static_cast<std::uint32_t>(SourceType::Monitor) |
                                 static_cast<std::uint32_t>(SourceType::Window);
    CursorMode cursor_mode = CursorMode::Embedded;
    bool multiple = false;
    /// A token from a previous session. Avoids re-prompting the user.
    std::string restore_token;
    /// 0 = do not persist, 1 = persist for this app run, 2 = persist until revoked.
    std::uint32_t persist_mode = 2;
};

/// Closes a ScreenCast session via org.freedesktop.portal.Session.Close.
/// MUST be called when done: a leaked session leaves a live capture stream on the compositor side,
/// and after a few of those, new sessions fail to negotiate a format.
void close_screencast_session(const std::string& session_handle);

/// Drives the full portal handshake: CreateSession -> SelectSources -> Start -> OpenPipeWireRemote.
/// Blocks on a private GMainLoop. Shows a picker dialog unless a valid restore_token is supplied.
/// Returns false and fills `error` on failure (including user cancellation).
bool negotiate_screencast(const PortalRequest& req, PortalSession& out, std::string& error);

} // namespace munkicast::spike

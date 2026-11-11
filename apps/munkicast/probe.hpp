// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace munkicast {

/// Prints an environment report and returns a process exit code.
/// Non-zero means "this environment cannot run MunkiCast as configured" — currently that means
/// running on XWayland (ADR-0003). Warnings alone do not fail.
int probe_main(bool strict);

} // namespace munkicast

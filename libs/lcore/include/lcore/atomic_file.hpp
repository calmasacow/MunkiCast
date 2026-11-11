// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace lc {

/// Write `data` to `path` so that a reader sees either the old contents or the new ones, never a
/// mixture and never a truncated file.
///
/// ADR-0015 rule 5 exists because of a specific failure: "randomly rearranging things" is what a
/// torn layout file looks like on the next launch. A plain `ofstream` truncates first and writes
/// second, so a crash or a full disk between the two leaves a zero-length file where a valid
/// layout used to be. This writes a sibling temporary, fsyncs it, renames it over the target, and
/// then fsyncs the directory so the rename itself is durable.
[[nodiscard]] std::expected<void, std::string> write_atomic(const std::filesystem::path& path,
                                                            std::string_view data);

[[nodiscard]] std::expected<std::string, std::string> read_file(const std::filesystem::path& path);

} // namespace lc

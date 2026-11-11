// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <expected>
#include <filesystem>
#include <ldoc/document.hpp>

namespace limport {
struct ObsOptions {
    int width = 0;
    int height = 0;
    ldoc::Fps fps{0, 0};
};

struct ObsResult {
    ldoc::Project project;
    std::vector<std::string> warnings;
};

// Needs a QGuiApplication for font/image metadata. Never opens devices or network resources.
// Destination must not exist. Original collection and conversion report remain in the new project.
[[nodiscard]] std::expected<ObsResult, std::string>
import_obs(const std::filesystem::path& collection, const std::filesystem::path& destination,
           const ObsOptions& options);
} // namespace limport

namespace limport {
// Writes OBS collection JSON and <destination>.report.md; refuses existing paths.
// Returned report lists format differences. Does not read output credentials.
[[nodiscard]] std::expected<std::string, std::string>
export_obs(const ldoc::Project& project, const std::filesystem::path& project_directory,
           const std::filesystem::path& destination);
} // namespace limport

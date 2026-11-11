// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "inspect.hpp"
#include "probe.hpp"

#include <QGuiApplication>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <lcore/log.hpp>
#include <limport/obs.hpp>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::puts(R"(munkicast - live streaming and recording studio

usage:
  munkicast probe [--strict]   report this machine's capture/encode/theme environment
  munkicast project <dir> [-v] describe a project directory and what is wrong with it
  munkicast import-obs <collection.json> <new-dir> <width> <height> <fps-num> <fps-den>
                             convert OBS scenes; save original and conversion report
  munkicast --version          print version
  munkicast --help             this message

Not yet implemented (see the plan's milestones):
  munkicast run                launch the session supervisor and UI
  munkicast attach             attach a UI to a running session)");
}

} // namespace

int main(int argc, char** argv) {
    lc::init_from_env();

    const std::vector<std::string_view> args(argv + 1, argv + argc);

    if (args.empty()) {
        usage();
        return 1;
    }

    const std::string_view cmd = args.front();

    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        usage();
        return 0;
    }
    if (cmd == "--version" || cmd == "-V") {
        std::puts("munkicast " MUNKICAST_VERSION);
        return 0;
    }
    if (cmd == "project") {
        if (args.size() < 2) {
            std::fputs("munkicast: project needs a directory\n", stderr);
            return 2;
        }
        const bool verbose = std::find(args.begin(), args.end(), "-v") != args.end() ||
                             std::find(args.begin(), args.end(), "--verbose") != args.end();
        return munkicast::inspect_project(std::filesystem::path(std::string{args[1]}), verbose);
    }
    if (cmd == "import-obs") {
        if (args.size() != 7) {
            std::fputs("import-obs requires collection, new directory, width, height, fps "
                       "numerator and denominator\n",
                       stderr);
            return 2;
        }
        limport::ObsOptions options;
        int* fields[] = {&options.width, &options.height, &options.fps.num, &options.fps.den};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto arg = args[i + 3];
            const auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), *fields[i]);
            if (parsed.ec != std::errc{} || parsed.ptr != arg.data() + arg.size()) {
                std::fputs("import-obs: dimensions and rate must be integers\n", stderr);
                return 2;
            }
        }
        const std::filesystem::path input{std::string(args[1])}, output{std::string(args[2])};
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication application(argc, argv);
        auto imported = limport::import_obs(input, output, options);
        if (!imported) {
            std::fprintf(stderr, "import-obs: %s\n", imported.error().c_str());
            return 1;
        }
        std::printf("Imported %zu scenes and %zu sources into %s\n",
                    imported->project.scenes.size(), imported->project.sources.size(),
                    output.c_str());
        for (const auto& warning : imported->warnings)
            std::printf("Review: %s\n", warning.c_str());
        std::puts("Read obs-import-report.md before using the imported project on air.");
        return 0;
    }
    if (cmd == "probe") {
        bool strict = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--strict") {
                strict = true;
            } else {
                lc::error("probe: unknown option '{}'", args[i]);
                return 1;
            }
        }
        return munkicast::probe_main(strict);
    }

    lc::error("unknown command '{}' (try --help)", cmd);
    return 1;
}

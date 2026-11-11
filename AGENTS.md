# Repository Guidelines

## Project Structure & Module Organization

MunkiCast is a Linux-first C++23/C17 application built with CMake and Qt/QML. Product applications live in `apps/` (`munkicast-ui` contains the workspace QML); reusable libraries live in `libs/` (for example, `ldoc` for the document model and `llayout` for layout math). Unit tests are in `tests/unit/`, with fixtures and golden data under `tests/fixtures/` and `tests/golden/`. `tools/` holds development spikes, not product modules. Architecture decisions live in `docs/adr/`; schemas live in `schema/`. Keep generated builds in a separate build directory.

## Build, Test, and Development Commands

Use the documented CMake presets from the repository root:

- `cmake --preset linux` configures a local build.
- `cmake --build --preset linux` builds applications, libraries, and tests.
- `ctest --preset linux` runs the test suite.
- `ctest --preset linux-ci` runs the suite with warnings treated as errors.
- `ctest --preset linux-asan` and `ctest --preset linux-tsan` run sanitizer configurations.

For UI changes, `./build/linux/apps/munkicast-ui/munkicast-ui --check` runs the headless QML and interaction checks.

## Coding Style & Naming Conventions

Follow `.editorconfig`: four spaces for C/C++ and CMake, two for JSON/TOML/YAML, UTF-8, LF, and a final newline. C++ formatting follows `.clang-format`; keep lines near its 100-character limit. Match nearby naming: lowercase snake_case for files and functions, and existing project conventions for types and QML panels (`<Name>Panel.qml`). Keep changes within the owning app or library where possible.

## Testing Guidelines

Add or update focused tests in `tests/unit/` for behavior changes; test files use the `test_*.cpp` pattern and are registered through CMake. Run the relevant test executable or `ctest --preset linux`; use the UI `--check` for QML or gesture changes. Property and golden tests cover serialization, layout, document commands, and rendering; preserve their invariants when changing those areas.

## Commit & Pull Request Guidelines

Recent commits use short, sentence-style summaries describing the change (for example, “A screen draws in the Program panel”); no strict prefix convention appears in history. Keep commit subjects concrete and concise. Pull requests should explain behavior and motivation, link related issues when available, list validation commands and results, and include screenshots for visible UI changes. Call out platform or dependency requirements.

## Configuration & Safety

Use out-of-source builds and avoid committing generated build output. Do not add capability assumptions at compile time; hardware and driver support varies and is detected at runtime. Follow the repository's GPL-3.0-or-later licensing headers for new source files.

# SPDX-FileCopyrightText: MunkiCast contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Run the headless self-check against a project directory that did not exist a moment ago.
#
# `--project` opens a directory when one is there, so the check used to inherit whatever the
# last run left -- including projects written by an older schema. That produced failures with
# no relationship to the change under test, and the habit of deleting the directory by hand
# before every run. One line of CMake is cheaper than remembering.
file(REMOVE_RECURSE "${CHECK_DIR}/self-check-project")

execute_process(
    COMMAND "${CHECK_EXE}" --check
            --layout "${CHECK_DIR}/self-check-layout.toml"
            --project "${CHECK_DIR}/self-check-project"
    RESULT_VARIABLE outcome
)

if(NOT outcome EQUAL 0)
    message(FATAL_ERROR "the shell self-check failed (${outcome})")
endif()

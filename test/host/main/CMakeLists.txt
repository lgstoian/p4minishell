# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# The host runner compiles the SHARED sources from test/main/ — it never
# copies them — so a test cannot drift between the board and the host. Only
# the file list lives here (mirrored by tools/host_test_manifest.json, which
# generates host_main.c via tools/host_gen_runner.py).

set(HOST_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main")

set(HOST_TEST_SRCS
    "${HOST_TEST_DIR}/test_ansi_format.c"
    "${HOST_TEST_DIR}/test_json.c"
    "${HOST_TEST_DIR}/test_markdown.c"
    "${HOST_TEST_DIR}/test_html.c"
    "${HOST_TEST_DIR}/test_gfx.c"
    "${HOST_TEST_DIR}/test_filetype.c"
    "${HOST_TEST_DIR}/test_shell_quoting.c"
    "${HOST_TEST_DIR}/test_header.c"
    "${HOST_TEST_DIR}/test_theme.c"
    "${HOST_TEST_DIR}/test_clock.c"
    "${HOST_TEST_DIR}/test_shell_abort.c"
    "${HOST_TEST_DIR}/test_history_search.c"
    "${HOST_TEST_DIR}/test_history_file.c"
    "${HOST_TEST_DIR}/test_shell_parser.c"
    "${HOST_TEST_DIR}/test_clipboard.c"
    # Production sources that are hardware-free by construction (see each
    # file's @hard-rule): compiled directly so the lane exercises the real
    # code rather than a copy or a stub.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/shell/shell_core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/shell/shell_transcript.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_layout.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_status.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_notify_queue.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_refresh.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/font/theme.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/clock/clock_core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/clock/clock_timer.c"
)

idf_component_register(
    SRCS
        "host_main.c"
        ${HOST_TEST_SRCS}
    INCLUDE_DIRS
        "."
        # Header-only use: some host tests include command.h for a shared
        # type or a pure formatter prototype. The command component itself is
        # NOT built here (it pulls display/USB/networking).
        "../../../components/command"
        "../../../components/shell"
        "../../../components/header"
        "../../../components/font"
        "../../../components/clock"
    REQUIRES
        unity
        strutil
        json
        ansi
        gfx
        filetype
        markdown
        esp_timer
)

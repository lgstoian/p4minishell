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
    "${HOST_TEST_DIR}/test_calc.c"
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
    "${HOST_TEST_DIR}/test_editor.c"
    "${HOST_TEST_DIR}/test_himem.c"
    "${HOST_TEST_DIR}/test_batch_expr.c"
    "${HOST_TEST_DIR}/test_svc.c"
    "${HOST_TEST_DIR}/test_usb_hid_layout.c"
    "${HOST_TEST_DIR}/test_css_color.c"
    "${HOST_TEST_DIR}/test_emoji.c"
    "${HOST_TEST_DIR}/test_js_canvas.c"
    # Production sources that are hardware-free by construction (see each
    # file's @hard-rule): compiled directly so the lane exercises the real
    # code rather than a copy or a stub.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/shell/shell_core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/shell/shell_print.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/shell/shell_log.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/shell/shell_transcript.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/storage/storage_path.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/batch_env.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/batch_forf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/calc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/himem.c"
    # The batch verbs under test: the evaluator + `set` helpers and the
    # service/sync cores. Their outside-world contact (SD session, input
    # source, INI registry, console line, header notification) goes through
    # optional registration tables that the firmware installs and this lane
    # leaves empty - no stub, no simulated filesystem.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/batch_io.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/batch_expr.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/sync.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/batch/svc.c"
    # Pure option parsers shared by the modal surfaces and `lock`/`wait`.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/modal/modal_args.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_layout.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_status.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_notify_queue.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/header/header_refresh.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/font/theme.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/clock/clock_core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/clock/clock_timer.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/editor/editor_doc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/editor/editor_spell.c"
    # Page-shot blob model: capture/restore/release of a rendered page as
    # bytes. Real production source, split out of tui.c so no LVGL is linked;
    # the live page stays in tui.c behind the accessor seam
    # (components/tui/tui_page_shot_internal.h).
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/tui/tui_page_shot.c"
    # Browser PSRAM caches (image LRU + rendered-page LRU). Reaches the page
    # only through the public tui_page_shot_* API above.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/browser/src/browser_cache.c"
    "${HOST_TEST_DIR}/test_browser_cache.c"
    # HID report-descriptor decode: pure, no USB/LVGL. The firmware compiles
    # the same TU, so the offsets a real mouse needs are tested with no mouse.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/usb/usb_hid_layout.c"
    # Canvas 2D rasterizer: pure, no QuickJS/LVGL. The JS bindings are
    # the thin layer on top; the drawing model is tested here.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/jsengine/js_canvas.c"
    # Emoji core: pure detection + the retro DOS fallback map (no LVGL,
    # no storage). The pack loader and image decode are on-target only.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/font/emoji_core.c"
    # Host-lane backing for the two storage-backed spellchecker calls (the
    # lane has no SD card). Never part of the firmware image.
    "host_spell_storage.c"
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
        "../../../components/storage"
        "../../../components/batch"
        "../../../components/modal"
        "../../../components/header"
        "../../../components/font"
        "../../../components/clock"
        "../../../components/editor"
        "../../../components/browser/include"
        "../../../components/tui"
        "../../../components/usb"
        "../../../components/jsengine/include"
    REQUIRES
        unity
        strutil
        json
        ansi
        gfx
        filetype
        markdown
        p4heap
        esp_timer
)

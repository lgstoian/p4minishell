# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# The host runner compiles the SHARED sources from test/main/ — it never
# copies them — so a test cannot drift between the board and the host. Only
# the file list lives here (mirrored by tools/host_test_manifest.json, which
# generates host_main.c via tools/host_gen_runner.py).

set(HOST_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main")
set(HOST_COMPONENTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../components")

# Production sources that are hardware-free by construction (see each file's
# @hard-rule). Compiled directly so both host lanes exercise the real code
# rather than a copy or a stub. Keeping the list shared between the Unity lane
# and the standalone .bat runner means they can never drift. The interpreter
# itself (batch.c) is in the list: compiling the real implementation is what
# lets a .bat fixture execute on the host. host_batch_bridge.c supplies only
# the missing link-time symbols; it is never part of a firmware image.
set(HOST_PROD_SRCS
    "${HOST_COMPONENTS}/shell/shell_core.c"
    "${HOST_COMPONENTS}/shell/shell_print.c"
    "${HOST_COMPONENTS}/shell/shell_log.c"
    "${HOST_COMPONENTS}/shell/shell_transcript.c"
    "${HOST_COMPONENTS}/storage/storage_path.c"
    "${HOST_COMPONENTS}/batch/batch_env.c"
    "${HOST_COMPONENTS}/batch/batch_forf.c"
    "${HOST_COMPONENTS}/batch/calc.c"
    "${HOST_COMPONENTS}/batch/himem.c"
    "${HOST_COMPONENTS}/batch/batch.c"
    "${HOST_COMPONENTS}/batch/batch_trap.c"
    "${HOST_COMPONENTS}/batch/batch_dim.c"
    "${HOST_COMPONENTS}/batch/batch_str.c"
    "${HOST_COMPONENTS}/batch/batch_map.c"
    "${HOST_COMPONENTS}/batch/error_strings.c"
    # Pure DOS redirection parser, shared with the firmware. Was split out of
    # command.c so the host lane compiles the real implementation instead of a
    # failure stub; it needs no display, USB or network component.
    "${HOST_COMPONENTS}/command/command_redirect.c"
    # The batch verbs under test: the evaluator + `set` helpers and the
    # service/sync cores. Their outside-world contact (SD session, input
    # source, INI registry, console line, header notification) goes through
    # optional registration tables that the firmware installs and the Unity
    # lane leaves empty - no stub, no simulated filesystem.
    "${HOST_COMPONENTS}/batch/batch_io.c"
    "${HOST_COMPONENTS}/batch/batch_expr.c"
    "${HOST_COMPONENTS}/batch/sync.c"
    "${HOST_COMPONENTS}/batch/svc.c"
    # Pure option parsers shared by the modal surfaces and `lock`/`wait`.
    "${HOST_COMPONENTS}/modal/modal_args.c"
    "${HOST_COMPONENTS}/header/header_layout.c"
    "${HOST_COMPONENTS}/header/header_status.c"
    "${HOST_COMPONENTS}/header/header_notify_queue.c"
    "${HOST_COMPONENTS}/header/header_refresh.c"
    "${HOST_COMPONENTS}/font/theme.c"
    "${HOST_COMPONENTS}/clock/clock_core.c"
    "${HOST_COMPONENTS}/clock/clock_timer.c"
    "${HOST_COMPONENTS}/editor/editor_doc.c"
    "${HOST_COMPONENTS}/editor/editor_spell.c"
    # Page-shot blob model: capture/restore/release of a rendered page as
    # bytes. Real production source, split out of tui.c so no LVGL is linked;
    # the live page stays in tui.c behind the accessor seam
    # (components/tui/tui_page_shot_internal.h).
    "${HOST_COMPONENTS}/tui/tui_page_shot.c"
    # Browser PSRAM caches (image LRU + rendered-page LRU). Reaches the page
    # only through the public tui_page_shot_* API above.
    "${HOST_COMPONENTS}/browser/src/browser_cache.c"
    # HID report-descriptor decode: pure, no USB/LVGL. The firmware compiles
    # the same TU, so the offsets a real mouse needs are tested with no mouse.
    "${HOST_COMPONENTS}/usb/usb_hid_layout.c"
    # Canvas 2D rasterizer: pure, no QuickJS/LVGL. The JS bindings are
    # the thin layer on top; the drawing model is tested here.
    "${HOST_COMPONENTS}/jsengine/js_canvas.c"
    # Emoji core: pure detection + the retro DOS fallback map (no LVGL,
    # no storage). The pack loader and image decode are on-target only.
    "${HOST_COMPONENTS}/font/emoji_core.c"
    # Opt-in signed native payload envelope + the one pure loader plan core.
    # No filesystem, no ELF and no control transfer on the host lane.
    "${HOST_COMPONENTS}/nativepay/native_payload.c"
    "${HOST_COMPONENTS}/nativeexec/native_exec.c"
    # Host-lane backing for two storage-backed spellchecker calls (the lane
    # has no SD card), plus the batch I/O bridge. Never in a firmware image.
    "host_spell_storage.c"
    "host_batch_bridge.c"
)

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
    "${HOST_TEST_DIR}/test_batch_control.c"
    "${HOST_TEST_DIR}/test_bind.c"
    "${HOST_TEST_DIR}/test_macro.c"
    "${HOST_TEST_DIR}/test_error_strings.c"
    "${HOST_TEST_DIR}/test_trap.c"
    "${HOST_TEST_DIR}/test_native_payload.c"
    "${HOST_TEST_DIR}/test_native_exec.c"
    "${HOST_TEST_DIR}/test_shell_variables.c"
    "${HOST_TEST_DIR}/test_shell_pipeline.c"
    "${HOST_TEST_DIR}/test_svc.c"
    "${HOST_TEST_DIR}/test_usb_hid_layout.c"
    "${HOST_TEST_DIR}/test_css_color.c"
    "${HOST_TEST_DIR}/test_emoji.c"
    "${HOST_TEST_DIR}/test_js_canvas.c"
    "${HOST_TEST_DIR}/test_browser_cache.c"
    ${HOST_PROD_SRCS}
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
        "${HOST_COMPONENTS}/command"
        "${HOST_COMPONENTS}/shell"
        "${HOST_COMPONENTS}/storage"
        "${HOST_COMPONENTS}/batch"
        "${HOST_COMPONENTS}/modal"
        "${HOST_COMPONENTS}/header"
        "${HOST_COMPONENTS}/font"
        "${HOST_COMPONENTS}/clock"
        "${HOST_COMPONENTS}/editor"
        "${HOST_COMPONENTS}/browser/include"
        "${HOST_COMPONENTS}/tui"
        "${HOST_COMPONENTS}/usb"
        "${HOST_COMPONENTS}/jsengine/include"
        "${HOST_COMPONENTS}/nativepay"
        "${HOST_COMPONENTS}/nativeexec"
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

# Standalone host webapp render probe. It has its own main() and prints a
# tui dump /a-style report consumed by tools/host_webapp_gate.py as a host
# pre-gate. It is deliberately NOT part of the Unity host_main.c sources.
add_executable(host_webapp_render.elf
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/host/main/host_webapp_render.c"
    "${HOST_COMPONENTS}/jsengine/js_canvas.c"
)
target_include_directories(host_webapp_render.elf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${HOST_COMPONENTS}/markdown"
    "${HOST_COMPONENTS}/ansi"
    "${HOST_COMPONENTS}/gfx"
    "${HOST_COMPONENTS}/jsengine/include"
    "${HOST_COMPONENTS}/p4heap/include"
    "${HOST_COMPONENTS}/strutil/include"
    "${HOST_COMPONENTS}/json/include"
    "${CMAKE_BINARY_DIR}/config"
)
target_link_libraries(host_webapp_render.elf PRIVATE
    __idf_markdown
    __idf_ansi
    __idf_gfx
    __idf_p4heap
    __idf_strutil
    __idf_json
    __idf_esp_timer
)
set_target_properties(host_webapp_render.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Standalone host .bat runner. Same production sources as the Unity lane
# (HOST_PROD_SRCS) plus a non-Unity main(); no second interpreter, parser or
# echo renderer. The only host-only code is the thin execute_command router in
# host_batch_run.c and the I/O bridge in host_batch_bridge.c. Consumed by
# tools/host_batch_run.py as a hardware-free pre-gate, never a firmware image.
add_executable(host_batch_run.elf
    "host_batch_run.c"
    ${HOST_PROD_SRCS}
)
target_include_directories(host_batch_run.elf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${HOST_COMPONENTS}/command"
    "${HOST_COMPONENTS}/shell"
    "${HOST_COMPONENTS}/storage"
    "${HOST_COMPONENTS}/batch"
    "${HOST_COMPONENTS}/modal"
    "${HOST_COMPONENTS}/header"
    "${HOST_COMPONENTS}/font"
    "${HOST_COMPONENTS}/clock"
    "${HOST_COMPONENTS}/editor"
    "${HOST_COMPONENTS}/browser/include"
    "${HOST_COMPONENTS}/tui"
    "${HOST_COMPONENTS}/usb"
    "${HOST_COMPONENTS}/jsengine/include"
    "${HOST_COMPONENTS}/p4heap/include"
    "${HOST_COMPONENTS}/strutil/include"
    "${HOST_COMPONENTS}/json/include"
    "${HOST_COMPONENTS}/nativepay"
    "${HOST_COMPONENTS}/nativeexec"
    "${HOST_COMPONENTS}/markdown"
    "${HOST_COMPONENTS}/ansi"
    "${HOST_COMPONENTS}/gfx"
    "${HOST_COMPONENTS}/filetype"
    "${CMAKE_BINARY_DIR}/config"
)
target_link_libraries(host_batch_run.elf PRIVATE
    __idf_markdown
    __idf_ansi
    __idf_gfx
    __idf_p4heap
    __idf_strutil
    __idf_json
    __idf_filetype
    __idf_esp_timer
)
# Mirror the IDF app link so unreferenced TUs (and their board-only symbols)
# are discarded exactly as they are in the Unity ELF.
target_compile_options(host_batch_run.elf PRIVATE -ffunction-sections -fdata-sections)
target_link_options(host_batch_run.elf PRIVATE -Wl,--gc-sections)
set_target_properties(host_batch_run.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

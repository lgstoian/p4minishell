# P4MiniShell Unit Tests

This directory contains the unit tests for the P4MiniShell components. Tests
use the Unity test framework (included in ESP-IDF) and run on the P4 target.

Current baseline: **372 tests, 0 failures, 2 ignored** (verify with
`tools/unit_run.py <COM_PORT>`). The runner completes cleanly with
`=== All tests completed ===` and no reboot; if the board is reset-looping,
check for a newly added test that calls an LVGL/heap path before `lv_init()`.

## Test Structure

```
test/
  CMakeLists.txt            # Test project build configuration
  main/
    CMakeLists.txt          # Test main component (lists every test_*.c)
    test_main.c             # Test runner entry point (RUN_TEST registrations)
    test_shell_parser.c     # split_args, trim, text_equals, percentage parse
    test_shell_history.c    # history store/recall/password mask
    test_shell_prompt.c     # prompt template + metacharacters
    test_shell_quoting.c    # quoting/escaping, chain split
    test_shell_pipeline.c   # pipe detection
    test_shell_variables.c  # variable expansion
    test_shell_debug_log.c  # debug log buffer
    test_storage_format.c   # size formatting, path helpers
    test_batch_expr.c       # set /a integer evaluator
    test_calc.c             # calc float/string evaluator
    test_findstr.c          # findstr matcher
    test_comp.c             # byte compare
    test_task_sort.c        # ps/tasks/top sort comparator
    test_editor.c           # editor document model
    test_keyboard.c         # keyboard/external-input state
    test_config.c           # CONFIG.SYS directive helpers
    test_applib.c           # applib runtime helpers
    test_db.c               # db record store
    test_alarm.c            # alarm parse/advance helpers
    test_modal.c            # modal option parsers
    test_power.c            # power/idle state
    test_serial.c           # serial framing/CRC
    test_tui.c              # TUI state, table width/parse helpers, draw hold
    test_clipboard.c        # RAM clipboard
    test_history_file.c     # history file round-trip
    test_markdown.c         # markdown renderer
    test_filetype.c         # filetype registry
    test_json.c             # json validate/pretty
    test_gfx.c             # RGB565 raster + BMP (24/32-bit, top-down, scaled decode, fit) + blit + row convert + toolkit (spans/tri/poly/ellipse/fill/text) + viewport (map/clip/nice-step) + frame-stats (intervals/jitter/dropped/format)
    test_asset.c            # CRC-32 vectors + asset manifest parser
    test_pkg.c              # pkg APPINFO-name helper
    test_theme.c            # UI theme registry (lookup/selection)
    test_header.c           # header layout policy (fit/compact/yield/smaller-font)
    test_wifi_state.c       # Wi-Fi state machine
    test_csv.c              # CSV splitter + R1C1 ref substitution + field formatter
    test_bind.c             # F-key bind table (set/lookup/count/index)
    test_crypt.c            # PBKDF2 key derivation + AES-GCM envelope round-trip
    test_tcpterm.c          # tcpterm target parse, escape expansion, reply sanitize
    test_userial.c          # userial VID:PID + line-coding parsers
```

## Running Tests

From the project root:

```powershell
$env:IDF_PATH = "<path-to-esp-idf-v5.5.5>"
. $env:IDF_PATH\export.ps1
cd test
idf.py build flash
# or capture the summaries on the host (replace <COM_PORT> with your port):
python ..\tools\unit_run.py <COM_PORT>
```

> The test app drives no UI: after flashing it the display stays black and
> only the serial console speaks (Unity output). This is expected. Always
> reflash the **main** firmware from the repo root (`idf.py flash`) when you
> are done, and take a screenshot to confirm the shell UI is back.

The committed `test/sdkconfig` is a generated file; if a test knob (e.g.
`CONFIG_LV_FONT_UNSCII_16`) silently reverts, re-check `test/sdkconfig.defaults`
and regenerate (see `bugs.md` M42).

## Coverage notes

- Pure/headless logic is unit-tested; anything needing the LVGL label, SD
  card, or display (transcript flush, TUI rendering, modals, screenshot) is
  hardware-verified via the `apps/companion` drivers and `tools/`.

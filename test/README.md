# P4MiniShell Unit Tests (firmware v1.3.0)

This directory contains the unit tests for the P4MiniShell components. Tests
use the Unity test framework (included in ESP-IDF) and run on the P4 target.

Current baseline: **423 tests, 0 failures, 2 ignored** (verify with
`tools/unit_run.py <COM_PORT>`). The runner completes cleanly with
`=== All tests completed ===` and no reboot; if the board is reset-looping,
check for a newly added test that calls an LVGL/heap path before `lv_init()`.
The latest additions — the 14 `test_pim.c` sync tests, the 3 `test_alarm.c`
render/parse tests, the editor CSV tests, the power deep-wake test, the new
`test_html.c` reader tests (12), the `test_markdown.c` strip/auto/form-feed
tests (3), `test_filetype_html`, the `test_editor.c` HTML lexer/comment
tests (2), the 4 `test_audio.c` output-mode/route/WAV tests, the 4 spell
overlay/cursor-word tests, the USB/HID spell key-map test, the batch
round (`test_batch_control.c`: `shift /n` parser, `for /f` defaults),
the shell round (`test_shell_prompt.c`: session title,
`test_shell_pipeline.c`: echo-glued detector, redirect devices/missing
targets/handle prefixes), and the 7 `test_netsvc.c` event-service tests
(MQTT codec, topic matcher, backoff, outbox framing) — bring the
suite to **495 RUN_TEST total**, build-verified this
pass; refresh the count on hardware with `tools/unit_run.py <COM_PORT>`.

## Test Structure

```
test/
  CMakeLists.txt            # Test project build configuration
  main/
    CMakeLists.txt          # Test main component (lists every test_*.c)
    test_main.c             # Test runner entry point (RUN_TEST registrations)
    test_shell_parser.c     # split_args, trim, text_equals, percentage parse, echo-glued detector
    test_shell_history.c    # history store/recall/password mask
    test_shell_prompt.c     # prompt template + metacharacters, session title
    test_shell_quoting.c    # quoting/escaping, chain split
    test_shell_pipeline.c   # pipe detection, redirect devices/missing targets/handle prefixes
    test_shell_abort.c      # foreground-break flag and worker-busy state
    test_shell_variables.c  # variable expansion
    test_shell_debug_log.c  # debug log buffer
    test_ansi_format.c      # ANSI format string builder
    test_storage_format.c   # size formatting, path helpers
    test_batch_expr.c       # set /a integer evaluator
    test_batch_control.c    # labels, on/goto parsing, for/while/switch helpers, shift parser, forf defaults
    test_macro.c            # macro recorder
    test_completion.c       # tab-completion and inline ghost providers
    test_history_search.c   # reverse-history search matching
    test_clock.c            # header-clock formatter
    test_calc.c             # calc float/string evaluator
    test_findstr.c          # findstr matcher
    test_comp.c             # byte compare
    test_task_sort.c        # ps/tasks/top sort comparator
    test_editor.c           # editor document model + OSK map + word count
    test_keyboard.c         # keyboard/external-input state
    test_config.c           # CONFIG.SYS directive helpers
    test_applib.c           # applib runtime helpers
    test_db.c               # db record store
    test_alarm.c            # alarm parse/advance helpers
    test_modal.c            # modal option parsers
    test_power.c            # power/idle state
    test_audio.c            # audio output-mode parse, route resolve, WAV params
    test_serial.c           # serial framing/CRC
    test_tui.c              # TUI state, table width/parse helpers, draw hold
    test_clipboard.c        # RAM clipboard
    test_history_file.c     # history file round-trip
    test_markdown.c         # markdown ANSI renderer + HTML serializer + print paginator
    test_filetype.c         # filetype registry
    test_json.c             # json validate/pretty
    test_gfx.c             # RGB565 raster + BMP (24/32-bit, top-down, scaled decode, fit) + blit + row convert + toolkit (spans/tri/poly/ellipse/fill/text) + viewport (map/clip/nice-step) + frame-stats (intervals/jitter/dropped/format)
    test_asset.c            # CRC-32 vectors + asset manifest parser
    test_pkg.c              # pkg APPINFO-name helper + ECDSA signature vectors
    test_screen.c           # screen list splitter + declarative flow router
    test_gfind.c            # gfind /files pure name filter (hidden/store/ext/kinds)
    test_theme.c            # UI theme registry (lookup/selection)
    test_header.c           # header layout policy (fit/compact/yield/smaller-font)
    test_wifi_state.c       # Wi-Fi state machine
    test_csv.c              # CSV splitter + R1C1 ref substitution + field formatter
    test_bind.c             # F-key bind table (set/lookup/count/index)
    test_crypt.c            # PBKDF2 key derivation + AES-GCM envelope round-trip (unit always runs; the former s07 hardware skip is gone — crypt now passes on both boards, F23 fixed)
    test_tcpterm.c          # tcpterm target parse, escape expansion, reply sanitize
    test_netsvc.c           # MQTT codec, topic matcher, backoff, outbox framing
    test_archive.c          # USTAR core
    test_hardware.c         # Tab5 hardware-support pure helpers
    test_sw_gcm.c           # software AES-256-GCM fallback
    test_userial.c          # userial VID:PID + line-coding parsers
    test_import.c           # vCard/iCalendar line + DATE-TIME parsers, JSON unescape
    test_pim.c              # PIM render buffer + uid mint + newer-wins + identity payload
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
and regenerate (see `bugs.md` M42). The unit image uses a custom partition
table (`test/partitions.csv`, 2 MB app) because the suite links every component
and the default 1 MB single-app partition ran out of room.

## Coverage notes

- Pure/headless logic is unit-tested; anything needing the LVGL label, SD
  card, or display (transcript flush, TUI rendering, modals, screenshot) is
  hardware-verified via the `apps/companion` drivers and `tools/`.

# P4MiniShell Unit Tests (firmware v1.4.1)

This directory contains the unit tests for the P4MiniShell components. They use
the Unity framework (included in ESP-IDF) and run in two places:

- **On the P4 target** - the authoritative run (`tools/unit_run.py COM3 COM6`).
- **On the host** - a hardware-free pre-gate for the pure logic (`test/host/`,
  ESP-IDF `linux` target, 283 cases at v1.4.3). See `harness.md` 2.2 and
  section "Host suite" below.

Current baseline: **681 tests, 0 failures, 2 ignored** on COM3 and COM6
(v1.4.3, after the 15 new colour/emoji/alpha cases), and the host lane is
**283/283** green. The static case count is **739** (what `tools/green_guard.py` sees in
`test/main/test_*.c`); the difference is cases that only compile for the
target. The runner completes cleanly with `=== All tests completed ===` and no
reboot; if the board is reset-looping, check for a newly added test that calls
an LVGL/heap path before `lv_init()`.

The suite is organised by component. Dated round-by-round detail lives in
`changelog.md`; this file documents the layout and how to run it.

## Test Structure

```
test/
  CMakeLists.txt            # Test project build configuration
  main/
    CMakeLists.txt          # Test main component (lists every test_*.c)
    test_main.c             # Test runner entry point (RUN_TEST registrations)
    test_breadcrumb.c       # breadcrumb trail formatter (pure)
    test_mcp.c              # MCP JSON-RPC request builder + client defaults
    test_shell_parser.c     # split_args, trim, text_equals, percentage parse, echo-glued detector
    test_shell_history.c    # history store/recall/password mask
    test_shell_prompt.c     # prompt template + metacharacters, session title
    test_shell_quoting.c    # quoting/escaping, chain split
    test_shell_pipeline.c   # pipe detection, redirect devices/missing targets/handle prefixes
    test_shell_abort.c      # foreground-break flag and worker-busy state
    test_shell_variables.c  # variable expansion
    test_shell_debug_log.c  # debug log buffer
    test_ansi_format.c      # ANSI format string builder + image glyph ramp
    test_storage_format.c   # size formatting, path helpers
    test_storage_volumes.c  # hexdump rows, DEBUG number parsing + guards, volume classification, MBR decode
    test_certs.c            # DER->PEM conversion (real CA cert fixture)
    test_portal.c           # portal HTML escaping
    test_pim_carddav.c      # CardDAV multistatus scan, card split, UID, URL join
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
    test_tmgr.c             # task-manager pool config, priority bounds/clamp, tmgr_* event names, nice parser
    test_editor.c           # editor document model + OSK map + word count
    test_keyboard.c         # keyboard/external-input state
    test_config.c           # CONFIG.SYS directive helpers
    test_applib.c           # applib runtime helpers
    test_db.c               # db record store
    test_alarm.c            # alarm parse/advance helpers
    test_modal.c            # modal option parsers
    test_himem.c            # himem size/handle parsers, CRC-32 vectors, handle bounds/quota/lock, env overlay isolation
    test_svc.c              # sync name/mutex/event semantics, service registry guards
    test_power.c            # power/idle state
    test_audio.c            # audio output-mode parse, route/amp-gate resolve, WAV params, mic stats, Goertzel
    test_serial.c           # serial framing/CRC
    test_tui.c              # TUI state, table width/parse helpers, draw hold, selftest (palette/coverage/metrics), bound cell info
    test_clipboard.c        # RAM clipboard
    test_history_file.c     # history file round-trip
    test_markdown.c         # markdown ANSI renderer + HTML serializer + print paginator
    test_filetype.c         # filetype registry
    test_json.c             # json validate/pretty
    test_gfx.c              # RGB565 raster + BMP (24/32-bit, top-down, scaled decode, fit) + blit + row convert + toolkit (spans/tri/poly/ellipse/fill/text) + viewport (map/clip/nice-step) + frame-stats (intervals/jitter/dropped/format)
    test_asset.c            # CRC-32 vectors + asset manifest parser
    test_pkg.c              # pkg APPINFO-name helper + ECDSA signature vectors + version compare + requires= parse
    test_assoc.c            # assoc extension normalize + argument split
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
    test_epub.c             # EPUB config, OPF/NCX parse, spine/TOC, render
    test_rss_store.c        # RSS store record editors (name/round-trip/reject/separator), read-line parse, CONFIG.SYS directives
    test_browser_cache.c    # browser PSRAM caches: image round-trip/miss/refresh/LRU/clear, page-cache no-page degradation
    test_hardware.c         # Tab5 hardware-support pure helpers
    test_sw_gcm.c           # software AES-256-GCM fallback
    test_userial.c          # userial VID:PID + line-coding parsers
    test_import.c           # vCard/iCalendar line + DATE-TIME parsers, JSON unescape
    test_pim.c              # PIM render buffer + uid mint + newer-wins + identity payload
    test_browser.c          # website renderer (DOM/CSS/images/JS hooks)
    test_cas.c              # CAS parser/simplify/diff/solve/series
    test_chat.c             # chat transport (config, inbox/log ring, sequence, send/poll/clear)
    test_error_strings.c    # error-code -> message table
    test_html.c             # HTML reader (components/markdown/html_read.c)
    test_httpget.c          # cookie jar for httpget/browser
    test_imgdec.c           # PNG/JPEG/BMP format probe, RGB565 mapping, decode caps
    test_rss_opml.c         # OPML parse: standard/extensions, sanitising, limits, write
    test_usb_hid_layout.c   # HID report-descriptor map, 16-bit axes, wheel, report IDs
    test_js_canvas.c        # Canvas 2D core: alpha, transforms, paths, text, snapshots
    test_locale.c           # locale formatting (date order, 12/24h, decimals, thousands)
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
# or both boards concurrently:
python ..\tools\unit_run.py COM3 COM6
```

> The test app drives no UI: after flashing it the display stays black and
> only the serial console speaks (Unity output). This is expected. Always
> reflash the **main** firmware from the repo root (`idf.py flash`) when you
> are done, and take a screenshot to confirm the shell UI is back.

## Host suite

The same sources also build natively with ESP-IDF's preview `linux` target
(`test/host/`), so the pure-logic cases run on the host and in CI without a
board. Coverage today is **283 cases** - the leaf components with no board
dependency (`ansi`, `json`, `markdown`/`html`, `gfx`, `filetype`), the
hardware-free halves of the shell (`shell_core.c` for text/scanning/history/
abort/help, `shell_transcript.c` for the transcript model behind an optional
sink), header (`header_layout/status/notify_queue/refresh.c`), theme
(`font/theme.c`) and clock (`clock_core.c`, `clock_timer.c`).

```sh
cd test/host && idf.py build && ./build/p4minishell_host_tests.elf
python3 tools/host_unit_run.py            # build + run + parse (repo root)
python3 tools/host_unit_run.py --static   # manifest/orphan/case-count gates
```

`tools/host_test_manifest.json` is the single source of truth: it lists the
included test files, the excluded ones with a reason, and the expected case
count. `tools/host_gen_runner.py` generates `test/host/main/host_main.c` from it.
To add a suite, the owning component must not pull a board-only `REQUIRES`
(`shell`/`storage`/`display`/LVGL); add it to `test/host/CMakeLists.txt` and the
manifest, then re-run the generator. This lane is a pre-gate only — see
`harness.md` §2.2.

## False-green guard

`python ../tools/green_guard.py` rejects assertion-free Unity tests, tests that
are declared but never `RUN_TEST`-ed, test files missing from
`main/CMakeLists.txt`, and tautological host-suite checks. The p4test runner
also treats a zero-assertion `Checklist` as a failure at run time, so a vacuous
suite can never report green. Run the guard after adding a test; the release
gate does (`release.md`). See `docs/comment-tags.md` for the header tags and
`AGENTS.md` / `BREADCRUMBS.md` for the navigation map.

The committed `test/sdkconfig` is a generated file; if a test knob (e.g.
`CONFIG_LV_FONT_UNSCII_16`) silently reverts, re-check `test/sdkconfig.defaults`
and regenerate (see `bugs.md` M42). The unit image uses a custom partition
table (`test/partitions.csv`, 2 MB app) because the suite links every component
and the default 1 MB single-app partition ran out of room.

## Coverage notes

- Pure/headless logic is unit-tested; anything needing the LVGL label, SD
  card, or display (transcript flush, TUI rendering, modals, screenshot) is
  hardware-verified via the `apps/companion` drivers and `tools/`.


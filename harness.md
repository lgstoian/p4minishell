# P4MiniShell Testing Harness

> The complete guide to testing this firmware. It covers the seven rungs of the
> ladder (static gates, the host unit suite, on-target Unity, hardware suites,
> focused drivers, one-shots, webcam diagnostics), how to run and extend them,
> the two-board workflow, and the visual checks. The companion implementation
> reference is [`tools/README.md`](tools/README.md) (every driver) and
> [`test/README.md`](test/README.md) (the Unity layout). Change and verification
> rules live in [`ai-context.md`](ai-context.md); open bugs in
> [`bugs.md`](bugs.md).

- **Version:** v1.6.0 · **Targets:** ESP32-P4 + ESP32-C6
- **Boards:** `jc1060p470c` (reference, e.g. COM3) and `m5stack_tab5` (COM6)

---

## 0. The one rule

Every change is finished only when: **firmware and `test/` build with zero
errors and zero warnings**, the on-board **unit suite is green** on the
affected board(s), and the **affected hardware behaviour is verified** on the
board. After flashing the test app, **reflash the main firmware**.
(`ai-context.md` → Hard Rules.)

The **host unit lane** (§2.2) is an additional pre-gate: it catches logic
regressions before a board is free, and it must stay green when a change
touches a source it compiles. It is **never** a substitute for this rule — a
host pass signs off nothing about hardware. The same applies to all v1.6 host
gates (batch, webapp, gfx, tui, modal, native payload checker, P4Sync bridge,
and the new `os`/`power`/`audio`/`video`/`engine3d` subsystem gates) and the
visual proof gates (web-screen generator, visual proof gate and its
canaries): they
compile or drive shipped code and the one shared pixel model, never a fake
backend, and none requires or replaces a board.

---

## 1. Prerequisites and environment

```powershell
# 1. Source the matching ESP-IDF in every new shell (v5.5.5)
$env:IDF_PATH = "C:\esp\v5.5.5\esp-idf"      # adjust to your install
. $env:IDF_PATH\export.ps1

# 2. Keep a fresh configure warning-free: the reference board BSP is vendored
#    under managed_components/ as an EXTRA_COMPONENT_DIR, which the component
#    manager otherwise flags as an "unexpected" managed file.
$env:IDF_COMPONENT_SUPPRESS_UNKNOWN_FILE_WARNINGS = "1"

# 2. Host tools need Python 3 with:
#    pyserial            - all serial drivers
#    cryptography        - tools/pkg_sign.py, tools/pkg_test.py
#    opencv-python (cv2) - webcam diagnostics (display_glitch_watch, led_watch)
#    Pillow (PIL)        - sprite generation + BMP->PNG conversion in suites
python -m pip install pyserial cryptography opencv-python Pillow
```

- **Do not** use a different IDF major for the build (the vendored
  `managed_components/` and `sdkconfig` are pinned to 5.5.5).
- The unit-test app has its own partition table (`test/partitions.csv`, 2 MB);
  it is built and flashed separately from the firmware.

## 2. Selecting a port and a board

Host tools resolve the port in this order: an explicit `port=`/`--port`
argument, a trailing `COMx` argument, `$env:P4_PORT`, then
`tools/board_ports.py` (which maps a board slug to a COM port via the live
`sysinfo` `board.id` and a `tools/board_ports.json` USB-serial fallback).

```powershell
python tools/board_ports.py list --probe     # show each port's board.id
python tools/board_ports.py get m5stack_tab5 # print that board's COM port
```

Set `$env:P4_BOARD` / `$env:P4_PORT` once per shell to avoid passing them
everywhere. **Always open the port through `tools/shell_session.open_port()`**
(it pre-sets DTR/RTS low before `open()` so opening the port does **not** reset
the board); never open a raw `serial.Serial`. Anything needing a fresh boot
calls `shell_session.hard_reset()`.

---

## 2.1 Layer 0 — static gates (no hardware)

Run these before a hardware run (or in CI without a board):

```powershell
python tools/comment_index.py          # machine-readable tags + layering
python tools/breadcrumbs.py            # breadcrumb map validity
python tools/green_guard.py --strict   # false-green / false-positive tests
python tools/check_assets.py           # UI colours: no LVGL default-theme blobs
python tools/gallery_audit.py          # gallery PNG identity + uniqueness
python tools/gallery_audit_selftest.py # red canary: the identity guard must go red
python tools/harness/verify.py         # on-disk invariants + harness markers
python tools/host_tui_gate.py --check      # v1.6: one cell renderer + dirty rects
python tools/host_modal_gate.py --check    # v1.6: focus/progress/session transitions
python tools/gfx_toolkit_test.py --host    # v1.6: gfx wiring/RGB565 contract
python tools/host_batch_run.py --check     # v1.6: real .bat fixtures
python tools/host_webapp_gate.py --check   # v1.6: shared DOM/CSS render path
python tools/host_browser_gate.py --check  # real renderer: wiki + 4 DOS pages, 271 assertions
python tools/host_apps_gate.py --check     # 34 apps, 679 host assertions, 45 honest SKIPs
python tools/make_web_screenshots.py --check  # DOS web proof PNGs (host render)
python tools/visual_proof_gate.py --check     # palette, box borders, ink bands, signatures
python tools/visual_proof_gate.py --self-test # red canaries (no board)
python tools/p4sync_gate.py --check           # P4Sync CLI/bridge: 51 checks, no serial port
python tools/p4sync_gate.py --self-test       # 18 P4Sync red canaries
python tools/host_os_gate.py --check          # v1.6: os facade, 14 cases / 144 checks
python tools/host_os_gate.py --self-test      # os facade red canaries (no board)
python tools/host_power_gate.py --check       # v1.6: power/sleep, 300 checks, 4 board profiles
python tools/host_power_gate.py --self-test   # power red canaries (no board)
python tools/host_audio_gate.py --check       # v1.6: audio engine, 99 checks
python tools/host_audio_gate.py --self-test   # audio red canaries (no board)
python tools/host_video_gate.py --check       # v1.6: video/EPUB/archive, 18 assertions
python tools/host_video_gate.py --self-test   # video red canaries (no board)
python tools/host_engine3d_gate.py --check    # v1.6: 3D engine, 4 fixtures / 41 assertions
python tools/host_engine3d_gate.py --self-test # engine3d red canaries (no board)
# size budget runs only when build/p4minishell.elf exists
python tools/p4test_run.py --static    # the SAME gate list, no board
```

The v1.6 gates are **hardware-free pre-gates**. `host_tui_gate.py` compiles
`tui_frame.h`, the one ANSI palette and the one CSS colour parser from the
shipped tree into a generated harness over scene fixtures; `host_modal_gate.py`
generates a harness from `tools/host/fixtures/modal/transitions.json` and links
the real `components/modal/modal_args.c`; `gfx_toolkit_test.py --host` checks
the RGB565 constants shared with `test/main/test_gfx.c` against
`tools/host/fixtures/gfx/expected.json`; `host_apps_gate.py` loads
`host_batch_run.py` as a library and checks the 34 `[M-RICH-*]` app fixtures
(including the flagship showcase), reporting the 45 hardware-only surfaces as
honest SKIPs (never simulated). `host_browser_gate.py --check` discovers the
shipped `host_webapp_render.elf` and **executes** the real DOM renderer on five
fixtures (the ESP32 wiki proof plus the four DOS pages): 271 assertions against
exact baselines captured from that renderer, plus 11 labelled STATIC wiring
checks; `--self-test` runs 9 red canaries. It proves no device behaviour:
live HTTPS, the full 2000-row page, real LVGL input and glass taps remain
DEVICE-ONLY. `host_all_gates.py` now runs **49 gates / 22 canaries** (the new gate ids are
`os`, `power`, `audio`, `video` and `engine3d` plus their `*_canary` ids,
alongside `batch`, `browser`, `visual` and `p4sync`/`p4sync_canary`). A pass is
a board-free pre-gate only, never hardware sign-off (§0).
Each has a red canary or exact baseline; none opens a serial port.

**Visual proof (board-free, never sign-off).** `tools/visual_proof_gate.py
--check` validates the committed 1024x600 proof PNGs using the ONE shared pixel
model (`p4test.screenshot.Bmp`, never a copied parser): a real share of
DOS-palette pixels, continuous CP437 box-border runs at the panel edges, ink in
the header/body/footer bands (so an empty grid fails), no LVGL default-theme
blue blobs (reusing `p4test.theme_guard.scan_bmp`) and no empty-box glyph runs,
plus a distinct per-screen signature so no two pages/apps share a proof and a
byte-exact `docs/assets/web/` mirror of each web page.
`--update` rewrites the signature manifest; a missing or stale signature makes
`--check` red. The checked set is the four DOS pages under `screenshots/web/`
plus `docs/assets/web-features.png` and `docs/assets/webapp-forms.png`.
`--self-test` runs seven in-memory canaries — blank frame, default-blue/shell
chrome, wrong palette, broken border, tofu glyph cells, a duplicate page
signature, and a clean frame that must be accepted — and is red if any bad case
passes or the clean control fails. `host_all_gates.py` exposes these as gate ids
`visual`/`visual_canary` and the real-execution page checks as `browser`/
`browser_canary`. The PNGs come from
`python tools/make_web_screenshots.py --update` (functional `portal`,
`dashboard`, `article` and `form` fixtures rendered through the real engine)
and `python tools/make_webapp_proof.py --update` (DOS feature/form proofs);
both have `--check` and `--self-test`. These are host proofs labelled
`HOST PROOF - NOT A DEVICE CAPTURE`: they assert presentation structure, not
hardware behaviour, and no device capture or board result is claimed.

`tools/harness/verify.py` and `tools/p4test/runner.py` now share one canonical
gate list (asserted by `python tools/p4test_run.py --self-test`), so `--static`
and `--guard` cannot silently diverge from the release gate. `--guard` runs the
same gates before a hardware run and aborts on a red gate.
A zero-assertion `Checklist`/`SuiteResult` is a failure at run time, not a pass.
The tag vocabulary is in [`docs/comment-tags.md`](docs/comment-tags.md); the
generated map is [`BREADCRUMBS.md`](BREADCRUMBS.md) / `breadcrumbs.json`.

The guards themselves have red canaries: `green_guard.py --self-test` (9
computed-tautology cases), `gallery_audit_selftest.py`,
`appdiff.py --self-test`, and `p4test_run.py --self-test` (marker, gate-parity
and dogfood cases). `p4test.session.read_until(..., required=True)` raises on a
missing marker instead of returning a stale buffer; `appdiff` uses it for all
load-bearing waits. A guard that has never been seen failing is not a guard.

## 2.2 Layer 1a — host unit suite (no hardware)

The same `test/main/test_*.c` sources also build and run natively on the host
with ESP-IDF's preview `linux` target, so a logic regression is caught on every
PR without a board. CI runs it in the `host-unit` job of
`.github/workflows/build.yml`. This lane compiles the **real interpreter** for
the host (`batch.c`, `batch_trap.c`, `batch_dim.c`, `batch_str.c`,
`batch_map.c`, `error_strings.c`, alongside the pure helpers) — not a second
implementation. `components/storage/storage.h` hides its one FATFS-typed
declaration behind `P4_STORAGE_NO_FATFS_TYPES`, which the interpreter defines
before including it; `test/host/main/host_batch_bridge.c` supplies only the
link-time symbols the firmware provides from command/storage/UI, returning an
honest "unavailable" for board-only calls; it lives exclusively in `test/host`
and is never part of a firmware image.

```powershell
# Linux or WSL2 (the "linux" target is a POSIX host target), IDF 5.5.5 sourced:
cd test/host
idf.py build
./build/p4minishell_host_tests.elf

# from the repository root, same shell:
python3 tools/host_unit_run.py            # build (if needed) + run + parse
python3 tools/host_unit_run.py --static   # manifest/orphan/case-count gates
python3 tools/host_gen_runner.py --check  # generated runner is up to date
python tools/host_webapp_gate.py --check  # webapp render pre-gate (separate; no board)
python tools/host_batch_run.py --check    # real .bat fixture pre-gate (separate; no board)
```

- **Coverage today: 375 generated cases** (manifest count for v1.6; refresh
  the last-run count with `tools/host_unit_run.py --build`) - the leaf components with no board
  dependency (`ansi`, `json`, `markdown`/`html`, `gfx`, `filetype`, plus the
  `strutil`/`p4heap` helpers they use), the hardware-free halves of the shell
  (`shell_core.c`: text helpers, statement scanner, history store, abort/busy,
  help registry; `shell_transcript.c`: the transcript model behind an optional
  sink; `shell_print.c`; `shell_log.c`), header
  (`header_layout/status/notify_queue/refresh.c`), theme (`font/theme.c`),
  clock (`clock_core.c`, `clock_timer.c`), the editor document model
  (`editor_doc.c`, `editor_spell.c`), the batch environment, expression core and
  verbs (`batch_env.c`, `batch_forf.c`, `calc.c`, `himem.c`, `batch_io.c`, plus
  the real `batch.c` interpreter and `batch_trap.c`/`batch_dim.c`/`batch_str.c`/
  `batch_map.c`/`error_strings.c` behind the host-only bridge), the pure
  storage path layer (`storage_path.c`), the modal argument parsers
  (`modal_args.c`), the browser page/image caches (`browser_cache.c`) and the
  TUI page-shot model (`tui_page_shot.c`). New suites cover batch control flow
  (`test_batch_control.c`), F-key binds (`test_bind.c`), macros (`test_macro.c`),
  the error-message table (`test_error_strings.c`), `trap` (`test_trap.c`),
  variable expansion (`test_shell_variables.c`) and pipes/redirection
  (`test_shell_pipeline.c`). Runs in well under a second.
- **What cannot run yet, and why:** most other components still reach a
  board-only component through `REQUIRES` — `batch → shell` is covered for the
  real interpreter by the host-only bridge above (LVGL/display, USB-Serial-JTAG
  and SD-backed paths still have no host backend), while `archive → storage`
  (SDMMC/FAT), `modal → windows/keyboard` and `clock → board RTC over I2C`
  remain excluded. `tools/host_test_manifest.json` records the reason for every
  excluded file. `tools/green_guard.py` and
  `tools/host_unit_run.py --static` both fail if a new `test_*.c` is neither
  included nor excluded, so a new suite cannot silently miss both lanes.
- **The split so far:** `shell.c` → `shell_core.c` moved the pure text helpers
  and the statement scanner, then the hardware-free shell state: the recall
  history store (with its password masking), the abort/busy flags the command
  worker polls, the reverse-search matcher and the help registry. State both
  files share is declared in `shell_core_internal.h`, so each object is
  defined once. The same recipe applies to the next block (`shell_transcript_*`,
  prompt/title), but the transcript's LVGL label and UART mirror must be
  separated first — that change is board-gated.
- **Never a substitute:** this lane is a pre-gate. A host pass says nothing
  about hardware behaviour, and §0 still governs. The honest way to grow it is
  to split hardware out of the mixed components or add a host-only bridge with
  honest "unavailable" behaviour — a bridge stub must never become fake
  hardware behaviour in production code, and `host_batch_bridge.c` is never
  compiled into firmware.
- **Webapp render pre-gate (separate from the Unity lane):** `tools/host_webapp_gate.py`
  renders each HTML fixture in `tools/host/fixtures/webapp/` through the
  firmware's **existing** DOM/CSS/render core via the standalone
  `tools/host/main/host_webapp_render.c` harness, then checks `tui dump /a`-style
  text and colour runs — and optional PNG/BMP frames through `p4test.screenshot`
  — against committed baselines. The ELF is discovered under `test/host/build*`
  (no hardcoded path: `--renderer` overrides) and a POSIX ELF is routed through
  WSL on Windows. `--check` is the default, `--update` rebaselines after an
  intentional render change, `--self-test` is the red canary, and `--no-png` is
  text-only. Current result: 8 fixtures / 432 assertions OK. It opens no serial port and
  touches no board: a pass only says the shared render path is stable on the
  host, never that hardware is signed off (§0 still governs).
  `apps/webapp/WEBAPP.BAT` is the board-facing reference shim for the same
  `webapp` facade. The DOS-presentation slot adds visual assertions on those
  parsed rows (boxed frame rows, a status/footer line, no empty-box glyphs and
  no raw debug labels); its red canaries cover that check too.
- **Browser real-execution pre-gate (v1.6 visual cycle):** `tools/host_browser_gate.py`
  uses the same ELF but asserts what the renderer actually emitted for five
  fixtures: the ESP32 wiki page (H1, infobox, bright-cyan `Espressif Systems`
  link, `240 MHz`, both image outcomes) and the four DOS showcase pages.
  `--check` reports 271 assertions plus 11 labelled STATIC wiring checks;
  `--self-test` runs 9 red canaries. The wiki PNG is a first-25-rows host proof,
  not live HTTPS or the full 2000-row device page; touch/mouse/keyboard-on-glass
  is device-only. Board-free pre-gate only, never hardware sign-off.
- **Batch app pre-gate (also separate from Unity):** `tools/host_batch_run.py`
  runs a **real `.bat` fixture** (`tools/host/fixtures/batch/HELLO.BAT`) through
  the real interpreter via `test/host/main/host_batch_run.c` (built as
  `host_batch_run.elf` by `test/host/main/CMakeLists.txt`). A thin host-only
  execute-command router calls the firmware's own `shell_command_*` verbs with
  the real expander/tokenizer; it does not duplicate the language and does **not**
  run every verb — lines needing full `command.c` dispatch are reported as
  unsupported, never faked. `test/host/main/host_batch_bridge.c::host_batch_fs_init(root)`
  installs a host-only POSIX I/O backend through the additive
  `batch_io_ops_t.file_open` / `stat_path` hooks; the fixture root comes from
  `--root` or `P4_HOST_SD_ROOT` (default: the fixture directory) and a missing
  root is an honest failure, not a simulated card. `--fixture`, `--check`
  (default), `--update` and `--self-test` (7 canaries) are supported; the ELF is
  discovered under the repo build dirs and POSIX ELFs run through WSL on Windows.
  Verified: now **3 fixtures / 42 assertions** (the added `REGISTRY.BAT`
  fixture covers `reg`/`lib`/`try`/`finally`) plus 10 red canaries; the Unity
  host lane is 375/0/0. This is a pre-gate only — never hardware sign-off; the
  on-device suites remain authoritative (§0).
- **TUI render pre-gate (v1.6):** `tools/host_tui_gate.py` compiles the **one**
  cell renderer (`components/tui/tui_frame.h`), the one DOS palette and the one
  CSS colour parser with a plain C compiler, drives `tools/host/fixtures/tui/`
  scenes and compares normalised text, `NNN@start-end:fg` colour runs and the
  dirty rect against `baseline/`. `--check` (default), `--update` and
  `--self-test` (red canary) are supported. No second renderer; no board.
- **Modal state pre-gate (v1.6):** `tools/host_modal_gate.py` generates a C
  harness from `tools/host/fixtures/modal/transitions.json`, links the **real**
  `components/modal/modal_args.c`, and asserts `modal.check` lines, markers and
  check count for `focus`, `progress` and `sessions` (push/pop/suspend/resume/
  switch/find/focus/bounds). `--fixture`, `--cc`, `--check`, `--self-test`.
  The LVGL/FreeRTOS paint half stays on-target.
- **Gfx wiring pre-gate (v1.6):** `python tools/gfx_toolkit_test.py --host`
  checks that the AA/gradient/atlas/composite constants in
  `test/main/test_gfx.c` still match `tools/host/fixtures/gfx/expected.json`
  and that `tools/host/fixtures/gfx/GFXAA.BAT` still exercises every new verb.
  It is a contract check, not a pixel or hardware proof.
- **Reference-app host proof (v1.6):** `python tools/host_apps_gate.py --check`
  drives every enriched `.bat` app through the same real interpreter ELF used
  by `host_batch_run.py` (fixtures under `tools/host/fixtures/apps/`, baselines
  under `baseline/`). Current result: **34 apps (33 plus the flagship showcase),
  679 host assertions PASS, 45 hardware surfaces SKIP (gfx/TUI/modal/webapp/
  native, each with a named reason) and 7 red-canary checks**. A SKIP is honest — the fixture contains no
  unsupported verb and the gate rejects the host-lane "unsupported command"
  string. `--update` rebaselines only after an intentional transcript change;
  `--self-test` proves the gate can fail. This is a host proof, not hardware
  sign-off. The new `[M-RICH-*]` lines intentionally change app transcripts, so
  `tools/appdiff/baseline/*.transcript.txt` must be regenerated on a board
  before hardware transcript parity is green. The DOS-redesign pass also
  asserts per-app screen structure from the real fixture emission (panel/header
  counts, box-border rows and colour runs where available) and distinct
  structures across apps; hardware-dependent surfaces still SKIP honestly.
- **Native payload checker:** `tools/make_native_payload.py check|selftest`
  verifies a signed test envelope in `tools/host/fixtures/native/` with a
  throwaway key; it never maps or runs the bytes. The keys are test-only and
  never installed on a board (`SECURITY.md`).
- **App proofs:** `tools/host_apps_gate.py` drives all **34 reference apps** (33
  updated ones plus the flagship showcase) through the real interpreter (679
  host assertions PASS, 45 hardware-dependent surfaces honestly SKIP with a
  named reason, 7 self-test canaries); hardware surfaces are never faked. The app identity/uniqueness
  gates (`gallery_audit.py` + `gallery_audit_selftest.py`) remain static
  pre-gates; a host-generated app proof is labelled as such in `docs/apps.md`
  and `docs/assets/proof-index.html`. **The v1.6 `[M-RICH-*]` transcript lines
  change app output, so `tools/appdiff/baseline/*.transcript.txt` goldens must
  be rebaselined on a board before those hardware parity checks can be green.**
- **P4Sync bridge/CLI pre-gate (board-free, no firmware).** `tools/p4sync/`
  is a host-only agent CLI plus a JSON-lines bridge that drive the **same**
  harness (`p4test` sessions, `sdbridge` SDFX framing, `pim_sync`,
  `p4test.screenshot`); they add no serial implementation, no second framing
  or merge engine and no firmware change. `python tools/p4sync_gate.py
  --check` exercises the shipped CLI composition and the bridge as a real
  subprocess in `--mock` mode (51 checks: handshake/capability refusal,
  marker and CRC contracts, push/pull/plan/screenshot/full-image, stale
  manifest recovery), and `--self-test` runs 18 red canaries. Both are wired
  into `host_all_gates.py` (`p4sync`/`p4sync_canary`) and into the `host-unit`
  CI job; neither opens a port or needs a board. The Windows WPF GUI in
  `apps/p4sync_gui/` is only a JSON client of the bridge (9 pages, SDFX
  push/pull, PNG screenshot preview with SHA-256/size proof, PIM/full-image
  stale-manifest recovery, credentials in Windows Credential Manager); its
  `dotnet build` + console tests run in a separate `windows-latest` CI job
  with no NuGet packages and no board. Contract and commands:
  [`tools/p4sync/README.md`](tools/p4sync/README.md) and
  [`apps/p4sync_gui/README.md`](apps/p4sync_gui/README.md). This is a
  pre-gate only: a mock pass never signs off hardware (§0).
- **OS facade pre-gate (v1.6, board-free):** `python tools/host_os_gate.py
  --check` compiles the pure parse/format region of the shipped
  `components/command/os_commands.c` between the `OS_PURE_IMPL` markers with
  mock accessor snapshots, and checks 14 cases / 144 assertions over `os
  pm|drv|io|fs|disk|net|mem|sec|ver|pwr`, the `/b` machine rows and the explicit
  `unavailable` / ERRORLEVEL 0/1/2 contract. `--self-test` runs 4 red canaries.
  No serial port; a pass never signs off hardware (§0).
- **Power/sleep pre-gate (v1.6, board-free):** `python tools/host_power_gate.py
  --check` drives the shipped `shell_power_clamp_idle_seconds()` and wake-cause
  helpers across 4 board profiles (300 checks: stale deepsleep timer, wake-source
  leakage, honest warnings, `DISPLAY_TIMEOUT` 64-bit clamp before narrowing).
  `--self-test` runs 10 red canaries. No board; never hardware sign-off (§0).
- **Audio engine pre-gate (v1.6, board-free):** `python tools/host_audio_gate.py
  --check` verifies the deterministic offline PCM of the one Q16 mixer, ADSR,
  sequencer, SFX bus and ducking path (99 checks); `--self-test` runs 12 red
  canaries. It compiles the shipped `components/audio/audio_engine.c`; no codec
  hardware or serial port is touched, so a pass is never hardware sign-off (§0).
- **Video/EPUB/archive pre-gate (v1.6, board-free):**
  `python tools/host_video_gate.py --check` drives the one `imagefile` decoder,
  the one TUI sampler, the pure EPUB nav/TOC core and the one USTAR/CRC core
  (18 assertions); `--self-test` runs 10 red canaries. No board; never hardware
  sign-off (§0).
- **3D engine pre-gate (v1.6, board-free):**
  `python tools/host_engine3d_gate.py --check` runs 4 fixtures / 41 assertions
  over the Q16 math and the one `gfx_surface` raster path; `--self-test` runs
  10 red canaries. BMDS math ideas are re-expressed, no code copied (see
  `licence.md`). No panel or serial port; never hardware sign-off (§0).
- **Extending it:** add the source (or the hardware-free translation unit) to
  `test/host/main/CMakeLists.txt`, add the test file to
  `tools/host_test_manifest.json`, then re-run `tools/host_gen_runner.py` (it
  writes `test/host/main/host_main.c` and the expected case count).

---

## 3. Layer 1 — on-target unit tests (Unity)

Pure/headless logic runs on the P4 in `test/` (Unity; the static case count is
**809** in the current tree - the v1.4.3 browser-cache, grid-resolve and
interactive-`for` cases, the `trap` cases in `test/main/test_trap.c`, and the
v1.6 native-payload/loader, gfx toolkit, TUI frame/widget, modal, HTML/CSS/JS
and batch-expression cases - refresh the hardware count with `tools/unit_run.py`;
includes the software AES-256-GCM vectors in `test_sw_gcm.c`).

```powershell
cd test
idf.py -DP4_BOARD=jc1060p470c build flash   # or -p COM3
cd ..
python tools/unit_run.py COM3               # parse summaries, non-zero on any failure
python tools/unit_run.py COM3 COM6          # both boards concurrently
```

- The test app drives **no UI**: the screen stays black and only serial speaks.
  That is expected.
- **Reflash the main firmware afterwards** (`idf.py -DP4_BOARD=jc1060p470c
  -p COM3 flash`).
- What belongs here: pure parsers, classifiers, evaluators, formatters, and
  math (e.g. `test_gfind.c`, `test_screen.c`, `test_pkg.c`, `test_calc.c`).
  Anything touching LVGL, the SD card, or the display is hardware-verified
  instead (see the lower layers).
- The runner exits non-zero on any failure, a missing completion marker, or a
  panic. If the board reset-loops, suspect a new test calling an LVGL/heap path
  before `lv_init()`.

## 4. Layer 2 — host hardware suites (`tools/p4test/` + `tools/suites/`)

The primary hardware regression: one shared framework drives the board over
serial, one suite per area, a single PASS/FAIL table.

```powershell
python tools/p4test_run.py COM3                  # every suite
python tools/p4test_run.py COM3 --list           # list suite names
python tools/p4test_run.py COM3 --only s01_smoke,s07_data
python tools/p4test_run.py COM3 --tag core
python tools/p4test_run.py COM3 --quick          # shorten soak/boot waits
python tools/p4test_run.py --boards COM3,COM6 --tier fast --fail-fast
# gate a feature on both boards in parallel (minutes)
python tools/p4test_run.py --boards COM3,COM6    # full release gate, merged table
python tools/p4test_run.py --boards m5stack_tab5 --only s12_power_audio
# slugs work too (resolved via tools/board_ports.py)
```

Suites (`tools/suites/sNN_*.py`) and what they cover:

| Suite | Covers |
|-------|--------|
| `s01_smoke` | identity (`about`/`version`/`sysinfo`), help, memory, screenshot geometry |
| `s05_storage` | `dir`/`copy`/`xdel`, `chkdsk`, `disk detail`, labels, recycle bin |
| `s06_batch` | the whole batch language (loops, `if`, pipes, redirection, `screen`, `screen flow`) + task-manager verbs (`suspend`/`resume`/`nice`, `start /b`, `tmgr_*` events) + app registration (`assoc`/`ftype`/`open` dispatch, `startup`, `launch /setup`, `pkg` install gates/outdated/remove/purge, `pkg unpack`/`pkg scan`, `services=` gate) |
| `s07_data` | `db`, `csv`, `export`/`import`, `archive`, `crypt` (lock/unlock round-trip, passes on both boards since F23 fixed in v1.2.1), `json`, `markdown` (render + `export` html/print/text pulled and validated, incl. the on-device HTML reader via `type`), alarms, `gfind` (+ `/files`) |
| `s08_display` | `draw`/`tui`/`gfx`/`plot`/`font`/`theme`/header/cursor + screenshot invariants |
| `s09_editor` | the `edit` editor surfaces and save round-trips, the spell user dictionary (`\spell-add` learn, `spell list/forget`), and the autosave/`recover` crash round-trip |
| `s10_input_ui` | keyboard pages, `ui` synthetic touch/key, modals, HID mouse cursor/click toggles + status, hyperlinks (`link`/`ui links`/`ui link` tap-confirm) + serial cursor simulation (`usb mouse move`/`click`, `Device.cursor_move/cursor_click`) |
| `s11_chat` | the `chat` verb surface on one board (argument validation, `peer`/`onmsg` round-trip, no SoftAP); the two-board host↔peer exchange lives in the focused `tools/chat_test.py` driver |
| `s11_connectivity` | Wi-Fi, `ipconfig`/`netstat`/`ping`/`dns`, `httpd`/`httpget`, USB, BT, `c6ota status` |
| `s12_power_audio` | audio (`status` route/jack, `output` mode + `/b` + `/v:`, generated-WAV `wavplay`, `CONFIG AUDIO_OUTPUT` persist), `battery`, `power`, `gpio`/`pwm`/`adc`/`i2c`, SPI master (`status`/`peek`/`poke`/`loopback`, C6 stays alive), RGB LED (skipped if absent) |
| `s13_perf` | frame pacing (`gfx stats`/`tui stats`) and app animation |
| `s14_apps` | every reference app (launch, drive, markers), incl. the hybrid demo and the `taskmgr` task-manager app (kill/suspend/resume/nice through its own UI) |
| `s15_p4sync` | P4Sync handshake (`sync status` lines), file push/pull round-trip, screenshot geometry |
| `s16_netsvc` | event service (`net status/subs/msg/outbox`, hook set/clear, usage errors); broker-dependent live checks skip cleanly without a LAN broker (documented Mosquitto fixture) |
| `s17_browser` | `browser` render matrix over the board's own httpd (heading/paragraph/list/link/image/table + CSS colours), `tui dump` readback, back/forward, bookmark round-trip, `browser js`, no-connection path, Basic auth |
| `s18_http` | `httpget` flags (`/h:`, `/post`, `/stream` to transcript and SD, `/cookie` show/clear/persist); loopback after httpd is up; a missing AP is a note (`s11` gates Wi-Fi) |
| `s19_app_features` | the opt-in `APPLIB.BAT::probe` (`P4_APP_FEATURES=1`) across a nonstop app, a menu app and a demo; direct `js eval/run`; locale persistence through a real reboot |
| `s20_website` | the offline browser dashboard: deploys `apps/website/` (HTML + BMP logo), renders `sd:/WEB/INDEX.HTML`, runs the JS event hook, captures `browser-website.bmp` for the docs |
| `s21_wikipedia` | the browser acceptance gate: deploys a trimmed `en.wikipedia.org/wiki/ESP32` fixture (infobox, table, list, links, a PNG and a JPEG) and asserts the rendered structure, link colour, missing-image alt text and a BMP capture. `P4_WIFI_LIVE=1` adds a live HTTPS fetch (credentials from `tools/wifi_local.json`) |
| `s22_canvas` | the HTML5 subset gate: renders `apps/website/canvas.html` (CSS page/banner backgrounds + a 96x48 canvas drawn through the 2D context), asserts the canvas width/height reached JS, the bitmap replaced the fallback, the red/green/cyan colour runs and pixels, background pixels, no panic, and a BMP capture |
| `s23_apps_usb` | the drive-aware apps gate: installs the fixture and HYBRID under `usb:/APPS`, asserts `launch /list` discovers them, that `%APPDRIVE%`/`%APPROOT%`/`%APPNAME%` report the USB root, that the same app on SD reports `sd:`, and that HYBRID completes from USB. **Requires a mounted USB stick**; without one it reports an explicit skip (0 checks, no false green) so the rest of the full tier can run |
| `s24_render_alpha` | the v1.4.3 presentation gate: launches TCMD three times (opaque, opaque control, `alpha=40%`/`scrim=40%`), snapshots its TUI with the documented `p` snap key, and asserts the two opaque runs are pixel-identical while the translucent run blends the surface background across the screen without changing content or text ink; restores the installed `TCMD.APPINFO`; opens a real `dialog /t:3` with `scrim` set (host capture while the modal is up, no panic); asserts `draw text "😀"` stores the retro U+263A glyph by comparing panel snapshots against a literal U+263A page and a cleared cell. **Reboots first** (the transcript surface keeps stale LVGL background state across a session) |
| `s25_apps_gallery` | the docs gallery: drives every reference app to its main screen, asserts each capture's identity, ink and theme, and stages PNGs for `tools/publish_gallery.py`; the committed captures are re-checked by `tools/gallery_audit.py` with `tools/gallery_audit_selftest.py` as its red canary |
| `s26_portrait` | portrait geometry and runtime rotation on the Tab5: grid resolve (80 columns, extra rows), surface/modal fill, reflow through `rotate 90`/`rotate 0`, touch after rotation, and PSRAM stability over five rotate cycles |
| `s27_apps_layout` | pins app layout to the live grid: no hard-coded 80x25 draw geometry, ≥60 % panel fill in portrait, and the classic 80-column frame preserved in landscape |

The framework (`tools/p4test/`): `session.py` (DTR-safe open, marker-synced
`run()`, panic detection, byte-exact `BMPX`/`SDFX` framing, ANSI+OSC 8 strip
so hyperlink escapes never reach assertions; the sync marker is re-sent every
2 s so a slow command cannot swallow it), `device.py`
(boot/run/screenshot/push/pull facade + the existing destructive commands +
`cursor_move`/`cursor_click`/`ui_links`/`ui_link` harness verbs),
`asserts.py` (`Checklist`/`SuiteResult`), `screenshot.py` (streaming capture +
a pixel model), `sdbridge.py` (`receive`/`send`), `perf.py` (frame stats),
`visual.py` + `visual_sweep.py` (geometry/pixel invariants), `runner.py`.

### Tiers, failure policy, and flake discipline

- **Tiers:** `--tier fast` runs the per-area smoke subset (`s01_smoke`,
  `s05_storage`, `s06_batch`, `s12_power_audio`, `s11_connectivity`, with
  quick waits) and gates feature delivery; `--tier full` (default) runs every
  suite (see the table above) and gates releases. Explicit `--only`/`--tag`
  bypass the tier.
- **Fail-fast:** `--fail-fast` stops remaining suites at the first failure;
  with `--boards` the other board finishes its current suite, skips the rest
  (marked SKIP), and the merged table names the red board. `--keep-going`
  restores run-to-completion.
- **Skips:** a suite that returns `SuiteResult(skipped=N)` with no failures is
  reported `SKIP` and counted separately (`RESULT OK (n/m suites, k skipped)`),
  never as `PASS 0/1` and never as a failure. A suite that asserts nothing at
  all stays a failure (the false-green rule), and a skip mixed with a failure
  is a failure.
- **Flakes vs bugs:** `--retry N` re-runs a failed suite (never a panic); a
  pass-on-retry is reported FLAKY for quarantine, fail-twice is a bug.
- **Clean-boot areas:** the runner samples `mem.internal` and `mem.dma.free`
  before each suite. Memory-sensitive areas (`editor`, `input-ui`,
  `power-audio`, `apps`, `browser`, `http`) always start from a fresh boot;
  other suites reboot when internal drops below 12 KB or DMA below 9 KB. SD
  files persist across the reset. This prevents cumulative LVGL span/service
  state from starving httpd/I2S (F53) while still exercising each feature on
  a representative clean heap.
- **Slow checks:** `--baseline <json>` (see `--write-baseline`) flags suites
  running far over their recorded durations — a perf regression signal.
- **Differential:** the merged table flags any suite failing on exactly one
  board (board-specific regression detector).

### Writing a suite

A suite is a module under `tools/suites/` exposing `NAME`, optional `TAGS`, and
`run(dev, ctx)` returning a `Checklist`:

```python
from p4test.asserts import Checklist

NAME = "mymodule"
TAGS = ["core"]

def run(dev, ctx):
    c = Checklist(NAME)
    out = dev.run("mymodule status")
    c.expect("status line", "mymodule.ok", out)
    c.equals("count", dev.display_size()[0], 1024)   # on the reference board
    return c
```

- Use `dev.run(cmd, timeout=..., settle=...)`. `settle` is how long to wait
  before queueing the marker `echo`; **raise it** when the command opens a
  modal (a modal open when the marker arrives eats it as input and desyncs the
  run — the batch/screen suites use `settle=4..6` for 1 s-timeout dialogs).
- Never hardcode board-specific values: read geometry/board/RGB capability from
  `dev.display_size()` / `dev.board_id()` / `dev.rgb_available()` (parsed from
  `sysinfo`) so the suite runs on both boards.
- Deploy files with `dev.push_file(remote, bytes)` / `dev.push_local(local,
  remote)` (bare `.APPINFO`/`.ASSETS` default to `APPS/`); pull with
  `dev.pull_file(remote)`.
- Prefer machine-readable `/b` output for assertions.

## 5. Layer 3 — focused hardware drivers

Point tools for a specific subsystem; reuse them rather than re-deriving serial
logic (`tools/README.md` has the full list).

| Driver | Purpose |
|--------|---------|
| `tools/regression.py COM3 [--quick]` | one-command host regression: one reset, then the full p4test suite plus the unique-coverage drivers (companion deep/db/alarm, package deploy/install, large-editor soak, touch sweep, TX integrity, boot regression), PASS/FAIL table, non-zero on failure. Area drivers duplicated by the suites are not re-run (see the map in `tools/README.md`). |
| `tools/unit_run.py COM3 [COM6]` | Unity capture, one or two ports concurrently (needs the test image flashed) |
| `tools/boot_regression.py COM3 [N] [COM6]` | N fresh boots per board concurrently: no panic, exactly one AUTOEXEC, SD ready, no stray W/E logs |
| `tools/tx_stress_test.py COM3 [N]` | output integrity under TX backpressure |
| `tools/stall_catch.py COM3` | long serial soak / stall detector |
| `tools/wifi_bench.py` | host peer for the firmware `wifi throughput` command |
| `tools/ui_touch_test.py COM3 [--sweep]` | exhaustive synthetic touch: every OSK key, header, modals |
| `tools/pkg_test.py COM3` | `pkg` round-trip + the full ECDSA signing matrix (fresh keypair) |
| `tools/make_pkg.py`, `tools/font_subset.py`, `tools/lvgl_image_pack.py`, `tools/dev_mcp_server.py` | host-only package/asset/dev tooling (no hardware); see `tools/README.md` |
| `tools/appdiff.py COM3 [--update]` | reference-app transcript parity gate |
| `tools/dogfood.py COM3 --minutes 15` | autonomous curious-user drive; screenshots every action, journals anomalies |
| `tools/completion_test.py`, `editor_test.py`, `header_test.py`, `plot_test.py`, `theme_test.py` | focused subsystem drivers |

### Live-browser proof (hard gate)

`s21_wikipedia` must run with `P4_WIFI_LIVE=1`. It fetches the real HTTPS
Wikipedia page, requires a full render (a size-cap degradation is not
accepted as proof), and stages `screenshots/proof/wikipedia_live.png`,
`web.png` and `wikipedia_live.txt`. A skipped live leg fails the suite.
`tools/publish_proof.py` copies the proof into `docs/assets/proof/`. The
host-vs-live distinction and reproduction commands are also collected in
[`web.md`](web.md).

### Present-level diagnostics (what the glass shows)

`docs` screenshots and `screenshot <file>` re-render the LVGL object tree, so
they CANNOT see a present bug: a sprite invalidated away, a stale pixel left on
the panel, a tear. Two gfx presents shipped broken in this release while every
model screenshot looked right. Use the panel view:

- `screenshot /panel <file.bmp>` reads the MIPI-DSI frame buffer (the memory
  the panel is scanning). It is native-oriented (rotated on the Tab5) and is
  meant for analysis, not for docs.
- `s22_canvas` presents two colour-coded frames and asserts, from the panel
  captures, that each present landed and the second replaced the first (no
  stale sprite). That is the check that catches a broken dirty-rectangle
  present.
- Panic backtraces in a suite failure are symbolized automatically from
  `build/` (COM3) or `build-tab5/` (COM6) via `p4test/panic_symbols.py`.

### Visual guards (unstyled widgets)

A capture may never contain LVGL's default-theme primary (`#2196F3`): this UI
is the DOS/theme palette, and a sized blob of that blue means a widget was left
unstyled (the shell's input-row buttons and the on-screen keys were, which read
as a blue flash whenever the row reappeared). Three places enforce it:

- `tools/check_assets.py` - scans the committed `docs/assets/apps/*.png` and
  `shell-idle.png`; wired into `tools/harness/verify.py` as the `ui colours`
  gate.
- `tools/gallery_audit.py` - identity-by-exclusion (no shell header/prompt/keyboard
  chrome) plus near-duplicate detection over the committed gallery; wired into
  `tools/harness/verify.py` as the `gallery identity` gate.
- `tools/gallery_audit_selftest.py` - the red canary: synthesizes a shell dump
  and a duplicate and fails unless the audit rejects both (and accepts a clean
  set). A guard never seen failing is not a guard.
- `s25_apps_gallery` - one check per app capture (live, so a regression fails
  the suite before a PNG is published).
- `dogfood.py` - journals a HIGH `unstyled-widget` anomaly (with the colour,
  count and bbox) for any live frame; run it after UI changes.

## 6. Layer 4 — `tools/harness/` one-shots and static checks

- **Static checks (no hardware):** `tools/harness/verify.py`,
  `final_check.py`, `final_verify.py` assert the on-disk invariants (sandboxed
  paths, required symbols/dispatch arms, generated assets present). Run from
  the repo root.
- **Fixtures with placeholder credentials:** the `tools/harness/wifi_*.py`
  drivers need a reachable AP; set your own SSID/password before use.
- **Screenshot capture:** `tools/harness/grab_screenshot.py --port COM3 --out
  out.png [--crop-transcript]` (works while a modal/editor is open),
  `capture_tui.py` for TUI goldens, `tools/pull.py` for SD files.

## 7. Layer 5 — webcam diagnostics (visual failures)

Some failures leave no serial trace; these use a webcam and prefer observation
over guessing (`ai-context.md` §5):

- **`tools/screenshot_gate.py`** — the deterministic docs-screenshot gate (no
  camera): renders the curated states from `tools/capture_docs.py` and compares
  each frame with `docs/assets/<name>.png` pixel for pixel. `--update` refreshes
  the baselines and immediately proves them reproducible; a plain run compares
  against them and writes `screenshots/visual/<name>.diff.png` (differences in
  red). `tools/screenshot_masks.json` documents the two masked transient
  regions: the header's live telemetry and the LVGL caret. Run it after any
  change that can move a pixel (theme, font, layout, header, OSK, gfx).
- **`tools/display_glitch_watch.py`** — detects the MIPI-DSI "BSOD" blue
  flash by tracking `mean(blue) − mean(red)` over a panel ROI and saving peak
  frames. Flags: `--calibrate`, `--preview`, `--roi`, `--index`, `--device`,
  `--reset`, `--stress` (drives `display stress on/off`). System Python + cv2.
  Calibrate after moving the camera.
- **`tools/led_watch.py COM3 [--camera N]`** — locates the WS2812, builds an
  exposure-invariant colour table, and checks solids, effects, auto-status, the
  `httpd` transient pulse, and the boot flash. Use it whenever a change touches
  `components/led/`, `rgb`, Wi-Fi status colours, or the boot flash.

Visual-bug workflow: reproduce → capture (still or camera) → assert the
pixels/colour/geometry in a host driver → fix → re-capture and keep the proof.

---

## 8. Two-board workflow

Both boards enumerate with the same USB VID/PID, so use `tools/board_ports.py`
or pass ports explicitly. A full two-board gate:

```powershell
# Firmware + test images, one build dir per board:
idf.py -B build -DP4_BOARD=jc1060p470c build
idf.py -B build-tab5 -DP4_BOARD=m5stack_tab5 build
cd test; idf.py -B build -DP4_BOARD=jc1060p470c build; cd ..
cd test; idf.py -B build-tab5 -DP4_BOARD=m5stack_tab5 build; cd ..
# Flash each board's image to its port, then:

# One gate for both boards (parallel, fail-fast, merged table):
python tools/p4test_run.py --boards COM3,COM6 --tier fast --fail-fast
python tools/p4test_run.py --boards COM3,COM6 --tier full --fail-fast --retry 1
python tools/unit_run.py COM3 COM6
python tools/boot_regression.py COM3 5 COM6

# Either board alone — same commands with a single port
```

Board-specific suites skip what a board lacks (RGB LED, camera) via
`sysinfo`, so the same suites run on both. `crypt` is not skipped: s07 passes
93/93 on COM3 and full-pass on COM6 (bugs.md F23 fixed in v1.2.1).
`sdkconfig.<board>` is generated per
profile and git-ignored; switching `-DP4_BOARD` in a reused build dir retargets
it automatically.

- **Always pass `-DP4_BOARD=<board>` on every `idf.py` invocation.** The CMake
  cache value defaults to `jc1060p470c` (root `CMakeLists.txt`), so a deleted
  or `fullclean`-ed build dir silently reconfigures to the reference board; the
  result compiles with zero warnings and flashes fine but is the wrong image
  (on the Tab5 it asserts in `esp32_p4_function_ev_board.c` and reset-loops).
  Use one build dir per board (`build/` for `jc1060p470c`, `build-tab5/` for
  `m5stack_tab5`) with `-B`, and confirm the staged config afterwards via
  `Select-String build-tab5\config\board_config.h -Pattern "BOARD_CFG_NAME|BOARD_CFG_BSP_M5STACK_TAB5"`.

---

## 9. Interpreting failures

- **A panic/assert aborts the suite immediately.** The runner records the
  backtrace; treat it as a bug, not a flake.
- **Known flakes** (treat one occurrence as non-reproducible until it repeats;
  see `bugs.md`): `tcpterm` occasionally missing a `RESULT OK` in a long
  sweep (`crypt` was flaky before bugs.md F23 was fixed in v1.2.1 and now
  passes); `s10_input_ui`'s ghost-completion check could corrupt its own prefix
  to `coo` when an OSK key event arrived late, leaving no ghost hint (fixed in
  this round — keys are now confirmed by observing the input line grow, and the
  cold-cache fallback uses the OSK enter key rather than Ctrl+C, which does not
  clear OSK-typed text); the Tab5's marginal C6 SDIO link can produce hosted-RPC
  timeouts under load (`bugs.md` F6).
- **A modal eating the marker** shows as `run(...): marker ... not seen`; raise
  `settle` (the command was still legitimately running). This is a harness
  timing issue, not a firmware bug.
- **Screenshot `could not acquire LVGL lock`** is transient; the capture helper
  retries it.
- **Never screenshot a batch app that owns the input with unwind enabled.**
  `p4test/screenshot.py::capture()` retries a missing `streaming` prelude with
  Ctrl-C by default (`unwind=True`), which aborts a batch at a line modal
  (`ask`) and kills the app under test (s14 ADVENT/TCMD, `bugs.md` F40).
  Suites that shoot foreground apps must pass `unwind=False`: the shot becomes
  a noted miss and the app survives. Shell-owned modals (dogfood, visual
  sweep) keep the default.
- The unit app's **black screen** is expected; the display suites require the
  **main** firmware to be flashed.

## 10. Adding coverage for a feature

0. **Run the static gates first** (`python tools/green_guard.py`,
   `python tools/comment_index.py`, `python tools/breadcrumbs.py`) and fix
   anything they report. A new test must assert; a new suite must record at
   least one check.
1. **Pure logic** — a `test/main/test_*.c` case + `RUN_TEST` in `test_main.c`,
   then classify the file in `tools/host_test_manifest.json`: either add it to
   the host lane (when the component's `REQUIRES` chain reaches no board-only
   component) or list it under `excluded` **with a reason**. An interpreter-level
   test whose source pulls board-only symbols goes in the host lane through
   `test/host/main/host_batch_bridge.c` (host-only, honest "unavailable" stubs),
   not by stubbing hardware in production code. `green_guard` and
   `host_unit_run.py --static` both fail on an unclassified file, because a
   test that runs in neither lane is a false green.
2. **Behaviour on hardware** → extend the matching `tools/suites/sNN_*.py`; add
   a focused driver only if it needs a bespoke loop.
3. **Cross-app behaviour** → the `s14_apps` markers and, if a reference app's
   transcript changes, re-baseline `tools/appdiff.py --update`. **Open for
   v1.6.0:** the 33 apps now emit `[M-RICH-*]` lines, so every
   `tools/appdiff/baseline/*.transcript.txt` golden needs a board rebaseline;
   do not report hardware transcript parity as green until that is done.
4. **Boot/serial/visual concerns** → `boot_regression.py`,
   `tx_stress_test.py`, or the webcam watchers.

Register any new host suite automatically by dropping `sNN_*.py` in
`tools/suites/` with the `NAME`/`TAGS`/`run` contract; the runner discovers it.
Keep new serial logic out of bespoke drivers — import `tools/p4test/`.


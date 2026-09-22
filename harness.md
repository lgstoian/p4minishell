# P4MiniShell Testing Harness

> The complete guide to testing this firmware on real hardware. It covers the
> four test layers, how to run and extend them, the two-board workflow, and the
> webcam diagnostics. The companion implementation reference is
> [`tools/README.md`](tools/README.md) (every driver) and
> [`test/README.md`](test/README.md) (the Unity layout). Change and verification
> rules live in [`ai-context.md`](ai-context.md); open bugs in
> [`bugs.md`](bugs.md).

- **Version:** v1.2.1 · **Targets:** ESP32-P4 + ESP32-C6
- **Boards:** `jc1060p470c` (reference, e.g. COM3) and `m5stack_tab5` (COM6)

---

## 0. The one rule

Every change is finished only when: **firmware and `test/` build with zero
errors and zero warnings**, the on-board **unit suite is green** on the
affected board(s), and the **affected hardware behaviour is verified** on the
board. After flashing the test app, **reflash the main firmware**.
(`ai-context.md` → Hard Rules.)

---

## 1. Prerequisites and environment

```powershell
# 1. Source the matching ESP-IDF in every new shell (v5.5.5)
$env:IDF_PATH = "C:\esp\v5.5.5\esp-idf"      # adjust to your install
. $env:IDF_PATH\export.ps1

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

## 3. Layer 1 — on-target unit tests (Unity)

Pure/headless logic runs on the P4 in `test/` (Unity, ~423 tests, 2 ignored;
includes the software AES-256-GCM vectors in `test_sw_gcm.c`).

```powershell
cd test
idf.py -DP4_BOARD=jc1060p470c build flash   # or -p COM3
cd ..
python tools/unit_run.py COM3               # parse summaries, non-zero on any failure
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
```

Suites (`tools/suites/sNN_*.py`) and what they cover:

| Suite | Covers |
|-------|--------|
| `s01_smoke` | identity (`about`/`version`/`sysinfo`), help, memory, screenshot geometry |
| `s05_storage` | `dir`/`copy`/`xdel`, `chkdsk`, `disk detail`, labels, recycle bin |
| `s06_batch` | the whole batch language (loops, `if`, pipes, redirection, `screen`, `screen flow`) |
| `s07_data` | `db`, `csv`, `export`/`import`, `archive`, `crypt` (lock/unlock round-trip, passes on both boards since F23 fixed in v1.2.1), `json`, `markdown`, alarms, `gfind` (+ `/files`) |
| `s08_display` | `draw`/`tui`/`gfx`/`plot`/`font`/`theme`/header/cursor + screenshot invariants |
| `s09_editor` | the `edit` editor surfaces and save round-trips |
| `s10_input_ui` | keyboard pages, `ui` synthetic touch/key, modals |
| `s11_connectivity` | Wi-Fi, `ipconfig`/`netstat`/`ping`/`dns`, `httpd`/`httpget`, USB, BT, `c6ota status` |
| `s12_power_audio` | audio, `battery`, `power`, `gpio`/`pwm`/`adc`/`i2c`, RGB LED (skipped if absent) |
| `s13_perf` | frame pacing (`gfx stats`/`tui stats`) and app animation |
| `s14_apps` | every reference app (launch, drive, markers), incl. the hybrid demo |

The framework (`tools/p4test/`): `session.py` (DTR-safe open, marker-synced
`run()`, panic detection, byte-exact `BMPX`/`SDFX` framing), `device.py`
(boot/run/screenshot/push/pull facade + the existing destructive commands),
`asserts.py` (`Checklist`/`SuiteResult`), `screenshot.py` (streaming capture +
a pixel model), `sdbridge.py` (`receive`/`send`), `perf.py` (frame stats),
`visual.py` + `visual_sweep.py` (geometry/pixel invariants), `runner.py`.

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
| `tools/regression.py COM3 [--quick]` | one-command host regression: resets and runs every guard, PASS/FAIL table, non-zero on failure |
| `tools/unit_run.py COM3` | Unity capture (needs the test image flashed) |
| `tools/boot_regression.py COM3 [N]` | N fresh boots: no panic, exactly one AUTOEXEC, SD ready, no stray W/E logs |
| `tools/tx_stress_test.py COM3 [N]` | output integrity under TX backpressure |
| `tools/stall_catch.py COM3` | long serial soak / stall detector |
| `tools/wifi_bench.py` | host peer for the firmware `wifi throughput` command |
| `tools/ui_touch_test.py COM3 [--sweep]` | exhaustive synthetic touch: every OSK key, header, modals |
| `tools/pkg_test.py COM3` | `pkg` round-trip + the full ECDSA signing matrix (fresh keypair) |
| `tools/appdiff.py COM3 [--update]` | reference-app transcript parity gate |
| `tools/dogfood.py COM3 --minutes 15` | autonomous curious-user drive; screenshots every action, journals anomalies |
| `tools/completion_test.py`, `editor_test.py`, `header_test.py`, `plot_test.py`, `theme_test.py` | focused subsystem drivers |

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
# Reference board (COM3)
idf.py -DP4_BOARD=jc1060p470c build
cd test; idf.py -DP4_BOARD=jc1060p470c build; cd ..
cd test; idf.py -DP4_BOARD=jc1060p470c -p COM3 flash; cd ..
python tools/unit_run.py COM3
idf.py -DP4_BOARD=jc1060p470c -p COM3 flash
python tools/p4test_run.py COM3

# M5Stack Tab5 (COM6) - same commands with -DP4_BOARD=m5stack_tab5
```

Board-specific suites skip what a board lacks (RGB LED, camera) via
`sysinfo`, so the same suites run on both. `crypt` is not skipped: s07 passes
93/93 on COM3 and full-pass on COM6 (bugs.md F23 fixed in v1.2.1).
`sdkconfig.<board>` is generated per
profile and git-ignored; switching `-DP4_BOARD` in a reused build dir retargets
it automatically.

---

## 9. Interpreting failures

- **A panic/assert aborts the suite immediately.** The runner records the
  backtrace; treat it as a bug, not a flake.
- **Known flakes** (treat one occurrence as non-reproducible until it repeats;
  see `bugs.md`): `tcpterm` occasionally missing a `RESULT OK` in a long
  sweep (`crypt` was flaky before bugs.md F23 was fixed in v1.2.1 and now
  passes); the Tab5's marginal C6 SDIO link can produce hosted-RPC timeouts under
  load (`bugs.md` F6).
- **A modal eating the marker** shows as `run(...): marker ... not seen`; raise
  `settle` (the command was still legitimately running). This is a harness
  timing issue, not a firmware bug.
- **Screenshot `could not acquire LVGL lock`** is transient; the capture helper
  retries it.
- The unit app's **black screen** is expected; the display suites require the
  **main** firmware to be flashed.

## 10. Adding coverage for a feature

1. **Pure logic** → a `test/main/test_*.c` case + `RUN_TEST` in `test_main.c`.
2. **Behaviour on hardware** → extend the matching `tools/suites/sNN_*.py`; add
   a focused driver only if it needs a bespoke loop.
3. **Cross-app behaviour** → the `s14_apps` markers and, if a reference app's
   transcript changes, re-baseline `tools/appdiff.py --update`.
4. **Boot/serial/visual concerns** → `boot_regression.py`,
   `tx_stress_test.py`, or the webcam watchers.

Register any new host suite automatically by dropping `sNN_*.py` in
`tools/suites/` with the `NAME`/`TAGS`/`run` contract; the runner discovers it.
Keep new serial logic out of bespoke drivers — import `tools/p4test/`.

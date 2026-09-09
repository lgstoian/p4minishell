# tools/

Host-side serial/screenshot/SD helpers (115200 baud). New scripts must reuse
the shared session driver instead of reimplementing serial loops.

## Port selection (all drivers)

First match wins: explicit `port=` argument, a trailing `COMx` argv token, the
`P4_PORT` environment variable, else `COM11`. Scripts with argparse take
`--port` (same fallback chain). Opening the port asserts DTR and the P4
resets on the transition — every driver drops DTR/RTS on open via
`shell_session.open_port()`; never open raw `serial.Serial` without it.

## Files

- `shell_session.py` — shared USB-Serial/JTAG session driver: `Shell`
  (prompt regex, panic-marker raise), `open_port()` (DTR-safe open),
  `default_port()` (port resolution). Import it, do not reimplement.
- `baseline_sweep.py`, `serial_sweep.py`, `serial_sweep2.py` — canned
  bring-up sweeps (versions, TUI/draw, modals, audio, periph, SD cycle).
- `bsod_watch.py` — timestamped long serial capture for the recurrent
  display-blink hunt (see `bugs.md` O3). One holder per port.
- `harness/` — one-shot host drivers, all on `shell_session` port handling:
  - Wi-Fi: `wifi_connect.py`, `wifi_connect2.py`, `wifi_connect3.py`,
    `wifi_test.py`, `test_wifi.py`, `test_wifi_cycles.py`, `wifi_dump.py`,
    `wifi_dump2.py`, `httpd_start_test.py` (all need a reachable AP; the
    `4G-CPE_5542` credential inside is lab-only).
  - Screenshots: `grab_screenshot.py` (`--port/--out/--crop-transcript`,
    BMPX binary protocol), `capture_tui.py` (`--port`, TUI regression +
    goldens), `get_screenshot.py`, `save_to_sd.py` (legacy helpers).
  - Static tree checks (no hardware): `verify.py`, `final_check.py`,
    `final_verify.py` — run from the repo root.
- `apps/companion/` drivers (not here, same conventions): `deep_test.py`
  (reactive marker-driven suite, 8/8 on hardware — the reference suite),
  `db_test.py`, `alarm_test.py`, `push_sd.py`, `run_companion.py` (retired
  stub forwarding to `deep_test.py`; the pre-TUI triggers it asserted can no
  longer occur).
- `managed_patches.patch` — backup of the working-tree patches inside
  `managed_components/` (currently: `unscii_16` comma fixes; the BSP
  graceful-degrade is committed in history).
  Re-apply after `idf.py update-dependencies` with
  `powershell -File tools/reapply_managed_patches.ps1`
  (`-Check` for a dry run). Regenerate after any managed edit with the
  `git diff -- managed_components` byte-pipe in `bugs.md` M40. See `bugs.md`
  M20/M28 and `changelog.md` 0.35.1/0.35.2.

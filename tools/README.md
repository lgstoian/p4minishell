# tools/

- `harness/` — host-side serial/screenshot/SD helpers (115200 baud, COM11 default):
  `grab_screenshot.py`, `capture_tui.py`, `get_screenshot.py`, `save_to_sd.py`,
  `httpd_start_test.py`, `test_wifi*.py`, `wifi_*.py`, `verify.py`,
  `final_check.py`, `final_verify.py`.
- `managed_patches.patch` — backup of the working-tree patches inside
  `managed_components/` (currently: `unscii_16` comma fixes; the BSP
  graceful-degrade is committed in history).
  Re-apply after `idf.py update-dependencies` with
  `powershell -File tools/reapply_managed_patches.ps1`
  (`-Check` for a dry run). Regenerate after any managed edit with the
  `git diff -- managed_components` byte-pipe in `bugs.md` M40. See `bugs.md`
  M20/M28 and `changelog.md` 0.35.1/0.35.2.
- `shell_session.py` — minimal USB-Serial-JTAG session driver (prompt regex,
  panic-marker raise). Import it, do not reimplement serial loops.
- `baseline_sweep.py`, `serial_sweep.py`, `serial_sweep2.py` — canned
  bring-up sweeps (versions, TUI/draw, modals, audio, periph, SD cycle).
- `bsod_watch.py` — timestamped long serial capture for the recurrent
  display-blink hunt (see `bugs.md` OPEN). One holder per port: check for
  running instances before opening COM11.

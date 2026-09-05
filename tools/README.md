# tools/

- `harness/` — host-side serial/screenshot/SD helpers (115200 baud, COM11 default):
  `grab_screenshot.py`, `capture_tui.py`, `get_screenshot.py`, `save_to_sd.py`,
  `httpd_start_test.py`, `test_wifi*.py`, `wifi_*.py`, `verify.py`,
  `final_check.py`, `final_verify.py`.
- `managed_patches.patch` — backup of the working-tree patches inside
  `managed_components/` (BSP graceful-degrade + `unscii_16` 384-glyph font).
  Re-apply after `idf.py update-dependencies` with
  `powershell -File tools/reapply_managed_patches.ps1`
  (`-Check` for a dry run). See `bugs.md` M20/M28 and
  `changelog.md` 0.35.1/0.35.2.

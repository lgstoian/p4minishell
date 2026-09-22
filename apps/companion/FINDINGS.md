# P4 Companion — findings log

The Companion (`apps/companion/`) is the largest pure-batch app in the tree. It
exists to exercise the batch engine, the modal surfaces, background jobs, and
the persistent-state primitives, and to surface bugs and missing features.

This file records what on-board testing of the Companion discovers. Fixed
findings are removed when a new test campaign starts (their history lives in
[`../../changelog.md`](../../changelog.md) and [`../../bugs.md`](../../bugs.md)).
Append new findings in the same shape as a firmware bug report.

## Current status

- Driver: `apps/companion/deep_test.py` (reactive marker-driven suite).
- Firmware: v1.2.1 · Boards: `jc1060p470c` (COM3), `m5stack_tab5` (COM6) · Date: 2026-09-21.
- Scope: Companion only (dogfood/visual runs are reported separately, not here).
- Modules exercised: menu, Live System dashboard, files/notes, network, fun,
  settings (persistence across reboot), shared-library routines, background
  jobs (`SVC.BAT`/`AGENDA.BAT`).
- Runs against the current firmware; see [`../../test/README.md`](../../test/README.md)
  for the verified baseline.

## Findings

*(none open)*

### Template

```
### <n>. <one-line summary>

- **Type:** firmware bug | app bug | missing feature
- **Where:** <module / command / file>
- **Symptom:** what the app shows
- **Repro:** minimal steps
- **Root cause:** (once known)
- **Fix:** (once fixed)
- **Verified:** the hardware check that proves it
```

## Behaviours that are by design

- **Modal output is silent on serial by design.** `dialog`/`list`/`ask` draw on
  the screen only; the reactive menu walk keys on the `[C-MENU]` serial echo
  marker (`deep_test.py:299,310`, exit on `[C-EXIT]`) rather than screen text.
- **`list` serial selection is 1-based** (the number you type) while its
  ERRORLEVEL is the 0-based index; `q` cancels with 255.
- **`set /a` prints its result**, so loop counters emit one line per iteration;
  the file manager uses `draw list /count:VAR /countonly` instead.
- **No-SD / offline runs degrade gracefully** and say so rather than failing.

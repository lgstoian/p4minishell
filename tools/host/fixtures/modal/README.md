# Modal host fixtures

`transitions.json` is the hardware-free modal state fixture consumed by
`tools/host_modal_gate.py`.

The gate does **not** re-implement the state machine in Python. It generates a
small C harness from this JSON, compiles it together with the **real**
`components/modal/modal_args.c` (the same pure translation unit the firmware
surfaces call), runs it natively and checks the emitted `modal.check` lines,
markers and check count. A pass therefore says something about shipped code,
not about a host-only copy.

Covered transitions:

* `focus` — HID key focus movement, clamping, page steps, ignored keys.
* `progress` — percentage math including zero total and large values.
* `sessions` — push/pop, suspend/resume, switch/rotation, find, focus, bounds.

The LVGL/FreeRTOS half (panel paint, event groups, screen save/restore) is
**not** exercised here: that needs the on-target suite. This is a pre-gate,
never hardware sign-off (see `harness.md` §0).

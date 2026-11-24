<!--
SPDX-FileCopyrightText: 2026 Stoian Alexandru
SPDX-License-Identifier: MIT
-->

# gfx toolkit host fixtures

Board-free evidence for the additive gfx toolkit extension. Nothing here runs
on a board: this directory holds two kinds of host-only material.

* `GFXAA.BAT` is a **board-facing reference script** for the new verbs
  (`lineaa`, `circleaa`, `rectaa`, `gradient`, `composite`, and `blitmany
  /a:`). It is syntactically identical to the shipped `.BAT` apps and prints
  `M-GFXAA*` markers, so a hardware suite can drive it later. This fixture only
  checks that the script still exercises every new verb; it never claims a
  hardware capture.
* `expected.json` records the exact RGB565 constants the pure Unity cases in
  `test/main/test_gfx.c` assert. `tools/gfx_toolkit_test.py --host` reads it and
  checks that the C cases still contain those values, so a host baseline and
  the compiled tests cannot drift apart.

The real pixel assertions run in the host Unity lane
(`python tools/host_unit_run.py`); `--host` here is a wiring/contract pre-gate,
not hardware sign-off (harness.md §0).

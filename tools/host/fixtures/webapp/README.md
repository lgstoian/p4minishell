<!-- SPDX-FileCopyrightText: 2026 Stoian Alexandru -->
<!-- SPDX-License-Identifier: MIT -->

# Webapp render fixtures (host pre-gate)

`tools/host_webapp_gate.py` renders every `*.html` file in this directory
through the firmware's **existing** HTML engine (`components/markdown`:
`dom_parse`, the CSS ruleset/`css_apply` pass and `dom_render_tree`) using the
standalone harness `tools/host/main/host_webapp_render.c`. There is no second
renderer, parser, stylesheet engine or colour model here.

**This is a hardware-free pre-gate only. A pass here is never hardware
sign-off** (`harness.md` §2.2 / ADR-12 still govern). It checks that the shared
render path produces stable text, colour runs and (optionally) pixels on a host
build. Board behaviour remains the on-target unit suite and `p4test`.

## Fixture format

- One self-contained `*.html` file per case: no external URLs, no network,
  no clock, no randomness, no SD/USB references.
- Put page rules in a `<style>` block and/or `style="..."` attributes so the CSS
  pass is exercised. Use `color:` (not only `background-color:`) to produce
  foreground colour runs that `tui dump /a`-style output can assert.
- Keep content inside the 80x25 logical page. Headings, paragraphs, a local link,
  a CSS colour and a `<canvas>` block with fallback content are the minimum
  shape exercised by `hello.html`.
- `<script>` and `onload` are documentation for the browser path. The host
  harness does **not** run QuickJS: it probes the pure `js_canvas` rasterizer
  through its public API and lets the DOM renderer draw the canvas fallback
  child, exactly as a no-script surface does.
- The harness emits a completion marker pair (`webapp.render.begin ...` /
  `webapp.render.end ...`) plus `tui.dump cols=...`, per-row `NNN|text` and
  `NNN@start-end:fg` lines, and `tui.dump.end`. A missing, stale or
  count-disagreeing marker is a hard failure, as is a dump with no rows/runs.

## Running the gate

```powershell
# text + optional PNG comparison (default)
python tools/host_webapp_gate.py --check
python tools/host_webapp_gate.py --self-test      # red canary: wrong baseline must fail
python tools/host_webapp_gate.py --update         # regenerate baselines after an intentional render change
python tools/host_webapp_gate.py --no-png         # text only
```

The gate invokes `tools/host/build-win/host_webapp_render.elf` (override with
`--renderer`). That executable is built separately - CMake wiring is not part
of these fixtures - and the gate fails clearly when it is absent. It takes no
serial port and touches no device.

## Baselines

`baseline/<fixture-stem>.txt` is the committed, exact normalised emission:
a `# webapp-baseline-v1 fixture=<name>` header followed by the marker and dump
lines. The header is a staleness guard: a changed tag or fixture name fails the
gate. `--update` rewrites these files from live renderer output - review the
diff before committing it, since a wrong baseline baked by `--update` is a
false green.

A harness may optionally write a PNG (or BMP) frame and announce it with
`webapp.frame path=<file>`. `--update` copies that frame into `baseline/`;
`--check` compares it with the ONE pixel model in `p4test.screenshot`
(imported, never copied), rejects a size/pixel diff, requires visible ink and
uses `p4test.visual` / `p4test.theme_guard` for analytics. PNG comparison needs
`pyserial`, `numpy` and `Pillow` on the host; text-only mode (`--no-png`) does
not. Frames are optional: `hello.html` exercises the text/colour-run path.

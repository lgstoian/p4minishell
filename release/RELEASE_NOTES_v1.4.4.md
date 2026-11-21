# P4MiniShell v1.4.4 — release notes

**Date:** 2026-11-21
**Boards:** `jc1060p470c` (reference, COM3) and `m5stack_tab5` (COM6)
**Firmware version:** `P4_CONFIG_VERSION_*` = 1.4.4

This is a correction release. v1.4.3 shipped documentation and a verification
harness that reported green while the published app gallery actually contained
shell screenshots, and it promised a live Wikipedia render that its gate never
captured. v1.4.4 fixes the gates, delivers the missing proof, and corrects the
docs. The grouped change history is in [`changelog.md`](../changelog.md); the
gate itself is in [`release.md`](../release.md) and [`harness.md`](../harness.md).

---

## Fixed

- **False greens are no longer possible in the gallery gate.** App identity
  markers (start/step/done) are asserted with `c.check()`, captures are staged
  away from `docs/`, and a retry-pass is reported FLAKY instead of green. The
  committed PNGs are re-checked by `tools/gallery_audit.py` (shell-chrome
  identity exclusion + near-duplicate detection), wired into
  `tools/harness/verify.py` as the `gallery identity` gate. Its red canary,
  `tools/gallery_audit_selftest.py`, synthesizes a shell dump and a duplicate
  and requires the guard to reject both — the gate has been seen going red.

- **The promised live Wikipedia proof now exists.** `s21_wikipedia` fails when
  the live leg is skipped, requires a full render (no size-cap degradation),
  and captures the panel framebuffer through the chunked path (the in-memory
  BMP OOMs on a large page). It stages `screenshots/proof/wikipedia_live.png`,
  `web.png` and `wikipedia_live.txt`; the dump shows the real URL and rendered
  article text. `tools/publish_proof.py` is the only publish path into
  `docs/assets/proof/`.

- **Snake and GFXTOOL render correctly on the live grid.** SNAKE's playfield
  follows the live TUI rows with portrait centring and a length-limited body
  (the old fixed 44x22 logic left the frame in the top-right corner and drew a
  length-3 snake as a 13-cell streak). GFXTOOL now draws text after its sprite
  stamps, so labels are no longer covered.

- **SoftAP documentation matches the build.** The docs no longer claim a
  client SoftAP path that `sdkconfig` does not compile in; Wi-Fi setup is
  described against the actual APSTA/portal configuration.

- **A flapping saved AP no longer floods the console.** The Wi-Fi auto-retry
  budget now spans watchdogs (six attempts, then one "gave up" line), and
  `wifi status` no longer runs a blocking C6 AP scan on every request (one scan
  per minute).

- **TUI apps restore the shell chrome on exit.** Leaving a TUI app always
  restores the shell header and input row; the previous stale surface stayed
  behind after the app closed.

- **MCP client version tracks firmware.** The reported MCP version is derived
  from the firmware version macro instead of a stale literal.

- **Documentation corrections.** First-public-release history, stale test and
  asset counts, obsolete "unsupported commands", stale DIRAM figures, broken
  links, the gallery capture workflow, and the deferred-hardware list (the
  `sync`/`net MQTT`/DOS-parity rounds were hardware-verified in v1.3.0 and are
  no longer called deferred). The changelog is newest-first again, with the
  misplaced 1.3.1 and 1.1.0 sections restored to order and the fake
  `Unreleased` headings dated or folded into historical addenda.

---

## Verification (current tree)

| Gate | Result |
|------|--------|
| `python tools/green_guard.py` | **758** Unity tests scanned, **0 errors / 0 warnings** (re-run for these notes) |
| On-board Unity (`unit_run.py COM3 COM6`) | **699 passed / 0 failed / 2 ignored** on both boards |
| Host unit lane (`host_unit_run.py`) | **285 / 285** |
| `comment_index`, `breadcrumbs`, `tools/harness/verify.py` | pass, including `gallery identity` and its canary |
| `s21_wikipedia` live leg (COM6) | **17 / 17**, full render, proof staged and published |
| Gallery | 22 visual app captures plus the separately published live-proof `web.png` |

F27 is resolved in v1.3.0; the three consecutive clean full sweeps on both
boards and the dogfood soak are recorded in [`bugs.md`](../bugs.md). Firmware
and `test/` build with zero errors and zero warnings on both board profiles.

---

## Known limits

- **Tear-free partial presentation at vsync is STILL OPEN.** The one-frame tear
  at a direction change has *not* been fixed in this release image. A
  back-buffer dirty blit at vsync is being implemented in the working tree
  (managed LVGL port hook + `gfx_commands` present path + an `s22_canvas` panel
  invariant); at the time these notes were written it was uncommitted and had
  not passed the on-panel present check on either board. Treat tear-free
  presentation as **not delivered** until that work lands and is verified.
  Panel-level diagnostics are described in [`harness.md`](../harness.md)
  ("Present-level diagnostics"); model screenshots cannot see this defect.

- **Deferred-hardware features remain build-verified only:** captive portal,
  the TLS `CERTS` store, the BLE HID host, serial PIM sync, and record editing.
  The v1.3.0 bring-up round exercised focused paths (portal timeout/TX-cap,
  CERTS PEM/DER activation, BLE mouse) but none has a full on-device acceptance
  gate. They are labelled experimental, not verified — see
  [`release.md`](../release.md) §0.

- **Two live network legs are bench-blocked:** the MQTT-over-TLS handshake and
  the HTTP CardDAV server end-to-end test need a LAN bench with a TLS broker and
  a CardDAV host. Local tooling (`amqtt`, `tools/radicale_fixture.py`) is ready;
  the refusal paths and verb wiring are verified.

- The gallery is captured at the 1024x600 reference panel size; a Tab5 run must
  not overwrite the committed set.

# P4MiniShell — Release Guide

The gate to clear **before** a public release, end to end: version freeze,
builds, the verification ladder, artifact packaging (firmware images, SD-card
content, source zip), legal/hygiene, the docs pass, and a sign-off checklist.

This is a checklist, not a second test manual. The mechanics of each step live
in their owning docs and are linked, not repeated:

| Topic | Owning document |
|-------|-----------------|
| Test harness, suites, diagnostics | [`harness.md`](harness.md) |
| Unit-test layout + counts | [`test/README.md`](test/README.md) |
| Hardware bring-up per board | [`PORTING.md`](PORTING.md), [`schematics.md`](schematics.md) |
| Command syntax | [`command.md`](command.md) |
| Change/verification workflow | [`ai-context.md`](ai-context.md) |
| Open defects | [`bugs.md`](bugs.md) |

Current target version: **v1.6.0** (`P4_CONFIG_VERSION_*` in
`p4minishell_config.h`). This is a **board-free release candidate**: the
hardware rows in §2-§3 and the on-board boxes in §8 are **not** signed off for
this version; the host/static gates are pre-flight only. Substitute your
release version throughout.

---

## 0. Release blockers (clear these first)

A public release must not ship with an open HIGH-severity defect. As of the
last campaign:

- **F27 (HIGH, resolved in v1.3.0)** — a transient Tab5 crash under long mixed `p4test`
  sweeps (Guru/WDT/silent reboot), unreproducible in targeted re-runs. **Required
  to close:** three consecutive clean full `p4test_run <COMx>` sweeps on both
  boards plus a `dogfood` soak, recorded in `bugs.md`. See
  [`bugs.md`](bugs.md) F27 for the current evidence and next experiment.
- **Deferred-hardware items** — features whose on-device acceptance is still
  open in [`changelog.md`](changelog.md): captive portal, TLS `CERTS` store,
  BLE HID host, serial PIM sync, and record editing. The v1.3.0 bring-up round
  exercised some focused paths (portal timeout/TX-cap, CERTS PEM/DER activation,
  BLE mouse), but none has a full acceptance gate; verify them on hardware
  before release or label them **experimental** in the release notes. Do not
  present them as verified. The `sync` USB handshake, the `net` MQTT event
  service with SD outbox, and the batch/redirection DOS-parity rounds were
  **shipped and hardware-verified in v1.3.0** (see [`readme.md`](readme.md))
  and are no longer deferred.
- **No open build warnings** — zero compiler warnings on both boards and the
  `test/` app is a gate, not a goal.
- **No build artifacts tracked in git** — `.gitignore` covers `build/`,
  `build-*/`, `test/build/`, and `test/build-*/`, but directories committed
  before those rules stay tracked and would ship in the source zip (they once
  made it ~367 MB). Untrack them and commit the removal **before** packaging:
  ```
  git rm -r --cached build build-tab5 test/build test/build-tab5
  ```
  (adjust to the build dirs that exist). Confirm with
  `git ls-files | rg '/build|^build'` returning nothing.

---

## 1. Preflight

1. **Working tree** — everything intended is committed and the tree is clean
   (`git status`). Generated files (`build*/`, `sdkconfig.*`, `release/`,
   `screenshots/`, `assets_out/`) are git-ignored and must not be staged.
   (`managed_components/` is tracked and intentionally stays tracked; only the
   source zip omits it.)
2. **Static gates** — no `// AI:` comments, the machine-readable tags are
   valid, the breadcrumb map is current, and no test is vacuous (see
   [`ai-context.md`](ai-context.md) §Audit and Hardening Rules and
   [`docs/comment-tags.md`](docs/comment-tags.md)):
   ```
   python tools/comment_index.py
   python tools/breadcrumbs.py
   python tools/green_guard.py --strict
   python tools/gallery_audit.py           # gallery identity + uniqueness
   python tools/gallery_audit_selftest.py  # red canary for that identity gate
   python tools/host_batch_run.py --check
   python tools/host_webapp_gate.py --check
   python tools/host_tui_gate.py --check
   python tools/host_modal_gate.py --check
   python tools/gfx_toolkit_test.py --host
   python tools/host_apps_gate.py --check  # 34 apps (incl. flagship), 679 host assertions, 45 SKIPs, 7 canaries
   python tools/make_web_screenshots.py --check  # DOS web proof PNGs
   python tools/visual_proof_gate.py --check     # palette/box borders/ink/signatures
   python tools/visual_proof_gate.py --self-test # red canaries
   ```

   These v1.6 host gates and the visual proof gates compile or drive shipped
   code and the one shared pixel model, and open no serial port; a pass is a
   pre-gate only, never hardware sign-off (`harness.md` §0/§2.2). The proof PNGs
   are labelled host proofs, not device captures.
3. **Version freeze** — update, in one change:
   - `p4minishell_config.h` `P4_CONFIG_VERSION_{MAJOR,MINOR,PATCH}` (+ the
     string macro) and `p4minishell_config.yaml` `config_version`;
    - the version stamps in `readme.md`, `ai-context.md`, `harness.md`,
      `test/README.md`, `PORTING.md`, `SDK.md`, `documentation.md`,
      `schematics.md`, `SECURITY.md`, `command.md`, `batch.md`, `architecture.md`,
      `roadmap.md`, `API.md`, `ABI.md`, `editor.md`, `release.md`;
    - `bugs.md` header;
    - move `changelog.md` `[Unreleased]` → `## [vX.Y.Z] — YYYY-MM-DD` and record
      the hardware matrix (board / port / suite result).

---

## 2. Build (both boards, firmware **and** test)

One build directory per board; always pass `-DP4_BOARD`. Source ESP-IDF first.

```powershell
. <idf>\export.ps1

# Firmware
idf.py -B build      -DP4_BOARD=jc1060p470c build
idf.py -B build-tab5 -DP4_BOARD=m5stack_tab5 build

# Unit-test app
cd test
idf.py -B build      -DP4_BOARD=jc1060p470c build
idf.py -B build-tab5 -DP4_BOARD=m5stack_tab5 build
cd ..
```

**Expected:** four `Project build complete`; **zero** compiler warnings/errors.
Two CMake notices are expected and benign (an "unexpected files" notice about
the staged `esp32p4_p4_function_ev_board` BSP, and the "Missing kconfig option"
re-run notice). Anything else is a failure.

CI mirrors this: [`.github/workflows/build.yml`](.github/workflows/build.yml)
builds firmware + `test/` for both boards.

---

## 3. Verify (the ladder)

Run in order; details and flags in [`harness.md`](harness.md). Reset the board
with `shell_session.hard_reset()` (already handled by the runners) — never open
a raw serial port.

| # | Step | Command | Pass condition |
|---|------|---------|----------------|
| 0 | Static gates | `python tools/p4test_run.py --static` | green guard + comment index + breadcrumbs all OK |
| 0b | Host unit lane (pre-gate) | `python3 tools/host_gen_runner.py --check && python3 tools/host_unit_run.py --static && python3 tools/host_unit_run.py --build` | up to date, manifest OK, 0 failures. Needs Linux/WSL2; **never** a substitute for step 2 |
| 0c | v1.6 host gates + app/browser/visual pre-gates | `python tools/host_batch_run.py --check`, `host_webapp_gate.py --check`, `host_browser_gate.py --check`, `host_tui_gate.py --check`, `host_modal_gate.py --check`, `gfx_toolkit_test.py --host`, `host_apps_gate.py --check`, `host_all_gates.py` (incl. `browser`/`browser_canary`, `visual`/`visual_canary`) | 34 apps (incl. flagship), 679 host assertions PASS, 45 honest SKIPs; `host_all_gates.py` 35/35 gates (15 canaries); webapp gate 8 fixtures / 432 assertions, browser gate 5 fixtures / 271 assertions; no serial port, no hardware sign-off |
| 0d | Visual proof gates (pre-gate) | `python tools/make_web_screenshots.py --check`, `python tools/visual_proof_gate.py --check`, `python tools/visual_proof_gate.py --self-test` | DOS palette, CP437 box borders, ink bands, distinct screen signatures, red canaries all pass; host proofs only, no device capture or hardware sign-off |
| 1 | Static checks | `python -m compileall tools apps` | no errors |
| 2 | On-target unit tests | `python tools/unit_run.py COM3 COM6` | `=== All tests completed ===`, 0 failures on both; **refresh `test/README.md` count** |
| 3 | Hardware suites | `python tools/p4test_run.py --boards COM3,COM6 --tier full --fail-fast --retry 1` | merged PASS table (both boards) + no board differential |
| 4 | Host regression | `python tools/regression.py <COMx>` | PASS (release runner) |
| 5 | Boot soak | `python tools/boot_regression.py COM3 20 COM6` | 20/20 clean per board (no panic, one AUTOEXEC, SD ready, no stray W/E) |
| 6 | Transcript parity | `python tools/appdiff.py` | no diff vs baseline (re-baseline only with intent). **Open for v1.6.0:** the new `[M-RICH-*]` lines change app transcripts; `tools/appdiff/baseline/*.transcript.txt` must be regenerated on a board first |
| 7 | Dogfood soak | `python tools/dogfood.py <COMx> --minutes 15` | no anomalies |
| 8 | Visual (optional) | `tools/display_glitch_watch.py`, `tools/led_watch.py` | no BSOD frames / LED table matches |

After flashing the test app, **reflash the main firmware** before the next step.

Steps 3–5 and 7 must pass on **both** boards. Step 5 is the F6/Tab5 hosted-SDIO
storm gate (it counts the storm signatures, not just PASS/FAIL).

---

## 4. Package the artifacts

`tools/make_release.py` assembles the release outputs into `release/`
(git-ignored). The SD content itself is owned by the pushers; the tool sequences
them, never re-implements them.

```powershell
# Merged firmware image per board (from the build dir's flash_args)
python tools/make_release.py bins --board jc1060p470c --build-dir build --test-bin test/build/p4minishell_tests.bin
python tools/make_release.py bins --board m5stack_tab5 --build-dir build-tab5 --test-bin test/build-tab5/p4minishell_tests.bin

# Per-board ESP32-C6 co-processor app images (staged from the C6 build dirs)
python tools/make_release.py c6

# Source zip (tracked files, refetchable managed_components/ omitted) + SHA256SUMS
python tools/make_release.py srczip
python tools/make_release.py manifest
```

Artifacts produced under `release/`:

| Artifact | Contents | Source |
|----------|----------|--------|
| `p4minishell-<board>.bin` | bootloader + partition table + app, merged, flash at `0x0` | `make_release.py bins` |
| `p4minishell-tests-<board>.bin` | unit-test app image (when `--test-bin` given) | build + `bins --test-bin` |
| `esp32c6_hosted_slave-<board>.bin` | ESP32-C6 co-processor app image (per board) | `make_release.py c6` |
| `p4minishell-src.zip` | committed source tree minus `managed_components/`, plus `RESTORE.md` (`git archive`) | `make_release.py srczip` |
| `SHA256SUMS` | checksums of the above | `make_release.py manifest` |

Flash the merged image with a single `esptool write_flash 0x0` (or `idf.py
flash`). The C6 co-processor firmware is flashed separately: push the matching
`esp32c6_hosted_slave-<board>.bin` to the SD root as `esp32c6_hosted_slave.bin`
(with `apps/push_c6.py`, which auto-detects the board) and run `c6ota default`
(see [`coprocessor/esp32c6_slave/README.md`](coprocessor/esp32c6_slave/README.md)).

---

## 5. SD-card file packages

The card is the app store and data volume. The firmware creates the base files
on first boot if absent (never overwriting a user file); the pushers fill the
rest. Layout:

| SD path | Contents | Created by |
|---------|----------|-----------|
| `/CONFIG.SYS`, `/AUTOEXEC.BAT` | Boot config + startup script | firmware first boot (`components/boot`) |
| `/<APP>.BAT` (root) | Launcher shims for the reference apps | `apps/push_apps.py` |
| `/APPS/*.APPINFO` | App metadata for the launcher | `apps/push_apps.py` |
| `/APPS/*.ASSETS`, generated BMPs | Asset manifests + art | `apps/push_assets.py` |
| `/WEB/` | Browser showcase (`INDEX.HTML`, `PAGE2.HTML`, `CANVAS.HTML`, `LOGO.BMP`) | `apps/push_apps.py` |
| `/EMOJI/` | Sample emoji pack (`EMOJI.INI` + BMPs), optional | `apps/push_apps.py` |
| `/PKGS/<APP>/` | Installable bundles (payload + `.APPINFO` + `.ASSETS`, optional `SIGN=`) | `apps/push_pkgs.py` |
| `/TEMPLATES/*.MD` | Writerdeck templates | `apps/push_templates.py` |
| `/DICTS/*.words` | Spellcheck wordlists (base lists; `user.words` is learned on-device, pulled home with `push_dicts.py pull|merge`) | `apps/push_dicts.py` |
| `/tmp/edit/` | Editor crash files (runtime spill, never shipped) | editor autosave; exempt from `temp clean` |
| `/FONTS/*` | Bundled TTFs (with licenses in-repo) | `tools/make_sd_archive.py` (or copy `assets/fonts/`) |
| `/CERTS/` | User CA trust store (optional) | `certs` command |
| `/esp32c6_hosted_slave.bin` | ESP32-C6 co-processor app image for `c6ota default` (per board) | `apps/push_c6.py` |
| `/DBS/`, `/ALARMS/` | User data (not shipped) | runtime |

Deploy to a board (drives the pushers in the correct order):

```powershell
python tools/make_release.py push <COMx> [--sign NAME.priv.pem]
```

`--sign` adds an ECDSA P-256 `SIGN=` line to each bundle manifest
([`ABI.md`](ABI.md)).

**Complete SD payload archive.** `python tools/make_sd_archive.py` builds
`release/p4minishell-sd-v1.6.0.zip` from the same `apps/app_manifest.py` list
plus templates, dictionaries and the DejaVu reading fonts - the exact tree the
pushers deploy. It is the public "copy to the card root" artifact; every file
is a repository asset, nothing is invented.

**Fresh-card verification (release ritual).** Before the final sweep, wipe the
boards and the card so no stale payload can green the run:

```powershell
config factory                 # board settings back to defaults
format /FS:FAT32 /Q            # SD card, quick format (asks to confirm)
python apps/push_apps.py <COMx>
python apps/push_templates.py <COMx>
python apps/push_dicts.py <COMx>
# FONTS/: copy assets/fonts/DejaVu*.ttf to the card (or extract the SD zip)
```

Then run the full gate on that clean state. The same content can be captured
from a fully provisioned card for archival; the committed source of truth is
the archive tool, not a golden image.

---

## 6. Legal and hygiene checklist

- **Source zip clean** — `git archive` ships tracked files only; confirm the zip
  has no `build*/`, `sdkconfig.*`, `release/`, logs, or screenshots. The vendored
  `managed_components/` (430 MB of the 480 MB tree) is **omitted** to keep the
  download small: `dependencies.lock` pins it and the archive's `RESTORE.md`
  tells the recipient to run `idf.py reconfigure` and then re-apply
  `tools/managed_patches.patch`. The repo still tracks the directory; only this
  artifact leaves it out.
- **Licenses present** — `LICENSE` + [`licence.md`](licence.md) (MIT + the
  third-party table) ship with the source; component `LICENSE`s stay with the
  full tracked tree and arrive again with the components `idf.py reconfigure`
  fetches (`boards/m5stack_tab5/board_bsp/` under Apache-2.0,
  `assets/fonts/` DejaVu/OFL are committed in the zip). Keep `icon/icon.png` (70 MB master)
  out of any bundle — only the derivatives are committed.
- **No secrets** — no keys, tokens, Wi-Fi credentials, or `.priv.pem` files in
  the tree or the zip. `pkg` signing keys stay with the distributor.
- **SPDX headers** on every source file; run the AI-comment sweep again after the
  final doc pass.

---

## 7. Docs gate

- [`readme.md`](readme.md) — version, feature tour, and the Documentation index
  (includes `architecture.md`, `release.md`, `schematics.md`).
- Tutorials (`tutorial_getting_started.md`, `tutorial_batch.md`,
  `tutorial_native.md`, `tutorial_edit.md`) reviewed against behaviour.
- [`PORTING.md`](PORTING.md) / [`schematics.md`](schematics.md) re-synced to the
  boards if any pin/signal/device changed this cycle.
- [`SECURITY.md`](SECURITY.md) current (lock, secrets, TLS, httpd scope).
- `test/README.md` and `harness.md` counts refreshed from the hardware run
  (they drift when new tests land).
- Gallery captures re-checked with `python tools/gallery_audit.py` and
  `python tools/gallery_audit_selftest.py` (the red canary); both are wired into
  `tools/harness/verify.py` as static gates, so a shell screenshot published as
  an app image fails the release.
- DOS visual proofs re-checked with `python tools/visual_proof_gate.py --check`
  and `--self-test`; `docs/apps.md` captions match the distinct DOS screens and
  every web/app proof is labelled as host-generated, not a device capture.
- `roadmap.md` curated (completed work folded into history, open items listed).

---

## 8. Sign-off

Release is ready when **all** boxes below are true:

- [ ] Version macros, `config_version`, all doc stamps, and `changelog.md`
      section updated; `bugs.md` header current.
- [ ] Zero build warnings on both boards (firmware + `test/`).
- [ ] Unit suite green on both boards; `test/README.md` count refreshed.
- [ ] v1.6 host gates + browser/visual pre-gates green (batch/webapp/browser/
      tui/modal/gfx/apps, 34 apps incl. the flagship, 35/35 gates, 15 canaries) and
      `tools/appdiff/baseline/*.transcript.txt` rebaselined on a board for the
      new `[M-RICH-*]` lines.
- [ ] Board-free visual proof gates green (`make_web_screenshots.py --check`,
      `visual_proof_gate.py --check` / `--self-test`); web proofs are labelled
      host-generated and `docs/apps.md` captions match the DOS screens. No
      device capture is claimed for them.
- [ ] Documentation screenshots refreshed and pixel-checked
      (`python tools/screenshot_gate.py --update`, then
      `python tools/screenshot_gate.py`; the browser dashboard shot comes from
      `s20_website`).
- [ ] `p4test` full sweep + `regression.py` + `boot_regression` green on both boards.
- [ ] F27 is closed (resolved in v1.3.0; the three consecutive clean full
      sweeps on both boards plus the dogfood soak are recorded in
      [`bugs.md`](bugs.md)) and the genuinely deferred-hardware features are
      verified or labelled experimental.
- [ ] `make_release.py bins/c6/srczip/manifest` produced `release/` artifacts
      (including the per-board `esp32c6_hosted_slave-<board>.bin`) and
      `SHA256SUMS` verifies.
- [ ] SD content confirmed on a provisioned card (`make_release.py push`).
- [ ] Legal/hygiene checklist complete; source zip clean.
- [ ] Docs gate complete.
- [ ] Tag the release commit and publish the artifacts + `SHA256SUMS`.

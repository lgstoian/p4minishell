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

Current target version: **v1.3.0** (`P4_CONFIG_VERSION_*` in
`p4minishell_config.h`). Substitute your release version throughout.

---

## 0. Release blockers (clear these first)

A public release must not ship with an open HIGH-severity defect. As of the
last campaign:

- **F27 (HIGH, open)** — a transient Tab5 crash under long mixed `p4test`
  sweeps (Guru/WDT/silent reboot), unreproducible in targeted re-runs. **Required
  to close:** three consecutive clean full `p4test_run <COMx>` sweeps on both
  boards plus a `dogfood` soak, recorded in `bugs.md`. See
  [`bugs.md`](bugs.md) F27 for the current evidence and next experiment.
- **Deferred-hardware items** — features marked "build-verified, on-device
  verification deferred" in [`changelog.md`](changelog.md) (captive portal, TLS
  `CERTS` store, BLE HID host, serial PIM sync, record editing, the `sync` USB
  handshake, the `net` MQTT event service + SD outbox, and the batch/redirection
  DOS-parity rounds). Either verify them on hardware before release or label
  them **experimental** in the release notes. Do not present them as verified.
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
   (`git status`). Generated files (`build*/`, `sdkconfig.*`, `managed_components/`,
   `release/`, `screenshots/`, `assets_out/`) are git-ignored and must not be
   staged.
2. **AI-comment sweep** — no `// AI:` comments anywhere (see
   [`ai-context.md`](ai-context.md) §Audit and Hardening Rules):
   ```
   rg -n "// AI:" components main boards test || echo "clean"
   ```
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
| 1 | Static checks | `python -m compileall tools apps` | no errors |
| 2 | On-target unit tests | `python tools/unit_run.py <COMx>` | `=== All tests completed ===`, 0 failures; **refresh `test/README.md` count** |
| 3 | Hardware suites | `python tools/p4test_run.py <COMx>` | full PASS table (both boards) |
| 4 | Host regression | `python tools/regression.py <COMx>` | PASS (release runner) |
| 5 | Boot soak | `python tools/boot_regression.py <COMx> 20` | 20/20 clean (no panic, one AUTOEXEC, SD ready, no stray W/E) |
| 6 | Transcript parity | `python tools/appdiff.py` | no diff vs baseline (re-baseline only with intent) |
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

# Clean source zip (tracked files only) + SHA256SUMS
python tools/make_release.py srczip
python tools/make_release.py manifest
```

Artifacts produced under `release/`:

| Artifact | Contents | Source |
|----------|----------|--------|
| `p4minishell-<board>.bin` | bootloader + partition table + app, merged, flash at `0x0` | `make_release.py bins` |
| `p4minishell-tests-<board>.bin` | unit-test app image (when `--test-bin` given) | build + `bins --test-bin` |
| `esp32c6_hosted_slave-<board>.bin` | ESP32-C6 co-processor app image (per board) | `make_release.py c6` |
| `p4minishell-src.zip` | committed source tree (`git archive`) | `make_release.py srczip` |
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
| `/PKGS/<APP>/` | Installable bundles (payload + `.APPINFO` + `.ASSETS`, optional `SIGN=`) | `apps/push_pkgs.py` |
| `/TEMPLATES/*.MD` | Writerdeck templates | `apps/push_templates.py` |
| `/DICTS/*.words` | Spellcheck wordlists (base lists; `user.words` is learned on-device, pulled home with `push_dicts.py pull|merge`) | `apps/push_dicts.py` |
| `/tmp/edit/` | Editor crash files (runtime spill, never shipped) | editor autosave; exempt from `temp clean` |
| `/FONTS/*` | Bundled TTFs (with licenses in-repo) | `push_fonts.py` |
| `/CERTS/` | User CA trust store (optional) | `certs` command |
| `/esp32c6_hosted_slave.bin` | ESP32-C6 co-processor app image for `c6ota default` (per board) | `apps/push_c6.py` |
| `/DBS/`, `/ALARMS/` | User data (not shipped) | runtime |

Deploy to a board (drives the pushers in the correct order):

```powershell
python tools/make_release.py push <COMx> [--sign NAME.priv.pem]
```

`--sign` adds an ECDSA P-256 `SIGN=` line to each bundle manifest
([`ABI.md`](ABI.md)). The pulldown of the same content can be captured from a
fully provisioned card for archival; there is no committed golden SD image.

---

## 6. Legal and hygiene checklist

- **Source zip clean** — `git archive` ships tracked files only; confirm the zip
  has no `build*/`, `sdkconfig.*`, `release/`, logs, or screenshots. The vendored
  `managed_components/` **is** included on purpose (the reference-board BSP and
  the pinned ESP-Hosted/LVGL/esp_h264 trees are committed so a clone builds
  without network fetches).
- **Licenses present** — `LICENSE` + [`licence.md`](licence.md) (MIT + the
  third-party table) ship with the source; vendored component `LICENSE`s
  (`managed_components/`, `boards/m5stack_tab5/board_bsp/` under Apache-2.0,
  `assets/fonts/` DejaVu/OFL) are preserved. Keep `icon/icon.png` (70 MB master)
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
- `roadmap.md` curated (completed work folded into history, open items listed).

---

## 8. Sign-off

Release is ready when **all** boxes below are true:

- [ ] Version macros, `config_version`, all doc stamps, and `changelog.md`
      section updated; `bugs.md` header current.
- [ ] Zero build warnings on both boards (firmware + `test/`).
- [ ] Unit suite green on both boards; `test/README.md` count refreshed.
- [ ] `p4test` full sweep + `regression.py` + `boot_regression` green on both boards.
- [ ] F27 closed (or the release explicitly documents it as a known issue with a
      repro) and deferred-hardware features verified or labelled experimental.
- [ ] `make_release.py bins/c6/srczip/manifest` produced `release/` artifacts
      (including the per-board `esp32c6_hosted_slave-<board>.bin`) and
      `SHA256SUMS` verifies.
- [ ] SD content confirmed on a provisioned card (`make_release.py push`).
- [ ] Legal/hygiene checklist complete; source zip clean.
- [ ] Docs gate complete.
- [ ] Tag the release commit and publish the artifacts + `SHA256SUMS`.

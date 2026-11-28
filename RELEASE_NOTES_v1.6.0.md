# P4MiniShell — v1.6.0

A modern take on retro MS-DOS: your apps keep the 80×25 CP437 grid, box
panels, status bars and keyboard-first feel, while gaining richer screens,
better browser pages, new multimedia tools and a companion desktop app.
Nothing from earlier releases was removed.

> **Release status:** being finalized. The hardware sign-off section is filled
> from the actual COM3/COM6 runs; host pre-gates are never hardware sign-off
> (`harness.md` §0).

## New Features

- **P4Sync desktop companion.** A Windows 11 app plus an agent-friendly CLI
  let you discover a board, browse its files, push/pull files over USB, run a
  full shell, capture screenshots, sync PIM data and manage packages. It uses
  the existing USB harness, so no firmware change or second transport is
  involved. A downloadable standalone binary is attached to this release; see
  [`p4sync.md`](p4sync.md).
- **DOS dashboards for the reference apps.** Sixteen apps plus the flagship
  `SHOWCASE.BAT` now present their data as in-app panels, charts and status
  bars on the same 80×25 cell grid, with per-app proof captures.
- **Wikipedia in the browser.** Live HTTPS pages render in the DOS engine with
  working JPG/PNG images, coloured links, and a labelled host proof in the
  README. The [browser tutorial](web.md) explains verbs, limits and image
  formats.
- **More ways to work with media.** Play a folder of images as a bounded
  frame-sequence video, convert WAV/MP3/FLAC assets for the firmware with
  `audio_asset_prepare.py`, and read EPUB books with improved chapter
  navigation.
- **A small 3D engine for games.** `gfx3d` draws fixed-point shaded or
  wireframe models through the one existing graphics surface and DOS palette,
  from batch scripts or SD-card apps.
- **A unified audio engine.** Mixer, envelopes, sequencer and sound-effect
  buses share the single existing audio output — no second driver.
- **OS subsystem reports.** `os pm|drv|io|fs|disk|net|mem|sec|ver|pwr` give a
  read-only view of the existing device services, with explicit `unavailable`
  instead of invented values.
- **Registry, shared libraries and structured batch control.** `reg` manages
  INI settings, `lib` shares batch/applib routine libraries, and
  `try`/`finally` add scoped error handling — all batch- and SD-app-compatible.
- **App switching.** Alt+Tab on a physical keyboard or a bottom-left touch
  swipe cycles foreground app sessions, with a DOS app-switcher overlay.

## Improvements

- **Power and battery behaviour.** Stale deepsleep timers, wake-source leaks
  and oversized display-timeout values are fixed, so sleep and idle-off behave
  as documented.
- **Multimedia tooling.** A host-side asset converter prepares firmware-ready
  WAV files from common formats and refuses invalid input.
- **Third-party verification.** LUFA-derived HID constants now check our real
  USB/HID descriptors, and miniaudio powers the audio asset cross-check. Both
  are host-only and attributed in [`licence.md`](licence.md).

## Bug Fixes

- **BOUNCE no longer freezes or crawls.** A zero-second key timeout was
  silently waiting ten seconds between frames; the ball now moves at roughly
  29 fps with only the deliberate fading trail.
- **Wikipedia images now load.** Deeply nested article images were skipped and
  large HTTP buffers failed; images now decode with a bounded fetch, and
  root-relative links resolve correctly.
- **PageUp/PageDown work** through the existing browser key handler.
- **Screenshot proofs are deterministic** — a stale TUI box no longer leaks
  into the next shell-idle capture.
- **Signed native payloads now launch**, while unsigned and tampered payloads
  remain refused.
- **Gallery publishing no longer overwrites** the deterministic graphics
  proof image.
- **P4Sync drop-downs are readable** in both light and dark themes (the dark
  popup was white-on-white).

## Breaking Changes

None. Existing batch verbs, app layouts and firmware behaviour are preserved.

## Verification (evidence, not marketing)

- **Host pre-gates:** `host_all_gates.py` 49/49 gates (22 canaries);
  `green_guard --strict` 843 tests, 0 errors, 0 warnings; comment and
  breadcrumb manifests 0 errors. P4Sync: 51 checks + 18 canaries; GUI
  Debug/Release builds 0 warnings/errors, 20 console tests, contrast canary
  verified. Visual proof gate 7/0; gallery 23 PNGs with no shell captures;
  proof index 39 refs / 0 missing.
- **Boards (COM3 `jc1060p470c`, COM6 `m5stack_tab5`):** firmware and test
  builds 0 errors / 0 project warnings; on-board Unity 750 tests, 0 failures,
  2 ignored per board; fast tier green; BOUNCE before `0.8 fps / 125 dropped`,
  after `28.5–29.1 fps / 1–3 dropped`; signed native launch verified from a
  `.bat` with refusal of unsigned/tampered payloads; screenshot determinism
  11/11; PageUp/PageDown proven with pixel diffs; appdiff 15 apps / 0 diffs;
  20/20 boots with 0 panics; 15-minute dogfood 7/7 on both boards; gallery and
  live Wikipedia proof published.
- **Full-tier result:** the last completed run returned `RESULT OK` with
  every suite passing on both boards and no board-differential failures. One
  suite (`apps_gallery` on COM3) passed on its second attempt: MOOD's real SD
  writes put `[M-MOOD-TREND]` at ~100 s, beyond the old 45 s step timeout, so
  the step/watchdog budget was calibrated (180 s / 420 s) without changing any
  marker, PNG or shell assertion. That calibration has **not** been re-proven
  by a completed clean run.
- **Screenshot determinism:** the root cause (capture must real-hard-reset
  before both passes) is fixed in the capture path and one 11-frame run was
  observed, but the `--update` rebaseline plus two plain verification passes
  were **not** completed. Do not claim this gate green from those runs.
- **Security trade-off:** `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n` is required for
  a writable+executable internal region under IDF 5.5.5, so internal SRAM is
  RWX (weakened W^X). Mitigations: ECDSA verification before execution, a fixed
  ABI with no relocation, and the `.bat` shim as the only front door.
- A host pass is never hardware sign-off.

## P4Sync binary

`release/p4sync-windows-x64-v1.6.0.zip` is attached to this GitHub release.
Extract on Windows 11 x64, install Python 3 with `pyserial`, and run
`run-p4sync.cmd` (or `P4SyncGui\P4SyncGui.exe --mock` for an offline demo).
`SHA256SUMS` contains its digest. It was validated from a clean extract: the
bundled GUI found the bundled bridge and completed a `p4sync-1` handshake.

## Licensing

Project code is MIT. Third-party notices are in [`licence.md`](licence.md).
BMDS (`jthwho/bmds`, MIT, © Jason Howard) is credited for the derived
`gfx3d_math.c` and for design ideas. LUFA (© Dean Camera) and miniaudio
(© David Reid) are host-only with verbatim notices; neither adds a second
firmware stack or decoder.

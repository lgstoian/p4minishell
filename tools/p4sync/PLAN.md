# P4Sync — plan, current state, and lessons applied

Status: **host/bridge gaps closed; GUI app owned by the GUI agent**
(`apps/p4sync_gui/**`). The host-side bridge, CLI and protocol are complete in
`tools/p4sync/`; the firmware is unchanged and the current release under
hardware test stays compatible. See `tools/p4sync/README.md` for the exact
CLI/bridge JSON contract and the board-free CI commands.

This document is the planning/scaffolding hand-off. It records what already
works, what this firmware/harness campaign taught us, and the ordered next
steps. It is deliberately concrete so the next agent does not re-derive it.

---

## 1. What exists today

`tools/p4sync/` is a **host CLI prototype** over the shared `p4test` transports
(no serial logic of its own: DTR-safe open, marker-synced `run()`, panic
detection, SDFX push/pull, BMPX screenshot).

| Piece | File | Notes |
|-------|------|-------|
| Client facade | `tools/p4sync/client.py` | `P4SyncClient`: `status()`, `check_status()`, `board_id()`, `display_size()`, `push_file/local`, `pull_file`, `screenshot()`. Handshake tag `p4sync-1`. |
| CLI | `tools/p4sync/__main__.py` | Every bridge verb as a subcommand with `--json`, `--port`/`--board`, `--mock`, `--timeout`, `--settle`; `shot` stays a backwards-compatible alias of `screenshot`. Non-zero exit when a command's own `ok` verdict is false. |
| Transport | `tools/p4test/{session,sdbridge,screenshot}.py` | The ONE framing/reader implementation. Reuse; never fork. |
| PIM engine | `tools/pim_sync.py` | Mirror-dir vCard/iCal merge over `pim` verbs (PIMX). Reused by `full-image`. |
| Wire contract | `tools/p4sync/protocol.py` | `PROTO_TAG=p4sync-1`, canonical + firmware `sync.*` capability keys, `image.json` schema, stable error codes, pure parsers. |
| GUI bridge | `tools/p4sync/bridge.py` | JSON-lines stdin/stdout; `devices/status/push/pull/screenshot/full-image/verify/plan/shell/pkg`; progress/log events; `--mock` fixtures. All I/O via `P4SyncClient`. Screenshot returns path + `png_base64`/`sha256`/`size`; full-image detects and rebuilds stale manifests. |
| Board-free gate | `tools/p4sync_gate.py` | `--check` (51 checks) / `--self-test` (18 red canaries), real bridge subprocess, fake Device sessions, CLI smokes. |

Run: `python tools/p4sync --port COM3 status` (or `full-image --mirror DIR --db NAME`).
Agent form: `python tools/p4sync --mock --json status` (same data object as the
bridge). GUI: `python tools/p4sync/bridge.py --mock` speaking JSON lines.

## 2. Firmware surface it depends on

- `sync status` — read-only capability snapshot (`sync_commands.c`): proto tag,
  board, SD/lock state, transfer magics, byte limits. It must NOT enumerate
  stores or add a second walker.
- `receive` / `send` — SDFX framing (magic + LE size), CRC-checked; the only SD
  transfer path.
- `screenshot` / `screencap` — BMPX streaming (handled by the console reader,
  not the worker).
- `pim get|put` — PIMX framing through the shared serial engine.
- `pkg` verbs — package install/verify (P4Sync must never install by copying
  files itself).

## 3. Lessons from the firmware/harness campaign (apply to P4Sync)

These are the hard-won rules that a desktop client must respect.

1. **Never open a raw `serial.Serial`.** Use `p4test.session`/`P4SyncClient` so
   DTR/RTS are pre-set low before `open()`. Opening raw resets the board and
   looks exactly like a crash (USB re-enumeration).
2. **Reset is unreliable on USB-Serial/JTAG.** `esptool --after hard_reset`
   intermittently leaves the chip latched in download mode (`boot:0x206 …
   waiting for download`). A fresh boot must be *proved* by boot text; a real
   reset is the shell `reboot` verb (or `esptool --after watchdog_reset`).
   Never treat "no output" as "the app is fine".
3. **Marker-synced, not time-synced.** `Device.run()` synchronises on an echoed
   unique marker. A command that opens a modal eats the marker: raise `settle`
   or pass `unwind=False`. Durations from `run()` prove nothing about board
   speed.
4. **One transport, one framing reader.** SDFX/BMPX/PIMX readers and the CRC are
   shared. The desktop client must use the same code, not a second parser.
5. **Two-board runs are a focused driver, not a runner suite.** The suite runner
   is single-board/synchronous (`run(dev, ctx)`); pairing lives in
   `tools/chat_test.py`. Model any P4Sync "device pair" feature the same way.
6. **Bench credentials are local-only.** `tools/wifi_local.json`
   (`{ssid,password}`), git-ignored, read via
   `p4test.local_config.wifi_credentials()`; skip live checks when absent. The
   P4Sync desktop app must use an OS keychain/credential store, never a file in
   the repo.
7. **Resource budgets are real.** The command worker is 32 KB and device RAM is
   tight; a sync operation that renders HTML/DOM on the worker can stack-fault.
   Prefer small, bounded commands and let the firmware stream (`receive`/
   `send`) rather than buffering whole trees in one command.
8. **FATFS descriptor pool is small (BSP `max_files=5`).** Long httpd/browser/
   receive sequences can exhaust it (`vfs_fat: open: no free file descriptors`).
   A sync client should transfer one file at a time and avoid overlapping SD
   holders.
9. **Boot-time internal RAM is the scarce resource.** Do not add boot-time
   consumers for sync; open the SD session per operation.
10. **No feature removal / no duplication.** P4Sync extends the existing verbs
    and transports; new behavior is a verb or an ops hook, never a parallel
    reader/dispatcher.

## 4. Protocol scaffolding (next increment)

Define the contract once, versioned, and share it between CLI and desktop:

- `tools/p4sync/protocol.py` — constants: `PROTO_TAG`, capability keys parsed
  from `sync status`, manifest schema (`image.json`), error codes. Both the CLI
  and the future app import it (single source).
- Handshake: request `sync status`; require `proto=p4sync-N`; parse
  `board=`, `sd=`, `lock=`, `magic.*=`, `limit.*=`. Refuse version mismatch with
  a clear error (do not guess).
- Transfer: one `receive`/`send` per file, CRC checked by the shared sdbridge.
- Incremental: host keeps `(path, size, mtime, crc32)`; device provides `dir /b`
  + `stat`/`attrib`. Sync = diff → transfer only changed files. Never add a
  device-side index.
- Conflict policy: newer-wins by mtime (same as `pim_sync`), with a
  `--dry-run`/`--report` that prints the plan before touching the card.
- Resume: per-file, via a temp `.part` destination and CRC verification; the
  device already writes atomically.

## 5. Desktop app roadmap (ordered)

1. DONE — `protocol.py` + capability parsing + `--dry-run` plan output.
2. DONE — incremental file sync (diff + CRC + resume), one SD session at a
   time; report bytes/files skipped.
3. GUI WORK — PIM integration as a "Contacts/Calendar" pane over `pim_sync`
   (the bridge exposes it through `full-image`).
4. DONE on the host / GUI WORK — screenshot over the shared BMPX path.
5. DONE on the host / GUI WORK — `pkg` bridge command maps to the existing
   firmware verbs (no new dispatcher).
6. DONE — device discovery reuses `tools/board_ports.py` (no new port scanner).
7. DONE on the host — `tools/p4sync_gate.py` is the board-free ladder
   (`--check` 51 checks, `--self-test` 18 red canaries, including CLI smokes and
   stale-manifest recovery). Board parity still requires the existing hardware
   suites (`tools/p4test_run.py --boards COM3,COM6`) and a board-run
   `full-image` rebaseline; the GUI agent owns the app-side tests.

## 6. Explicit non-goals

- No SD code execution on the device (ABI.md).
- No second serial/framing/merge implementation.
- No device-side sync index (must stay live).
- No credentials in the repository.

## 7. Open questions to settle before the desktop build

1. Transport: USB-Serial/JTAG only, or also the USB CDC `userial`/P4Sync channel?
   (Today: serial only; a native USB channel would be a new verb pair.)
2. Sync scope ordering: files before PIM before packages, or a single staged
   plan with rollback?
3. Conflict UX: silent newer-wins vs a per-file prompt (headless serial has no
   prompt; the desktop app can).
4. Multi-device: one at a time (current `Device`) vs paired (focused driver).

---

## 8. Final gap closure (this increment)

Four gaps reported by the host/GUI loop are closed, all inside
`tools/p4sync/**` and `tools/p4sync_gate.py` (no firmware, no board, no changes
outside this scope):

1. **PNG preview payload.** `screenshot` writes the PNG once through the
   existing `p4test.screenshot` path and reads that file back. `data` now has
   `out`/`path`/`png` (same absolute path), `png_base64` (standard base64, no
   data URI), `sha256` (64 lowercase hex), `size`/`bytes` (integer length),
   `width`, `height`. Default path is stable — `p4sync-shot-<port>.png` under
   `P4SYNC_ARTIFACTS` or `<repo>/.agent/p4sync-artifacts` — instead of a
   wall-clock temp name. Missing/non-PNG output is `E_IO` (no false payload).
2. **Stale `image.json` recovery.** `validate_manifest()` rejects the old
   boolean `shot` toggle and a non-numeric `time`; the bridge additionally
   checks that a named shot PNG still exists and is a PNG. A stale manifest is
   removed before the shipped CLI composition runs; recovery reports
   `stale_manifest`, `rebuilt`, `stale_reason` plus a `warn` log event. An
   invalid rebuilt manifest is a hard `E_IO`, never a silent green.
3. **Mock capability completeness.** `MOCK_STATUS` carries `sync.shot_max:
   1048576`, mapped to `limit.screenshot` by `CAP_ALIASES`. It stays optional
   (`CAP_OPTIONAL`, not `CAP_KEYS`): real firmware may omit it, but the GUI's
   strict client check can now be green in mock mode.
4. **CLI/agent contract.** All ten bridge commands remain CLI subcommands with
   `--json`, `--port`/`--board`, `--mock`, `--timeout`, `--settle`; `shot`
   aliases `screenshot`. A command verdict of `ok:false` exits 1.

Verification (Windows, no board): `py_compile` clean; gate `--check` **51 PASS**;
`--self-test` **18 PASS**; CLI mock smoke for `status`, `shell`, `screenshot`,
`full-image` (plus `shot` alias) all exit 0 with one JSON object; failed
`verify` exits 1; `green_guard --strict` reports **809 unity tests, 0 errors,
0 warnings**. Exact commands are in `README.md` §4.

Remaining limitations: no board was accessed, so real BMPX/PIMX/SDFX parity and
the hardware `full-image` baseline rebaseline are still outstanding; GUI-side
code and tests belong to the GUI agent; adding `p4sync_gate.py` to
`tools/host_all_gates.py` belongs to the parent.

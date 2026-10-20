# P4Sync — plan, current state, and lessons applied

Status: **prototype exists** (`tools/p4sync/`), desktop app not started.
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
| CLI | `tools/p4sync/__main__.py` | `status`, `push`, `pull`, `shot`, `full-image`. `full-image` composes handshake + PIM sync + file provisioning + app inventory + screenshot + `image.json`. |
| Transport | `tools/p4test/{session,sdbridge,screenshot}.py` | The ONE framing/reader implementation. Reuse; never fork. |
| PIM engine | `tools/pim_sync.py` | Mirror-dir vCard/iCal merge over `pim` verbs (PIMX). Reused by `full-image`. |

Run: `python tools/p4sync --port COM3 status` (or `full-image --mirror DIR --db NAME`).

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

1. **`protocol.py` + capability parsing + `--dry-run` plan output.**
2. **Incremental file sync** (diff + CRC + resume), driven from one SD session
   at a time; report bytes/files skipped.
3. **PIM integration** surfaced as a first-class "Contacts/Calendar" pane over
   the existing `pim_sync` engine (no new parser).
4. **Screenshot/notifications view** over BMPX; do not add a second capture path.
5. **App/package management** that shells out to the existing `pkg` verbs.
6. **Device discovery**: `tools/board_ports.py` already resolves slugs from
   `sysinfo`; the app should reuse it (no new port scanner).
7. **Test ladder for the app**: reuse `tools/unit_run.py`,
   `tools/p4test_run.py --boards COM3,COM6`, and `tools/chat_test.py`; add a
   `tools/p4sync/README.md` smoke test that runs the CLI `full-image` against
   both boards.

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

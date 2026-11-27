# P4Sync — host CLI, GUI bridge, and protocol

P4Sync is a **host-side** companion to the current P4MiniShell release. It wraps
the existing harness (`tools/p4test` + `tools/pim_sync` + `P4SyncClient`) and
adds no firmware, no serial port code and no second framing/merge engine.
Nothing here changes the firmware under hardware test.

```
GUI apps/p4sync_gui/**  ──JSON lines──>  tools/p4sync/bridge.py
agent / shell           ──CLI────────>  tools/p4sync            (same data schema)
                                          │
                                          └─> P4SyncClient -> p4test Device/sdbridge
                                                            -> pim_sync (PIMX)
```

## 1. CLI (agent-friendly)

Run from the repo root. Global flags go **before** the verb:

```
python tools/p4sync [--port COM3 | --board jc1060p470c] [--json] [--mock]
                    [--timeout SECONDS] [--settle 0.2] [--quick] <verb> ...
```

| Verb | Arguments | Notes |
|------|-----------|-------|
| `devices` | – | Ports via existing `tools/board_ports.py` (no new scanner). |
| `status` | – | `sync status` handshake; proto tag is enforced. |
| `shell` | `COMMAND` [`--plain`] | One existing firmware verb via marker-synced `Device.run`; returns ERRORLEVEL unless `--plain`. |
| `push` | `LOCAL REMOTE` | Uploads via the existing SDFX `receive` path. |
| `pull` | `REMOTE [LOCAL]` | Downloads via SDFX `send`; raw bytes to stdout when `LOCAL` is omitted (legacy, never with `--json`). |
| `screenshot` / `shot` | [`--out FILE`] | BMPX capture through `p4test.screenshot`; returns an absolute PNG path plus `png_base64`, `sha256` and `size`. |
| `verify` | `REMOTE --crc32 HEX` | Host CRC comparison over bytes from `sdbridge`. |
| `plan` | `LOCAL [REMOTE=/sdcard]` [`--cache FILE`] [`--dry-run` \| `--apply`] | Incremental diff; `--apply` executes one file at a time. |
| `full-image` | `--mirror DIR --db NAME` [`--files DIR`] [`--shot FILE`] | Shipped composition + validated `image.json`; stale manifests are detected, removed and rebuilt, never silently accepted. |
| `pkg` | `VERB [PACKAGE]` | `list/install/verify/uninstall/update/info/check/purge/outdated/unpack`. |

`--json` prints **one JSON object** on stdout using exactly the bridge's `data`
schema; progress and diagnostics go to stderr. Without `--json`, the original
human output of `status/push/pull/shot/full-image` on a real board is unchanged.

Examples:

```powershell
python tools/p4sync --mock devices
python tools/p4sync --mock --json status
python tools/p4sync --board jc1060p470c --json shell "dir /b /s /sdcard"
python tools/p4sync --port COM3 --json plan C:\sync /sdcard --dry-run
python tools/p4sync --port COM3 --json plan C:\sync /sdcard --apply
python tools/p4sync --port COM3 pull /sdcard/NOTES.TXT C:\sync\NOTES.TXT
python tools/p4sync --port COM3 verify /sdcard/NOTES.TXT --crc32 90e37ea3
python tools/p4sync --port COM3 pkg list
```

`--port` wins over `--board`; `--board` uses `board_ports.resolve_port()`;
otherwise `P4SYNC_PORT` is used. Credentials are **never** stored in this repo
(the GUI owns OS credential storage; `p4test.local_config.wifi_credentials()`
is the only local config reader and is bench-only).

## 2. GUI bridge contract (authoritative, shared with apps/p4sync_gui)

Start the process from the repo root:

```
python tools/p4sync/bridge.py [--mock] [--timeout SECONDS] [--debug]
```

One JSON request per stdin line. **Every stdout line is JSON** — responses and
events; human diagnostics and tracebacks go to stderr only.

```json
{"id": 7, "cmd": "status", "port": "MOCK1"}
{"id": 7, "ok": true, "data": {...}}
{"id": 7, "ok": false, "error": "E_PROTO: proto mismatch: ..."}
{"event": "progress", "id": 7, "pct": 0, "message": "pull /sdcard/NOTES.TXT"}
{"event": "log", "id": 7, "level": "warn", "message": "..."}
```

| `cmd` | Request keys | `data` keys |
|-------|--------------|-------------|
| `devices` | *(none)*; optional `probe` | `devices:[{port,board,proto,status}]` |
| `status` | `port` | `port,board,proto,sd,lock,caps,display`, optional `free_bytes` |
| `shell` | `port,command,timeout,settle,plain` | `port,command,output,errorlevel,ok` (output already ANSI-stripped) |
| `push` | `port,local,remote,timeout` | `port,local,remote,bytes,crc32,part_verified` |
| `pull` | `port,remote,local,crc32?,timeout` | `port,remote,local,path,bytes,crc32,part_verified` |
| `screenshot` | `port,out?,path?,png?,timeout` | `port,out,path,png,png_base64,sha256,size,bytes,width,height` |
| `full-image` | `port,mirror,db,files?,shot?,timeout` | `port,manifest,manifest_path,failures,ok,stale_manifest,rebuilt,stale_reason,summary,output` |
| `verify` | `port,remote,crc32` | `port,remote,ok,crc32,expected,bytes` |
| `plan` | `port,local,remote?,cache?,dry_run?,apply?,timeout` | `actions:[{op,path,reason,...}],summary,local_root,remote_root,dry_run` |
| `pkg` | `port,verb,package?,timeout` | `verb,package,command,output,rows,ok` |
| `cancel` *(control)* | `id` | `cancelled:true` (sets the cooperative cancel flag) |

**Screenshot PNG proof.** `screenshot` still captures exactly once through
`P4SyncClient -> p4test.screenshot` (BMPX); `png_base64` is just standard base64
of the PNG bytes the capture wrote, with no data-URI prefix. `sha256` is the
lowercase hex digest of those same bytes, and `size`/`bytes` are their integer
length. `out`, `path` and `png` are the **same absolute path** to that PNG on
disk, so the GUI can either decode the inline payload or open the file and
save-as. When no path is requested, the bridge picks a deterministic
`p4sync-shot-<port>.png` under the artifacts dir: `$P4SYNC_ARTIFACTS` when set,
otherwise `<repo>/.agent/p4sync-artifacts` (git-ignored). A missing or non-PNG
file is an `E_IO` error, never an empty success.

**Stale `image.json` recovery.** Before re-running the shipped composition the
bridge validates any existing `<mirror>/image.json`; an unreadable manifest, a
boolean `shot` toggle from the old GUI bug, or a `shot` path whose PNG is
missing/not a PNG is reported as stale, removed, and rebuilt from the current
mirror. The response then carries `stale_manifest: true`, `rebuilt: true` and a
non-empty `stale_reason`, plus a `log` event (`level: warn`). `ok` is true only
when the freshly written manifest validates and the composition returned 0;
if the rebuilt manifest is still invalid the bridge answers `E_IO` with no
`data` rather than a false success.

`pkg` mappings: `list`→`pkg list /b`, `install`→`pkg install <pkg>`,
`verify`→`pkg verify <pkg>`, `uninstall`→`pkg remove <pkg>`,
`update`→`pkg outdated` (there is no firmware `update` verb; nothing is
installed for it). `data.command` always names the verb actually sent.
For `full-image`, `files` and `shot` may be either paths or booleans: a boolean
toggle is resolved under `mirror` (`files/`, `shot.png`) before the shipped CLI
sees it, so a GUI toggle can never be mistaken for a file descriptor.

`plain=true` on `shell` omits the second `Device.run("echo P4SYNC_EL=...")`
probe and returns `errorlevel: null`. Requests are handled one at a time; a
`cancel` request is read by a separate stdin thread and is checked between
files (an in-flight SDFX transfer cannot be interrupted without dropping the
session, so the bridge closes sessions on shutdown and never leaves a partial
final file).

`--mock` uses deterministic fixtures: two boards (`MOCK1`/`MOCK2`), fixed
`sync status` capabilities, four SD files (`NOTES.TXT`, `notes.md`,
`ALARMS.ICS`, `apps/DEMO/APP.BAT`), fixed manifest clock, and a real 1×1 PNG.
Mock file lookup is FATFS-style case-insensitive. It opens **no** serial port and does **not** require pyserial: the
serial-backed harness modules are imported lazily and mock mode installs only
an import-shape placeholder (never used for I/O). The GUI can therefore be
exercised on a Windows machine with no board and no pyserial; real mode always
imports the genuine pyserial through the shared harness. `--mock` also advertises
`limit.screenshot: "1048576"` in `status.data.caps` (from a `sync.shot_max` line
mapped by `protocol.CAP_ALIASES`). It is deliberately an **optional** capability
in `CAP_OPTIONAL`, never in the required `CAP_KEYS`: real firmware may not print
it, but the GUI's strict client check needs it to go green in mock mode.

## 3. Protocol / incremental sync

- `PROTO_TAG = "p4sync-1"`. The shipped firmware prints `sync.proto`,
  `sync.board`, `sync.sd`, `sync.lock`, `sync.send_magic`, `sync.pim_magic`,
  `sync.shot_magic`, `sync.send_max`, `sync.rx_max`, `sync.pim_rx_max`,
  `sync.inventory_max`. `protocol.parse_sync_status()` normalizes those to
  canonical keys (`magic.sdfx`, `limit.transfer`, ...); `require_capabilities()`
  is a hard refusal on a tag mismatch or missing key. No firmware change.
- `image.json` schema: `version,board,time,pim,files,apps,shot`
  (`version` is optional on read for compatibility with existing manifests).
  `validate_manifest()` now also rejects a non-string `shot` (the old boolean
  toggle) and a non-numeric `time`; `protocol.PNG_MAGIC` is the shared BMPX/PNG
  signature used by both screenshot and full-image proof checks.
- Stable error codes: `E_OK, E_PROTO, E_CAPS, E_TIMEOUT, E_MARKER, E_CRC,
  E_NOTFOUND, E_CONFLICT, E_IO, E_CANCELLED, E_UNSUPPORTED`; each is prefixed
  to the bridge `error` string.
- Inventory is host-side `(path,size,mtime,crc32)` only. Device paths come from
  `dir /b /s`, metadata from detailed `dir /s` with `sd stat` fallback; CRC is
  computed over bytes from the shared `sdbridge`. **No device-side index.**
- Conflict policy matches `pim_sync`: newer `mtime` wins, device wins ties;
  `plan` reports `conflict:true` when the host cache shows both sides changed.
- Resume: uploads go to `<remote>.part`, are read back and CRC/size-verified,
  then renamed with the existing `ren` verb. Downloads write to a local temp
  `.part`, verify the bytes on disk, then `os.replace()` into place. A failed
  transfer never becomes a final file.
- Device discovery reuses `tools/board_ports.py`; PIM reuse stays in
  `tools/pim_sync.py`; screenshots stay in `p4test.screenshot`.

## 4. Board-free tests and verification

CI-friendly (PowerShell; explicit file list because `*.py` is not expanded by
`py_compile` on Windows):

```powershell
python -m py_compile tools/p4sync/__init__.py tools/p4sync/__main__.py tools/p4sync/bridge.py tools/p4sync/client.py tools/p4sync/protocol.py tools/p4sync_gate.py
python tools/p4sync_gate.py --check       # 51 checks: real bridge subprocess, fakes, CLI smokes
python tools/p4sync_gate.py --self-test   # 18 red canaries, no bridge subprocess
python tools/p4sync --mock devices        # CLI smoke, human output
python tools/p4sync --mock --json status  # CLI smoke, structured output
python tools/p4sync --mock --json shell "echo hello"
python tools/p4sync --mock --json screenshot
python tools/p4sync --mock --json full-image --mirror .agent/p4sync-cli-smoke --db demo
python tools/green_guard.py --strict      # static guard, unchanged by this work
```

`--json` mock commands exit 0 only when the command's own `ok` verdict is not
false; a failed `verify` or a failed `full-image` exits 1. Gate cases cover all
five CLI verbs above plus the `shot` alias, the deterministic artifacts path,
and stale-manifest recovery.

The gate exercises the shipped CLI composition and sdbridge through fake Device
sessions. Its canaries must go red for a bad handshake, a corrupt CRC, a missing
marker and a second serial implementation (`import serial`, `serial.Serial`,
`open_port`). A green gate is **not** hardware sign-off: keep using
`tools/p4test_run.py --boards COM3,COM6` and board-run `full-image` baselines.

## 5. Integration needs (report to parent)

- Add `python tools/p4sync_gate.py --check` to the host pre-gate set in
  `tools/host_all_gates.py` (owned by another agent; not edited here).
- The GUI agent owns `apps/p4sync_gui/**` and calls the bridge contract above;
  it should resolve the bridge path relative to the repo root.
- Windows Credential Manager storage belongs in the GUI; the bridge accepts
  only env/local-only config (`P4SYNC_PORT`) and never reads credentials.
- Board parity for the v1.6/v1.7 app baselines still needs a hardware run; no
  `.transcript.txt` baseline was changed here.

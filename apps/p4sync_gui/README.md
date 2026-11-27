# P4Sync GUI (native Windows 11 / WPF)

A native WPF companion app for P4Sync. **It contains no serial, framing, SDFX,
BMPX or PIM implementation.** Every device action is a JSON-lines request to
`python tools/p4sync/bridge.py [--mock]`, owned by the P4Sync bridge work in
`tools/p4sync/`. C# only starts that process, correlates request ids, and
renders the JSON it receives verbatim.

## Requirements

- Windows 11, .NET SDK **10.0.115** (WPF targeting pack ships with the SDK).
- Python 3 for the bridge. No NuGet packages are used (all code is BCL/WPF).

## Build

```powershell
dotnet build apps/p4sync_gui/P4SyncGui.sln
# expected: Build succeeded, 0 Warning(s), 0 Error(s)
```

Projects:

| Project | Target | Role |
|---------|--------|------|
| `src/P4SyncBridge` | `net10.0` | Pure JSON-lines codec + defensive data projections (no WPF) |
| `src/P4SyncGui` | `net10.0-windows` | WPF app: MVVM pages, bridge process, themes, credentials |
| `tests/P4SyncBridge.Tests` | `net10.0` | Console assertion runner using mock JSON lines |

## Run without a board (offline demo)

```powershell
# --mock is a launch flag; it forces bridge.py --mock regardless of settings
dotnet run --project apps/p4sync_gui/src/P4SyncGui -- --mock

# or after building:
apps\p4sync_gui\src\P4SyncGui\bin\Debug\net10.0-windows\P4SyncGui.exe --mock
```

Add `--page <key>` to open a specific page on launch (useful for smoke tests):
keys are `devices`, `dashboard`, `files`, `screenshot`, `pim`, `apps`, `shell`,
`plan`, `settings`. Example:

```powershell
dotnet run --project apps/p4sync_gui/src/P4SyncGui -- --mock --page shell
```

Alternatively open **Settings → Mock mode** (the bridge restarts on the next
request). No COM port is hardcoded: port discovery always comes from the
bridge `devices` response, and the selected board's `port` is injected into
device requests automatically.

The bridge script path is resolved from Settings, else by walking up from the
build output to `tools/p4sync/bridge.py`. If it is missing, the GUI reports a
clear error rather than inventing data.

## Test without hardware

```powershell
dotnet run --project apps/p4sync_gui/tests/P4SyncBridge.Tests
# 20 assertions: response/error/progress/log frames, request serialisation,
# devices/status/plan/screenshot/shell/verify/pkg/full-image projections,
# strict limit.screenshot, screenshot sha256/size/malformed-base64 metadata,
# stale/rejected full-image flags, proto-mismatch refusal, invalid JSON,
# drop-down contrast >=4.5:1 in both themes, accessible ComboBox names.
# Exit code 0 = pass.
```

The runner uses the fixtures in `tests/P4SyncBridge.Tests/MockBridgeData.cs`;
it does not launch the bridge, open a port, or touch the repo.

### Drop-down contrast proof

`powershell -ExecutionPolicy Bypass -File apps/p4sync_gui/capture_dropdowns.ps1`
launches the shipped `--mock` GUI, audits all nine pages in both themes,
expands every ComboBox with UI Automation and captures each popup with
`PrintWindow` (never `CopyFromScreen`). It fails below 4.5:1 contrast and
writes before/after proof PNGs under [`docs/`](docs/). The dark-theme
white-on-white popup was fixed in the shared `Styles.xaml` ComboBox template;
the guard deliberately goes red if a light brush is substituted.

### Offline JSON smoke (no board, no GUI window)

```powershell
# One request per stdin line; stdout is JSON only, stderr is empty on success.
'{"id":1,"cmd":"status"}' | python tools/p4sync/bridge.py --mock
'{"id":2,"cmd":"screenshot"}' | python tools/p4sync/bridge.py --mock
'{"id":3,"cmd":"full-image","mirror":"C:\\temp\\p4g","db":"p4sync","files":true,"shot":true}' |
    python tools/p4sync/bridge.py --mock

# Sibling gate (owns tools/p4sync/**): all commands, deterministic fixtures.
python tools/p4sync_gate.py --check
python tools/p4sync_gate.py --self-test

# Same requests the pages send, with a deep link into each affected page:
dotnet run --project apps/p4sync_gui/src/P4SyncGui -- --mock --page screenshot
dotnet run --project apps/p4sync_gui/src/P4SyncGui -- --mock --page pim
```

## Bridge contract used by the GUI

Requests are one JSON line each: `{"id":N,"cmd":"...",...args}`. Responses are
`{"id":N,"ok":true,"data":{...}}` or `{"id":N,"ok":false,"error":"..."}`.
Events are `{"event":"progress","id":N,"pct":0..100,"message":"..."}` and
`{"event":"log","id":N,"level":"info|warn|error","message":"..."}`.

| Command | Args sent by the GUI | Data consumed |
|---------|----------------------|---------------|
| `devices` | `{}` | `devices[].{port,board,proto,status}` |
| `status` | `port` | `{port,board,proto,sd,lock,caps,display}` |
| `push` | `port,local,remote` | one file per request |
| `pull` | `port,remote,local` | one file per request |
| `screenshot` | `port` | `out`/`path`/`png` (saved PNG), `png_base64`, `sha256`, `size`/`bytes`, `width`, `height` |
| `full-image` | `port,mirror,db,files,shot,force_rebuild` | `manifest`, `manifest_path`, `failures`, `ok`, `stale_manifest`, `rebuilt`, `stale_reason` |
| `verify` | `port,remote,crc32` | `remote`, `ok`, `crc32` |
| `plan` | `local,remote,dry_run,cache` | `actions[].{op,path,reason}`, `summary` |
| `shell` | `port,command,timeout,settle,plain` | `port,command,output,errorlevel,ok` |
| `pkg` | `port,verb,package` | `verb,package,command,output,rows,ok` |

Only the response with a matching `id` completes a request; progress/log events
update the bars and log pane. If a frame is valid JSON but not part of the
contract, it is shown verbatim as an `unknown` event — never guessed into a
field. Bridge stderr is labelled `[bridge stderr]` and kept as text.

### Version refusal

The Dashboard compares the advertised `proto` with `p4sync-1` and validates
`board`, `sd`, `lock`, `magic.sdfx`, `magic.pimx`, `magic.bmpx` and
`limit.transfer` (flattening the `caps` container). `limit.screenshot` is
optional by presence (older firmware does not print it), but **strict when
advertised**: it must parse as a positive byte count, otherwise the version
verdict fails and names the bad value. `bridge.py --mock` advertises
`limit.screenshot: 1048576`, so the strict check goes green offline. A mismatch
or missing core capability shows a red banner and
**refuses every device command** (`status`, `push`, `pull`, `screenshot`, `full-image`, `verify`,
`shell`, `pkg`) until a compatible snapshot is read. Discovery (`devices`)
still works so the user can recover.

## Pages

- **Devices** — bridge-provided board slug / port / state; select one board.
- **Dashboard** — capability snapshot and explicit version refusal.
- **Files** — local listing from disk, device listing from `plan`, dry-run
  steps, conflict-policy display, one-file-at-a-time push/pull with progress.
- **Screenshot** — `screenshot` capture; the preview is decoded from the exact
  PNG bytes returned by the bridge (`png_base64`, or the bridge-written
  `out`/`path` file as fallback). A declared `sha256`/`size` is compared with
  the decoded bytes and the verdict is shown under the image. Malformed base64,
  unreadable paths and bad PNG data are logged with the verbatim exception
  message; no placeholder image is invented. Save-as writes the preview bytes
  to a `.png` file.
- **PIM** — `full-image` mirror sync with Contacts / Calendar / Details tabs.
  A pre-existing `image.json` produces a warning that the bridge will validate
  and rebuild it. After the reply, authoritative `stale_manifest`,
  `rebuilt` and `stale_reason` fields drive a warning banner: stale data is
  removed and rebuilt before rows are displayed. A failed build or a rebuilt
  manifest the bridge still rejects leaves every pane empty rather than
  presenting stale rows as fresh. **Retry clean rebuild** re-runs `full-image`
  with `force_rebuild: true` (advisory; the bridge always regenerates
  `image.json`) after clearing the panes.
- **Apps & Packages** — `pkg` verbs and `verify`; all device commands are
  bridge requests, never direct C# serial.
- **Shell** — command entry with Up/Down history, ANSI/plain toggles, timeout
  and settle controls, and a destructive-command warning that must be
  acknowledged before the command can run.
- **Plan & Report** — dry-run, progress log, last error and verbatim raw JSON.
- **Settings** — bridge path, Python path, mock mode, theme, and OS credentials.

## Credentials

Secrets are stored in **Windows Credential Manager** with a **DPAPI** fallback
under `%APPDATA%\P4SyncGui\credentials`. They are never written to
`settings.json`, a log, or the repository. If a credential exists for the
configured target, it is passed to the bridge child process through
`P4SYNC_CRED_USER` / `P4SYNC_CRED_PASSWORD` environment variables; the GUI
only displays the user name. There is no `wifi_local.json` dependency.

## Cancel

Cancel stops waiting on the current request and restarts the bridge process,
so a transfer cannot be left mid-flight. The next request starts a fresh
bridge. This is reported in the log as "Cancelled by user; bridge was
restarted."

## Known limitations / dependencies

- This app does not validate hardware behaviour; a bridge `--mock` pass is not
  board sign-off.
- `pkg`, `shell`, `full-image` and `verify` depend on those commands existing in
  the sibling's `bridge.py`; errors are displayed verbatim if they do not.
- A preview needs the bridge to return a readable `png_base64` or an
  `out`/`path` PNG; otherwise the GUI logs the exact failure and shows no image.
  A bridge that returns neither field is reported as such, never faked.
- `force_rebuild` is advisory: the sibling bridge currently regenerates
  `image.json` on every `full-image` call and ignores unknown request keys.
- A failed bridge command is printed on the bridge's stderr as diagnostic text;
  the GUI labels it `[bridge stderr]`. Successful screenshot/PIM requests leave
  stderr empty in `--mock`.
- A mock pass is not hardware sign-off: SHA-256/size verification proves only
  that C# previewed the bytes the bridge reported, not that the BMPX capture
  is correct on a board.
- No third-party theme pack: styling is native WPF with Fluent-like rounded
  surfaces, dark/light tokens and labelled controls for accessibility.

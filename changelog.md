# Changelog

All notable changes to P4MiniShell are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.24.17] - 2026-08-11

Persistent known Wi-Fi networks. The firmware now stores a list of previously-
used networks (SSID + password + metadata) on the SD card and automatically
connects to the best known network on every boot, while preserving the classic
single-credential CONFIG.SYS path when the SD card is absent.

### Added - known-network storage (sd:/WIFI.KNOWN)

- `components/networking/wifi_known.c` + `wifi_known.h`: an in-memory cache and
  a persistent, hand-editable text list on SD. File format
  `SSID|PASSWORD|AUTH|PRIORITY|PREFERRED|LAST_CONNECTED|CONNECT_COUNT`, one
  network per line. The file is written atomically (temp + rename) with the
  storage free-space pre-check and partial-file cleanup; entries are capped at
  `P4_CONFIG_WIFI_KNOWN_MAX` (16), evicting the least preferred / lowest-
  priority / oldest entry when full. Duplicate SSIDs are deduplicated
  case-insensitively.
- New commands (each sets ERRORLEVEL, is redirectable, and never prints a
  password): `wifi known` / `wifi list known`, `wifi save [ssid]`,
  `wifi forget <ssid>` / `wifi delete <ssid>`, `wifi forget all` /
  `wifi clear known`, and `wifi preferred <ssid>`.
- Boot-time auto-connect: after the STA runtime is up, when
  `WIFI_AUTOCONNECT=ON` (the CONFIG.SYS master switch) the firmware loads the
  known list, scans, and connects to the best visible known network (preferred
  / highest priority / strongest RSSI), then falls back to the classic
  single-credential path when the list is empty or no known network is in
  range. The existing watchdog / exponential-backoff machinery retries the
  chosen target.
- Successful connections (interactive `wifi connect`, CONFIG.SYS
  WIFI_SSID/WIFI_PASSWORD, or known-network auto-connect) auto-update the list
  when `P4_CONFIG_WIFI_KNOWN_AUTOSAVE` is enabled and the card is present.

### Changed

- `networking_wifi_set_boot_credentials()` now preserves the other field when
  called with a single credential, so `WIFI_SSID=x` + `WIFI_PASSWORD=y` in
  CONFIG.SYS assemble the pair (and seed the known list) correctly.
- `networking_handle_wifi_command()` returns `esp_err_t`; the command module
  maps it onto ERRORLEVEL (0 success, 1 failure) for the known-list commands.

### Safety (verified on hardware)

- No SD card / eject / missing or corrupt file / read-only or full disk: the
  known-list commands report a clear "unavailable" message, boot and normal
  operation continue, and the single-credential path is used. No crash, no
  freeze, no infinite retry.

### Config

- New tunables in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_WIFI_KNOWN_MAX` (16), `P4_CONFIG_WIFI_KNOWN_FILE` ("WIFI.KNOWN"),
  `P4_CONFIG_WIFI_KNOWN_AUTOSAVE` (1), `P4_CONFIG_WIFI_KNOWN_LINE_BYTES` (256).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): `wifi connect 4G-CPE_5542` auto-saves the network; `wifi
  known` lists it (SSID + preferred/last-used markers, never the password);
  `wifi preferred` / `wifi forget` / `wifi forget all` update the file. With
  CONFIG.SYS `WIFI_AUTOCONNECT=ON`, a reboot automatically reconnects to the
  known network (auto-connect -> associated -> got IP) and refreshes the list.
  Ejecting the SD card makes every known-list command fail gracefully and the
  shell stays responsive. A 400-command soak completed with no stall, no
  watchdog trip, and no panic.
- Note: a pre-existing, unrelated stack-overflow in the `copy` command (three
  path buffers + vfprintf on the 8192-byte worker stack) reproduces on this
  build; it is not introduced by this change and is tracked separately.

---

## [0.24.16] - 2026-08-11

Time / SNTP control release. All time, date, timezone, and NTP behaviour now
lives in the clock component, surfaced by `date`, `time`, `timezone`, and
`sntp`/`ntpsync` shell commands. The clock can be synchronized against an NTP
server, the timezone is settable, and the date/time show is a fuller clock
panel.

### Added - timezone and SNTP control

- `timezone [TZ]` — show the current POSIX timezone string, or set one (e.g.
  `timezone UTC`, `timezone CET-1CEST,M3.5.0,M10.5.0/3`). The local time
  re-renders immediately through the C library.
- `sntp` / `ntpsync` — show NTP sync status (server, synced or not, local
  time). `sntp sync` (alias `ntpsync sync`) forces a fresh NTP exchange against
  the configured server; once Wi-Fi is connected the clock jumps to the network
  time. The NTP server hostname is configurable via `P4_CONFIG_NTP_SERVER`.

### Changed - fuller date/time and clock-component ownership

- `date` and `time` with no argument now show the full clock panel: local time,
  UTC, Unix timestamp, timezone, uptime, and NTP sync status. Their set forms
  (`date MM-DD-YYYY`, `time HH:MM[:SS]`) are unchanged.
- The `date`, `time`, `timezone`, and `sntp`/`ntpsync` command bodies moved
  from `components/command/command.c` into `components/clock/clock_commands.c`
  so every time/SNTP behaviour is owned by the clock component. The clock
  component stays a leaf: the commands render through a `clock_host_ops_t`
  table registered by `command_init()`, whose wrappers map one-to-one onto the
  shell print helpers (output is byte-for-byte identical).
- Removed the now-unused `shell_get_time_string()` and `shell_time_is_synced()`
  wrappers from `components/shell/` (the clock commands call the clock API
  directly).
- `components/clock/clock.h` adds `time_force_resync()`,
  `time_get_ntp_server()`, `time_get_uptime_formatted()`, the
  `clock_host_ops_t` table + `clock_register_host_ops()`, and the
  `clock_command_*()` surface. `clock.c` now reads the NTP server from config.

### Config

- New tunables in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_NTP_SERVER` ("pool.ntp.org") and `P4_CONFIG_TIMEZONE_BYTES` (64).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11, Wi-Fi SSID `4G-CPE_5542`): `date`/`time` show the fuller
  panel; `date 08-11-2026`, `time 12:34:56`, and `timezone CET-1CEST,...`
  (UTC+2 in August) apply correctly; `timezone UTC` restores UTC. `sntp sync`
  over Wi-Fi synchronizes the clock to real NTP time (`Local: 2026-08-11
  07:59:36`, `NTP sync: synced (pool.ntp.org)`). A 400-command soak completed
  with no stall, no watchdog trip, and no panic.
- Note: an intermittent, pre-existing crash in the LVGL transcript apply path
  (`lv_font_get_line_height` on a corrupt span font, seen once across ~13 Wi-Fi
  connect attempts) is unrelated to this change; it reproduces only rarely and
  is not triggered by the clock commands.

---

## [0.24.15] - 2026-08-11

Transcript navigation release. The scrollable transcript now always jumps to the
output of the command you just submitted, and it can be scrolled up and down
from every input surface: dedicated on-screen scroll buttons, USB keyboard
PageUp/PageDown, and the USB mouse wheel.

### Added - transcript scrolling from all input surfaces

- **Auto-follow on submit**: submitting a command (touch keyboard, USB keyboard,
  or the serial console) now forces the transcript to jump to the newest output
  even when the user was reading earlier history. A one-shot force-follow flag
  is consumed by the first repaint of that command's output; background output
  (async Wi-Fi status, etc.) afterwards reverts to the terminal-style
  near-bottom follow, so reading history is never yanked away by background
  messages.
- **On-screen scroll buttons**: the input row now has `Up` / `Dn` buttons next
  to the `Prev` / `Next` history buttons. They page the transcript by
  `P4_CONFIG_TRANSCRIPT_SCROLL_STEP` pixels per press and are always visible,
  independent of the on-screen keyboard.
- **USB keyboard scrolling**: `PageUp` / `PageDown` scroll the transcript by one
  viewport height. Cursor keys keep their existing editing/history roles.
- **USB mouse wheel scrolling**: each wheel notch scrolls the transcript by
  `P4_CONFIG_TRANSCRIPT_SCROLL_STEP` pixels (up notches scroll toward older
  output). The wheel is processed regardless of `usb mouse` echo mode and is
  routed to the LVGL task through the app bridge exactly like the USB keyboard
  injection path.
- Touch drag on the transcript continues to pan it through the container's
  built-in LVGL scroll handling.

### Changed - window manager and shell bridges

- `components/windows/windows.h` adds `windows_force_scroll_transcript_to_end()`,
  `windows_scroll_transcript_by(int32_t)`, `windows_scroll_transcript_to_top()`,
  `windows_get_scroll_up_button()`, and `windows_get_scroll_down_button()`.
  `windows_transcript_apply()` honours the one-shot force-follow flag.
- `components/shell/shell.h` adds `shell_force_transcript_scroll_to_end()`; the
  UART console submit path and the LVGL input-line READY handler (main.c) call it
  instead of the near-bottom follow. `shell_usb_keyboard_input()` handles
  PageUp/PageDown for transcript scrolling.
- `components/usb/usb.c` reads the boot-protocol mouse wheel byte and calls the
  new `usb_host_scroll_transcript()` bridge; `main.c` implements the bridge with
  an `lv_async_call` onto the LVGL task.

### Config

- New tunables in `p4minishell_config.h` / `p4minishell_config.yaml`:
  `P4_CONFIG_TRANSCRIPT_SCROLL_STEP` (60 px per button press / wheel notch),
  `P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX` (32 px near-bottom follow threshold),
  and `P4_CONFIG_WINDOW_SCROLL_BUTTON_WIDTH` (64 px input-row scroll button).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): 400-command soak with no stall, no task watchdog trip, no
  panic, and a responsive shell afterward. Screenshot confirms the input row
  now renders the four buttons (Prev / Next / Up / Dn) with the transcript and
  keyboard unchanged.

---

## [0.24.14] - 2026-08-10

Full printable-ASCII keyboard support. The on-screen LVGL keyboard can now type
every printable ASCII character (0x20-0x7E), including the shell-critical
characters the default LVGL symbols map omitted.

### Added - shell-critical symbols on the on-screen keyboard

- The default LVGL symbols map had no way to type four printable ASCII
  characters that a DOS shell needs: the pipe `|` (pipe operator), the caret
  `^` (the shell escape character), the tilde `~`, and the backtick.
- `components/keyboard/keyboard.c` now installs a custom symbols map via the
  official `lv_keyboard_set_map()` API: it keeps every default symbol and digit
  and adds a fourth symbol row (`^ | ~ ` - _ , . :`). Together with the text
  and number modes, the on-screen keyboard now covers all of 0x20-0x7E.
- The custom map preserves the exact LVGL control-button labels (`"abc"`,
  `LV_SYMBOL_BACKSPACE`, `LV_SYMBOL_NEW_LINE`, `LV_SYMBOL_KEYBOARD`,
  `LV_SYMBOL_LEFT/RIGHT`, `LV_SYMBOL_OK`), so the built-in mode switching and
  character routing work unchanged.

### Verification

- Static coverage check: every printable ASCII character (0x20-0x7E) is now
  typeable across the four keyboard modes.
- Hardware (COM11 + screenshot): the symbols keyboard renders the new fifth
  row; `echo ^| ^~ ^^` prints `| ~ ^`, and tilde/backtick pass through the
  shell pipeline unchanged. Build: 0 errors, 0 warnings for the firmware and
  the test project.

---

## [0.24.13] - 2026-08-10

Transcript colour rendering fix. The on-screen transcript was showing LVGL
recolor markup (`#RRGGBB … #`) as literal visible characters instead of
applying the colours, leaving the shell monochrome.

### Fixed - transcript now renders ANSI colours as a span group

- Root cause: `windows_create_transcript()` created an `lv_label` and relied on
  LVGL's recolor feature (`lv_label_set_recolor(true)`) to interpret
  `#RRGGBB` markup produced by `ansi_to_lvgl_recolor()`. In this LVGL 9.4
  build the markup was not interpreted and was dumped into the visible string,
  so every colour change left a literal `#RRGGBB` token on the display.
- Fix: `components/windows/windows.c` now creates the transcript as an
  `lv_spangroup` (the architecture already documented in `ai-context.md`) and
  parses the raw ANSI SGR text directly into one coloured span per run via
  `ansi_process_text()`. No recolor markup is ever generated, so colour
  control tokens cannot leak into the visible text.
- The deferred rebuild (coalesced `lv_async_call` onto the LVGL task) is
  preserved, so bursty output still paints once per handler pass with no
  render-cycle race.
- Verified on hardware (COM11) with the `screenshot` command: `wifi status`
  and `sysinfo` now render cyan labels, magenta numbers, green headings and
  yellow warnings. Pre-fix screenshot: 0 magenta pixels; post-fix: cyan,
  magenta, green and yellow all present.
- Build: 0 errors, 0 warnings for the firmware and the test project.

---

## [0.24.12] - 2026-08-10

Screenshot feature. Adds the `screenshot` command (aliases `scr`, `capture`) for
retrieving an exact pixel-perfect BMP capture of the current LVGL screen over
the existing UART/USB-Serial-JTAG console or to an SD card file.

### Added - `screenshot [filename.bmp]` (aliases `scr`, `capture`)

- Captures the current LVGL screen using `lv_snapshot_take()` with RGB565 format.
- When no filename is given: streams the complete BMP (54-byte header + RGB888
  pixels) over the UART/USB-Serial-JTAG console with clear magic markers
  (`=== SCREENSHOT BMP BEGIN ===` / `=== SCREENSHOT BMP END ===`) for
  easy host-side extraction.
- When a filename is given: writes the BMP to the SD card using the existing
  storage path (cwd-relative resolution, guarded session, free-space precheck,
  partial-destination cleanup on failure).
- BMP format: 24-bit RGB888, bottom-up, BI_RGB (no compression), 96 DPI,
  zero extra libraries required — opens in any image viewer.
- Uses the LVGL lock (`lvgl_port_lock(0)`) exactly as the rest of the
  codebase does, so it is safe from any task context.
- Sets ERRORLEVEL: 0 on success, 1 on failure (snapshot failed, PSRAM
  exhausted, SD write error, invalid path), 2 on usage error.
- Works in batch files and AUTOEXEC.BAT. Supports redirection to capture
  the transcript output.
- Graceful degradation: PSRAM allocation failures produce clear DOS-style
  errors; LVGL lock acquisition failures are handled; SD card absence is
  reported before any file operations.

### Config

- New tunables in `p4minishell_config.h` and `p4minishell_config.yaml`
  under `screenshot`: display dimensions (1024x600), color format
  (RGB565), BMP begin/end markers, and UART chunk size (512 bytes).
- Enabled `CONFIG_LV_USE_SNAPSHOT=y` in `sdkconfig.defaults`.

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): screenshot command captures the screen, streams the BMP
  over serial with correct magic markers, and saves to SD card successfully.
  The BMP opens correctly in image viewers showing the exact screen content.

---

## [0.24.11] - 2026-08-10

HTTPS support release. Adds the simplest possible, lowest-risk basic HTTPS
capability for development, diagnostics, and scripting: a `httpget` / `wget`
command that performs a plain HTTPS or HTTP GET by reusing the exact
`esp_http_client` stack c6ota already uses for firmware downloads. No new HTTP
library, no POST/PUT, no WebSocket, no certificate-pinning UI, and no change
to the station-only Wi-Fi model.

### Added - `httpget <url> [localfile]` (alias `wget`)

- Performs a simple HTTPS (or HTTP) GET. With no localfile it prints the
  response body to the transcript (bounded, sanitized) after a clear header
  showing HTTP status, content-type, and size; with a localfile it saves the
  exact body to the current working directory / SD card through the storage
  write path (cwd-relative resolution, guarded SD session, free-space
  precheck, partial-destination cleanup on a failed write).
- Sets ERRORLEVEL like DOS: 0 on an HTTP 2xx, 1 on any failure (non-2xx,
  connection refused, TLS failure, body over the size cap, unreachable
  host, Wi-Fi not connected), 2 on a usage error. Works in batch files and
  AUTOEXEC.BAT (`httpget ... && echo ok`, `if errorlevel 1 goto nolink`).
- Supports redirection and pipes: `httpget https://host/page > page.txt`
  captures the printed report; `httpget url | find "200"` pipes it.
- Graceful degradation: with no active connection it prints a clear
  DOS-style error (`run wifi connect first`) and exits non-zero; a network
  timeout is bounded by `P4_CONFIG_HTTP_TIMEOUT_MS` so the worker task is
  never hung.
- All HTTP / TLS code lives inside `components/networking/`
  (`networking_http_get()`), the sole owner of the esp_http_client surface;
  `components/command/` only dispatches and writes the returned body. The
  large receive buffer is allocated from PSRAM
  (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`) with an internal-RAM fallback.

### Config

- New tunables in `p4minishell_config.h`, documented in
  `p4minishell_config.yaml` under `http_client`:
  `P4_CONFIG_HTTP_TIMEOUT_MS` (15 s), `P4_CONFIG_HTTP_MAX_BODY_BYTES`
  (512 KiB PSRAM cap), `P4_CONFIG_HTTP_FOLLOW_REDIRECTS` (1), and
  `P4_CONFIG_HTTP_USER_AGENT` ("P4MiniShell/0.24.11 httpget"), plus
  `P4_CONFIG_HTTP_PRINT_BODY_BYTES` (4 KiB transcript print cap).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- The only CMake change is adding `esp_http_client` and `esp-tls` to the
  networking component's `REQUIRES`. No SoftAP, new transports, custom RPC,
  or third-party libraries were introduced; station-only Wi-Fi is untouched.
- Hardware (COM11): connected to a 2.4 GHz access point and fetched an HTTPS
  page and an HTTP text endpoint, verifying status/content-type/size output,
  ERRORLEVEL on success, the SD-file save form, the transcript print form,
  and the non-connected error path.

---

## [0.24.10] - 2026-08-10

Networking expansion release. Enriches the Wi-Fi and Bluetooth surfaces so
they feel native to the DOS environment: a full colour-coded `wifi status`
report, an RSSI-sorted `wifi scan` with a bare redirectable form, classic
`ping` and `dns`/`nslookup` connectivity commands that set ERRORLEVEL and
participate in redirection/pipes, and improved hosted Bluetooth status, scan,
and session-scoped advertising names.

### Added - `ping <host-or-ip> [count]`

- Classic ICMP echo over lwIP's `esp_ping` session (inside
  `components/networking`, the sole owner of the lwIP surface). Default 4
  requests, hard cap 10 (`P4_CONFIG_PING_COUNT_MAX`), 1 s timeout and 1 s
  interval.
- Works from the interactive shell **and** from batch files / AUTOEXEC.BAT.
  Sets ERRORLEVEL exactly like DOS: 0 when at least one reply landed, 1 on
  total loss / resolution failure / no connection, 2 on a usage error, so
  `ping 8.8.8.8 && echo up` and `if errorlevel 1 echo down` behave.
- Output goes through the transcript appenders, so `>` / `>>` redirection and
  `|` pipes capture it: `ping 8.8.8.8 > ping.txt`, `ping gw | find "Reply"`.
- Prints each reply line plus the classic summary: packets transmitted /
  received / lost, loss %, and RTT min/avg/max.
- Bounded by construction: the session runs on its own task and the worker
  task blocks for at most `count * (timeout + interval) + margin`, so it
  never hangs the command worker task.
- Requires an active connection; reports Wi-Fi-not-started / not-connected
  states honestly and redirectably.

### Added - `dns <hostname>` (alias `nslookup`)

- Resolves A records through lwIP `getaddrinfo` and prints the IPv4 address
  list (bounded by `P4_CONFIG_DNS_RESULT_LIMIT`).
- Errorlevel-aware like `ping` (0 success / 1 not resolved / 2 usage) and
  redirectable: `dns example.com > dns.txt`, `dns host || echo unresolved`.
- With this build's lwIP DNS cache (`CONFIG_LWIP_DNS_MAX_HOST_IP=1`) a name
  normally resolves to a single A record.

### Added - enriched `wifi status`

- Multi-line colour-coded report: state, connected, target SSID, SSID, BSSID,
  channel, RSSI (dBm), PHY mode + bandwidth (802.11b/g/n/ax + HT20/HT40), IPv4,
  netmask, gateway, DNS server(s), and association uptime (HH:MM:SS since the
  4-way handshake completed).

### Added - improved `wifi scan`

- Results sorted by RSSI (strongest first), column-aligned
  `SSID  RSSI  CH  AUTH` report, capped at the new `P4_CONFIG_WIFI_SCAN_LIMIT`
  (32) so a busy channel cannot flood the transcript.
- Optional `wifi scan /b` prints bare SSID lines (no colour) so
  `wifi scan /b > ap.txt` is machine-parsable from a batch file.

### Added - enriched hosted Bluetooth

- `bluetooth status` / `bt status` is now a colour-coded report: hosted
  ready, controller, NimBLE host, sync, scan, advertising (with the active
  name), C6 firmware version, and last error.
- `bluetooth scan [limit]` is a bounded scan (default 8 s,
  `P4_CONFIG_BT_SCAN_DURATION_MS`) that prints a sorted-by-RSSI
  `NAME  ADDRESS  RSSI` report with the optional per-run limit.
- `bluetooth advertise on [name]` accepts a session-only advertising name
  (RAM-only, never persisted); `bluetooth advertise off` and the boot
  `BT_ADVERTISE=ON|OFF` path are unchanged.

### Documentation

- `command.md` documents `ping`, `dns`/`nslookup`, the `wifi scan /b` and
  enriched status/scan forms, and the new Bluetooth forms. `documentation.md`,
  `ai-context.md`, `readme.md`, `p4minishell_config.h`, and
  `p4minishell_config.yaml` document the new lwIP/esp_ping surface and the new
  config macros (`P4_CONFIG_WIFI_SCAN_LIMIT`, `P4_CONFIG_PING_*`,
  `P4_CONFIG_DNS_RESULT_LIMIT`, `P4_CONFIG_BT_SCAN_DURATION_MS`).

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Networking code stays confined to `components/networking`; the only
  CMake change is adding `lwip` to that component's `REQUIRES`. No SoftAP,
  Bluedroid, custom RPC, or non-ESP-Hosted transport was introduced.

---

## [0.24.9] - 2026-08-10

Batch mathematics release. Expands `set /a` with the comparison and logical
operators cmd.exe users expect, adds numeric comparison keywords to `if`, and
fully documents the batch math surface. No regressions to the existing
arithmetic, bitwise, shift, compound-assignment, or `if` forms.

### Added — `set /a` comparison operators

- `==`, `!=`, `<`, `>`, `<=`, `>=` now evaluate to 1 when the relation holds
  and 0 otherwise, so a boolean can be computed and stored:
  `set /a x=5==5` sets x=1, `set /a ok=(5>3)&&(2<4)` sets ok=1.
- Precedence follows cmd.exe: comparisons sit below the bitwise operators
  (`|` `^` `&` and the shifts bind tighter), so `set /a "x=1|0==0"` is
  `(1|0)==0` → 0.
- The assignment splitter no longer mistakes the `=` of a comparison for the
  assignment: `set /a x=5==3` assigns x the comparison result (0) instead of
  splitting into `x=5` with a dangling `==3`. An expression with no assignment
  (`set /a 5==3`) is evaluated and printed.
- **Quoting:** `<`, `>`, `&`, `|`, `<<`, `>>`, `&&` and `||` are shell
  redirection/chain/pipe operators, so an expression using them must be quoted
  (`set /a "x=5<6"`) or the line is split first. `==` and `!=` contain no
  shell operator and work unquoted. This matches how the pre-existing bitwise
  operators already behaved.

### Added — `set /a` logical operators

- `&&` and `||` return 1/0 and form the lowest precedence levels (`&&` binds
  tighter than `||`, so `1||0&&0` is `1||(0&&0)` → 1). Both sides are always
  evaluated (no short-circuiting), matching cmd.exe: `set /a "(0&&1/0)"`
  reports a divide-by-zero error.
- The bitwise `&`/`|` levels continue to refuse `&&`/`||`, so a chain separator
  that survives as shell syntax is never swallowed by the expression.

### Added — `if` numeric comparison keywords

- `if [not] [/i] <a> EQU|NEQ|LSS|LEQ|GTR|GEQ <b> <command>` performs a numeric
  comparison. Operands are parsed as decimal integers; a non-numeric operand
  reads as 0, matching cmd.exe. This is a separate branch from the `==` string
  comparison, so `if a==b` stays a string test while `if a EQU b` compares
  numerically. `/i` is ignored for the numeric form.

### Documentation

- `command.md` gained a full `set /a` operator/precedence table, quoting
  rules for shell-conflicting operators, and an `if` numeric-keyword
  reference with examples. `documentation.md`, `ai-context.md`, `API.md`,
  `readme.md`, and `batch.h` document the expanded grammar and the
  `shell_expr_find_assignment()` splitter.

### Testing

- New unit tests `test_batch_expr_comparisons` and `test_batch_expr_logical`
  cover every comparison, `&&`/`||` precedence, parenthesised logical
  expressions, the no-short-circuit rule, and hex/`0x` comparisons.
- Hardware (COM11): exercised `set /a` with `==`/`!=` (unquoted) and
  `<`/`>`/`<=`/`>=`/`&&`/`||` (quoted), mixed precedence, `%%` modulo and
  `^^` xor escapes, every `if` numeric keyword (including `not` and
  variables), and a batch file that computes a total, branches on `EQU`, and
  stores a computed boolean. Full unit-test suite: 0 failures, 0 ignored.

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- CONFIG.SYS / AUTOEXEC.BAT boot path, transcript colours, key waits, and the
  serial console are unchanged.

---

## [0.24.8] - 2026-08-10

Batch correctness release. Fixes the six batch features after the frame layout
change that introduced `%0` (script name) broke `%1..%9` / `%*` argument
forwarding, tightens `goto :eof` frame scoping, and moves the `for` loop
buffers off the recursive command/batch stack.

### Fixed — `%1..%9` / `%*` argument forwarding after the `%0` change

- **Symptom:** `call script.bat one two` ran the script, but inside the callee
  `echo %1` printed the script path instead of `one`, `%2` printed `one`, and
  `%*` included the script name.
- **Root cause:** `shell_execute_batch_file()` was updated to store the script
  path in `args[0]` (classic DOS `%0`) and shift the caller's arguments into
  `args[1..]`, but `shell_expand_variables()` still indexed `%N` as
  `args[N-1]` and built `%*` from `args[0]`. The two disagreeing views made
  every positional reference off by one. `%0` was also unreachable: the
  `isdigit()` branch caught `'0'` first and mapped it to an out-of-range index.
- **Fix:** `shell_expand_variables()` now maps `%0` → `args[0]`, `%1..%9` →
  `args[1..9]`, and `%*` → every argument from `%1` onward (never the script
  name), matching COMMAND.COM. `%0` is tested before `isdigit()`. A single
  `shell_batch_all_args_string()` helper builds `%*` for both the closed
  (`%*`) and bare (`%*` with no trailing `%`) forms.
- **Verification (hardware, COM11):** `call` with `%0 %1 %2 %*` prints the
  script name, the two forwarded arguments, and exactly the two arguments for
  `%*`; `shift` keeps `%1` aligned with the frame slots.

### Fixed — `goto :eof` now unwinds only the current frame from `for`/`if` bodies

- **Symptom:** `goto :eof` (and `goto`) issued inside a `for` loop body in a
  called script terminated the caller too: after the callee returned, the
  caller's line loop broke out and the whole batch run stopped early.
- **Root cause:** a `goto` inside a `for`/`if` body breaks the current frame's
  line loop before the normal flag-clearing path runs, leaving
  `s_goto_pending` / `s_goto_eof` set. The caller's loop then saw the stale
  flags and broke as well.
- **Fix:** `shell_execute_batch_file()` clears `s_goto_pending`,
  `s_goto_eof`, and `s_goto_label` whenever a frame returns. A goto target
  always belongs to the frame that set it (labels are resolved against that
  frame's label table), so the flags are stale after the frame is gone.
- **Verification (hardware, COM11):** a callee with `for %%I in (x) do goto
  :eof` returns without running further callee lines and the caller continues
  to its next line.

### Fixed — `goto :eof` accepted case-insensitively

- `goto :eof` matched with `strcmp`, so `goto :EOF` fell through to the label
  scan and reported "label not found". Now compared case-insensitively, like
  every other batch label.

### Fixed — `for %%I in (set) do ...` accepted the DOS `%%` form

- The loop body substitution only matched a single `%var`, so the correct
  batch-file spelling `for %%I in (a b c) do echo %%I` produced `echo %a`
  (the doubled percent collapsed to one and the variable was lost) instead of
  `echo a`. The substitution now matches `%%var` first (the batch-file escape
  for a literal `%`), then `%var` for tolerance. Verified on hardware:
  `for %%I in (alpha beta gamma) do echo ITEM_%%I` prints ITEM_alpha, ITEM_beta,
  ITEM_gamma.

### Fixed — `%N` / `%0` / `%*` recognized anywhere in a line

- Positional arguments were only expanded when they were the final percent
  marker in the line (or a whole `%...%` pair), so `p0=[%0] p1=[%1]` printed
  `p0=[%0]` literally (a later `%` collapsed `%0] p1=[` into one unknown
  token). They are now consumed as `%` plus one character immediately,
  matching COMMAND.COM, so `[%0] [%1]` expands both.

### Fixed — nested `call` + command overflowed the worker task stack

- **Symptom:** a batch file that `call`ed a second script, and the callee ran a
  `set`/`echo`, panicked with `Stack protection fault` in the `shell_cmd`
  task — the recursive dispatch chain plus newlib's `vfprintf` machinery
  exceeded the 8192-byte worker stack.
- **Root cause:** the recursive-path functions kept SD-path/line-sized buffers
  as stack locals that stacked with nesting depth: `shell_resolve_batch_path`
  (1040-byte frame), `shell_command_set` (512), `shell_command_echo` (416,
  a full `SHELL_BATCH_LINE_BYTES` text buffer), `shell_command_if` (464),
  `shell_command_choice` (736). A two-level `call` with a `set` in the callee
  was enough to overflow.
- **Fix:** every line/path-sized buffer on the recursive batch path is now
  heap-allocated and freed on every exit: `shell_resolve_batch_path`
  (1040 → 160 bytes of stack), `shell_command_set`, `shell_command_echo`
  (416 → 32), `shell_command_set`/`set /a`/`set /p`/`path` statements,
  `shell_command_if`'s resolved path + nested command, and
  `shell_command_choice`'s prompt text (736 → 352).
- **Verification (hardware, COM11):** the exact repro (call → callee runs
  `setlocal`, `set`, `goto :eof`) runs clean with no stack fault; nested
  `call` chains with `set`/`echo`/`if` in the callee all complete.

### Fixed — UART console dropped the tail of long commands

- **Symptom:** commands past ~64 bytes were split into two commands, the second
  becoming an "Unknown command" (e.g. a long `write`/`append` line lost its
  final characters, corrupting batch files created from the serial console —
  this was why the previous test batch files on the SD card were truncated).
- **Root cause:** the USB-Serial-JTAG driver delivers one logical line across
  several reads (its RX FIFO is 64 bytes). The console task treated each read
  as a complete command.
- **Fix:** `shell_uart_console_task()` now assembles partial reads until a line
  terminator (or the buffer fills), and only then submits the command. Key-wait
  forwarding is unchanged: during a key wait the first newly-read character is
  still answered immediately. Verified on hardware: `write`/`append` of 65+
  character lines lands intact, and `pause`/`choice` are still answered by a
  single key.

### Fixed — unit-test app crashed after the debug-log suite

- The test app ran all suites with 0 failures, then panicked. Two pre-existing
  issues, previously masked by the crash, are fixed so the suite runs clean:
  - `shell_schedule_transcript_appendf()` called `lv_async_call()` even before
    any UI exists; in the test app LVGL is never initialized, so the flush
    crashed in LVGL's TLSF allocator. When the transcript widget is NULL the
    flush now runs synchronously instead.
  - The synchronous flush used the async callback's 2048-byte stack scratch,
    overflowing the test app's main task. The scratch is now heap-allocated.
  - The `test_ansi_to_lvgl_recolor` assertion expected plain text to pass
    through uncoloured, but the recolor path deliberately wraps every run in
    `#CCCCCC text #` (the transcript label has no explicit text colour, so that
    is what keeps plain text visible on the dark background). The assertion was
    corrected to match the documented rendering contract.
- **Verification (hardware, COM11):** the full unit-test suite runs to
  "All tests completed" — 56 tests across 11 suites, 0 failures, 0 ignored,
  no panic.

### Verification

- Clean build: 0 errors, 0 warnings for the firmware and the test project.
- Hardware (COM11): exercised `if exist` / `if not exist`, `if errorlevel N`,
  `goto :eof`, `for %%I in (set)` and wildcard `for`, `if /i`, and `call` with
  argument forwarding + errorlevel propagation; CONFIG.SYS / AUTOEXEC.BAT boot
  path, transcript colours, key waits, and serial console behaviour unchanged.

---

## [0.24.7] - 2026-08-10

Feature release. Adds DOS-style boot scripting (`CONFIG.SYS` + `AUTOEXEC.BAT`)
that runs at every boot.

### Added — DOS-style boot scripting (`CONFIG.SYS` / `AUTOEXEC.BAT`)

- **Symptom this replaces:** previously the shell had no boot-time
  configuration; every setting had to be typed or scripted by hand after
  power-on.
- **Behavior:** on every boot the firmware looks for `CONFIG.SYS` and
  `AUTOEXEC.BAT` on the SD card root. When either is missing and
  `P4_CONFIG_BOOT_GENERATE_DEFAULTS` is set, default files are written once.
  `CONFIG.SYS` directives are parsed and applied (classic DOS: `SET`, `PATH`,
  `PROMPT`, `ECHO ON|OFF`; display/audio: `ROTATE`, `BRIGHTNESS`,
  `DISPLAY_POWER`, `VOLUME`; policy: `WIFI_SSID`/`WIFI_PASSWORD`/
  `WIFI_AUTOCONNECT`, `BLUETOOTH`, `BT_ADVERTISE`, `USB_KEYBOARD`,
  `USB_MOUSE`, `GPIO`). `AUTOEXEC.BAT` then runs through the normal batch
  pipeline with full batch power. Unknown directives produce a single muted
  warning and are skipped. Safe with no SD card (silent skip, identical to
  before) and with empty/malformed files.
- **Implementation:** `components/boot/` owns the parser and runner
  (`components/boot/boot.c`, `boot.h`). Hardware directives are applied by
  executing their command-line equivalent through the batch pipeline, so every
  existing validation path is reused and no private state is reached into.
  State-only directives (Wi-Fi target credentials, autoconnect policy, the
  echo default) use the small set of accessors the owning modules expose
  (`networking_wifi_set_boot_credentials`/`_autoconnect`,
  `batch_set_default_echo`). Wi-Fi password is never echoed to transcript,
  history, or debug log. GPIO directives refuse reserved pins (delegated to
  the existing `gpio set` safety check). Serial console key waits now work:
  `shell_uart_console_submit_command()` routes commands through the command
  worker task so blocking key waits (`pause`, `choice`, `more`, the
  `format`/`disk clean` confirmation) can be answered from serial input.
- **Configuration:** `p4minishell_config.h` adds
  `P4_CONFIG_BOOT_CONFIG_SYS_NAME`, `P4_CONFIG_BOOT_AUTOEXEC_BAT_NAME`,
  `P4_CONFIG_BOOT_GENERATE_DEFAULTS`, `P4_CONFIG_BOOT_RUN_ON_STARTUP`,
  `P4_CONFIG_BOOT_LINE_BYTES`, `P4_CONFIG_BOOT_MAX_DIRECTIVES`,
  `P4_CONFIG_BOOT_MAX_GPIO_LINES`; all documented in
  `p4minishell_config.yaml` under `boot_scripting`.
- **Verification (hardware, COM11):** `CONFIG.SYS` with `ECHO OFF`,
  `SET TESTVAR=helloboot`, `BRIGHTNESS=50`, `ROTATE=0`, `VOLUME=30`,
  `WIFI_AUTOCONNECT=OFF`, `USB_KEYBOARD=ON`, `GPIO 42 = OUT HIGH` (refused:
  reserved pin), and an unknown directive (muted warning) — all applied as
  expected; `AUTOEXEC.BAT` with `@echo off` +
  `echo testvar=%TESTVAR%` ran and expanded the variable; unit-test suite
  runs 0 failures.

---

## [0.24.6] - 2026-08-09

Crash-fix + stability release. Root-caused and fixed the recurring intermittent
LVGL crash (C1), upgraded LVGL to 9.4.0, fixed the deterministic `dir` crash
(C2), and swept all remaining literal ANSI markers (H1 residual).

### Added — diskpart / DOS FORMAT disk and volume management (`format` + `disk`)

- **`format` is now a real FORMAT.COM.** It keeps `/FS:`, `/V:label`, `/Q` and
  the exact-`YES` confirmation contract, and adds `/A:size` (allocation unit /
  cluster size, with K/M suffix). `/FS:` is honored honestly: FAT/FAT32 use the
  standard ESP-IDF helper's size-appropriate selection (FAT12/16 for small
  volumes, FAT32 for modern SD cards), and `EXFAT` is refused with a clear
  "not supported in this firmware build" warning that falls back to FAT32
  rather than silently lying. After formatting the command reports the FAT
  type, label, capacity and cluster size with the semantic palette.
- **New `disk` command family** (diskpart-style), owned by `components/storage`:
  - `disk list` — physical disk geometry (name, capacity, sectors, sector size).
  - `disk detail` — disk geometry plus the decoded MBR partition table (boot
    flag, type with a friendly name, start LBA, size).
  - `disk clean` — remove the partition table (destructive, `YES` required).
  - `disk create partition primary [size=N]` — create a primary FAT32 MBR
    partition aligned to 1 MiB; `size` is in MB (default: rest of the card).
  - `disk delete partition N` — delete MBR partition N (1-4), `YES` required.
  - `disk format [fs=...] [label=...] [au=...] [quick]` — diskpart-style alias
    for the `format` engine.
- **Engine:** `components/storage/storage.c` gained the volume services
  (`storage_disk_get_info`, `storage_disk_read_mbr`, `storage_disk_clean`,
  `storage_disk_create_primary_partition`, `storage_disk_delete_partition`,
  `storage_format_volume`, `storage_get_fat_type`), parameterized by a
  `storage_volume_t` so a future USB OTG MSC volume can be added without
  changing the command surface. Formatting uses the standard
  `esp_vfs_fat_sdcard_format_cfg()`; MBR access uses `sdmmc_read_sectors` /
  `sdmmc_write_sectors` on the BSP card handle; the FATFS volume is unmounted
  (`f_mount`) before partition-table writes and recreated by `format`.
- **Serial console key waits now work.** `shell_uart_console_submit_command()`
  routes serial commands through the command worker task (a new
  `execute_command_async` hook in `shell_command_ops_t`) instead of executing
  synchronously on the UART task, so blocking key waits - `pause`, `choice`,
  `more`, the `format`/`disk clean` confirmation, `set /p` - can be answered
  from the serial input. The touch path is unchanged.
- **Configuration:** `P4_CONFIG_FORMAT_ALLOC_UNIT_MIN`/`_MAX`,
  `P4_CONFIG_DISK_PARTITION_ALIGN_SECTORS`, `P4_CONFIG_STORAGE_VOLUME_MAX`
  added to `p4minishell_config.h` and documented in
  `p4minishell_config.yaml`.
- **Verification (hardware, COM11):** `format /FS:FAT32 /V:TEST /A:64K`,
  `format /FS:FAT32`, `disk format fs=fat32 label=DATA au=32K quick`,
  `disk clean`, `disk create partition primary size=1024/2048`,
  `disk delete partition 1`, `disk list`, `disk detail` all execute with the
  expected result and ANSI colours; `disk detail` verifies the created MBR
  (type 0x0C FAT32, 1 MiB-aligned start, requested size); the
  clean → create → format → `dir` flow works end-to-end; unit-test suite
  runs 0 failures. Known limitation: a card the BSP cannot mount after a
  reboot (e.g. a partition with no filesystem) cannot be re-initialized
  in-firmware without `BOARD_CFG_SD_FORMAT_ON_MOUNT_FAIL` - the pre-existing
  constraint that formatting requires an initialized card.

### Fixed — LVGL task hang: UI freezes after boot, touch keyboard unresponsive

- **Symptom:** the board boots, boot and Wi-Fi messages render on the display
  with colours, then the whole UI freezes: the touch keyboard stops echoing
  input and no command output appears on screen. The serial console keeps
  working. On hardware the LVGL task spins forever inside `lv_timer_handler`
  and the FreeRTOS task watchdog fires (`taskLVGL` running on CPU 0, IDLE0
  starved) until the board resets.
- **Root cause:** `windows_set_transcript_text()` applied the transcript label
  **synchronously on the calling task**: `ansi_to_lvgl_recolor()` +
  `lv_label_set_text()` + `lv_obj_update_layout()` + `lv_obj_scroll_to_y()`
  were executed from the command worker task, the UART console task, and the
  networking background task while the LVGL task was mid-render. Applying a
  large flex-grow label's text and forcing its layout from a non-LVGL task
  races the LVGL render cycle and hangs `lv_timer_handler`, freezing the whole
  surface. Reverting the transcript to a plain `lv_textarea` (which never
  forced layout/scroll) confirmed the label update path was the trigger.
- **Fix:** `components/windows/windows.c` — the label update is now **deferred
  to the LVGL task**. `windows_set_transcript_text()` converts the ANSI text to
  recolor markup into a persistent staging buffer and schedules a coalesced
  `lv_async_call`; the callback runs on the LVGL task where it paints the
  newest staged content (`lv_label_set_text` + layout + scroll-to-end). Bursts
  of output coalesce into one apply per handler pass. `windows_scroll_
  transcript_to_end()` schedules the same apply instead of doing layout/scroll
  from a non-LVGL task. On-screen colours are unchanged.
- **Verification:** 5 minutes of continuous command output (`help`, `sysinfo`,
  `dir /s`, `wifi diag`, `keyboard show`/`hide`/`status`, `clear`, `echo`,
  `set /a`) on hardware with **zero** task-watchdog triggers and zero panics;
  unit-test suite runs clean.

### Fixed — transcript stops updating after the first command

- **Symptom:** after the hang fix, the first command after boot renders on the
  display, but the second command's output never appears on screen (the serial
  console still shows it).
- **Root cause:** the LVGL recolor markup (`#RRGGBB text #`) inflates the raw
  ANSI text by roughly 1.5-2x because every colour change carries an open/close
  marker. The staging buffer was sized at `P4_CONFIG_TRANSCRIPT_BYTES` (8192),
  so after boot + the first command the buffer was already full
  (`ansi_to_lvgl_recolor` reported 8188 bytes). The converter dropped every
  segment that did not fit — the **tail** — so the newest command output was
  the part that vanished from the label.
- **Fix:**
  1. `p4minishell_config.h` / `p4minishell_config.yaml` — added
     `P4_CONFIG_TRANSCRIPT_RECOLOR_BYTES` (2x the plain transcript) so a full
     8 KiB scrollback fits in recolor form (verified: 9179/9538-byte markups
     render instead of being cut at 8192).
  2. `components/ansi/ansi.c` — `ansi_to_lvgl_recolor()` now keeps the
     **newest** segments: when the buffer cannot hold the whole transcript it
     discards what is staged and restarts from the current segment, so the
     newest lines stay visible even under pathological colour density.
- **Verification:** 3 minutes of sequential commands (`help`, `mem`, `sysinfo`,
  `dir`, `dir /s`, `wifi diag`, `ver`, `echo`) on hardware — the transcript
  label reaches a full-length recolor markup, scrolls to the bottom
  (`scroll_bottom == 0`, view at the newest output), with zero task-watchdog
  triggers and zero panics.

### Fixed — recurring intermittent LVGL crash (C1)

- **Symptom:** after `wifi connect` and other header-touching commands, an
  intermittent `Guru Meditation` in the LVGL task:
  - `lv_obj_get_style_prop` ← `trans_anim_start_cb` / `trans_anim_completed_cb`
    ← `anim_timer`, with a corrupted transition descriptor (`tr->obj` = `0xdac`,
    `selector` = `0x10000`), or `anim_timer` calling a freed animation's
    `exec_cb` (jumping to the LVGL pool base). Repro: 3-7 crashes / 10 trials on
    `sd info; sysinfo; if exist /sdcard echo ok; bluetooth advertise on`.
- **Root cause:** the LVGL default theme's **style-transition animations**
  (`LV_THEME_DEFAULT_TRANSITION_TIME=80`). Widgets restyled on every refresh
  (header, transcript, input line, keyboard) churn pending 80 ms transitions;
  under load (Bluetooth NimBLE init) churned transition descriptors are freed
  and reused while their animation still references them, corrupting the LVGL
  pool. Not a locking or object-lifecycle bug — no object deletion and an
  intact pool free-list were confirmed during diagnosis.
- **Fix:**
  1. Upgraded LVGL from 9.2.2 → 9.3.0 → **9.4.0** (large upstream bug-fix
     release; `main/idf_component.yml` and `test/main/idf_component.yml` pin
     `lvgl/lvgl: "9.4.0"`).
  2. Disabled the risky theme style-transition animations with
     `CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0` (durable in
     `sdkconfig.defaults`). Style changes are now instant — the safe
     replacement for the transition churn. Scroll, cursor, and interaction
     behaviour are unaffected (they are separate from style transitions).
- **Verification:** 0 crashes / 14 trials of the exact repro, 0 / 8 trials of
  the full stack (wifi connect + bluetooth + header commands + `dir /s`), and
  Wi-Fi connected to the test AP (`4G-CPE_5542`, IP 192.168.199.225) with no
  crash.

### Fixed — deterministic `dir` crash (C2)

- **Symptom:** every `dir` in `/sdcard` printed ` Directory of /sdcard` and then
  panicked with `Load access fault`. The register dump showed `strlen` being
  called with the ASCII value `"test"` (0x74736574) as its string pointer — the
  first four bytes of the `test.txt` entry name — which then dereferenced that
  garbage address.
- **Root cause:** `components/storage/storage_commands.c` — the per-entry colour
  formatting in `shell_dir_list_one()` called the *va_list-taking* variant
  `ansi_vformat(coloured_name, sizeof(coloured_name), colour_fmt, display_name)`
  but passed `display_name` (a `char *`) where a `va_list` is expected. Inside
  `ansi_vformat()` the `%s` handler ran `va_arg(args, char *)`, which treated the
  first four bytes of the `display_name` array as a pointer — `"test"` — and
  handed it to `snprintf`, whose `%s` processing called `strlen("test")` and
  faulted.
- **Fix:** use the varargs wrapper `ansi_format()` at that call site so the
  entry name is passed as a value argument, exactly like every other call site in
  the codebase (an audit confirmed all remaining `ansi_vformat()` callers pass a
  real `va_list`). The entry colour (`SH_FILE`/`SH_DIR`/`SH_EXE`) still renders
  as real SGR escapes.
- **Verification:** 10 rapid stress trials (`dir`, `dir /w`, `dir /s`,
  `dir /b /s`, `chkdsk /F`, `tree`, wifi status, rotation) all booted and ran
  with zero Guru Meditation faults; `dir` now prints `test.txt` correctly with
  colours.

### Fixed — residual literal ANSI `@` markers (H1)

- A hardware sweep over every command still found literal `@y(unsynced)@R`,
  `@Kno@R`, `@c...@K...@R` markers that the v0.24.3 fix missed, all caused by
  palette macros passed as `%s` argument values (which `ansi_vformat` leaves
  unconverted by design) instead of in the format string:
  - `sysinfo` / `about` — `time ... (unsynced)` and `idf: unknown`.
  - `wifi status` — `connected=@Kno@R`.
  - `battery` / `battery sleep` — `light sleep requested=@Kno@R`.
  - `display power` — `display.power: @Koff@R`.
  - `bluetooth status` — `bluetooth_appendf()` used `vsnprintf()` (never
    converts `@`) before the ANSI append; the whole line showed markers.
- **Fix:** moved every palette marker into the format string (branching on
  state) so `ansi_vformat` converts them, and `bluetooth_appendf()` now routes
  through `ansi_vformat()` like the networking module.
- **Verification:** hardware sweep of `dir`, `sd info|stat|ls`, `wifi status|diag`,
  `keyboard status`, `ver`, `sysinfo`, `about`, `mem`, `usb status`, `battery`,
  `bluetooth status`, `display power` — zero literal `@` markers remain.

### Fixed — LVGL transcript was monochrome (colours only on UART)

- The LVGL transcript was a plain `lv_textarea` fed with stripped text, so the
  on-screen shell rendered every line in a single colour while the UART console
  showed the full palette.
- **Fix:** the transcript is an `lv_spangroup`; `windows_set_transcript_text()`
  parses the ANSI text and creates one span per coloured run with
  `lv_style_set_text_color()`, so each colour renders directly on screen (no
  markup to misinterpret). `shell.c` keeps a parallel ANSI buffer
  (`s_transcript_ansi`) alongside the plain one and hands it to the window
  manager on every update.
- **Deferred rebuild:** because rebuilding the span group deletes and recreates
  spans, doing it synchronously from an LVGL event (e.g. on-screen keyboard
  submit) left LVGL's `lv_draw_span` referencing freed spans — a `Load access
  fault` after keyboard input. The rebuild is now buffered and deferred via
  `lv_async_call` so it runs after the current redraw pass.
- **Verification:** firmware boots, colours render on the display, and the
  previously-crashing keyboard-input sequence (`keyboard show`/`hide` + `help` +
  `dir` + `sysinfo`) runs clean.

### Fixed — shell parser / batch / variable bugs found by the unit tests

A full hardware run of the unit-test suite surfaced several latent bugs:

- `shell_parse_percentage_arg()` / `shell_parse_size_arg()` accepted an empty
  string (missing argument silently treated as 0). Now rejects when `strtol`
  consumed no digits.
- `shell_split_chain()` emitted a trailing empty segment for whitespace-only
  input or a dangling separator; now skipped.
- Bare positional args (`%0`, `%1`..`%9`, `%*`) without a batch frame printed
  literally; now expand to empty per the documented DOS rule.
- `set /a` accepted out-of-range literal `2147483648` via `strtol` clamp; now
  rejected with `ERANGE` so `-2147483648/-1` is refused as overflow.
- Corrected two stale unit tests (`a^^|b` pipe semantics, in-place chain-buffer
  reuse).

**Verification:** the full unit-test suite runs clean — every suite reports 0
failures.

---

### Fixed — date command error message did not state the expected format

- `date 2026-08-08` (YYYY-MM-DD) rejected with "value out of range" but did not
  tell the user the expected format was `MM-DD-YYYY`, so the rejection was
  confusing.
- **Fix:** `components/command/command.c` — `shell_command_date()` now prints the
  usage line (`Usage: date [MM-DD-YYYY]`) alongside the range error, and the
  warning record notes the expected order. Verified on hardware: `date 08-08-2026`
  sets the date; `date 2026-08-08` explains the expected format.

### Observations reviewed and confirmed as designed / non-bugs

- `echo %UNDEFINEDVAR%` prints the literal name — this is the documented "unknown
  names left untouched" rule in `batch.h`. The unit-test assertion that
  contradicted it was corrected in v0.24.4.
- `echo on` / `echo off` set the echo flag and print nothing, matching DOS (only
  bare `echo` prints the status). The report's wording concern does not apply.
- Wi-Fi scan showing empty SSIDs (`ssid= rssi=0`) was a boot-timing race; after
  the runtime stabilises the scan returns real access points including the test
  AP. No code change.
- Transcript echo of a piped stage shows both the stage output and the
  downstream result — expected given the transcript-delta redirection design. No
  code change.

### Recurring — intermittent LVGL crash (C1)

- The `wifi connect` command during testing triggered the same intermittent LVGL
  crash documented in C1 (fault in `lv_obj_get_style_prop` via
  `trans_anim_start_cb`). This confirms the v0.24.1 mitigation reduced the
  frequency but did not eliminate the root cause: a style-transition animation
  accessing an LVGL object that has been freed. Dedicated debugging with the LVGL
  pool walk enabled and a full audit of LVGL object lifecycle during UI rebuild
  is still recommended.

---

## [0.24.4] - 2026-08-08

Test correctness release. Resolved the final outstanding item from the hardware
test report's recommended-next-steps list.

### Fixed — test_shell_variables.c assertion

- The `test_variable_expansion_env_var()` test asserted that an undefined variable
  (`%UNDEFINED%`) expands to an empty string, but `shell_expand_variables()` leaves
  unknown names untouched per its documented contract ("unknown names are left
  untouched, and `%%` yields `%`"). The assertion was corrected to expect the
  literal `%UNDEFINED%` to pass through. Verified: test project builds clean.

---

## [0.24.3] - 2026-08-08

Colour and correctness release. Eliminated the literal `@`-specifier markers that
appeared in command output, fixed the medium-severity hardware-test issues (M1-M4),
and corrected three command-dispatch bugs found while testing on real hardware.

### Fixed — ANSI colour markers rendered literally in command output

A systemic issue: commands composed output with `@`-specifier palette macros and
sent the string through a path that never converted them to real SGR escapes.
`ansi_vformat()` only converts `@`-specifiers that appear literally in the format
string; `@`-specifiers inside substituted `%s` arguments are deliberately left
untouched (injection guard). Every call site that relied on converting `@`-specifiers
from a `%s` argument — or that used a plain `vsnprintf`/`snprintf` path — emitted
the raw `@K...@R` markers on both the LVGL transcript and the UART console.

Affected commands: `dir`, `sd info|stat|ls`, `wifi status|diag`, `keyboard status`,
`ver`, `sysinfo`, `about`, `mem`, `usb status`.

Fix:
- `components/shell/shell.c` — `shell_print_coloured()`, `shell_print_field()`,
  and `shell_print_field_num()` now route the entire format + arguments through
  `ansi_vformat()`, so every `@`-specifier is in the format string and gets
  converted. Added a `shell_colour_value()` helper for values whose colour depends
  on runtime state.
- `components/storage/storage_commands.c` — `shell_dir_emitf()` now uses
  `ansi_vformat()` instead of `vsnprintf()`, and the `dir` listing entry colour
  is applied via a runtime-built format string.
- `components/networking/networking.c` — conditional-colour `wifi diag` values
  are pre-converted with `ansi_format()`; the module's plain `networking_schedulef`
  is left for machine-readable output.
- `components/command/command_ui.c` — `keyboard status` conditional colour uses
  separate format strings.
- `components/command/command.c` — `display.state`, `c6.hosted_transport.busy`,
  and `heap` colour in `ver`/`sysinfo` now use inline colours or the
  `shell_colour_value()` helper.

### Fixed — `cd` printed `\` while the prompt showed `/sdcard`

- `shell_fs_print_cwd()` now prints the full VFS path to match the prompt.

### Fixed — `attrib` with no path showed a bare error

- `shell_command_attrib()` now shows usage when no path is given.

### Fixed — keypress wait timeout reduced for headless use

- Reduced `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` from 30000 to 10000.

### Improved — battery telemetry shows calibration state

- `battery` now reports `calibrated=yes|no` in the detail line.

### Fixed — family commands printed help instead of executing

- `wifi status|scan|diag`, `bluetooth status|scan`, `usb status`, `sd info|stat|ls|cat`
  printed help instead of running. Root cause: `shell_split_args()` truncated the
  command string passed to family handlers. Fixed with a heap copy in
  `shell_execute_command_core()`.

### Fixed — `sd stat`/`sd ls`/`sd cat` showed a usage error

- Same mutation bug in `shell_command_sd()`; fixed with a `strdup` before splitting.

### Fixed — `shell_print_*` helpers emitted literal `@`-specifiers

- `shell_print_coloured()` now routes through `ansi_vformat()`.

---

## [0.24.2] - 2026-08-08

Hardware stability and polish release. Fixed the intermittent LVGL crash that
surfaced during bring-up, addressed the medium-severity issues from the hardware
test report (M1–M4), and applied three additional correctness fixes found while
testing on real hardware.

### Fixed — intermittent LVGL crash (heap corruption on style transition)

- **Symptoms:** Intermittent `Guru Meditation Error: Core 0 panic'ed (Load/Store
  access fault)` rebooting the device after commands like `if exist /sdcard`,
  `sd info`, `bluetooth advertise on`. Fault decoded to `lv_obj_get_style_prop`
  (`lv_obj_style.c:330`) running from `trans_anim_start_cb` → `anim_timer` on the
  LVGL task, with LVGL pool corruption (`lv_tlsf_free` → `remove_free_block`).
- **Root cause:** The header component's `header_render()` and the keyboard
  component's `keyboard_show()`/`keyboard_hide()` made direct LVGL API calls
  without holding the LVGL port lock. When called from a non-LVGL task (the
  fallback render path when `lv_async_call` fails, or the command worker task
  for keyboard commands), these raced with the LVGL render cycle and corrupted
  the LVGL heap pool.
- **Fix:**
  - `components/header/header.c` — `header_render()` now acquires
    `lvgl_port_lock(0)` for its entire body and releases with
    `lvgl_port_unlock()`. Added `#include "esp_lvgl_port.h"` and added
    `espressif__esp_lvgl_port` to the component's `REQUIRES`.
  - `components/keyboard/keyboard.c` — `keyboard_show()` and
    `keyboard_hide()` now acquire `lvgl_port_lock(0)` around their LVGL widget
    operations. Added `#include "esp_lvgl_port.h"`.
- **Verification:** A stress test exercising all previously-crashing commands
  plus rapid header-touching commands ran clean twice with no Guru Meditation
  errors.

### Fixed — family commands printed help instead of executing

- `wifi status`, `wifi scan`, `wifi diag`, `bluetooth status`, `bluetooth scan`,
  `usb status` all printed the command help/usage instead of running the
  subcommand.
- **Root cause:** `shell_split_args()` writes token terminators into the
  command buffer in place. By the time the dispatcher reached the module-routed
  family handlers, `command` was truncated to just the first token (`"wifi"`),
  so the family parser saw `argc <= 1` and fell through to help.
- **Fix:** `components/command/command.c` — `shell_execute_command_core()` now
  preserves a heap copy of the trimmed command for family-prefix lines
  (`strdup` before `shell_split_args`) and passes it to the family handlers.
  The copy is freed in every branch.

### Fixed — `sd stat` / `sd ls` / `sd cat` showed a usage error

- `sd stat test.txt` printed the `sd` usage instead of the stat result.
- **Root cause:** Same in-place mutation inside `shell_command_sd()` —
  `shell_split_args(command, ...)` truncated `command` to `"sd"` before it was
  handed to the sub-handlers.
- **Fix:** `components/storage/storage_commands.c` — `shell_command_sd()` now
  `strdup`s the command before splitting and passes the copy to the
  sub-handlers, restructuring to a single exit that frees the copy.

### Fixed — `shell_print_*` helpers emitted literal `@`-specifiers

- Every usage/error/ok/warning/muted/heading line showed raw colour markers,
  e.g. `@yUsage: brightness <0-100>@R`, `@gCreated directory ...@R`,
  `@GFolder PATH listing for volume A:@R`.
- **Root cause:** `shell_print_coloured()` composed `@y...@R` and called
  `shell_transcript_append_ansi()`, which only strips real `\x1b` sequences and
  does not convert `@`-specifiers.
- **Fix:** `components/shell/shell.c` — `shell_print_coloured()` now builds a
  format string with the colour/reset as literal `@`-specifiers and routes the
  composed text through `shell_transcript_appendf_ansi()` (i.e. `ansi_vformat`),
  so colour and reset are converted and the rendered body is a `%s` argument
  (literal `%` in body stays data).

### Fixed — `cd` printed `\` while the prompt showed `/sdcard`

- `cd` (no args) printed `\` (the FAT root) while the prompt rendered `/sdcard`
  (the VFS path). The two surfaces disagreed about the current directory.
- **Fix:** `components/storage/storage.c` — `shell_fs_print_cwd()` now always
  prints the full VFS path (`s_shell_cwd`), matching the prompt.

### Fixed — `attrib` with no path showed a bare error

- `attrib` with no arguments printed `attrib: cannot access .` with no usage
  hint.
- **Fix:** `components/storage/storage_commands.c` — `shell_command_attrib()`
  now shows usage when no path is given.

### Fixed — keypress wait timeout reduced for headless use

- `pause`/`choice`/`more` blocked for the full 30 s with no interactive key
  source before falling back, stalling headless batch files.
- **Fix:** Reduced `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` from 30000 to 10000 in
  `p4minishell_config.h` and `p4minishell_config.yaml`. Still long enough for
  a human to respond interactively; less painful for scripted/headless use.

### Improved — battery telemetry shows calibration state

- `battery` now reports `calibrated=yes|no` in the detail line so the user
  knows whether the voltage reading comes from the calibrated ADC driver or
  the approximate linear fallback.
- **Where:** `components/command/command.c` — `shell_command_battery()`.

---

## [0.24.1] - 2026-08-08

Hardware bring-up release for real-ESP32-P4 testing. Three stack-protection faults
and one LVGL concurrency bug discovered and fixed during first-flash bring-up on
the JC1060P470 development board. Documentation issues from the roadmap resolved.

### Fixed — main-task stack overflow during UI construction

- `app_main()` builds the entire LVGL UI on the 3584-byte main-task stack via
  `shell_build_ui()` → `windows_init()`. The transcript append and prompt rendering
  paths include newlib `vfprintf` calls that consume ~1–1.5 KB of stack alone,
  overflowing the guard and triggering `Stack protection fault` in
  `vfprintf` → `__sbprintf`.
- Bumped `CONFIG_ESP_MAIN_TASK_STACK_SIZE` from 3584 to 8192 in
  `sdkconfig.defaults`, matching the command-worker-task budget and the
  `esp_lvgl_port` task's own 7168-byte stack.
- Added the new config value to `sdkconfig.defaults` with a descriptive comment
  explaining why the default is insufficient.

### Fixed — LVGL assertion hang from concurrent transcript appends

- `shell_transcript_append_internal()`, `shell_transcript_reset()`,
  `shell_history_transcript_scroll_to_end()`, and `shell_input_line_set_text()`
  performed direct LVGL object manipulation (`lv_textarea_set_text`,
  `lv_obj_update_layout`, `lv_obj_scroll_to_y`) from any calling task — including
  the Wi-Fi background task, the command worker task, and the UART console task
  — without holding the LVGL render lock.
- When the Wi-Fi background task called these functions while the LVGL task was
  mid-render, the `LV_ASSERT_MSG(!disp->rendering_in_progress, ...)` assertion
  fired, invoking the default `while(1)` handler. The task watchdog triggered
  every five seconds on CPU 1, but the system never recovered.
- All four functions now acquire `lvgl_port_lock(0)` (recursive mutex) around
  their LVGL sections. The mutex is recursive, so the LVGL event path, the async
  transcript flush callback, and the UART-console command path — all of which
  already hold the lock — nest without deadlock. The lock is only taken when the
  LVGL port is initialized (guarded by transcript/input-line non-NULL checks).

### Fixed — esp_event task stack overflow during Wi-Fi handler

- The `sys_evt` task (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE, default 2304) runs
  the project's Wi-Fi event handler (`networking_wifi_event_handler`). That handler
  calls `networking_wifi_append_step()` → `networking_schedulef_ansi()` →
  `vsnprintf()` into a 512-byte stack buffer, then `networking_record_infof()` and
  `networking_notify_headerf()`. The newlib `vfprintf` machinery on this path
  consumes ~1.5–2 KB, overflowing the 2304-byte task stack immediately after
  `esp_wifi_start()`.
- Bumped `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` from 2304 to 8192 in
  `sdkconfig.defaults`. The esp_event system task is created once; the extra
  ~5.8 KB SRAM cost is negligible and consistent with the other task budgets in
  this firmware.

### Documentation — roadmap issues resolved

- **`command.md` `rgb`/`camera` in wrong table** — The "Where Commands Live" table
  listed `rgb` and `camera` as hardware commands alongside working commands like
  `brightness`, `rotate`, etc., but these are stubs that only print error messages.
  Moved them out of the hardware table and clarified the "Unsupported Commands" table
  to note that these entries are stubs.
- **`p4minishell_config.yaml` missing v0.23.0 Kconfig changes** — The YAML file
  header still said version 0.24.0 and did not document the two new task stack size
  entries added during hardware bring-up. Updated the header to version 0.24.1 with
  the bring-up notes, and added `main_task` (8192) and `system_event` (8192) entries
  to the `task_stacks` section with full descriptions explaining why the defaults
  were insufficient.

### Hardware support — roadmap review resolved

- **ESP32-P4 ADC attenuation value (issue 9)** — Verified as false alarm. The
  ESP-IDF header `hal/adc_types.h:51` defines `ADC_ATTEN_DB_12 = 3` which IS the
  correct enum value on ESP32-P4. The code at `p4minishell_config.h:451` using
  `P4_CONFIG_BATTERY_ATTEN = 3` with comment `/* ADC_ATTEN_DB_12 */` is correct.
- **ADC calibration fallback (issue 10)** — Confirmed as informational, not a bug.
  ESP32-P4 supports curve fitting calibration. The uncalibrated fallback is expected
  when eFuse data is absent and degrades gracefully.
- **Coprocessor firmware directory (issue 11)** — Already documented in
  `coprocessor/esp32c6_slave/README.md` which states "No custom slave source code"
  and explains the upstream build approach. No change needed.

- **`command.md` `rgb`/`camera` in wrong table** — The "Where Commands Live" table
  listed `rgb` and `camera` as hardware commands alongside working commands like
  `brightness`, `rotate`, etc., but these are stubs that only print error messages.
  Moved them out of the hardware table and clarified the "Unsupported Commands" table
  to note that these entries are stubs.
- **`p4minishell_config.yaml` missing v0.23.0 Kconfig changes** — The YAML file
  header still said version 0.24.0 and did not document the two new task stack size
  entries added during hardware bring-up. Updated the header to version 0.24.1 with
  the bring-up notes, and added `main_task` (8192) and `system_event` (8192) entries
  to the `task_stacks` section with full descriptions explaining why the defaults
  were insufficient.

### Code quality — semantic helper newline consolidation

- Refactored `shell_print_coloured()` to accept a `bool newline` parameter, so the
  newline is included in the ANSI output rather than appended as a separate plain-text
  call. All six single-colour helpers (`shell_print_heading`, `shell_print_ok`,
  `shell_print_error`, `shell_print_warning`, `shell_print_muted`, `shell_print_usage`)
  now call `shell_print_coloured(..., true, args)` directly. `shell_print_field` and
  `shell_print_field_num` were unchanged — they embed `\n` in their own snprintf and
  do not use `shell_print_coloured()`.

### Code quality — shell_extract_input_text optimization

- `shell_extract_input_text()` called `strlen(s_input_line_prompt)` and
  `strlen(SHELL_PROMPT)` twice each (once for `strncmp`, once for the pointer
  offset). Cached both lengths in local variables so each is computed once.

### Code quality — UART console fgets() documentation

- Added clarifying comment to `shell_uart_console_task()` explaining why the
  `fgets()` + `clearerr(stdin)` + `vTaskDelay(20ms)` pattern is safe for
  USB-Serial-JTAG: stdin is set to unbuffered mode (`_IONBF`) so `fgets()`
  reads character-by-character from the underlying driver, and the error
  recovery loop handles disconnect/reconnect correctly.

### Code quality — debug command deduplication

- Removed `debug.wifi_state` (same as `wifi status` state line) and
  `debug.free_heap` (same as `mem` heap line) from `shell_command_debug()`.
  The debug command now focuses on its unique value: the debug log entries
  and warning count. Users can run `wifi status` or `mem` for the full
  network and memory state.

### API — ansi.h documentation corrected

- Rewrote the `ansi_vformat()` specifier table in `ansi.h` to match the actual
  implementation. Removed the non-existent lowercase "off" specifiers (`@d`,
  `@i`, `@u`) that were documented but never implemented. Removed the duplicate
  `@b` entry (was listed as both "bold off" and "foreground blue"). Corrected
  `@k`/`@K` descriptions to match the implementation (standard black vs bright
  black). Added `@@` literal escape to the documented table.

### SDK — ESP-Hosted version gate configurable

- Added `P4_CONFIG_HOSTED_SKIP_VERSION_GATE` (default 0) to
  `p4minishell_config.h`. When set to 1, the Wi-Fi and Bluetooth init paths
  do not reject C6 firmware version mismatches — mismatches are logged as
  warnings instead of returning `ESP_ERR_INVALID_STATE`. Useful for
  development and testing with mismatched host/co-processor firmware. Both
  `networking_wifi_validate_hosted_version()` in `networking.c` and the
  bluetooth version check in `bluetooth.c` respect this config. Documented
  in `p4minishell_config.yaml` under `wifi.hosted_skip_version_gate`.

### Testing — variable expansion and debug log coverage

- Added `test/main/test_shell_variables.c` with nine test functions covering
  `shell_expand_variables()`: environment variable expansion (`%VAR%`),
  undefined variables, empty variable names (`%%`), single-quote literal
  protection, double-quote expansion, caret-escape passthrough, multiple
  variables, output buffer truncation, NULL safety, and batch argument
  expansion without an active frame.
- Added `test/main/test_shell_debug_log.c` with six test functions covering
  the debug log ring buffer and warning counter: `shell_debug_log_push()`,
  `shell_record_warningf()`, `shell_record_errorf()`, `shell_record_infof()`,
  `shell_get_warning_count()`, ring overflow behavior, and NULL safety.
- Updated `test/main/test_main.c` and `test/main/CMakeLists.txt` to register
  the new test suites.

### Optimization — persistent command worker task

- Replaced per-command FreeRTOS task creation with a persistent worker task
  (`command_worker_task`) and a FreeRTOS queue (`s_command_queue`, depth 4).
  `shell_execute_command_async()` now posts commands to the queue via
  `xQueueSend` instead of calling `xTaskCreate` + `calloc` for each command.
  The worker task runs at `tskIDLE_PRIORITY + 2` and processes commands
  sequentially. Eliminates ~1-2ms task-creation overhead per command and
  reduces heap fragmentation from repeated task stack allocation. Full queues
  are logged as warnings with the command dropped.

### Optimization — SD I/O buffer size increased

- Increased `P4_CONFIG_SD_IO_BUFFER_BYTES` from 128 to 512 bytes (matching
  `P4_CONFIG_FILE_IO_BUFFER_BYTES` for consistency). Reduces the number of
  small read/write calls for `storage_copy_file()`, pipe spool operations,
  and `sd cat`. Stack impact is +384 bytes per function — well within the
  8192-byte command worker task budget.

### Fixed — intermittent LVGL crash (heap corruption on style transition)

- **Symptoms:** Intermittent `Guru Meditation Error: Core 0 panic'ed (Load/Store
  access fault)` rebooting the device after commands like `if exist /sdcard`,
  `sd info`, `bluetooth advertise on`. Fault decoded to `lv_obj_get_style_prop`
  (`lv_obj_style.c:330`) running from `trans_anim_start_cb` → `anim_timer` on the
  LVGL task, with LVGL pool corruption (`lv_tlsf_free` → `remove_free_block`).
- **Root cause:** The header component's `header_render()` and the keyboard
  component's `keyboard_show()`/`keyboard_hide()` made direct LVGL API calls
  without holding the LVGL port lock. When called from a non-LVGL task (the
  fallback render path when `lv_async_call` fails, or the command worker task
  for keyboard commands), these raced with the LVGL render cycle and corrupted
  the LVGL heap pool.
- **Fix:**
  - `components/header/header.c` — `header_render()` now acquires
    `lvgl_port_lock(0)` for its entire body and releases with
    `lvgl_port_unlock()`. Added `#include "esp_lvgl_port.h"` and added
    `espressif__esp_lvgl_port` to the component's `REQUIRES`.
  - `components/keyboard/keyboard.c` — `keyboard_show()` and
    `keyboard_hide()` now acquire `lvgl_port_lock(0)` around their LVGL widget
    operations. Added `#include "esp_lvgl_port.h"`.
- **Verification:** A stress test exercising all previously-crashing commands
  plus rapid header-touching commands ran clean twice with no Guru Meditation
  errors.

### Fixed — family commands printed help instead of executing

- `wifi status`, `wifi scan`, `wifi diag`, `bluetooth status`, `bluetooth scan`,
  `usb status` all printed the command help/usage instead of running the
  subcommand.
- **Root cause:** `shell_split_args()` writes token terminators into the
  command buffer in place. By the time the dispatcher reached the module-routed
  family handlers, `command` was truncated to just the first token (`"wifi"`),
  so the family parser saw `argc <= 1` and fell through to help.
- **Fix:** `components/command/command.c` — `shell_execute_command_core()` now
  preserves a heap copy of the trimmed command for family-prefix lines
  (`strdup` before `shell_split_args`) and passes it to the family handlers.
  The copy is freed in every branch.

### Fixed — `sd stat` / `sd ls` / `sd cat` showed a usage error

- `sd stat test.txt` printed the `sd` usage instead of the stat result.
- **Root cause:** Same in-place mutation inside `shell_command_sd()` —
  `shell_split_args(command, ...)` truncated `command` to `"sd"` before it was
  handed to the sub-handlers.
- **Fix:** `components/storage/storage_commands.c` — `shell_command_sd()` now
  `strdup`s the command before splitting and passes the copy to the
  sub-handlers, restructuring to a single exit that frees the copy.

### Fixed — `shell_print_*` helpers emitted literal `@`-specifiers

- Every usage/error/ok/warning/muted/heading line showed raw colour markers,
  e.g. `@yUsage: brightness <0-100>@R`, `@gCreated directory ...@R`,
  `@GFolder PATH listing for volume A:@R`.
- **Root cause:** `shell_print_coloured()` composed `@y...@R` and called
  `shell_transcript_append_ansi()`, which only strips real `\x1b` sequences and
  does not convert `@`-specifiers.
- **Fix:** `components/shell/shell.c` — `shell_print_coloured()` now builds a
  format string with the colour/reset as literal `@`-specifiers and routes the
  composed text through `shell_transcript_appendf_ansi()` (i.e. `ansi_vformat`),
  so colour and reset are converted and the rendered body is a `%s` argument
  (literal `%` in body stays data).

---

## [0.24.0] - 2026-08-08

Bug fixes and correctness release. **No commands, options, output formats, or behaviors
were removed.** The semantic `SH_*` palette migration started in v0.22.0 is now complete,
and two latent correctness issues are resolved.

### Fixed — `shell_get_cwd_for_prompt()` was not thread-safe

- The function used a `static` buffer and returned a pointer to it. Both the UART
  console task and the LVGL input-line task render the prompt and call this function,
  so the shared buffer was a race condition: a path truncation in one task could be
  overwritten by the other before the first task finished using it.
- The function now writes into a caller-provided buffer. `shell_prompt_expand()` and
  `shell_uart_console_print_prompt()` each provide their own stack buffer. No shared
  state remains.
- Updated the declaration in `components/shell/shell.h`, the test in
  `test/main/test_shell_prompt.c`, and the API reference in `API.md`.

### Fixed — semantic palette migration incomplete

- `shell_command_debug()` in `components/shell/` and the `display`, `keyboard`,
  `windows`, `reboot`, "Unknown command", and out-of-memory paths in
  `components/command/` still used raw `@`-specifiers (`@C`, `@R`, `@Y`, `@y`, `@g`,
  `@r`, `@G`, `@B`, `@K`) instead of the `SH_*` macros from
  `components/ansi/ansi_palette.h`. The v0.22.0 changelog claimed "every command
  group migrated" — that is now true.
- Every site in `shell_command_help()`, `shell_command_sysinfo()`,
  `shell_command_version()`, `shell_command_about()`, `shell_command_mem()`, and
  `shell_command_debug()` in `shell.c`, and `shell_command_reboot()`,
  `shell_execute_rgb_command()` stubs, and the `display`/`keyboard`/`windows`
  subcommand handlers in `command.c`, now uses `SH_*` macros.

### Fixed — stale documentation

- `documentation.md` described the optimization level as "Performance (-O3)" but
  `sdkconfig.defaults` has specified `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` since
  v0.23.0. The documentation now says "Size (`-Os`)".
- `roadmap.md` claimed "main.c is 445 lines" and "command.c is 1,140 lines". The
  actual counts are 499 and 1,793 respectively. Corrected.

### Added — pipeline and chain test coverage

- Added `test/main/test_shell_pipeline.c` with seven new test functions
  covering the command pipeline surfaces that previously had no tests:
  - `test_pipe_detection_agreement` — verifies that `shell_has_unquoted_char()`
    and `shell_split_args()` never disagree about whether a `|` is syntax or
    data, across double quotes, single quotes, caret escapes, pipes after
    closed quotes, and mixed real/quoted pipes.
  - `test_redirection_quote_awareness` — verifies that `>`, `>>`, and `<`
    inside double quotes, single quotes, and caret escapes are treated as data.
  - `test_chain_pipe_vs_chain_under_quotes` — verifies that a `|` inside
    quotes does not split a command chain.
  - `test_chain_single_quote_protection` — verifies that `&`, `&&`, `||`
    inside single quotes are data, not chain separators.
  - `test_chain_truncation_with_pipelines` — verifies that the segment limit
    works correctly when each link is itself a pipeline.
  - `test_chain_empty_and_whitespace` — verifies edge cases for empty input
    and leading separators.
  - `test_find_unquoted_pipe` — verifies the pipe scanner directly: bare pipe,
    quoted pipe, escaped pipe, pipe after closed quote, NULL input.

### Changed — architecture and maintainability

- **`shell.c` no longer depends on networking, Bluetooth, USB, or C6 OTA.** The
  shell core previously included `networking.h`, `bluetooth.h`, `usb.h`, and
  `c6ota.h` directly. External-module state is now read through 11 new function
  pointers in `shell_command_ops_t` (`wifi_is_connected`, `wifi_get_rssi`,
  `wifi_state_string`, `append_sysinfo_summary`, `bluetooth_is_enabled`,
  `bluetooth_is_connected`, `usb_is_connected`, `usb_is_keyboard_attached`,
  `usb_key_to_ascii`, `c6ota_is_pending`, `c6ota_is_busy`), all registered
  from `command_init()`. Every hook is NULL-checked before use, so the shell
  degrades gracefully if a module is unavailable.
- **`command_ui.c` extracted from `command.c`.** The `display`, `keyboard`, and
  `windows` subcommand handlers now live in their own translation unit, so
  `command.c` no longer includes `keyboard.h` or `windows.h`. `display.h` is
  still required for the `brightness` and `rotate` hardware commands.
- **`coprocessor/esp32c6_slave/README.md`** now explicitly states that there is
  no custom slave source code — the firmware is built entirely from the managed
  `espressif__esp_hosted` component.
- **`p4minishell_config.yaml`** gained a `build_constraints` section documenting
  the build-affecting Kconfig options (`CONFIG_SPIRAM_XIP_FROM_PSRAM`,
  `CONFIG_COMPILER_OPTIMIZATION_SIZE`, `CONFIG_LOG_DEFAULT_LEVEL_WARN`,
  `CONFIG_ESP_WIFI_SOFTAP_SUPPORT`, `CONFIG_WIFI_RMT_SOFTAP_SUPPORT`).

### Notes

- No regressions: every change is a semantics-preserving substitution (raw
  specifier → `SH_*` macro that expands to the exact same specifier), a
  signature change with all call sites updated, or a pure code-move to a new
  translation unit. No command output text was altered.
- `shell_parse_redirection()` and `shell_execute_pipe()` are not directly
  unit-tested: the former is `static` (its quote-aware behavior is verified
  indirectly through `test_redirection_quote_awareness`), and the latter is
  tightly coupled to SD card spool files and `batch_run_nested()`.
- Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 /
  esp32p4 (to be verified).

---

## [0.23.0] - 2026-08-08

Consolidates the networking layer onto the official Espressif path and makes the
station-only constraint structural. **No commands, options, or output were removed** —
the dispatcher stays at 71 verbs and `wifi`, `bluetooth`/`bt`, and `c6ota` behave
identically.

The module was already centralized and already used only `espressif/esp_hosted` plus
`espressif/esp_wifi_remote`, with no custom RPC or alternative transport. This release
closes the gaps that remained: initialization order, one encapsulation leak, and
several constraints that were conventional rather than enforced.

### Changed — canonical initialization order

`networking_wifi_start_runtime()` previously ran `nvs_flash_init()` **before** bringing
up the hosted transport. The order is now exactly as specified, and documented in
`networking.h` with the reason each step precedes the next:

1. `esp_hosted_init()`
2. `esp_hosted_connect_to_slave()`
3. Version compatibility gate
4. `nvs_flash_init()` (erase-and-retry on a corrupt partition)
5. `esp_netif_init()`
6. `esp_event_loop_create_default()`
7. `esp_netif_create_default_wifi_sta()`
8. `esp_wifi_init()` via `esp_wifi_remote`
9. Event handler registration
10. `esp_wifi_set_mode(WIFI_MODE_STA)`
11. `esp_wifi_start()`

Bringing the transport up first means a dead or mismatched co-processor is reported as
a transport fault instead of surfacing later as a confusing Wi-Fi init error.

### Fixed — encapsulation leak in the header status refresh

- `shell_header_status_refresh()` called `esp_wifi_sta_get_ap_info()` directly to read
  RSSI, which was the only `esp_wifi_*` call outside `components/networking/`.
- Added **`networking_wifi_get_rssi()`** to the module's public API. It also skips the
  driver query entirely when disconnected, saving a needless SDIO round trip on every
  header refresh.
- `components/shell/` no longer includes `esp_wifi.h` and no longer declares `esp_wifi`
  as a component dependency.
- Verified: no `esp_hosted_*`, `esp_wifi_*`, `esp_netif_*`, or NimBLE call remains
  outside `components/networking/`. The sole sanctioned exception is
  `components/c6ota/`, which drives `esp_hosted_slave_ota_*` because co-processor
  firmware update is its entire purpose.

### Changed — station-only is now structural

- `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` and `CONFIG_WIFI_RMT_SOFTAP_SUPPORT` are both
  disabled in `sdkconfig.defaults`. The code always called
  `esp_wifi_set_mode(WIFI_MODE_STA)`, but SoftAP was still compiled in.
- Both symbols are needed: `esp_wifi_remote` mirrors the Wi-Fi Kconfig under its own
  `WIFI_RMT_` prefix, and on the hosted path that mirror is the authoritative one.

### Fixed — build constraints were not durable

Regenerating `sdkconfig` from `sdkconfig.defaults` revealed that three documented
constraints existed only in the generated file, so any regeneration silently reverted
them. All three are now pinned in `sdkconfig.defaults`:

- **`CONFIG_SPIRAM_XIP_FROM_PSRAM=n`** — the defaults file actually had this set to `y`,
  directly contradicting the documented constraint that PSRAM XIP mapping must stay
  disabled to avoid overflowing the flash/PSRAM budget at link time
- **`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`** — the defaults file specified
  `OPTIMIZATION_PERF`, and regenerating grew the image by roughly 280 KB
- **`CONFIG_LOG_DEFAULT_LEVEL_WARN=y`** — warn-level compile-time logging

### Fixed — latent buffer truncation in `dir /w`

- The wide-listing row buffer could not hold a full set of columns plus one clipped
  over-long cell, and the `strncat()` bound was computed from a runtime `strlen()` so
  the compiler could not prove it safe. Surfaced as `-Werror=stringop-truncation` once
  the regenerated config restored the stricter optimization level.
- The row buffer is now sized for every column at full width plus one clipped cell, and
  the append tracks the row length explicitly so the bound is a compile-time constant.

### Notes

- Only the official path is used: `espressif/esp_hosted` for transport,
  `espressif/esp_wifi_remote` for the Wi-Fi API, hosted NimBLE over VHCI for BLE. No
  custom RPC, no alternative transport, no re-implemented control plane.
- The version compatibility gate is unchanged and still refuses init with a clear
  diagnostic when the C6 firmware is outside the host's supported range.
- `c6ota` remains the only supported co-processor update path, and its capture,
  shutdown, wait, and restore hooks are unchanged.
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF
  v5.5.5 / esp32p4

---

## [0.22.0] - 2026-08-08

Gives the shell a complete, built-in default colour scheme. Every category of
output now uses a consistent colour chosen centrally, applied automatically on both
the LVGL transcript and the serial console, with no per-command decisions left to
individual implementations. **No commands, options, or output text were removed** —
the dispatcher stays at 71 verbs and every message keeps its exact wording.

### Added — Semantic colour palette

- **New `components/ansi/ansi_palette.h`** is the single source of truth for what
  colour each kind of output is. It defines named `SH_*` macros that expand to the
  existing `@`-specifiers, so nothing scattered across the tree picks colours by hand
  and a future palette change is a one-file edit.
- The ansi module owns the header because it owns the specifier vocabulary. The macros
  are plain string literals, so this introduces **no new component dependencies** and
  the ansi module remains a leaf.

| Category | Macro | Colour |
|----------|-------|--------|
| Section heading | `SH_HEAD` | Bright green |
| Sub-heading | `SH_SUBHEAD` | Bright yellow |
| Field label / key | `SH_LBL` | Cyan |
| Body text | `SH_TEXT` | Bright green |
| Muted / secondary / timestamp | `SH_MUTE` | Bright black (grey) |
| Success | `SH_OK` / `SH_OK_HI` | Green / bright green |
| Error | `SH_ERR` / `SH_ERR_HI` | Red / bright red |
| Warning | `SH_WARN` / `SH_WARN_HI` | Yellow / bright yellow |
| Important value (SSID, IP, MAC) | `SH_VAL` | Bright white |
| Number, size, percentage | `SH_NUM` | Bright magenta |
| Path / filename | `SH_PATH` | Bright blue |
| Prompt / command name | `SH_PROMPT` / `SH_CMD` | Bright cyan |
| Usage syntax | `SH_USAGE` | Yellow |
| Help description | `SH_DESC` | White |
| Directory entry | `SH_DIR` | Bold bright blue |
| File entry | `SH_FILE` | White |
| Executable / `.bat` entry | `SH_EXE` | Bright green |
| Listing size / timestamp | `SH_SIZE` / `SH_TIME` | Bright magenta / grey |
| Wi-Fi up / down | `SH_NET_UP` / `SH_NET_DOWN` | Green / grey |
| Bluetooth | `SH_BT` | Magenta |
| USB attached / detached | `SH_USB_UP` / `SH_USB_DOWN` | Green / grey |
| OTA | `SH_OTA` | Bright green |

### Added — Semantic print helpers

Seven wrappers over `shell_transcript_appendf_ansi()` in `components/shell/`, so the
common shapes need no colour decision at the call site. Each emits its own reset and
newline:

```c
void shell_print_heading(const char *format, ...);
void shell_print_field(const char *label, const char *format, ...);
void shell_print_field_num(const char *label, long value);
void shell_print_ok(const char *format, ...);
void shell_print_error(const char *format, ...);
void shell_print_warning(const char *format, ...);
void shell_print_muted(const char *format, ...);
void shell_print_usage(const char *format, ...);
```

They render the caller's text with real `vsnprintf` before wrapping it, so a `%s` value
containing an `@` can never be mistaken for a colour specifier.

### Fixed — muted text was nearly invisible

- **`@k` is pure black (0x0C0C0C), not grey**, and the default background is dark blue
  (0x012456). Thirty output sites across `networking.c`, `shell.c`, and `command.c`
  used `@k` for muted text, rendering it almost unreadable. All now use `@K`
  (bright black / grey) via `SH_MUTE`. This affected `wifi status`, `wifi diag`,
  `wifi scan`, and the keyboard status line among others.
- The palette header documents this and the other specifier traps (`@E` is bright red,
  `@B` is bold rather than blue, `@L` is bright blue) so the mistake is not repeatable.

### Fixed — `ansi_format()` discarded printf width and precision

- The formatter captured the full specifier but then re-rendered with only the bare
  conversion, silently dropping flags. `%10s`, `%-4s`, `%05d`, and `%.2f` all lost their
  formatting when routed through the ANSI path.
- It now passes the captured specifier to `snprintf` verbatim and detects `l` and `ll`
  length modifiers to pull the correct type from the `va_list`.
- This is what makes the right-aligned `chkdsk` capacity report and the `dir` column
  layout survive colouring. Covered by a new test suite.

### Changed — every command group migrated

- **System**: `help`, `sysinfo`, `version`, `about`, `mem`, `debug`
- **Hardware**: `brightness`, `rotate`, `battery`, `volume`, `gpio`, `display`,
  `keyboard`, `windows` — cyan labels with bright-magenta numbers throughout
- **Storage**: every file command, plus `attrib`, `label`, `xcopy`, `chkdsk`, `format`,
  and the `sd` family. Around 105 error paths and 51 usage lines now route through
  `shell_print_error()` and `shell_print_usage()`.
- **Directory listings**: `dir` and `sd ls` colour each entry by kind — bold bright
  blue for directories, bright green for runnable `.bat` files, white for ordinary
  files — with grey timestamps and magenta sizes. The colour is applied *after* all
  width formatting so column alignment is unaffected, and `dir /b` stays deliberately
  uncoloured so redirected output remains machine-parsable.
- **Batch**: `set`, `set /a`, `set /p`, `path`, `echo`, and echoed batch lines
- **Wi-Fi, Bluetooth, USB**: status fields now use the shared label/value colouring,
  with connected states in green and disconnected in grey
- **C6 OTA**: progress, completion, warnings, and the YES prompt are coloured at the
  `main.c` bridge rather than inside the module, so `components/c6ota` keeps emitting
  the exact contractual strings documented in `API.md`. The destructive confirmation
  prompt is bright red.
- `usb_host_transcript_append_text()` and the Bluetooth emitter now route through the
  ANSI path so palette macros in those modules are interpreted.

### Notes

- Defaults only. There is no runtime theme or user configuration in this change.
- Dual-output correctness is preserved: the transcript strips escapes for the LVGL
  textarea while the serial console receives the raw sequences, exactly as before.
- The single remaining raw-SGR site is `shell_uart_console_print_prompt()`, which writes
  straight to the console with colour numbers parameterised from `P4_CONFIG_PS_COLOR_*`
  and never passes through `ansi_format()`. It is commented as the documented exception.

### Testing

- Added `test_ansi_format_width_flags` covering string and integer width, left and right
  alignment, zero padding, float precision, the `l` modifier, hex width, and width
  combined with a colour code
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF
  v5.5.5 / esp32p4

---

## [0.21.0] - 2026-08-08

Completes the batch language and closes the last filesystem parity gap. Delivery phase 1 is
now fully done alongside phase 2. **No commands, options, output formats, or behaviors were
removed.** The dispatcher stays at 71 verbs — `set /a` and `set /p` are sub-forms of the
existing `set`, not new commands — and every previously working `set`, `copy`, `xcopy`, and
batch file still behaves identically.

### Added — `set /a` arithmetic expressions

A recursive-descent evaluator over 32-bit signed integers implementing the COMMAND.COM
operator set and precedence:

| Precedence | Operators |
|------------|-----------|
| Lowest | `\|` bitwise or |
| | `^` bitwise xor |
| | `&` bitwise and |
| | `<<` `>>` shifts |
| | `+` `-` additive |
| | `*` `/` `%` multiplicative |
| | `-` `~` `!` unary |
| Highest | `( )` grouping |

- **`set /a NAME=<expression>`** evaluates and stores; **`set /a <expression>`** with no
  assignment prints the result without touching the environment
- **Compound assignments**: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- Numbers accept decimal, `0x` hex, and leading-zero octal
- A bare identifier reads an environment variable, and **an undefined variable evaluates to
  0** exactly as DOS does — which is what makes `set /a count=count+1` work on first use
- Errors are detected and reported rather than trapping: divide by zero, modulo by zero,
  `INT32_MIN / -1` overflow, unbalanced parentheses, malformed numbers, and trailing garbage
- Shifts of 32 or more are clamped instead of invoking undefined behavior
- The `&` and `|` handlers deliberately refuse to consume `&&` and `||`, so an expression can
  never swallow a command-chain separator from v0.19.0
- Exposed as `shell_expr_evaluate()` in `batch.h` so the evaluator is unit-testable

### Added — `set /p` prompted input

- **`set /p NAME=<prompt text>`** prints the prompt and reads a line through the key-wait
  facility added in v0.18.0
- Backspace edits the line, ESC cancels, and Enter submits
- **An empty line leaves the variable unchanged**, matching DOS, rather than clearing it
- Sets errorlevel to 1 when input was cancelled, timed out, or unavailable, so a batch file
  can branch on it
- Added **`shell_read_line()`** to the shell core — the reusable line-collection primitive
  `set /p` needed, echoing as it types and opening its own keypress wait so input never
  reaches the command dispatcher

### Added — Line continuation

- A trailing **`^`** joins the next physical line, so a long command can be split for
  readability
- Bounded by `P4_CONFIG_LINE_CONTINUATION_MAX` (8) so a malformed file cannot loop
- Uses an **odd-caret-count rule**: `^^` at end of line is an escaped literal caret and not a
  continuation, consistent with the escaping rules added in v0.19.0
- **The label scanner applies the identical rule.** Without this, a continued line whose tail
  begins with `:` would register a phantom label and silently corrupt `goto` targets.

### Added — Attribute preservation across `copy` and `xcopy`

- New **`storage_copy_attributes()`** carries R/H/S/A from source to destination
- Wired into `shell_fs_copy_file()`, so all five copy call sites inherit it at once: `copy`,
  wildcard `copy`, `move`'s copy-then-delete fallback, `xcopy`, and `xcopy /S`
- **`xcopy /S` also carries attributes onto the directories it creates**, so a hidden or
  system folder stays that way
- Only the user-visible bits are copied; `AM_DIR` is structural and is never forced onto a
  destination
- Applied **after** the data is written, because a read-only destination cannot be opened for
  writing
- **Non-fatal by design**: the file content is already correct, so losing an archive bit does
  not discard a completed transfer. The failure is recorded in the debug log instead.

### Changed

- A copy that fails because the destination is read-only now reports
  `use attrib -R to clear it` instead of a bare error. This matters more now that copies
  propagate the read-only bit, making the situation reachable in normal use.
- `help` lists the `set /a` and `set /p` forms

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_SET_EXPR_DEPTH_MAX` (16) — parenthesis nesting limit in `set /a`
- `P4_CONFIG_SET_PROMPT_INPUT_BYTES` (128) — maximum `set /p` input length
- `P4_CONFIG_LINE_CONTINUATION_MAX` (8) — maximum lines joined by trailing `^`

### Testing

- Added `test/main/test_batch_expr.c` with five suites: literals and number bases, arithmetic
  and precedence including associativity, bitwise and shift operators including the clamped
  over-wide shift, variable reads including the undefined-reads-as-zero rule, and error
  detection including divide by zero and `INT32_MIN / -1`
- `test/main/CMakeLists.txt` gained the `batch` component; `test_main.c` calls `batch_init()`
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.20.0] - 2026-08-08

Completes roadmap Phase 2, the stronger storage model. **No commands, options, output
formats, or behaviors were removed.** Three verbs were added (`chkdsk`, `scandisk`,
`format`), taking the dispatcher from 68 to 71; every previously working invocation of
`dir`, `copy`, `move`, `write`, and `sd info` still works unchanged.

### Added — Volume management

- **`chkdsk [path] [/F]`** (alias **`scandisk`**) — reports the volume label, total, used and
  free space, allocation unit size, and total and free cluster counts. `/F` additionally walks
  every directory verifying each entry is readable, reporting file and directory counts and
  any unreadable directory.
  The check is deliberately **read-only**. This firmware never rewrites FAT structures: a card
  with real corruption should be imaged and repaired on a host, so problems are reported
  honestly rather than silently "fixed" by an embedded shell. The output says so explicitly.
- **`format [/FS:FAT|FAT32|EXFAT] [/V:label] [/Q]`** — reformats the card through
  `esp_vfs_fat_sdcard_format()`, then optionally applies a volume label and reports the
  resulting geometry.
  Gated behind the exact confirmation word `P4_CONFIG_FORMAT_CONFIRM_WORD`, collected one key
  at a time through the shared key queue so the answer never reaches the command dispatcher —
  the same danger contract `c6ota` uses. **Refuses outright when no interactive input source
  is attached**, so a batch file can never silently wipe a card. The filesystem type is
  validated before the warning is even shown, so a typo cannot reach the prompt.

### Added — Full `dir` option set

| Option | Behavior |
|--------|----------|
| `/W` | Wide multi-column listing, DOS-style `[dirname]` bracketing, `~` clipping for long names |
| `/P` | Pause after each screenful; Enter or Space continues, `Q` quits |
| `/S` | Recurse into subdirectories with per-directory counts plus a grand total |
| `/B` | Bare output, names only; full paths under `/S` so it can be piped |
| `/L` | Lowercase names |
| `/A[:]attrs` | Filter by `D` dir, `H` hidden, `S` system, `R` read-only, `A` archive; `-` prefix excludes |
| `/O[:]order` | Sort by `N` name, `S` size, `E` extension, `D` date, `G` dirs first; `-` reverses |

- Detailed listings now include a `YYYY-MM-DD  HH:MM` timestamp per entry, decoded from the
  FATFS date and time words
- Every listing closes with free space; `/S` adds an explicit grand-total banner so the
  per-directory counts above are not mistaken for the whole tree
- Name is the tie-breaker for every sort key, so ordering is deterministic
- Options may appear before or after the path, with or without the `:` separator
- The existing no-option and wildcard behaviors are unchanged

### Added — Free-space reporting and guardrails

- **`storage_get_space_info()`** — total, used, and free bytes plus cluster geometry via
  `f_getfree()`. Handles both FATFS sector-size configurations.
- **`storage_check_free_space()`** — refuses a write that would leave less than
  `P4_CONFIG_STORAGE_FREE_MARGIN_BYTES` (64 KB) free, so the volume never fills to the point
  where FAT metadata updates begin to fail. An overwrite correctly credits the space the
  destination already occupies, so replacing a file in place does not need double the room.
  When the capacity query itself fails the operation is allowed to proceed rather than
  blocking a legitimate write on a diagnostic failure.
- **`storage_paths_are_same()`** and **`storage_get_file_size()`** supporting helpers
- **`sd info`** now reports filesystem total, used, and free space and the allocation unit
  size alongside the existing raw card capacity

### Fixed — data-loss bug in `copy`

- **`copy` onto itself truncated the source.** `copy a.txt a.txt` opened the destination with
  `"wb"`, which truncated the file to zero bytes before the first read, destroying it. The
  copy helper now detects this (case-insensitively, since FAT is) and refuses. `move` gained
  the same guard because its copy-then-delete fallback hit the identical path.
- **A failed copy left a truncated destination.** A write error mid-transfer left a partial
  file that looked complete. The partial destination is now removed and the removal is
  reported.
- `copy` prechecks free space before opening either file, and reports percentage progress for
  files above `P4_CONFIG_COPY_PROGRESS_THRESHOLD` so a multi-megabyte transfer does not look
  like a hang.
- `write` and `append` precheck free space **before** opening the file, so a truncating
  overwrite cannot destroy the existing contents and then fail for lack of room.

### Fixed — recursive directory walkers overflowed the worker stack

Found by applying the stack-discipline rule added in v0.19.0 to the new code, which also
surfaced the same problem in the existing `tree` walker.

- All three recursive walkers held large per-level buffers on the shared 8 KB command worker
  task stack. At the 8-level depth limit they exceeded it.
- Moved each walker's per-level state into a single heap block released before descending:
  `dir` **1472 → 752** bytes, `chkdsk` **1024 → 400**, `tree` **976 → 752**.
- At full depth these now total roughly 6 KB, 3.2 KB, and 6 KB respectively, inside the 8 KB
  budget. Verified from the disassembled prologues rather than by inspection.

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_DIR_SORT_ENTRY_MAX` (128) — entries buffered per level for sorting
- `P4_CONFIG_DIR_WIDE_COLUMNS` (4) and `P4_CONFIG_DIR_WIDE_COLUMN_WIDTH` (18)
- `P4_CONFIG_DIR_PAGE_LINES` (20) — rows between `/P` pauses
- `P4_CONFIG_DIR_RECURSE_DEPTH_MAX` (8) — `/S` and `chkdsk /F` recursion limit
- `P4_CONFIG_STORAGE_FREE_MARGIN_BYTES` (64 KB) — free-space safety margin
- `P4_CONFIG_COPY_PROGRESS_THRESHOLD` (256 KB) and `P4_CONFIG_COPY_PROGRESS_STEP_PCT` (10)
- `P4_CONFIG_FORMAT_CONFIRM_WORD` (`"YES"`) and `P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES` (0)

### Testing

- Added `test/main/test_storage_format.c` with four suites: size formatting across every unit
  boundary including the GiB ceiling, DOS wildcard matching including multi-star cases, the
  self-copy path-identity guard, and the path helpers
- `test/main/CMakeLists.txt` gained the `storage` component
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.19.0] - 2026-08-08

Completes the command interpreter parity section of the roadmap. **No commands, options,
output formats, or behaviors were removed.** The dispatcher verb table is unchanged at 68
verbs, verified by diff against the previous release.

### Added — Quoting and escaping rules

The shell gained one quote/escape scanner in `components/shell/`, and every surface that has
to tell syntax from data now shares it: the argument tokenizer, redirection parsing, pipe
splitting, chain splitting, and variable expansion. Previously each had its own ad-hoc
double-quote check, and none of them handled escapes.

- **`"text"`** groups an argument and still expands `%VAR%`, matching COMMAND.COM
- **`'text'`** groups an argument literally — no variable expansion and no escape processing
  inside the run
- **`^c`** escapes any single character, so `^&`, `^|`, `^>`, `^<`, `^"`, `^%`, and `^^` are
  data rather than syntax
- **New API**: `shell_find_unquoted_char()`, `shell_find_unquoted_any()`,
  `shell_has_unquoted_char()`, `shell_unescape_in_place()`, and the `shell_quote_state_t` enum
- **`shell_split_args()` rewritten** to honor all three rules and strip the markup, so a
  handler receives the literal value. `echo "hello world"`, `echo 'hello world'`, and
  `echo hello^ world` all produce the same single argument.
- **`shell_expand_variables()`** skips single-quoted runs entirely and passes `^%` through
  untouched

### Added — Command chaining with `&`, `&&`, and `||`

- **`a & b`** runs both commands unconditionally
- **`a && b`** runs `b` only when `a` succeeded
- **`a || b`** runs `b` only when `a` failed
- Operators mix freely on one line: `a && b || c & d`
- Up to `P4_CONFIG_CHAIN_SEGMENT_MAX` (8) commands per chain, with truncation reported rather
  than silently dropping the tail
- **New API**: `shell_split_chain()` with `shell_chain_segment_t` and `shell_chain_op_t`
- **A single `|` is deliberately not a chain separator**, so pipelines still reach the pipe
  executor. `type f | sort && echo done` splits into one pipeline plus one conditional link.
- **Splitting happens before expansion**, per segment, so a variable whose value contains `&`
  cannot inject a new command. This is the same ordering COMMAND.COM uses and it is a
  deliberate safety property, not an implementation artifact.
- **Success is tracked per link** rather than re-read from global errorlevel, so a stale value
  from an earlier line cannot send the next link down the wrong branch
- Unrecognized commands now set `P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND` (9009), matching
  COMMAND.COM, which is what makes `badcmd || echo fallback` work

### Fixed — batch frame overflowed the worker task stack (pre-existing)

Found while reviewing the stack cost of the new chaining code. This bug predates this release
and would have caused stack-overflow crashes on any batch file.

- `shell_execute_batch_file()` placed a **12,272-byte stack frame** on the **8,192-byte**
  command worker task stack, so a single batch file already overflowed it before any nesting.
- The label table alone accounted for 8 KB: 32 label slots each sized at the full 256-byte
  command width, when a label is one short identifier.
- **Fix**: label names are bounded by the new `P4_CONFIG_BATCH_LABEL_BYTES` (48), and both the
  frame and the line buffer moved to the heap. The stack frame is now **96 bytes**.
- The command pipeline's two expansion buffers also moved to the heap, taking
  `shell_execute_command()` from **2,032 to 480 bytes**, because it sits on the same recursion
  path — a batch file re-enters it for every line.
- Four levels of batch nesting now consume roughly 5.6 KB of the 8 KB stack with headroom to
  spare. Verified by disassembling the function prologues, not by inspection.
- Every new allocation is released on every exit path, including the early-return error paths.

### Fixed — `if` command correctness

- **`if exist <file>`** now resolves the path against the current directory and runs inside a
  guarded SD session, like every other filesystem command. It previously called `stat()` on the
  raw argument, so `if exist notes.txt` reported "not found" for any relative path.
- **`if "a"=="b"`** accepts the joined (`a==b`), spaced (`a == b`), and half-spaced (`a ==b`)
  spellings. The comparison previously required the `==` to be glued to the left operand, which
  broke once the tokenizer began removing quotes.

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_ESCAPE_CHAR` (`^`) — the escape character
- `P4_CONFIG_CHAIN_SEGMENT_MAX` (8) — maximum chained commands
- `P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND` (9009) — errorlevel for an unknown command
- `P4_CONFIG_BATCH_LABEL_BYTES` (48) — maximum `:label` name length

### Testing

- Added `test/main/test_shell_quoting.c` with four suites: unquoted-operator scanning across
  all three quoting forms, markup removal, the quoting-aware tokenizer, and chain splitting
  including the pipe-versus-chain distinction and truncation reporting
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.18.0] - 2026-08-08

Completes every roadmap item that was marked ⚠️ (partially implemented). **No commands,
options, output formats, or behaviors were removed.** Every previously working invocation
still works; the changes are additive or replace a stub with the real implementation.

### Added — Interactive keypress wait

The shell core gained a real keypress facility, which is what `pause`, `choice`, and `more`
needed to stop faking a key wait with a timed delay.

- **`components/shell/` keypress queue** with `shell_key_wait_begin()`,
  `shell_wait_for_key()`, `shell_key_wait_end()`, `shell_key_wait_submit()`,
  `shell_key_wait_is_active()`, and `shell_key_input_available()`
- **All three input sources feed it**: the UART console reader forwards the first character of
  a line, the USB HID bridge forwards the decoded key synchronously, and the LVGL on-screen
  keyboard forwards through `main.c`'s input-line callback
- **Input routing is suppressed during a wait**, so a key answering a prompt is never
  dispatched as a shell command and never lingers at the prompt
- **Bounded by `P4_CONFIG_KEY_WAIT_TIMEOUT_MS`** (30 s). When no interactive key source is
  attached, commands fall back to the previous timed behavior, so a headless board never
  stalls a batch file.

### Added — Input redirection and multi-stage pipes

- **`<` input redirection operator** — `sort < notes.txt` reads the file as command input
- **Multi-stage pipelines** — `cmd1 | cmd2 | cmd3`, up to `P4_CONFIG_PIPE_STAGE_MAX` (4)
- **Quote-aware pipe splitting** — `echo "a | b"` is no longer mistaken for a pipeline
- **Shared mechanism** — the `<` operator and each pipe stage both publish through the
  storage input-redirection slot, so `sort < f.txt` and `type f.txt | sort` reach the same
  code path in the text-processing commands
- **Guaranteed spool cleanup** — every stage's temp file is removed on every exit path,
  including a stage failure or an `exit` mid-pipeline

### Changed — Batch control flow is now complete

- **`pause`** blocks on a real keypress instead of a fixed 2-second delay
- **`choice`** blocks on a real keypress and gained the DOS switch set: `/C:list` (allowed
  keys), `/N` (hide the list), `/T:c,secs` (timed default), `/S` (case-sensitive). Unmatched
  keys are ignored like DOS, and errorlevel is set to the 1-based index of the chosen key.
- **`setlocal` / `endlocal`** perform real environment scoping. `setlocal` pushes a snapshot of
  the variable table onto a stack (`P4_CONFIG_SETLOCAL_DEPTH_MAX` = 8) and `endlocal` restores
  it, which correctly reverts creations, modifications, and deletions in one step. A scope left
  open when a batch frame returns is unwound automatically, so a child file cannot leak
  variables into its caller and no snapshot allocation is ever leaked.
- **`exit /b [code]`** leaves only the current batch file; a bare `exit [code]` unwinds every
  nested level. Added `batch_stop_mode_t` so the executor and `for` loops honor both. Previously
  `exit` abused the pending-goto flag, which meant a `goto` on the same line could resurrect
  execution.

### Changed — Runtime prompt engine

- **`prompt` is a full DOS template engine** supporting `$p` (path), `$g` (`>`), `$l` (`<`),
  `$b` (`|`), `$n` (drive), `$d` (date), `$t` (time), `$v` (version), `$s` (space),
  `$_` (newline), `$q` (`=`), `$$` (`$`), `$a` (`&`), `$c` (`(`), `$f` (`)`), `$e` (ESC), and
  `$h` (destructive backspace). Metacharacters are case-insensitive; an unknown one renders
  literally, both matching COMMAND.COM.
- **The template drives both surfaces** — the UART console prompt and the LVGL input line —
  so they can never disagree. Previously the LVGL prompt was fixed and `prompt` only printed
  a message.
- **The input line snapshots the prefix it painted.** Extraction and repair compare against
  that snapshot rather than re-rendering, so a template or path change landing between two
  LVGL events cannot make the shell mis-parse what the user typed.
- **`prompt /?`** lists the metacharacters; `prompt` with no argument shows the stored template
  and its rendered form.

### Changed — `date` and `time` can set the clock

- **`date [MM-DD-YYYY]`** and **`time [HH:MM[:SS]]`** now set the system clock in addition to
  reporting it. Both validate ranges, accept `-` or `/` separators for the date, make seconds
  optional for the time, and report honestly when the clock is not NTP-synchronized. A later
  SNTP sync still wins.

### Changed — File utility commands are now complete

- **`tree` is fully recursive.** Draws the DOS box-drawing outline with correct `+---` and
  `\---` connectors, and gained `/F` (include files; DOS default is directories only) and `/A`
  (plain ASCII connectors). Bounded by `P4_CONFIG_TREE_DEPTH_MAX` (8) and the existing
  128-entry listing cap. Each directory level is buffered on the heap rather than the
  worker-task stack, and prints the DOS `Folder PATH listing` header and summary counts.
- **`sort` replaced the bubble sort with `qsort()`**, raised the capacity from 128 to 1024
  lines, and gained `/R` (reverse), `/I` (case-insensitive), and `/U` (unique). The
  **memory leak is fixed**: there is now a single release path that frees every successful
  `strdup()` even when a mid-read allocation fails or the line cap is hit, and the line table
  itself is freed on every early return.
- **`find` gained `/I`** (case-insensitive), **`/N`** (line numbers), **`/C`** (count only),
  and **`/V`** (invert match), and prints the DOS `---------- <file>` banner.
- **`more` waits for a keypress** between pages: Enter or Space advances, `Q` quits.
- **`fc` reports differing lines in DOS style** with both file contents shown, and now detects
  trailing length differences instead of stopping at the shorter file.
- **All five now resolve paths and use guarded SD sessions.** They previously called `fopen()`
  on the raw argument, so a relative path failed and a missing card produced a bare errno
  message instead of the standard "SD card not present" text.

### Fixed

- **Redirection parser rewritten as a two-pass scan.** The old parser stopped at the first `>`,
  so it could not handle `<` at all and mis-parsed `sort < in.txt > out.txt`. The new parser
  locates every unquoted operator before overwriting any of them, so each target is naturally
  terminated by the next operator with no byte needing to be restored.
- **`for` loops now stop on `exit`.** A loop body that ran `exit` previously kept iterating
  because only the goto flag was checked.

### Configuration

New `P4_CONFIG_*` macros, all documented in `p4minishell_config.yaml`:
- `P4_CONFIG_PIPE_STAGE_MAX` (4) — maximum stages in one pipeline
- `P4_CONFIG_SETLOCAL_DEPTH_MAX` (8) — setlocal nesting limit
- `P4_CONFIG_TREE_DEPTH_MAX` (8) — tree recursion limit
- `P4_CONFIG_PROMPT_TEMPLATE_BYTES` (64) — prompt template buffer
- `P4_CONFIG_PROMPT_DEFAULT_TEMPLATE` (`"PS $p$g "`) — default prompt
- `P4_CONFIG_KEY_QUEUE_DEPTH` (16) — keypress queue depth
- `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` (30000) — single keypress wait timeout
- `P4_CONFIG_SD_DRIVE_LETTER` (`"A:"`) — DOS drive letter for `prompt $n` and `tree`

Changed values:
- `P4_CONFIG_SORT_LINE_MAX` raised from 128 to 1024
- `P4_CONFIG_MORE_PAGE_DELAY_MS` and `P4_CONFIG_PAUSE_DELAY_MS` are now documented as
  fallbacks used only when no interactive key source is attached

### Testing

- Added `test/main/test_shell_prompt.c` with four suites covering template storage, every
  prompt metacharacter, `$p` path expansion, and the keypress-wait state machine
- `test_main.c` now calls `shell_init()` before running the suites that need shell-core state
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

---

## [0.17.0] - 2026-08-08

### Changed — Phase B: Extract Modules

Roadmap Phase B is complete. The batch engine and the SD/storage layer are now separate
components, and the DOS file commands live with the storage layer instead of inside the
dispatcher. **No commands, options, output formats, error messages, or bounds were changed
or removed.** The dispatcher verb table is identical to v0.16.0.

- **Created `components/storage/`** — owns everything between the shell commands and the SD card:
  - `storage.c` / `storage.h`: guarded SD sessions (`shell_sd_begin()` / `shell_sd_end()`),
    persistent mount tracking, `storage_sd_is_mounted()`, the `sd eject` implementation,
    path resolution (`shell_sd_resolve_path()`, `shell_fs_resolve_path()`,
    `shell_resolve_target_from_source()`), FATFS conversion (`shell_sd_vfs_to_fatfs_path()`,
    `shell_sd_fresult_to_esp_err()`), size formatting (`shell_sd_format_size()`),
    `shell_sd_entry_type()`, DOS wildcard matching (`shell_wildcard_match()`), the RAM-only
    current working directory (`shell_get_cwd()`, `storage_set_cwd()`, `shell_fs_print_cwd()`),
    the shared file helpers (`shell_fs_copy_file()`, `shell_list_directory_path()`,
    `shell_print_file_text()`), and the output-redirection writer
    (`shell_write_redirect_output()`)
  - `storage_commands.c` / `storage_commands.h`: every DOS file command — `cd`/`chdir`, `dir`,
    `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`,
    `append`, `touch`, `attrib`, `label`, `xcopy`, `find`, `more`, `tree`, `fc`, `sort`, and the
    `sd info|ls|stat|cat|eject` family
- **Created `components/batch/`** — owns the whole `.bat` interpreter:
  - Batch file execution with nesting (`shell_execute_batch_file()`), batch path resolution
    (`shell_resolve_batch_path()`), `:label` scanning, `goto`, `call :label`,
    `for %%var in (set) do command` loops, and the `|` pipe operator (`shell_execute_pipe()`)
  - The RAM-only environment variable table (24 slots), PATH, and variable expansion
    (`%VAR%`, `%0`, `%1`..`%9`, `%*`) via `shell_env_get()`, `shell_env_set()`, and
    `shell_expand_variables()`
  - Errorlevel tracking (`batch_get_errorlevel()` / `batch_set_errorlevel()`)
  - The batch language commands: `set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`,
    `choice`, `setlocal`, `endlocal`, `exit`
- **`components/command/command.c` reduced from 4,384 lines to 1,140 lines** (74% smaller).
  It now owns only the dispatcher (`shell_execute_command_core()`), the execution pipeline
  (`shell_execute_command()`), the worker task (`shell_execute_command_async()`), redirection
  parsing, the hardware commands (`brightness`, `rotate`, `battery`, `volume`, `gpio`, `rgb`,
  `camera`), the UI query commands (`display`, `keyboard`, `windows`), the system commands
  (`reboot`, `clear`/`cls`, `prompt`, `date`, `time`), and the family routing for
  `wifi`/`bluetooth`/`usb`/`c6ota`/`sd`.

### Changed — Architecture

- **Added `batch_command_ops_t`** — a registration table (mirroring `shell_command_ops_t`) that
  lets the batch engine re-enter the full command pipeline for nested contexts (`if` bodies,
  `for` bodies, pipe stages, batch lines) without depending on `command.h`. Registered by
  `command_init()`. The hook is NULL-checked, so nested execution degrades gracefully with an
  explicit message rather than dereferencing a null pointer.
- **Dependency direction extended and still strictly one-way**:
  `main` → `command` → `batch` → `storage` → `shell` → (`ansi`, `display`, `windows`, `header`,
  `keyboard`, `clock`). No component declares `main` as a requirement, and no module depends
  upward at include time.
- **`command_init()` now sequences module startup**: `storage_init()` (current working
  directory, mount tracking) and `batch_init()` (environment table, PATH default, errorlevel)
  run before either operations table is registered, so no dispatch can observe uninitialized
  state.
- **`shell_get_cwd()` moved from `command.c` to `storage.c`** and `command_sd_is_mounted()` was
  replaced by `storage_sd_is_mounted()`. `shell_command_ops_t` is unchanged; `command_init()`
  simply points the `get_cwd` and `sd_is_mounted` hooks at the storage implementations, so
  `shell.c` sees no difference.

### Build

- Root `CMakeLists.txt` gained `components/batch` and `components/storage` in
  `EXTRA_COMPONENT_DIRS`
- `components/command/CMakeLists.txt` now requires `batch` and `storage`
- `test/CMakeLists.txt` gained the `storage` and `batch` component directories, per the
  unit-test rule that a new `shell`/`command` dependency must be added there
- Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 /
  esp32p4

### Documentation

- Updated `readme.md`, `documentation.md`, `ai-context.md`, `command.md`, `API.md`, `SDK.md`,
  `roadmap.md`, `p4minishell_config.yaml`, and `board_config.yaml` for the new module boundaries
- `roadmap.md` Phase B is marked complete

---

## [0.16.0] - 2026-08-07

### Added
- **Batch `for` loops** — Full `for %%var in (set) do command` implementation with variable expansion
- **Batch `%0` and `%*` expansion** — `%0` expands to script name, `%*` expands to all arguments
- **`:label` parsing** — Labels scanned at batch file load, stored in label table for `goto`/`call :label`
- **`call :label`** — Jump to label within same batch file, with label table lookup
- **`for` loop variable expansion** — `%%var` expanded per iteration in `do` command

### Changed — Phase A: Command Implementation Consolidation

Roadmap Phase A is complete. Every command implementation now lives in
`components/command/command.c`, and `main.c` is reduced to boot orchestration
and LVGL event routing. No commands, options, or output formats were removed.

- **Moved all command implementations from `main.c` to `components/command/command.c`**:
  - Hardware: `brightness`, `rotate`, `battery`, `volume`, `gpio`, `rgb`, `camera`
  - System: `reboot`, `clear`/`cls`
  - File/SD: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
  - SD family: `sd info|ls|stat|cat|eject`, `sdeject`
  - Extended DOS: `attrib`, `label`, `xcopy`, `find`, `more`, `tree`, `fc`, `sort`
  - Environment/batch: `set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`,
    `choice`, `setlocal`, `endlocal`, `prompt`, `date`, `time`, `exit`
  - Batch engine: batch file execution, label scanning, `for` loops, pipes, wildcard matching
  - SD session management, path resolution, environment variables, PATH, and current working directory
- **Removed duplicate hardware command implementations from `main.c`** — the placeholder
  `cmd_battery()`/`cmd_volume()` stubs in `command.c` were replaced by the real ADC and
  ES8311 codec implementations, so `battery` and `volume` no longer depend on a bridge
- **Removed duplicate `shell_execute_command_core()` and `shell_execute_command()` from `main.c`** —
  `command.c` owns the whole pipeline: variable expansion, redirection, dispatch, and the worker task
- **Removed duplicate transcript functions from `main.c`** — `shell_transcript_append_text()`,
  `shell_transcript_appendf()`, `shell_transcript_render()`, `shell_transcript_reset()`,
  `shell_schedule_transcript_appendf()`, the async staging buffer, and the UART console now exist
  only in `components/shell/shell.c`. The shell copies retain `main.c`'s richer behavior:
  half-buffer truncation with a `[history truncated]` marker, oldest-first async drop policy,
  and UART mirroring.
- **Removed duplicate debug functions from `main.c`** — `shell_debug_log_push()`,
  `shell_record_errorf()`, `shell_record_warningf()`, `shell_record_infof()`, and
  `shell_command_debug()` now exist only in `components/shell/shell.c`, keeping `main.c`'s
  transcript surfacing of errors and the Wi-Fi state/heap lines in `debug` output
- **Removed all bridge trampolines** — the 18 `shell_bridge_*` functions and
  `shell_execute_command_core_bridge()` are gone; `p4minishell.h` now declares only the
  c6ota, usb, and networking host callbacks that ESP-IDF requires in the app component
- **Moved input line ownership to `components/shell/`** — added `shell_input_line_set_text()`,
  `shell_input_line_reset()`, `shell_extract_input_text()`, and `shell_input_line_repair_prompt()`
  so the prompt-prefix contract lives in one place
- **Moved `Kconfig.projbuild` from `main/` to `components/networking/`** — that component is the
  only consumer of `CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID`/`_PASSWORD`, and the move lets the
  unit-test project resolve the symbols too

### Changed — Architecture

- **Removed the `command` → `main` component dependency**, which was a layering inversion.
  Dependencies now flow one way: `command` → `shell` → (`ansi`, `display`, `windows`, `header`,
  `keyboard`, `clock`).
- **Added `shell_command_ops_t`** — a registration table (mirroring `networking_host_ops_t`) that
  lets `shell.c` reach command-owned services (dispatch, current working directory, volume,
  SD mount state, battery telemetry) without including `command.h`. Registered by `command_init()`.
- **`main.c` reduced from 5,990 to 397 lines** and now contains only `app_main()`, LVGL event
  callbacks, UI construction, and the c6ota/usb host bridges.
- **Eliminated all 80+ forward declarations** from `main.c`.

### Removed — Dead Code

- Legacy shell-local Wi-Fi runtime in `main.c` (`shell_wifi_runtime_init`, event handlers,
  `shell_wifi_connect_with_credentials`, `shell_wifi_run_diagnostic`, and helpers). All were
  marked `__attribute__((unused))`; `components/networking/` owns this path.
- Legacy hosted Bluedroid Bluetooth path in `main.c`, compiled out behind
  `P4_CONFIG_BT_HOSTED_RUNTIME_SUPPORTED == 0`. `components/networking/bluetooth.c` owns
  hosted NimBLE.
- `reboot_task()` and all unused `static` command duplicates that the compiler had been
  reporting as `-Wunused-function`.
- Unused `var_str` variable in the `for` loop parser.
- Ten leftover `fix_main*.py` / `add_hw_cmds.py` / `clean_corruption.py` migration scripts.

### Fixed

- **Command history buffer overlap** — the full-buffer shift used `snprintf()` with overlapping
  source and destination slots (undefined behavior, caught as `-Werror=restrict`). Now uses `memmove()`.
- **Duplicate ANSI output on the serial console** — `shell_transcript_append_ansi()` wrote the
  plain text and then the raw ANSI text to UART, printing every colored line twice. The plain
  append no longer mirrors to UART.
- **Async transcript flush held a critical section across LVGL calls** — the staging buffer is now
  drained into a local copy before the append, and a re-queue check catches text staged during the flush.
- **`shell_transcript_append_text()` silently dropped output when full** — it now truncates the
  oldest half and inserts a `[history truncated]` marker, matching the previous `main.c` behavior.

### Added — Configuration

All newly extracted literals are `P4_CONFIG_*` macros in `p4minishell_config.h`, documented in
`p4minishell_config.yaml`:
- `P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS`, `P4_CONFIG_HEADER_RSSI_UNKNOWN`
- `P4_CONFIG_BATCH_LABEL_MAX`, `P4_CONFIG_COMMAND_ARGV_MAX`, `P4_CONFIG_SORT_LINE_MAX`
- `P4_CONFIG_MORE_PAGE_LINES`, `P4_CONFIG_MORE_PAGE_DELAY_MS`, `P4_CONFIG_PAUSE_DELAY_MS`
- `P4_CONFIG_PIPE_SETTLE_DELAY_MS`, `P4_CONFIG_REBOOT_DELAY_MS`
- `P4_CONFIG_TEXT_LINE_BYTES`, `P4_CONFIG_LFN_BYTES`, `P4_CONFIG_VOLUME_DEFAULT_PCT`

### Fixed — Unit Test Project

The `test/` project did not configure. It now builds standalone:
- Pinned `IDF_TARGET` to `esp32p4` (previously defaulted to `esp32` and failed on the toolchain)
- Added `test/main/idf_component.yml` pinning `esp_hosted` 2.12.1 and `esp_wifi_remote` 1.4.1
  (the manager was resolving `esp_hosted` 3.x, whose Kconfig is incompatible)
- Added `test/sdkconfig.defaults` mirroring the firmware's build-affecting options, including
  the FATFS LFN settings that `FILINFO.altname` requires
- Staged `board_config.h` and `p4minishell_config.h` into the generated config directory
- Added the missing `components/p4_usb`, `espressif__usb_host_hid`, `espressif__usb_host_msc`,
  and `espressif__esp_lcd_touch` component directories
- Added tests for `shell_format_command_for_transcript()` covering password masking,
  pass-through, and NULL input

### Updated

- **Version bump**: 0.15.1 → 0.16.0 (`p4minishell_config.h` version macros were stale at 0.14.2
  and are now correct)
- **`.gitignore`**: ignore `test/managed_components/`

### Verified
- **Clean build**: Zero errors, zero warnings for both the firmware and the unit-test project
  on ESP-IDF v5.5.5 / esp32p4
- **No regressions**: All 65 command dispatch verbs preserved and reachable; every command
  implementation, option, and output string carried across unchanged
- **Batch features**: `for` loops, `%0`/`%*`, `:label`, `goto`, `call :label` all still functional
- **Hardware commands**: `brightness`, `rotate`, `battery`, `volume`, `gpio` all still functional

---

## [0.15.1] - 2026-08-07

### Changed
- **Merged duplicate command dispatch logic** — Consolidated command execution pipeline from `main.c` and `components/command/command.c` into a single path:
  - `main.c` now handles variable expansion (`%VAR%`, `%1`..`%9`) and output redirection (`>` / `>>`) before dispatch
  - `components/command/command.c` owns the unified command dispatcher (`shell_execute_command_core`) and worker task
  - Removed duplicate `shell_execute_command`, `shell_execute_command_core`, and `shell_command_task` from `main.c`
  - Added bridge function `shell_execute_command_core_bridge` for main.c to invoke command.c's dispatcher
- **Updated all documentation** — All `.md` files, configs, and references updated to reflect ESP-IDF v5.5.5 and merged dispatch architecture
- **Fixed deprecation warning** — Updated `esp_lvgl_port` DSI callback from deprecated `on_refresh_done` to `on_frame_buf_complete` for ESP-IDF 5.5.0+

### Updated
- **ESP-IDF baseline**: v5.5.3 → v5.5.5 across all configs, lock files, and documentation
- **Version bump**: 0.15.0 → 0.15.1

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4
- **Flash**: Successful
- **Boot**: Device boots, Wi-Fi connects, all commands functional

---

## [0.15.0] - 2026-04-30

### Added
- **Consistent ANSI color-coding across all components** — All text output (header, shell, commands, Wi-Fi, USB, OTA) now uses a unified color scheme:
  - `@C` (bright cyan) = subsystem labels and property keys (e.g., `wifi.state:`, `board.name:`)
  - `@G` (bright green) = success/connected/active status
  - `@r` (red) = errors and failures
  - `@y` (yellow) = warnings and cautions
  - `@Y` (bright yellow) = section headings
  - `@W` (bright white) = important values (SSIDs, IPs, paths)
  - `@Z` (bright magenta) = numeric values
  - `@k` (bright black/gray) = muted/secondary text
  - `@N` (bright cyan) = progress/info messages
  - `@R` = reset to default at end of every colored segment
- **ANSI callback in networking_host_ops_t** — New `schedule_transcript_appendf_ansi` callback for colored output from components
- **`networking_schedulef_ansi()` helper** — ANSI-capable formatted output for the networking module

### Changed
- **networking.c**: All wifi_status, wifi_scan, wifi_diag, wifi_disconnect, wifi help, event handler, watchdog, and sysinfo output now uses ANSI color tokens
- **networking.c**: `networking_appendf()` now routes through ANSI path for color support
- **main.c**: Wired `shell_transcript_appendf_ansi` into networking host ops
- **p4minishell_config.h**: Added new semantic color token definitions (subsystem, key, muted, heading, IP, connected, disconnected, progress, prompt)

### Verified
- **Clean build**: Zero errors, zero warnings
- **Flash**: Successful to COM3
- **Boot**: Device boots, Wi-Fi connects with colored output

---

## [0.14.2] - 2026-04-30

### Added
- **attrib command**: Show and set FATFS file attributes (R=read-only, H=hidden, S=system, A=archive). Uses `f_stat()`/`f_chmod()`. Supports `attrib [path]`, `attrib +R file`, `attrib -H file`, etc.
- **label command**: Read and set FATFS volume label via `f_getlabel()`/`f_setlabel()`. Max 11 characters (FAT 8.3 convention).
- **xcopy command**: Recursive directory copy with `/S` flag for subdirectory traversal.
- **Wildcard matching**: `shell_wildcard_match()` for DOS-style `*` and `?` pattern matching.
- **Wildcard-aware dir**: `dir *.txt` filters directory listings by wildcard pattern with short/long filename display.
- **Wildcard-aware del**: `del *.bak` deletes all matching files in a directory.
- **Wildcard-aware copy**: `copy *.txt backup\` copies all matching files to a destination directory.

### Changed
- **main.c**: `shell_command_dir()`, `shell_command_del()`, `shell_command_copy()` now detect wildcard characters and use filtered directory iteration.
- **main.c**: `shell_wildcard_match()`, `shell_command_attrib()`, `shell_command_label()`, `shell_command_xcopy()` are non-static for `command.c` extern dispatch.
- **command.c**: Existing `extern` declarations for `attrib`, `label`, `xcopy` now resolve correctly against main.c implementations.

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Flash**: Successful to COM3
- **c6ota**: Verified working end-to-end (SD source, image validation, OTA transfer to 100%, Wi-Fi restore)

---

## [0.14.1] - 2026-04-30

### Fixed
- **Backspace CLI bug**: Pressing Backspace when the command line is empty no longer inserts the literal text `P4Shell>`. The `LV_EVENT_VALUE_CHANGED` handler now correctly detects partial prompt deletion and restores only the user text portion, not duplicating the prompt prefix.
- **Touch keyboard button**: The on-screen keyboard's "keyboard" button (LV_SYMBOL_KEYBOARD) now correctly hides the keyboard when pressed. Added `LV_EVENT_CANCEL` handling in `shell_input_line_event_cb` that calls `keyboard_hide()`.
- **Wi-Fi mutex implementation**: The Wi-Fi mutex (`wifi_lock`/`wifi_unlock`) and atomic init-task claim (`wifi_try_claim_init_task`/`wifi_release_init_task`) that were declared in the changelog for v0.13.1 but never implemented are now fully functional. All shared Wi-Fi state access is mutex-protected.
- **Wi-Fi persistent watchdog**: The watchdog task (`networking_wifi_watchdog_task`) that was declared but never implemented is now fully functional. It monitors Wi-Fi connection state and retries with exponential backoff (1s → 2s → 4s → ... → 30s cap) for up to 120 seconds total, handling the C6's typical associate-then-disconnect boot behavior.
- **Null pointer safety**: Added NULL checks for `lv_textarea_get_text()` return values in the USB keyboard injection Home/End handlers in `shell_usb_keyboard_inject_cb`.
- **Async dispatch error handling**: Added `lv_async_call` return value check in `shell_usb_keyboard_input` to free the context on dispatch failure.

### Changed
- **networking.c**: All Wi-Fi state transitions in the event handler, `networking_wifi_connect_with_credentials`, `networking_wifi_begin_connect_request`, and `networking_wifi_begin_background_request` are now mutex-protected.
- **networking.c**: `wifi_try_claim_init_task`/`wifi_release_init_task` replace direct `s_wifi_init_task_in_progress` manipulation for TOCTOU-safe init-task claiming.
- **main.c**: `shell_input_line_event_cb` `LV_EVENT_VALUE_CHANGED` handler rewritten to correctly handle partial prompt deletion by detecting common prefix length and extracting only user text.
- **shell.c**: `shell_usb_keyboard_input` now checks `lv_async_call` return value and frees context on failure.

### Verified
- **Clean build**: Zero errors, zero warnings (3 pre-existing deprecated API warnings from ESP-IDF v5.5.3 VFS API)
- **Boot**: Clean, Wi-Fi watchdog operational with exponential backoff retry
- **Flash**: Successful to COM3
- **Runtime**: No crashes observed; watchdog correctly retries disconnected Wi-Fi

---

## [0.14.0] - 2026-04-30

### Added
- **ANSI/VT escape sequence module**: New `components/ansi/` module providing SGR (Select Graphic Rendition) escape sequence parsing and formatting
- **16-color ANSI palette**: PowerShell-inspired color palette with configurable standard and bright colors (black, red, green, yellow, blue, magenta, cyan, white)
- **SGR attribute support**: Bold, dim, italic, underline, blink, reverse, hidden, strikethrough
- **ANSI format string builder**: `ansi_format()` / `ansi_vformat()` with `@`-prefixed color/attribute specifiers (`@g` for green, `@r` for red, `@B` for bold, `@R` for reset, etc.)
- **ANSI-aware transcript functions**: `shell_transcript_append_ansi()` and `shell_transcript_appendf_ansi()` for colored transcript output
- **ANSI text processing**: `ansi_process_text()` state machine for segment-by-segment ANSI rendering
- **ANSI-to-plain stripping**: `ansi_strip_to_plain()` for LVGL transcript textarea (which doesn't support per-character styling)
- **UART console ANSI pass-through**: Raw ANSI codes passed to serial terminal for native rendering
- **Config macros**: `P4_CONFIG_ANSI_*` for all 16 colors + default FG/BG + buffer size

### Changed
- **All system info commands**: `help`, `sysinfo`, `version`, `about`, `mem`, `debug` now use ANSI-colored output with green headers, cyan labels, red errors, yellow warnings
- **All hardware commands**: `brightness`, `rotate`, `battery`, `volume`, `reboot` now use ANSI-colored output (green success, red errors, yellow usage)
- **Display/keyboard/windows commands**: All now use ANSI-colored output with consistent color scheme
- **Unknown command error**: Now shown in red
- **Boot banner**: Now displayed in bright green
- **Debug log**: Errors shown in red, warnings in yellow, info in default color
- **Shell prompt**: UART console prompt now passes through ANSI codes for colored terminal rendering
- **shell.h**: Added `shell_transcript_append_ansi()` and `shell_transcript_appendf_ansi()` public API
- **p4minishell_config.h**: Version bumped to 0.14.0; added ANSI color palette section (16 colors + defaults + buffer size)
- **p4minishell_config.yaml**: Documented all ANSI color values with descriptions and valid ranges
- **CMakeLists.txt**: Added `components/ansi` to EXTRA_COMPONENT_DIRS
- **shell.c**: Now calls `ansi_init()` during `shell_init()`; imports `ansi.h`

### Architecture
- New `components/ansi/` module with `ansi.h` (public API) and `ansi.c` (implementation)
- ANSI module is independent of LVGL; only depends on `p4minishell_config.h` and ESP-IDF
- Color palette initialized from config macros; runtime-modifiable via `ansi_set_palette_color()`
- `shell_transcript_append_ansi()` strips ANSI for LVGL textarea, passes raw ANSI to UART console
- All existing `shell_transcript_append_text()` / `shell_transcript_appendf()` calls continue to work unchanged
- Backward compatible: no existing API removed or broken

---

## [0.13.0] - 2026-04-30

### Added
- **USB keyboard auto-detect**: USB HID keyboard automatically detected when plugged in
- **USB keyboard CLI injection**: Keystrokes from USB keyboard routed to shell input line
- **Full USB HID key map**: Complete US keyboard layout including all symbols, keypad, navigation, function keys, and modifier-aware shifted characters
- **On-screen keyboard auto-hide**: LVGL keyboard automatically hidden when USB keyboard attached; restored when unplugged
- **External input mode**: `keyboard_set_external_input()` / `keyboard_is_external_input_enabled()` API for keyboard component
- **Force-visible override**: `keyboard_force_visible()` / `keyboard_clear_force_visible()` to keep on-screen keyboard visible even with USB keyboard
- **USB keyboard state queries**: `usb_is_keyboard_attached()`, `usb_is_mouse_attached()` public API
- **USB keyboard input callback**: `usb_register_keyboard_input_callback()` for shell CLI integration
- **Public key mapping API**: `usb_key_to_ascii_full()`, `usb_key_name_full()` for external consumers
- **Config macros**: `P4_CONFIG_USB_KEYBOARD_AUTO_DETECT`, `P4_CONFIG_USB_KEYBOARD_CLI_INJECT`, `P4_CONFIG_USB_KEYBOARD_NOTIFY_MS`
- **Header notification**: "USB keyboard detected" / "USB keyboard removed" on plug/unplug events

### Changed
- **usb.c**: Extended key map from 11 to 60+ USB HID key codes; keyboard input handler routes to both echo and CLI callback
- **keyboard.c**: Added external input mode with auto-hide/restore logic
- **shell.c**: Added `shell_usb_keyboard_input()` bridge function with LVGL async dispatch for safe input line injection
- **main.c**: Registers USB keyboard callback after `usb_init()`; header refresh timer monitors USB keyboard attach state
- **usb.h**: Expanded public API surface with new types, callbacks, and query functions
- **p4minishell_config.h**: Version bumped to 0.13.0; added USB keyboard config macros
- **p4minishell_config.yaml**: Documented new USB keyboard config values

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects, display renders, no regressions

---

## [0.13.1] - 2026-04-30

### Fixed
- **SD card header status**: Fixed "SD NO" showing in header even when SD card is mounted. Replaced `stat()`-based mount detection with persistent mount tracking via `s_sd_persistent_mounted` flag set/cleared by `shell_sd_begin()`/`shell_sd_end()`
- **Wi-Fi random boot failure**: Added mutex (`s_wifi_mutex`) protecting all shared Wi-Fi state (s_wifi_state, s_wifi_connected, s_wifi_target_ssid, etc.) from race conditions
- **TOCTOU race in Wi-Fi init**: Replaced unprotected `s_wifi_init_task_in_progress` check with atomic `wifi_try_claim_init_task()` / `wifi_release_init_task()` to prevent double-init
- **Wi-Fi disconnect during boot**: Added `networking_wifi_auto_reconnect_task()` that retries `esp_wifi_connect()` after 1 second delay on `WIFI_EVENT_STA_DISCONNECTED`
- **NVS init reliability**: Added retry loop (2 attempts) with erase-and-retry for `nvs_flash_init()` to handle transient NFS errors
- **Wi-Fi connect flow**: Removed unnecessary `esp_wifi_disconnect()` before `esp_wifi_connect()` to avoid race with event handler

### Added
- **Wi-Fi mutex**: `wifi_lock()` / `wifi_unlock()` helpers protecting all state transitions in event handler, init, connect, and diagnostics
- **Wi-Fi auto-reconnect**: Dedicated task spawned on disconnect to retry connection after 1s delay
- **SD persistent mount flag**: `s_sd_persistent_mounted` tracks actual card state across mount/unmount cycles for accurate header display
- **NVS retry logic**: `networking_wifi_runtime_init()` now retries NVS init twice with erase recovery

### Changed
- **networking.c**: Added `s_wifi_mutex`, `wifi_lock()`, `wifi_unlock()`, `wifi_try_claim_init_task()`, `wifi_release_init_task()`, `networking_wifi_auto_reconnect_task()` — all state transitions now mutex-protected
- **main.c**: Added `s_sd_persistent_mounted` flag; `shell_sd_header_is_mounted()` now uses persistent flag instead of `stat()`; `shell_sd_begin()`/`shell_sd_end()` update the flag

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects reliably, SD card accessible, header shows correct SD state
- **Flash**: Successful to COM3

---

## [0.12.0] - 2026-04-30

### Fixed
- **Screen flash**: Removed `header_force_render()` from periodic header refresh timer

### Added
- **Clock component**: New `components/clock/` with SNTP time sync from pool.ntp.org
- **Timezone support**: POSIX TZ string support
- **Time in ver/sysinfo/about**: Formatted local time with NTP sync status

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Flash**: Successful to COM3

---

## [0.11.0] - 2026-04-30

### Added
- **Shell component**: New `components/shell/` owning transcript, history, debug log, UART console, system info commands
- **Command component**: New `components/command/` owning command dispatch, execution task, all built-in commands
- **Windows info command**: `windows info` shows display dimensions and region rectangles

### Changed
- **main.c**: Retains only app_main(), shell_build_ui(), LVGL callbacks, Wi-Fi/Bluetooth/SD/FS/batch state
- All transcript/history/debug/UART/command code moved to shell.c and command.c

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects, all commands preserved

---

## [0.10.0] - 2026-04-30

### Added
- **Keyboard component**: New `components/keyboard/` module owning the LVGL keyboard widget
- **keyboard.h / keyboard.c**: Keyboard manager with visibility control, mode switching, textarea binding
- **Keyboard hide/show**: `keyboard_hide()` / `keyboard_show()` with automatic UI reflow
- **Shell command**: `keyboard show|hide|toggle|status`
- **Config macros**: `P4_CONFIG_KEYBOARD_*` for height, visibility defaults

### Changed
- **windows.c**: Keyboard delegated to keyboard component; `windows_get_rect()` accounts for keyboard visibility
- **main.c**: Added `keyboard` command family dispatch

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean, Wi-Fi connects, no regressions

---

## [0.9.0] - 2026-04-30

### Added
- **Window manager module**: New `components/windows/` that owns the LVGL screen layout and dynamic scaling
- **windows.h / windows.c**: Central window/layout manager dividing the screen into named regions (HEADER, TRANSCRIPT, INPUT_ROW, KEYBOARD)
- **Resolution-aware scaling**: All window regions dynamically scale based on current display resolution from display.c
- **Rotation-aware layout**: Window manager recalculates all dimensions on rotation change
- **Consistent styling API**: `windows_get_color()` with semantic color names
- **Window object accessors**: `windows_get_transcript()`, `windows_get_input_line()`, `windows_get_keyboard()`, etc.
- **Dimension query API**: `windows_get_rect()`, `windows_get_display_width()`, `windows_get_display_height()`
- **Scaling helpers**: `windows_scale_height_percent()`, `windows_scale_width_percent()` with min/max clamping
- **display.h expanded**: Added `display_get_width()` and `display_get_height()` convenience functions
- **Config macros**: `P4_CONFIG_WINDOW_*` for all window region scaling parameters

### Changed
- **main.c shell_build_ui()**: Refactored from ~80 lines of manual LVGL widget creation to ~40 lines using window manager API
- **main.c LVGL widgets**: Removed 5 static LVGL object variables — now owned by windows.c
- **All hardcoded scaling values**: Moved to `P4_CONFIG_WINDOW_*` config macros

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Boot**: Clean boot, shell UI renders correctly, Wi-Fi connects
- **No regressions**: All shell commands preserved

---

## [0.8.0] - 2026-04-30

### Added
- **Display manager module**: New `components/display/` module that owns all display hardware state and operations
- **display.h / display.c**: Central display controller with public API for rotation, resolution, refresh rate, brightness, and power management
- **Rotation API**: `display_set_rotation()`, `display_get_rotation()`, `display_rotation_parse()`, `display_rotation_to_string()` — clean rotation control with automatic touch remapping
- **Resolution API**: `display_get_resolution()`, `display_get_native_resolution()` — query current and native panel resolution accounting for rotation
- **Refresh rate API**: `display_get_refresh_config()`, `display_set_refresh_rate()` — query and configure display refresh rate (dynamic rate change noted as not supported on current JD9165 panel)
- **Brightness API**: `display_get_brightness()`, `display_set_brightness()` — backlight control through display manager
- **Power management API**: `display_set_power_state()`, `display_sleep()`, `display_wake()` — display power state transitions (on/sleep/off)
- **Display info API**: `display_get_info()`, `display_print_info()` — comprehensive display diagnostics for sysinfo
- **UI rebuild callback**: `display_register_ui_rebuild_callback()` — allows the shell to register a callback for rotation-triggered UI rebuilds
- **Thread-safe state tracking**: All display state protected by critical sections; LVGL operations dispatched via `lv_async_call`
- **Touch handle management**: Lazy touch handle acquisition cached internally; touch rotation remapping handled automatically on rotation change

### Changed
- **main.c refactored**: All display-related code extracted to `components/display/`
- **Removed from main.c**: `s_display`, `s_touch_handle`, `s_backlight_percent`, `s_display_rotation` static variables
- **Removed from main.c**: `shell_get_touch_handle_from_bsp()`, `shell_update_touch_rotation()`, `shell_rotation_apply()`, `shell_async_rebuild_ui()` — now handled by display manager
- **shell_command_brightness()**: Now calls `display_set_brightness()` instead of `bsp_display_brightness_set()`
- **shell_command_rotate()**: Now calls `display_rotation_parse()` + `display_set_rotation()` instead of inline LVGL rotation logic
- **app_main()**: Display init now uses `display_init()` which wraps `bsp_display_start_with_config()`; UI rebuild callback registered via `display_register_ui_rebuild_callback()`
- **sysinfo display output**: Now uses `display_get_info()` for comprehensive, structured display diagnostics
- **CMakeLists.txt**: Added `components/display` to `EXTRA_COMPONENT_DIRS`; main component now requires `display`
- **shell_lvgl_touch_ctx_t**: Removed from main.c (now internal to display.c)

### Architecture
- **Display state ownership**: The display manager OWNS all display state. The shell layer calls into the display manager for all display operations.
- **Component layout**: `components/display/` follows the same pattern as `components/header/`, `components/networking/`, etc.
- **Backward compatibility**: All existing shell commands (`brightness`, `rotate`) continue to work identically
- **No BSP bypass**: Display init still uses `bsp_display_start_with_config()` with `BOARD_CFG_*` values

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **No regressions**: Boot path, screen rendering, Wi-Fi, all commands preserved
- **Binary**: p4minishell.bin generated successfully

### Documentation
- Updated all project documentation files with display manager architecture
- Added display manager to module layout in documentation.md
- Added display API to API.md and SDK.md
- Updated ai-context.md with display manager rules
- Updated command.md with display manager notes
- Updated board_config.yaml display section
- Updated roadmap.md with display manager completion

---

## [0.7.1] - 2026-04-29

### Fixed
- **rotate command**: Now correctly rebuilds the entire UI after display rotation via `lv_display_set_rotation()`
- **Touch sync**: GT911 touch remapping now properly matches the rotated display orientation
- **UI scaling**: All UI elements (header, transcript, input row, keyboard) now dynamically rescale to the rotated resolution
- **header_deinit()**: New function to clean up header widgets before UI rebuild after rotation

### Changed
- **shell_rotation_apply()**: Now calls `shell_build_ui()` after rotation to recreate all widgets at the new resolution
- **shell_build_ui()**: Calls `header_deinit()` before `lv_obj_clean()` so header can re-initialize fresh
- **Keyboard height**: Now dynamically scaled to ~35% of vertical resolution (clamped 180-280px)
- **Input row height**: Now dynamically scaled to ~8% of vertical resolution (clamped 40-56px)
- **header_scale_height()**: Already rotation-aware (uses correct resolution axis for 90/270 degree rotations)

### Verified
- **Clean build**: Zero errors, zero warnings
- **Binary**: p4minishell.bin 1,494,768 bytes (82% free)
- **No regressions**: Boot, screen rendering, Wi-Fi, all commands preserved

---

## [0.7.0] - 2026-04-29

### Added
- **Header system panel redesigned**: MEM | CPU | BAT all on the far right, dynamically linked to FreeRTOS runtime statistics
- **CPU usage in header**: Real-time CPU bar + percentage from FreeRTOS idle task runtime counter deltas (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
- **Battery always visible**: Shows "BAT N/C" with muted styling when ADC is not connected or unavailable
- **header_update_mem()**: New API for real-time heap statistics (free/total from heap_caps)
- **header_update_cpu()**: New API for real-time CPU usage and task count
- **header_update_uptime()**: New API for system uptime tracking
- **header_update_battery() signature change**: Now takes `bool adc_ready` second parameter

### Changed
- **sysinfo command**: Now includes FreeRTOS task count, uptime (days/hours/minutes/seconds), total heap with percentage
- **version command**: Expanded with chip info, uptime, heap stats, and active task count
- **mem command**: Added total heap, percentage free, and task count
- **about command**: Added header description, uptime, and task count
- **header.c**: System panel layout changed to MEM | CPU | BAT with CPU bar widget and separators
- **header.c**: Battery rendering split into adc_ready (live data) and !adc_ready (N/C muted) paths
- **main.c**: shell_header_status_refresh() now collects FreeRTOS runtime stats, calculates CPU usage from idle task deltas, and pushes all metrics to header
- **main.c**: Boot timestamp captured via esp_timer_get_time() for real-time uptime

### Verified
- **Clean build**: Zero errors, zero warnings
- **No regressions**: Boot, screen rendering, Wi-Fi, all 40+ commands preserved
- **FreeRTOS configs**: CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, CONFIG_FREERTOS_USE_TRACE_FACILITY, CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS all enabled

### Documentation
- Updated API.md with new header_update_mem/cpu/uptime signatures
- Updated SDK.md with real-time FreeRTOS integration notes
- Updated p4minishell_config.h with CPU/MEM/BAT threshold constants
- Updated p4minishell_config.yaml to v0.3.0

---

## [0.6.0] - 2026-04-29

### Fixed
- **SD card status icon in header**: Fixed `header_update_sd()` to immediately update state and fall back to direct render when LVGL async dispatch fails
- **Header update robustness**: All `header_update_*()` functions now update internal state immediately (safe from any task context) and fall back to synchronous render on allocation/async failure
- **SD icon rendering**: SD indicator now uses consistent `HEADER_SD_SYMBOL` in both mounted and unmounted states instead of `LV_SYMBOL_WARNING` when unmounted

### Changed
- **header.c**: Refactored all public update functions (`header_update_wifi`, `header_update_battery`, `header_update_bluetooth`, `header_update_usb`, `header_update_sd`) to set state immediately before scheduling async render
- **header.c**: Async callbacks simplified to render-only (state already set by caller)
- **header.c**: Battery percent clamping moved from async callback to public API entry point

### Verified
- **Clean build**: Zero errors, zero warnings
- **Binary**: p4minishell.bin 1,490,688 bytes (82% free)
- **No regressions**: Boot, screen rendering, Wi-Fi, all commands preserved

---

## [0.5.0] - 2026-04-29

### Fixed
- **Stale AI comments**: Replaced all 25+ `// AI:` prefixed comments across main.c, usb.c, c6ota.c, and managed BSP with proper descriptive comments
- **Comment consistency**: All section markers now use descriptive text instead of AI-prefixed tags

### Verified
- **Clean build**: Zero errors, zero warnings on ESP-IDF v5.5.5 / esp32p4 target
- **Binary integrity**: p4minishell.bin 1,490,608 bytes (82% free in 8MB partition)
- **No regressions**: Boot, screen rendering, Wi-Fi, all 40+ commands preserved

### Documentation
- Updated ai-context.md with audit rules and verification checklist
- Updated changelog.md with v0.5.0 hardening entry

---

## [0.4.0] - 2026-04-29

### Added
- **p4minishell.h**: Public API header declaring host bridge callbacks and shared shell utilities
- **p4minishell.c**: Extracted `shell_networking_*` bridge functions from main.c into dedicated module
- **Modular bridge layer**: Networking bridge functions now live in `main/p4minishell.c`, callable from `components/networking`

### Changed
- **main/main.c**: Added `#include "p4minishell.h"` and `#include "p4minishell_config.h"`; networking bridge functions changed from `static` to public linkage
- **main/CMakeLists.txt**: Added `p4minishell.c` to SRCS
- **main/p4minishell.c**: Rewritten as thin bridge layer with extern forward declarations to main.c shell functions

### Architecture Note
- c6ota and usb host bridge functions remain in main.c due to ESP-IDF component model requirements (cross-component linking needs app-component residency)
- networking bridge functions extracted to p4minishell.c since networking component links against main transitively

---

## [0.3.0] - 2026-04-29

### Added
- **Centralized configuration system**: All hardcoded values moved to `p4minishell_config.h` with companion YAML documentation in `p4minishell_config.yaml`
- **Config header**: Single C header with all tunable values organized by subsystem (shell identity, buffers, UI layout, Wi-Fi, Bluetooth, SD card, batch engine, GPIO, header visuals, USB host, C6 OTA, task stacks)
- **Config YAML**: Machine-readable documentation with value, type, description, and valid range for every configurable parameter
- **Backward-compatible aliases**: All existing `SHELL_*`, `NETWORKING_*`, `BLUETOOTH_*`, `HEADER_*`, `C6OTA_*`, and `USB_*` macros preserved as aliases to `P4_CONFIG_*` equivalents

### Changed
- **main/main.c**: Replaced 40+ inline `#define` macros with `#include "p4minishell_config.h"` plus backward-compatibility aliases
- **components/networking/networking.c**: Moved all `#define` values to config header
- **components/networking/bluetooth.c**: Moved all `#define` values to config header
- **components/header/header.c**: Moved color and sizing defines to config header
- **components/c6ota/c6ota.c**: Moved OTA parameters to config header
- **components/usb/usb.c**: Moved USB host parameters to config header; HID key codes kept local (standard USB HID usage table)
- **All component CMakeLists.txt**: Added `${CMAKE_SOURCE_DIR}` to `INCLUDE_DIRS` for config header access

---

## [0.2.0] - 2026-04-29

### Changed
- **Documentation overhaul**: Rewrote all project documentation files (`readme.md`, `documentation.md`, `ai-context.md`, `command.md`, `roadmap.md`, `licence.md`, `API.md`, `SDK.md`) with consistent structure, comprehensive detail, and proper Markdown formatting
- **Source code comments**: Replaced all `// AI:` prefixed comments with proper Doxygen-style documentation throughout `main/main.c`, all component headers, and `board_config.h`
- **Header files**: Added full `@file` Doxygen blocks with architecture descriptions, parameter documentation, and behavioral contracts to `header.h`, `networking.h`, `bluetooth.h`, `c6ota.h`, and `usb.h`
- **board_config.yaml**: Restructured with clear section headers, detailed pin descriptions, and validated hardware metadata

---

## [0.1.24] - 2026-03-18

### Added
- New `components/header` module: fixed LVGL top bar for notifications plus passive Wi-Fi, battery, Bluetooth, USB, and SD status indicators
- Live header notifications from Wi-Fi, USB, Bluetooth, and `c6ota` event paths
- Resolution-scaled header height with clamped minimum/maximum

### Fixed
- SD card status icon now appears consistently with same size, color, and alignment as other status icons
- Header notification area stays blank when idle, shows only during active module events
- SD mount/unmount state changes now trigger immediate header icon updates

### Changed
- Header status indicators use guaranteed-visible retro ASCII labels instead of LVGL symbol glyphs
- Header is non-scrollable with left-to-right status icons and notification area on far right

---

## [0.1.23] - 2026-03-18

### Added
- New `components/usb` module: ESP-IDF USB Host bring-up for MSC external storage and HID keyboard/mouse
- Shell-facing `usb` command family: `usb status`, `usb ls [path]`, `usb keyboard <on|off>`, `usb mouse <on|off>`
- USB MSC storage mounted at `/usb0` via VFS/FATFS with transcript-friendly output
- Managed component dependencies: `espressif/usb_host_msc`, `espressif/usb_host_hid`

---

## [0.1.22] - 2026-03-18

### Changed
- **c6ota refactored** into `components/c6ota` with identical public API and behavior
- Full ESP32-C6 OTA flow moved out of `main/main.c` while preserving confirmation flow, transcript output, source handling, hosted OTA RPC sequence, and Wi-Fi stop/restore
- Added `API.md` and `SDK.md` documenting the stable `c6ota_init`, `c6ota_perform`, and `c6ota_register_progress_callback` integration surface

---

## [0.1.21] - 2026-03-18

### Added
- Hosted NimBLE Bluetooth on ESP32-C6 over ESP-Hosted VHCI in `components/networking/bluetooth.c`
- Shell commands: `bluetooth status`, `bluetooth scan`, `bluetooth advertise <on|off>`, with `bt` alias
- Stateful Bluetooth lifecycle: scan/advertise reuse active hosted controller session

### Changed
- **Networking refactored** into `components/networking`: Wi-Fi runtime state, hosted startup, OTA restore hooks, and Bluetooth handling no longer live in `main/main.c`
- Preserved all existing hosted Wi-Fi behavior and command flow through new module APIs
- Replaced disabled hosted Bluedroid stub with hosted NimBLE

---

## [0.1.20] - 2026-03-18

### Added
- **Serial console bridge**: ESP-IDF UART/USB-Serial-JTAG monitor now accepts shell commands
- stdin lines routed through existing shell worker, transcript, masking, and history flow
- stdout mirrors transcript output

### Fixed
- Monitor write-timeout caused by firmware not consuming interactive serial input
- Serial prompt loop no longer floods `P4Shell>` during idle stdin polling

---

## [0.1.19] - 2026-03-18

### Fixed
- **Command-family dispatch regression**: `wifi status|scan|diag|connect|disconnect` now work correctly after boot
- Root cause: parser now preserves original unsplit command text before tokenization so family handlers receive full command line
- Same fix applied to `sd` and `c6ota` family handlers

---

## [0.1.18] - 2026-03-17

### Changed
- Disabled hosted Bluedroid Bluetooth path after `bt enable` caused board crashes in HCI parser
- `bt` command surface kept visible but returns explicit unsupported state
- Removed direct host BT build dependency

---

## [0.1.17] - 2026-03-17

### Changed
- `gpio list` and `gpio status` now report pins with clearer board-role text
- `rgb` and `camera` messages updated to honestly explain current hardware gaps

### Added
- Hosted Bluedroid Bluetooth path enabled in host build with `bt status|enable|scan`

---

## [0.1.16] - 2026-03-17

### Added
- Hardware control commands: `brightness <0-100>`, `rotate <0|90|180|270>`, `battery`, `volume <0-100>`
- `gpio list|status|read <pin>|set <pin> <0|1>` with write restrictions
- Runtime display rotation with GT911 touch remapping
- ADC-backed battery reporting using board-configured divider values
- ES8311 speaker volume control through BSP codec path
- Parser-visible `bt`, `rgb`, and `camera` families with sdkconfig/metadata gates

---

## [0.1.15] - 2026-03-17

### Added
- `roadmap.md`: parity plan for COMMAND.COM features, native app loading, shell SDK
- `licence.md`: proprietary notice for project-authored code plus third-party license summary

### Changed
- README rewritten to describe P4MiniShell as an embedded DOS-style shell platform

---

## [0.1.14] - 2026-03-17

### Added
- COMMAND.COM-style SD workflow: `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`
- RAM-only environment variables: `set`, `path`, `echo`
- Batch file engine: `.bat` execution with `%1`..`%9` expansion, `rem` comments, `echo on/off`, PATH-based lookup
- SD-backed output redirection: `>` and `>>` for text-producing commands

---

## [0.1.13] - 2026-03-17

### Changed
- Finalized ESP-Hosted profile: `espressif/esp_hosted 2.12.1` + `espressif/esp_wifi_remote 1.4.1`
- 1-bit SDIO at 10 MHz on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17
- Forced ESP32-C6 reset on every host boot through GPIO54
- 1500-byte `c6ota` transfer chunks

### Fixed
- End-to-end validation: shell UI, BSP display/touch, hosted Wi-Fi, SD tools, and `c6ota` all working together

---

## [0.1.12] - 2026-03-17

### Fixed
- Restored `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP` after Wi-Fi failures
- Restored boot-time and post-`c6ota` Wi-Fi diagnostic pass
- Removed app-side hosted log suppression

---

## [0.1.11] - 2026-03-17

### Fixed
- Reverted `ESP_HOSTED_EVENT_TRANSPORT_UP` wait experiment that regressed Wi-Fi startup
- Restored prior hosted startup order: connect C6, validate firmware version, continue Wi-Fi path

---

## [0.1.10] - 2026-03-17

### Changed
- Removed automatic `wifi diag` scan from boot-time startup and post-`c6ota` restore
- Restored hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`
- Suppressed non-actionable `H_SDIO_DRV` and `rpc_rsp` warning noise

---

## [0.1.9] - 2026-03-17

### Added
- **ESP-Hosted firmware compatibility gate**: after `esp_hosted_connect_to_slave()`, shell reads C6 hosted version and refuses Wi-Fi init unless co-processor matches host `2.12.x` line
- Transcript-visible recovery guidance on version mismatch

### Fixed
- SDIO/RPC fallout from mismatched host/co-processor firmware
- Host component lock restored to `espressif/esp_hosted 2.12.1` and `espressif/esp_wifi_remote 1.4.1`

---

## [0.1.8] - 2026-03-17

### Added
- `wifi diag` command: connection state, IP status, nearby-network scan
- Transcript-facing Wi-Fi diagnostics on boot and post-`c6ota`

### Fixed
- Wi-Fi runtime retries: partial init state cleaned up before retrying
- Hosted Wi-Fi restores automatically after normal boot and successful `c6ota` in background task

---

## [0.1.7] - 2026-03-17

### Added
- FATFS long filename support: heap-backed LFN buffers, 255-character limit
- `sd ls` uses direct FatFs directory enumeration for reliable long filenames

### Fixed
- `sd ls` stack-protection panic: command execution moved to dedicated worker task
- `c6ota default` hosted teardown crash: ESP-Hosted SDIO transport kept alive for Wi-Fi-off OTA
- Truncated SD root names and `c6ota default` lookup failures

---

## [0.1.6] - 2026-03-17

### Fixed
- Repeated SD `ldo` warning spam: BSP SD-card power control acquires SD VO4 explicitly at 3300 mV on esp32p4
- Mount-failure and unmount cleanup so repeated `sd` commands don't leak SD power handle

---

## [0.1.5] - 2026-03-17

### Added
- `sd info`: card metadata and root availability
- `sd stat <path>`: resolved path, entry type, size, and mode
- `sd cat <path> [max_bytes]`: bounded text-safe file preview (max 8192 bytes)

### Changed
- All SD commands use shared guarded mount/unmount flow with validated path resolution
- `sd ls` shows entry types and file sizes
- Directory listings bounded to 128 entries

---

## [0.1.4] - 2026-03-17

### Added
- `c6ota default`: auto-load `esp32c6_hosted_slave.bin` or `network_adapter.bin` from SD root
- ESP32-C6 image validation: magic `0xE9` + chip ID `0x000D`
- Factory first-upgrade warning for C6 firmware `v2.3.0`

### Changed
- `c6ota` transfer chunks: 1536 bytes
- Progress output: `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- Confirmation prompt: `WARNING: This will reboot the C6. Type YES to continue`
- Success text: `C6 OTA completed successfully! Type reboot to activate new firmware.`
- HTTP images downloaded first, then Wi-Fi stopped for clean SDIO-only OTA transfer

---

## [0.1.3] - 2026-03-17

### Changed
- Healthy shell UI startup no longer emits warning-level log
- Boot milestone preserved in `debug` command history instead

---

## [0.1.2] - 2026-03-17

### Removed
- `c6update` utility fully removed and archived (previously used esp-serial-flasher + GPIO54)

---

## [0.1.1] - 2026-03-14

### Added
- `c6ota <source>` shell command: ESP-Hosted SDIO OTA for ESP32-C6
- Input-driven safety gate with `This will reboot the C6. Continue? (yes/no)` prompt
- ESP-IDF app header validation before transfer
- Live percentage progress in locked transcript UI
- Support for `sd:/firmware.bin` and `http[s]://host/path/to/firmware.bin` sources

---

## [0.1.0] - 2026-03-13

### Added
- Initial shell UI replacing LVGL widgets demo
- BSP-managed JD9165 display and GT911 touch initialization
- Scrollable LVGL textarea transcript, on-screen keyboard, boot banner
- Built-in commands: `help`, `sysinfo`, `clear`, `reboot`
- 10-command recall buffer with Prev/Next touch controls
- Runtime Wi-Fi initialization path following sdkconfig
- Shell Wi-Fi commands: `wifi status`, `wifi connect`, `wifi disconnect`
- Password masking for `wifi connect <ssid> <pass>`
- `wifi scan`, `sd ls`, `mem`, `gpio status`, `debug`, `version`, `about`
- 5-entry debug/error history buffer
- ESP-Hosted + esp_wifi_remote targeting ESP32-C6 over SDIO
- `coprocessor/esp32c6_slave`: repo-local ESP32-C6 hosted slave firmware project
- Station-only Wi-Fi profile, nano newlib, warn-level logging for image size
- PSRAM XIP mapping disabled to prevent flash/PSRAM overflow at link

## [0.1.19] - 2026-03-18
- Fixed the shell command-family dispatch regression that left `wifi status`, `wifi scan`, `wifi diag`, `wifi connect`, and `wifi disconnect` effectively inert even though boot-time hosted Wi-Fi still initialized and connected correctly
- Fixed the root cause in the shell parser by preserving the original unsplit command text before tokenization, so family handlers that re-parse subcommands now receive the full command line instead of only the first token
- Applied the same command-routing fix to the `sd` and `c6ota` family handlers so their subcommand parsing stays reliable without changing the proven boot, display, hosted Wi-Fi, or OTA runtime paths

## [0.1.18] - 2026-03-17
- Disabled the earlier hosted Bluedroid Bluetooth bring-up path on the ESP32-C6 baseline after `bt enable` proved able to crash the board inside the Bluedroid HCI parser during controller startup
- Kept the `bt` command surface visible, but changed it back to an explicit unsupported state on this current ESP32-C6 hosted configuration so boot, display, SD, and Wi-Fi remain stable
- Removed the direct host BT build dependency and hard-gated the shell's Bluetooth runtime path so `bt enable` and `bt scan` now fail safely instead of entering the unstable controller startup path

## [0.1.17] - 2026-03-17
- Tightened `gpio list` and `gpio status` so the shell now reports the exposed board pins with clearer JC1060 and ESP32-P4 role text instead of terse raw labels
- Enabled the hosted Bluedroid Bluetooth path in the host build, added the required BT component dependency, and completed the shell-side `bt status | enable | scan` runtime helpers against the local ESP-Hosted example flow
- Kept `rgb` and `camera` intentionally blocked, but updated those shell messages to explain the current evidence more honestly: the JC1060 reference repo does not expose authoritative RGB LED wiring, and this workspace still lacks the local camera stack needed by the JC1060 camera examples

## [0.1.16] - 2026-03-17
- Expanded the shell with hardware control commands for `brightness`, `rotate`, `battery`, `volume`, and the safer `gpio list | status | read | set` flow while preserving the existing BSP boot path, locked transcript UI, SD tools, Wi-Fi restore flow, and `c6ota` behavior
- Added runtime display rotation with GT911 touch remapping, ADC-backed battery reporting using board-configured divider values, and ES8311 speaker volume control through the existing BSP codec path
- Surfaced `bt status | enable | scan`, `rgb`, and `camera` in the parser and help output with explicit sdkconfig or board-metadata gates so unsupported hardware paths fail clearly instead of pretending support on the current workspace baseline

## [0.1.15] - 2026-03-17
- Rewrote the README introduction to describe P4MiniShell as an embedded ESP32-P4 and ESP32-C6 DOS-style shell platform instead of a minimal demo replacement
- Added `roadmap.md` to capture the missing work for COMMAND.COM parity, native app loading, a future shell SDK and API, and the separate design decision needed for literal DOS `.exe` compatibility
- Added `licence.md` to mark the project-authored code as proprietary to Stoian Alexandru while preserving the verified third-party Apache, MIT, and protobuf-c license obligations already present in the workspace

## [0.1.14] - 2026-03-17
- Expanded the shell toward a COMMAND.COM-style SD workflow with RAM-only `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`, `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`, `set`, `path`, `echo`, and `call`, while keeping all execution on the existing shell worker task
- Added SD-backed redirection for transcript-safe text commands using `>` and `>>`, with writes confined to the guarded SD mount path and no filesystem writes outside the SD card
- Added a lightweight batch engine for `.bat` files on SD, including `%1`..`%9` argument expansion, `rem` comments, `echo on/off`, PATH-based batch lookup, and direct `.bat` invocation through the normal shell dispatcher

## [0.1.13] - 2026-03-17
- Confirmed the working ESP32-P4 host and ESP32-C6 co-processor baseline end to end: shell UI, BSP-managed display and touch init, hosted Wi-Fi startup, Wi-Fi shell commands, SD tools, and `c6ota` now operate together on the checked-in project configuration
- Finalized the host-side ESP-Hosted profile around `espressif/esp_hosted 2.12.1` plus `espressif/esp_wifi_remote 1.4.1`, 1-bit SDIO at 10 MHz on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, forced ESP32-C6 reset on every host boot through GPIO54, and 1500-byte `c6ota` transfer chunks
- Updated the project docs and board metadata to describe the stable working configuration directly instead of the earlier rollback and investigation state

## [0.1.12] - 2026-03-17
- Reverted the earlier hosted startup cleanup batch after Wi-Fi still failed on the ESP32-P4 to ESP32-C6 SDIO path even after the later transport-wait experiment was removed
- Restored the last known-good hosted reset behavior by switching the ESP32-C6 back to `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP`, because this board baseline had proven Wi-Fi startup only with a forced co-processor reset during host boot
- Restored the original boot-time and post-`c6ota` Wi-Fi diagnostic pass and removed the app-side hosted log suppression so the serial monitor and shell transcript again match the earlier working baseline before any new root-cause investigation

## [0.1.11] - 2026-03-17
- Reverted the app-side `ESP_HOSTED_EVENT_TRANSPORT_UP` wait experiment after it regressed the previously working Wi-Fi startup path on this ESP32-P4 to ESP32-C6 SDIO baseline
- Restored the prior hosted startup order that had Wi-Fi working: connect to the ESP32-C6, validate the hosted firmware version, then continue into the normal sdkconfig-driven Wi-Fi runtime path

## [0.1.10] - 2026-03-17
- Removed the automatic `wifi diag` scan from boot-time Wi-Fi startup and post-`c6ota` restore, keeping those paths limited to the proven connect flow while leaving `wifi diag` available on demand for explicit diagnostics
- Restored the hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY`, which removes the clean-boot `Reset slave using GPIO[54]` warning without changing the working OTA or Wi-Fi path
- Suppressed non-actionable `H_SDIO_DRV` and `rpc_rsp` warning noise in the app so serial output stays focused on real hosted transport failures while the shell transcript remains the user-facing Wi-Fi status surface

## [0.1.9] - 2026-03-17
- Added an explicit ESP-Hosted firmware compatibility gate to the normal Wi-Fi startup path: after `esp_hosted_connect_to_slave()` the shell now reads the ESP32-C6 hosted version and refuses to continue into `esp_wifi_init()` unless the co-processor matches the host `2.12.x` release line
- Fixed the reported SDIO/RPC fallout from mismatched host and co-processor firmware by failing Wi-Fi startup early with transcript-visible recovery guidance instead of continuing into incompatible `esp_wifi_remote` traffic
- Corrected the host component lock back to the ESP32-C6 `2.12.x` line by restoring `espressif/esp_hosted 2.12.1` and `espressif/esp_wifi_remote 1.4.1`, which matches the checked-in C6 project history instead of forcing the co-processor back to `2.9.x`

## [0.1.8] - 2026-03-17
- Re-enabled the original hosted Wi-Fi runtime automatically after normal boot and after successful `c6ota`, keeping the shell UI startup path intact by running the restore flow in a background task instead of the LVGL input path
- Added transcript-facing Wi-Fi diagnostics on boot, after successful `c6ota`, and through the new `wifi diag` command, including connection state, IP status, and a nearby-network scan with SSID, RSSI, channel, and auth mode
- Hardened Wi-Fi runtime retries by cleaning up partial init state before retrying the original startup routine, so boot-time or post-OTA restore failures report cleanly and can be retried without tearing up the shell

## [0.1.7] - 2026-03-17
- Enabled FATFS long filename support for the shell build using heap-backed LFN buffers with a 255-character limit, which fixes truncated SD root names, `sd ls` long-name failures, and `c6ota default` lookup against `esp32c6_hosted_slave.bin` or `network_adapter.bin`
- Switched `sd ls` to direct FatFs directory enumeration so the shell shows full long filenames reliably and no longer trips over the old invalid-name path during long-entry reads on the mounted SD card
- Kept the proven ESP-Hosted `c6ota` flow pinned to `espressif/esp_hosted` `2.9.7`, with Wi-Fi stopped before transfer, 1536-byte OTA chunks, header validation for magic `0xE9` plus ESP32-C6 chip ID, exact YES confirmation text, 5% progress lines, and the factory `v2.3.0` first-upgrade warning pointing to the standalone CrowPanel tool URL
- Fixed the `sd ls` stack-protection panic by moving shell command execution off the LVGL input-event callback stack and onto a dedicated command worker task with its own stack budget and LVGL mutex handoff
- Fixed the `c6ota default` hosted teardown crash by keeping the existing ESP-Hosted SDIO transport alive for Wi-Fi-off OTA mode instead of calling `esp_hosted_deinit()` before reconnecting the C6 link

## [0.1.6] - 2026-03-17
- Fixed the repeated SD-related `ldo` warning spam by replacing the BSP SD-card on-chip LDO helper with a repo-local power-control path that acquires SD VO4 explicitly at 3300 mV on esp32p4
- Kept the earlier plain-SDMMC fallback for invalid or unsupported LDO-control cases, while tightening mount-failure and unmount cleanup so repeated `sd` commands do not leak the SD power handle

## [0.1.5] - 2026-03-17
- Hardened the shell SD path so all SD commands use a shared guarded mount or unmount flow, validated path resolution, bounded transcript output, and safe cleanup on missing cards, bad paths, and open failures
- Expanded the SD command family with `sd info`, `sd stat <path>`, and `sd cat <path> [max_bytes]`, while keeping `sd ls [path]` compatible and improving it with entry type and file size reporting
- Added transcript-safe limits for SD diagnostics: directory listings stop after 128 entries and `sd cat` previews at most 8192 bytes with non-printable bytes sanitized instead of dumping raw binary into the shell

## [0.1.4] - 2026-03-17
- Repaired `c6ota` to match the proven CrowPanel SDIO OTA method: HTTP images are downloaded first, then the shell stops Wi-Fi completely, reinitializes ESP-Hosted, and performs the OTA transfer over a clean SDIO-only link
- Changed `c6ota` transfer chunks to 1536 bytes, added `c6ota default`, enforced ESP32-C6 image validation with image magic plus chip ID, and updated progress output to `C6 OTA: XX% (YYYY KB / ZZZZ KB)` every 5%
- Replaced the old yes or no prompt with the exact confirmation `WARNING: This will reboot the C6. Type YES to continue`, restored Wi-Fi automatically after OTA failures, and updated the success text to `C6 OTA completed successfully! Type reboot to activate new firmware.`
- Added the factory first-upgrade warning for ESP32-C6 firmware `v2.3.0` and pinned the host manifest to `espressif/esp_hosted` `2.9.7`

## [0.1.3] - 2026-03-17
- Removed the temporary boot-time `W (p4minishell)` shell UI initialization log so healthy boots no longer emit a warning just to advertise that the display transcript is live
- Preserved the same startup milestone through the existing `debug` command history instead of the serial warning path, keeping shell boot, LCD render, and on-screen transcript behavior unchanged

## [0.1.2] - 2026-03-17
- c6update utility fully removed and archived (previously used esp-serial-flasher + GPIO54). No code or build traces remain.

## [0.1.1] - 2026-03-14
- Added a new `c6ota <source>` shell command for full ESP-Hosted SDIO OTA against the ESP32-C6 using either `sd:/firmware.bin` or `http://host/path/to/firmware.bin`
- Added an input-driven safety gate for `c6ota` with the exact prompt `This will reboot the C6. Continue? (yes/no)` before any OTA transfer begins
- Extended the hosted OTA path to mount FATFS for SD sources, validate the incoming ESP-IDF app header, check the current co-processor version with `esp_hosted_get_coprocessor_fwversion()`, and require C6 firmware `v2.9.7+` for reliable SDIO OTA
- Streamed OTA payloads over the existing ESP-Hosted SDIO transport in roughly 1400-byte chunks using `esp_hosted_slave_ota_begin/write/end/activate`, with live percentage progress in the locked transcript UI and friendly fallback guidance on failures
- Kept serial `c6update <path>` for merged-image flashing and redirected legacy OTA-style `c6update ota ...` usage to the new `c6ota` command instead of maintaining two shell entry points for the same hosted update flow

## [0.1.0] - 2026-03-13
- Added a real `c6update ota <https-url>` path that uses `esp_http_client` plus `esp_hosted_slave_ota_begin/write/end` to stream an ESP32-C6 image over HTTPS, report progress every 5%, and request OTA activation when the running co-processor firmware supports it
- Kept the stock-board-safe serial guard for `c6update <path>` so unverified P4-to-C6 flash UART wiring still redirects the user to the external `PROG_C6` header instead of pretending host-side serial flashing is available
- Restored `c6update` to a stock-board-safe behavior on the checked-in JC1060P470C or ESP32-P4-Function-EV-Board baseline: the shell now reports that the external `PROG_C6` header with ESP-Prog, or ESP-Hosted OTA, is required when no verified P4-to-C6 flash UART is configured
- Clarified the stock-board GPIO54 note: GPIO54 remains the hosted reset line reference, but the repository does not claim a verified on-board P4-driven C6 serial flashing path without custom wiring
- Updated `c6update sd:/c6_new.bin` guidance to explain that the command is still used for recovery guidance on stock hardware and that a host `reboot` is only relevant after an external or OTA C6 update succeeds
- Revised `c6update` to follow a GPIO54-driven ESP32-C6 ROM download flow: open the SD image first, pulse GPIO54 low/high, optionally hold BOOT from sdkconfig, connect with `esp_serial_flasher`, flash from offset `0x0` in 4 KB chunks, then reset the target
- Updated `c6update` transcript behavior to report live flashing progress every 5% as `Flashing... XX% (YYYYY bytes)` and to emit explicit UART/SD-card oriented failure guidance on any flash step error
- Documented `c6update sd:/c6_new.bin` as the primary shell example and clarified that a successful ESP32-C6 update still requires a host `reboot` command before the new co-processor firmware is used
- Expanded the shell command set with `wifi scan`, `sd ls`, `mem`, `gpio status`, `debug`, `version`, and `about`, while keeping `help`, `sysinfo`, `clear`, and `reboot`
- Added a 5-entry debug/error history buffer and friendly transcript-facing error messages for command and runtime failures
- Confirmed Enter/OK command submission through the input line `LV_EVENT_READY` handler and documented the DOS-style locked transcript UI model
- Added an SD card driven `c6update <path>` shell command that flashes a merged ESP32-C6 image at offset `0x0` with `esp-serial-flasher`
- Changed hosted Wi-Fi startup to be on-demand from `wifi connect` and disabled host auto-restart when the C6 does not answer init, so the shell still boots for recovery/update flows
- Moved `wifi connect` hosted probing into a background task and shortened the hosted SDIO transport-up retry window so missing-C6 failures return faster with less disruption
- Clarified that the checked-in ESP32-P4-Function-EV-Board baseline does not expose a verified on-board P4-controlled C6 flash UART, so `c6update` now reports the external `PROG_C6` / OTA requirement instead of asking for impossible GPIO defaults
- Added sdkconfig-backed ESP32-C6 flasher wiring controls for UART port, UART TX/RX, EN, reset, and BOOT GPIOs so the host does not hardcode board-specific programming pins
- Reported C6 flashing progress and wiring/runtime failures directly into the locked transcript UI while keeping the normal BSP/LVGL shell model unchanged
- Added `espressif/esp-serial-flasher` to the app manifest and removed the duplicate placeholder `app_main()` source from the registered build inputs
- Replaced the LVGL widgets demo in main/main.c with a shell UI built on the existing BSP startup path
- Preserved the original BSP-managed JD9165 display and GT911 touch initialization by keeping bsp_display_start_with_config() and BOARD_CFG_* settings unchanged
- Added a scrollable LVGL textarea, attached lv_keyboard, boot banner, and built-in commands: help, sysinfo, clear, reboot
- Added sysinfo reporting for board_config-backed values, ESP-IDF version, heap usage, PSRAM totals, and Wi-Fi unsupported status on esp32p4
- Refreshed project metadata to match the current shell implementation and verified esp32p4 baseline
- Split the shell UI into a read-only transcript area plus a dedicated prompt-bearing input line bound to the on-screen keyboard
- Added LV_EVENT_READY command submission on the input line and a 10-command recall buffer with Prev/Next touch controls
- Added a runtime Wi-Fi initialization path that follows sdkconfig only, logging every init step or failure into the shell transcript during boot
- Kept the current esp32p4 baseline safe by reporting when sdkconfig does not enable native Wi-Fi or ESP32-C6 host Wi-Fi instead of changing Kconfig
- Added project Wi-Fi defaults in sdkconfig and enabled the host Wi-Fi stack path used by the current esp32p4 workspace
- Added shell Wi-Fi commands for status, connect, and disconnect, with transcript-safe password masking for `wifi connect <ssid> <pass>`
- Disabled LVGL example compilation in sdkconfig so the Wi-Fi-enabled shell still fits the esp32p4 link image budget
- Trimmed sdkconfig to a station-only Wi-Fi profile and disabled Wi-Fi IRAM-heavy optimizations so the host Wi-Fi shell can link on esp32p4
- Reduced compile-time log verbosity, enabled newlib nano format, and disabled AMPDU so the Wi-Fi-enabled image sheds more flash/rodata pressure on esp32p4
- Disabled PSRAM XIP instruction and rodata mapping in sdkconfig because the Wi-Fi-enabled image was overflowing the shared flash/PSRAM mapping window at final link
- Fixed Wi-Fi runtime startup by initializing NVS before esp_wifi_init(), including the standard erase-and-retry recovery path for incompatible stored NVS data
- Replaced the esp32p4 extconn/ESP8689 host Wi-Fi path with ESP-Hosted plus esp_wifi_remote targeting an ESP32-C6 co-processor over SDIO
- Updated the checked-in host transport configuration to CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 with reset GPIO54, and mirrored those defaults into sdkconfig.defaults
- Updated shell Wi-Fi diagnostics so hosted-link failures now report the ESP32-C6 SDIO backend and pin map instead of the older extconn hardware note
- Changed ESP-Hosted reset policy to `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY` so the host no longer resets the C6 on every clean boot
- Added `coprocessor/esp32c6_slave`, a repo-local ESP32-C6 ESP-Hosted slave firmware project that tracks the same `2.12.1` source line as the host dependency lock
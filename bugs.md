# P4MiniShell — Hardware Test Bug Report

Date: 2026-08-08
Hardware: ESP32-P4 (rev 1.0) on COM11, ESP32-C6 co-processor, JD9165 display, SD card present
Firmware: v0.24.1 (with bring-up fixes applied during this session)

Scope: Full command-surface test over the UART console (USB-Serial-JTAG, 115200 baud).
Every documented command and feature was exercised on the real board. This report
catalogs every bug and issue observed, ordered by severity. Bugs that were fixed
during the session are marked **FIXED** with the change described.

---

## CRITICAL

### C1. Intermittent LVGL crash — style-transition use-after-free / heap corruption

- **Symptoms:** `Guru Meditation Error: Core 0 panic'ed (Load access fault)` or
  `Store access fault`, rebooting the device. Reproduced intermittently after
  several different commands:
  - `if exist /sdcard echo ok`
  - `sd info`
  - `bluetooth advertise on`
- **Decoded fault (all three):**
  - `lv_obj_get_style_prop` — `lv_obj_style.c:330`
  - called from `trans_anim_start_cb` — `lv_obj_style.c:907`
  - via `anim_timer` — `lv_anim.c:529` on the LVGL task (`lvgl_port_task`)
  - The `if exist` variant also showed `lv_tlsf_free` → `remove_free_block`
    (`lv_tlsf.c:591`), i.e. the LVGL heap pool itself is corrupted.
- **Assessment:** The faulting addresses (MTVAL 0x0be0 / 0x0bc4 / 0x0dd4) are low
  dangling pointers, consistent with a **use-after-free of an LVGL object/style**
  or **heap corruption in the LVGL pool** detected on the next allocation. It is
  intermittent (same command succeeds on a later boot), timing-dependent, and not
  reproducible on a single command — it surfaces on the next LVGL alloc/render.
- **Likely areas to investigate:**
  1. A style transition (`trans_anim_start_cb`) started on a widget that is then
     deleted/rebuilt (e.g. rotation rebuild, keyboard show/hide) without killing
     the animation.
  2. Any remaining LVGL API call from a non-LVGL task that is not covered by
     `lvgl_port_lock` (the shell transcript paths were locked in v0.24.1; the
     header fallback render path and any direct widget calls should be audited).
  3. An out-of-bounds write somewhere in the storage/SD path that lands in the
     LVGL pool (both LVGL crashes occurred after SD-touching commands).

---

## HIGH — ANSI colour markers shown literally

A systemic issue: many commands compose output with `@`-specifier palette macros
(`@K`, `@M`, `@w`, `@R`, `@g`, `@c`, `@W`, ...) and send the string through a path
that never converts `@`-specifiers to real SGR escapes. `ansi_vformat()` only
converts `@`-specifiers that appear literally in the *format* string; `@`-specifiers
inside substituted `%s` arguments are deliberately left untouched (injection
guard). Every call site that relies on converting `@`-specifiers from a `%s`
argument — or that calls `shell_transcript_append_text()`/`appendf()` with a
`@`-containing string — renders the raw markers on both the transcript and the
UART console. The same applies to any use of `shell_transcript_append_ansi()`
with a string that still contains `@`-specifiers.

Affected output observed on hardware:

| Command | Literal text shown |
|---|---|
| `dir` (detailed) | `@K1980-01-01 00:08@R @M 12 B@R @wtest.txt@R@K@R` |
| `sd info` | `@csd.mount_point:@R @W/sdcard@R` |
| `sd stat <path>` | `@csd.path:@R @W/sdcard/test.txt@R` |
| `wifi status` / `wifi diag` | `wifi.default_profile: @ymissing@R`, `connected=@Kno@R` |
| `keyboard status` | `keyboard: @gvisible@R, mode=0, height=280` |
| `ver` | `time: ... @y(unsynced)@R` |
| `sysinfo` | `c6.hosted_transport: ... busy=@gno@R`, `heap: ... (@g95%%@R)` |

**Where:** the `SH_*` macros (`components/ansi/ansi_palette.h`) are passed either
as `%s` arguments to `shell_transcript_appendf_ansi()` (e.g. `networking.c`
`networking_appendf`, `usb.c`, `shell_command_version()`, `keyboard` status) or
embedded in strings handed to `shell_transcript_append_text()`/`append_ansi()`
(the `dir` and `sd` listing code in `components/storage/`).

**Note:** the `shell_print_*` semantic helpers themselves were **fixed** this
session (see F3) — usage/error/ok/warning/muted/heading now render colour
correctly. The remaining literal markers are in the call sites listed above.

---

## MEDIUM

### M1. `cd` prints `\` while the prompt shows `/sdcard`

- `cd` (no args) prints `\` (the FAT root path) but the prompt renders `/sdcard`
  (the VFS path). The two surfaces disagree about the current directory spelling.
- Not a functional break (commands still resolve), but confusing and inconsistent.
- Where: `shell_command_cd` / cwd rendering in `components/storage/`.

### M2. `attrib` with no path shows a bare error

- `attrib` with no arguments prints `attrib: cannot access .` — the user gets no
  usage hint. DOS lists the current directory. A usage line or a cwd listing
  would be friendlier.
- Where: `shell_command_attrib` in `components/storage/storage_commands.c`.

### M3. `pause` / `choice` / `more` in a headless session time out after 30 s

- With no interactive key source attached during the wait, `pause` reports
  `pause: timed out waiting for a key` after 30 seconds and `choice` prints
  `[Y,N]? Y (timed out)`. This is documented fallback behaviour, but the 30 s
  stall is long for scripted/headless use and the timeout message wording varies
  by command.
- Where: `shell_wait_for_key` timeout path in `components/shell/shell.c`.

### M4. Battery telemetry reads 0 % / 1.976 V (uncalibrated ADC)

- `battery` reports `0%, 1.976 V` (`raw=1030 gpio_mv=988 scaled_mv=1976`). The
  uncalibrated linear fallback (`gpio_mv = raw * 3300 / 4095`) plus the 2:1
  divider yields a reading below the 3300 mV empty threshold, so percent = 0.
- Likely the battery sense rail is not driven (bench supply), the calibration
  eFuse is absent, or the divider constants need review. Confirms the roadmap
  item 10 (ADC calibration fallback) concern in practice.
- Where: `command_battery_read` in `components/command/command.c`.

---

## FIXED DURING THIS SESSION

### F1. Family commands (wifi / bluetooth / usb / sd) printed help instead of executing

- `wifi status`, `wifi scan`, `wifi diag`, `bluetooth status`, `bluetooth scan`
  all printed the command help/usage instead of running the subcommand.
- **Root cause:** `shell_split_args()` writes token terminators (`\0`) into the
  command buffer in place. By the time the dispatcher reached the module-routed
  family handlers, `command` was truncated to just `"wifi"` / `"bluetooth"`, so
  the family parser saw `argc <= 1` and fell through to help.
- **Fix:** `components/command/command.c` — `shell_execute_command_core()` now
  preserves a heap copy of the trimmed command for family-prefix lines and passes
  it to `networking_handle_wifi_command()`, `bluetooth_handle_command()`,
  `usb_handle_command()`, and `shell_command_sd()`. The copy is freed in every
  branch. Verified: `wifi status`, `wifi scan`, `bluetooth status`, `usb status`,
  `sd info`, `sd ls`, `sd stat` all execute correctly after the fix.

### F2. `sd stat` / `sd ls` / `sd cat` showed a usage error

- `sd stat test.txt` printed the `sd` usage instead of the stat result.
- **Root cause:** same in-place mutation inside `shell_command_sd()` —
  `shell_split_args(command, ...)` truncated `command` to `"sd"` before it was
  handed to the sub-handlers.
- **Fix:** `components/storage/storage_commands.c` — `shell_command_sd()` now
  strdups the command before splitting and passes the copy to the sub-handlers.
  Verified: `sd stat test.txt` returns the file stat.

### F3. `shell_print_*` helpers emitted literal `@`-specifiers

- Every usage/error/ok/warning/muted/heading line showed raw markers, e.g.
  `@yUsage: brightness <0-100>@R`, `@gCreated directory ...@R`,
  `@GFolder PATH listing for volume A:@R`.
- **Root cause:** `shell_print_coloured()` composed `@y...@R` and called
  `shell_transcript_append_ansi()`, which only strips real `\x1b` sequences and
  does not convert `@`-specifiers.
- **Fix:** `components/shell/shell.c` — `shell_print_coloured()` now builds a
  format string with the colour/reset as literal `@`-specifiers and routes the
  composed text through `shell_transcript_appendf_ansi()` (i.e. `ansi_vformat`),
  so colour and reset are converted and the rendered body is a `%s` argument
  (literal `%` in body stays data). Verified: `brightness 150`, `md`, `rd`,
  `tree`, `chkdsk`, `echo` all render colour correctly.

---

## LOW / OBSERVATIONS

- `date 2026-08-08` (YYYY-MM-DD) rejects with "value out of range" — the expected
  format is `MM-DD-YYYY`. `date 08-08-2026` works. The error text does not state
  the expected format.
- `echo %UNDEFINEDVAR%` prints `%UNDEFINEDVAR%` literally (unknown names are left
  untouched by design, per `batch.h`). Note this contradicts the assertion in
  `test/main/test_shell_variables.c::test_variable_expansion_env_var`, which
  expects `echo %UNDEFINED%` → `echo ` (empty). That unit test would fail if the
  test runner actually executed it.
- `echo on` / `echo off` print `on` / `off` rather than DOS's `ECHO is on|off`
  wording (only `echo` with no args prints `ECHO is on`). Cosmetic.
- Wi-Fi scan results during the session showed empty SSIDs in the second scan
  (`wifi.scan[N]: ssid= rssi=0`) immediately after boot — likely a C6 scan
  race; the boot-time diagnostic scan showed real APs. Worth re-testing after a
  stable Wi-Fi runtime.
- The transcript echo of a piped stage (`type f | find ...`) shows both the
  stage output and the downstream `find` header/result — expected given the
  transcript-delta redirection design, but noisier than DOS.

---

## Summary

| Severity | Count |
|----------|-------|
| Critical (intermittent LVGL crash) | 1 (C1) |
| High (literal ANSI markers) | 1 systemic (H1) |
| Medium | 4 (M1–M4) |
| Fixed during session | 3 (F1–F3) |
| Low / observations | 6 |

### Recommended next steps

1. **C1 is the top priority** — reproduce with the LVGL pool walk enabled
   (`lv_mem_monitor` / `lv_mem_test`) and audit every remaining LVGL API call
   made from non-LVGL tasks (header fallback render, any direct widget access)
   for missing `lvgl_port_lock`. Also audit the SD/storage path for any
   out-of-bounds write that could land in the LVGL pool.
2. Route the remaining `@`-specifier call sites (dir/sd listings, wifi diag
   `%s` args, keyboard status, ver/sysinfo colour fragments) through a converting
   path or inline the palette macros in the format string.
3. Align `cd` output with the prompt (single cwd spelling) and add an `attrib`
   usage line.
4. Correct the `test_shell_variables.c` assertion for undefined-variable
   expansion.

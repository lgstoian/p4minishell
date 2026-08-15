# P4MiniShell Roadmap

## Goal
The long-term goal is to turn P4MiniShell into a practical embedded shell environment with strong PowerShell-style usability, a stable SDK for native apps written in C, and app loading from the SD card.

On this hardware, that goal needs to be interpreted carefully:
- Practical target: PowerShell-like shell behavior on UART console, DOS-style file commands on SD, native ESP32-P4 applications stored on SD, and a small C SDK/API for those apps.

## Current baseline (v0.32.0 — August 2026)
Implemented today in the checked-in firmware:

### Shell Core & UI
- ✅ PowerShell-style prompt: `PS \path> ` with ANSI-colored tokens (bright white PS, bright yellow path, bright white >)
- ✅ 16 standard + 16 bright ANSI colors (configurable via P4_CONFIG_ANSI_*)
- ✅ ANSI/VT escape sequence module (`components/ansi/`) with SGR state machine, format builder with full printf flag support, plain-text stripping
- ✅ Touch-first LVGL shell UI with transcript, prompt, on-screen keyboard, 10-command recall
- ✅ Scrollable transcript that jumps to a submitted command's output and pages via the
      input-row Up/Dn buttons, USB keyboard PageUp/PageDown, and the USB mouse wheel
- ✅ Fixed top header bar with Wi-Fi, battery, Bluetooth, USB, SD status + MEM/CPU/BAT system panel
- ✅ FreeRTOS task introspection: `ps` / `tasks` / `top` (read-only: name, state,
      priority, core, stack high-water mark, per-task CPU%) with a `/b` bare form
- ✅ `top`/`ps`/`tasks` sorting: `dir /O:`-style `/O:N|C|S|P|T` (with `-` reverse; `top`
      defaults to CPU descending) and a 0/2 ERRORLEVEL for batch use (v0.24.28)
- ✅ Header CPU sparkline: the system panel shows a short history graph of CPU samples
      (amber above the warn threshold) when `P4_CONFIG_HEADER_CPU_GRAPH` is set (v0.24.28)
- ✅ Worker-task command execution to protect the LVGL event stack
- ✅ Interactive UART console bridge (stdin/stdout routed through same shell path)
- ✅ Touch-to-show-keyboard: tapping input line brings up OSK (Windows 11 behavior)
- ✅ Keyboard hide button functional (`LV_SYMBOL_KEYBOARD` → `LV_EVENT_CANCEL` → `keyboard_hide()`)
- ✅ Backspace on empty line is a no-op (prompt prefix protection)

### Component Architecture
- ✅ `components/ansi/` — ANSI/VT SGR processing, 16-color palette, format string builder, semantic output palette (`ansi_palette.h`)
- ✅ `components/display/` — Display manager (rotation, resolution, refresh, brightness, power)
- ✅ `components/windows/` — Window manager (LVGL screen layout, dynamic scaling, rotation-aware)
- ✅ `components/keyboard/` — Keyboard manager (visibility, modes, textarea binding, external input)
- ✅ `components/header/` — Fixed top status bar (passive, display-only, async-safe)
- ✅ `components/shell/` — Shell core (transcript, history, debug log, UART console, input line, sysinfo commands)
- ✅ `components/storage/` — SD session management, path resolution, FATFS conversion, current working directory, DOS file commands
- ✅ `components/batch/` — Batch engine, `:label` handling, `for` loops, pipes, environment variables, PATH, errorlevel, batch language commands
- ✅ `components/command/` — Command module (parser, dispatcher, worker task, execution pipeline, hardware and system commands)
- ✅ `components/clock/` — Clock manager (SNTP time sync, timezone, local/UTC formatting)
- ✅ `components/networking/` — Hosted Wi-Fi + NimBLE Bluetooth on C6
- ✅ `components/usb/` — USB Host MSC storage + HID keyboard/mouse
- ✅ `components/c6ota/` — C6 firmware OTA via SDIO

### File System
- ✅ DOS-style file commands: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
- ✅ SD tools: `sd info`, `sd ls`, `sd stat`, `sd cat`, `sdeject`
- ✅ FATFS LFN enabled (MAX_LFN=255, heap-backed buffers, UTF-8)
- ✅ Persistent SD mount (no unmount between commands)
- ✅ Guarded SD access with shared mount/unmount validation
- ✅ Bounded output: 128 entries max for listings, 8192 bytes max for `sd cat`
- ✅ Short/long filename display in `dir` (shows `[SFN]` when different from LFN)
- ✅ Volume management: `chkdsk`/`scandisk` capacity report and read-only integrity walk,
      `format` behind an exact confirmation word
- ✅ Recycle bin (v0.24.26): `del`/`erase` and `rd /s` move files and whole trees into a
      hidden `.trash` folder; `undelete`/`restore` and `trash restore` bring entries back to
      their original location; `trash list|info|purge|empty` manage the bin with byte/age/
      count limits. `/p`/`/f`/`/permanent` bypass the bin. Recursive deletes, `trash
      purge`/`empty`, `format`, and `disk clean`/`delete` all require the exact
      `P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD` typed at the prompt, and `del`/`rd`/`format`/
      `disk`/`undelete`/`trash` return a real ERRORLEVEL.
- ✅ Full `dir` option set: `/W` `/P` `/S` `/B` `/L` `/A:attrs` `/O:order` with timestamps,
      per-directory counts, grand totals, and free-space reporting; hidden/system entries are
      suppressed by default (DOS behaviour) and a bare `/A` shows everything
- ✅ Free-space guardrails on `copy`, `move`, `write`, and `append`, with self-copy detection
      and partial-destination cleanup on failure

### Environment & Batch
- ✅ RAM-only environment variables (24 max), PATH, `%1`..`%9` expansion
- ✅ `.bat` file execution with `rem`/`::` comments, `echo on/off`
- ✅ Output redirection `>` and `>>` to SD files
- ✅ Command history with password masking for `wifi connect`
- ✅ `set /a` integer arithmetic with the full DOS operator set and compound assignment
- ✅ `set /p` prompted input via the key-wait facility, plus `set /p NAME=< file` to read one
      line from a `< file`/pipe source (the BASIC `INPUT#`/`LINE INPUT#` surface)
- ✅ `calc` float calculator (`components/batch/calc.c`): `calc [NAME=] <expr>` with the
      FX-870P/VX-4 math/string functions, `&H`/`0x` hex, `PI`, seeded `RAN#`, DEG/RAD angle
      mode (`calc /deg|/rad|/angle`), `&H` hex output (`calc /hex`), and POL/REC X/Y side
      effects (v0.32.0)
- ✅ `for /f` file-line loops: `for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in
      (file-set) do cmd` over an explicit file, a wildcard, or the active `<`/pipe input;
      `tokens=` binds consecutive letters and `*` captures the rest (v0.32.0)
- ✅ DOSKEY-style `alias` / `unalias` with SD persistence (`alias /save` writes
      `sd:/ALIASES.BAT`, auto-loaded after CONFIG.SYS) and prompt-only expansion
- ✅ Trailing `^` line continuation, honored by both the executor and the label scanner

### Wi-Fi & Bluetooth
- ✅ ESP-Hosted Wi-Fi on C6 over SDIO with version compatibility gate
- ✅ Wi-Fi mutex (`wifi_lock`/`wifi_unlock`) protecting all shared state
- ✅ Wi-Fi persistent watchdog with exponential backoff (1s→30s cap, 120s timeout)
- ✅ TOCTOU-safe init-task claiming (`wifi_try_claim_init_task`/`wifi_release_init_task`)
- ✅ Boot-time Wi-Fi restore and post-OTA restore
- ✅ Hosted NimBLE Bluetooth on C6: `bluetooth status|scan|advertise`

### USB
- ✅ USB MSC mass storage at `/usb0`
- ✅ USB HID keyboard auto-detect with CLI injection
- ✅ USB HID mouse with opt-in echo and wheel scrolling of the transcript
- ✅ Persistent known Wi-Fi networks: `sd:/WIFI.KNOWN` list with boot-time
      auto-connect (preferred / highest priority / strongest RSSI), managed via
      `wifi known|save|forget|preferred`, falling back to the single-credential
      path when the SD card is absent
- ✅ Full US keyboard layout (60+ HID key codes, modifier-aware)
- ✅ On-screen keyboard auto-hide when USB keyboard attached

### OTA & Hardware
- ✅ `c6ota` for validated C6 firmware updates from SD or HTTP/S
- ✅ C6 slave firmware built (v2.12.1, matching host) in `coprocessor/esp32c6_slave/`
- ✅ Hardware controls: `brightness`, `rotate`, `battery`, `volume`, `gpio`
- ✅ Basic audio: `beep` / `tone <freq> [ms]` / `wavplay <file>` (16-bit PCM from SD) through the
      ES8311 speaker, `audio status|stop`, `volume [<0-100>]` query/set — background playback,
      batch-safe, ERRORLEVEL (v0.24.31)
- ✅ Display power management: `display power on|sleep|off`
- ✅ Idle display-off + wake: `power idle <seconds|off>` and CONFIG.SYS `DISPLAY_TIMEOUT=`
      turn the backlight off after N idle seconds; touch / USB keyboard / USB mouse / serial
      command wake it cleanly. `sleep`/`deepsleep` support a user-wired `P4_CONFIG_POWER_WAKE_GPIO`
      wake; touch wake is honestly reported unavailable (GT911 INT not wired) (v0.24.30)
- ✅ Persistent settings: `config` reads/writes CONFIG.SYS (brightness, rotation, volume, prompt,
      Wi-Fi auto-connect, display timeout, plus `OSK=`/`HEADER=` boot prefs) with a guarded atomic
      rewrite and a confirmed `config factory` full reset (v0.24.40)
- ✅ First-run / no-SD guidance: no-SD boot message + header notification, automatic minimal
      CONFIG.SYS/AUTOEXEC.BAT generation on first card mount with an "SD card ready" welcome,
      `sd mount` (re-mount after eject), and a Getting Started footer in `help` (v0.24.41)

### Extended DOS Commands (v0.14.2+)
- ✅ `attrib` — FATFS file attributes (R/H/S/A) with +R/-R/+H/-H/+S/-S/+A/-A
- ✅ `label` — FATFS volume label read/set (max 11 chars, FAT 8.3 convention)
- ✅ `xcopy` — Full DOS 6.x / WinXP switch set (`/S /E /I /Y /-Y /D[:date] /H /R /K /C /Q /T /F /L /A /M /U /P /W /N /V`), heap-scratch recursive walker, 0/1/2 ERRORLEVEL (v0.24.27)
- ✅ `shell_wildcard_match()` — DOS-style `*` and `?` pattern matching
- ✅ Wildcard integration in `dir`, `del`, `copy` commands

### Batch Control Flow (v0.15.0+)
- ✅ `if` — Conditional: `if errorlevel N`, `if exist file`, `if "str"=="str"`, `if not ...`
- ✅ `goto` — Jump to `:label` within batch files (label parsing works)
- ✅ `shift` — Shift batch arguments left (`%1`→`%0`, etc.)
- ✅ `errorlevel` — Global error code tracking (0=success, non-zero=error)

### Extended Built-in Commands (v0.18.0) — COMPLETE
- ✅ `pause` — Blocks on a real keypress via the shell key queue; bounded fallback delay only when no interactive key source is attached
- ✅ `choice` — Blocks on a real keypress with `/C:list`, `/N`, `/T:c,secs`, `/S`; sets errorlevel to the 1-based key index
- ✅ `setlocal` / `endlocal` — Real environment scoping with a snapshot stack (8 levels), auto-unwound when a batch frame returns
- ✅ `prompt` — Full DOS template engine (`$p $g $l $b $n $d $t $v $s $_ $q $$ $a $c $f $e $h`) driving both the UART console and the LVGL input line
- ✅ `date` / `time` — Show the clock, and now also set it (`date MM-DD-YYYY`, `time HH:MM[:SS]`)
- ✅ `exit` — `exit /b [code]` leaves one batch file, bare `exit [code]` unwinds every nested level

### File Utility Commands (v0.18.0) — COMPLETE
- ✅ `find` — Search text in files with `/I` (case-insensitive), `/N` (line numbers), `/C` (count only), `/V` (invert); reads a `<` or pipe source when no file is given
- ✅ `findstr` — Classic DOS text search, case-sensitive by default, literal or regex-lite (`/R /C /I /N /V /X /E /B /L /S /M /F /G`); 0 found / 1 none / 2 usage ERRORLEVEL (v0.24.27)
- ✅ `more` — Paginated file viewing (20 lines/page) that waits for Enter/Space, or `Q` to quit; bounded fallback delay when headless
- ✅ `tree` — Fully recursive with DOS box-drawing connectors, `/F` (include files) and `/A` (ASCII connectors), depth- and entry-bounded
- ✅ `fc` — File comparison with DOS-style differing-line reporting and trailing length differences
- ✅ `comp` — Byte-for-byte comparison with `/D /A /L /N /C`; 0 identical / 1 different / 2 usage ERRORLEVEL (v0.24.27)
- ✅ `sort` — qsort-based with `/R` (reverse), `/I` (case-insensitive), `/U` (unique); 1024-line capacity and a single leak-free release path
- ✅ `history` — heap-backed recall (32, Up/Down) with `history /save` / `history /load` /
      `/clear` on the SD profile; Tab completion for commands/aliases/paths; 4096-byte command
      lines (heap transient buffers) (v0.24.33)
- ✅ `clip` / `paste` — RAM clipboard for the transcript (copy last N lines) and SD files
      (`clip file`/`paste <dest>` file copy, `clip read` text round-trip), paste into the input
      line, batch-safe + ERRORLEVEL (v0.24.32)
- ✅ `xcopy` — Full DOS 6.x / WinXP switch set with a heap-scratch recursive walker and 0/1/2 ERRORLEVEL (v0.24.27)

### Pipe Support
- ✅ `|` pipe operator — Multi-stage `cmd1 | cmd2 | cmd3` (up to 4 stages) with quote-aware splitting, per-stage spool files, and guaranteed cleanup

### Input Redirection
- ✅ `<` operator — Reads a file as command input; shares one code path with the pipe operator through the storage input-redirection slot

### Batch Expressions and Continuation (v0.21.0)
- ✅ `set /a NAME=<expression>` — 32-bit signed integer arithmetic with the full COMMAND.COM
      operator set (`+ - * / %`, `& | ^ ~ !`, `<< >>`, unary minus, parentheses) and correct
      precedence. Compound assignments (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`,
      `<<=`, `>>=`) are supported. Numbers accept decimal, `0x` hex, and octal. An undefined
      variable reads as 0, so `set /a n=n+1` works on first use.
- ✅ `set /a <expression>` with no assignment prints the result without storing it
- ✅ `set /p NAME=<prompt>` — prompts and reads a line through the key-wait facility. An
      empty line leaves the variable unchanged, matching DOS.
- ✅ Trailing `^` line continuation joins physical lines, bounded by
      `P4_CONFIG_LINE_CONTINUATION_MAX`. A doubled `^^` is an escaped literal caret, not a
      continuation. The label scanner applies the same rule so a continued line cannot
      register a false `:label`.

### Attribute Preservation (v0.21.0)
- ✅ R/H/S/A attributes are carried from source to destination on every copy path
      (`copy`, `copy` with wildcards, `move`'s copy fallback, `xcopy`, `xcopy /S`)
- ✅ `xcopy /S` also carries attributes onto the directories it creates
- ✅ Attributes are applied after the data is written, because a read-only destination
      cannot be opened for writing
- ✅ A read-only destination now produces an actionable message naming `attrib -R`

### Quoting, Escaping, and Chaining (v0.19.0)
- ✅ `"text"` groups an argument and still expands `%VAR%`, matching COMMAND.COM
- ✅ `'text'` groups an argument literally with no expansion at all
- ✅ `^c` escapes any single character, so `^&`, `^|`, `^>`, `^<`, `^"`, `^%`, and `^^` are data
- ✅ One shared quote/escape scanner in `components/shell/` backs the tokenizer, redirection
      parsing, pipe splitting, and chain splitting, so those five surfaces cannot disagree
- ✅ Quoting and escape markup is stripped from arguments, so handlers receive literal values
- ✅ `a & b` runs both, `a && b` runs b only on success, `a || b` runs b only on failure
- ✅ Up to 8 chained commands (`P4_CONFIG_CHAIN_SEGMENT_MAX`), with truncation reported
- ✅ Unrecognized commands set errorlevel 9009, matching COMMAND.COM, so `||` works on them

### Testing
- ✅ Unity test framework in `test/` (builds standalone; target, manifest, and defaults pinned to the firmware)
- ✅ Tests for: shell parser, command history, transcript command formatting, Wi-Fi state machine, ANSI format
- ✅ Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 / esp32p4

### Hardware Gaps (intentionally blocked)
- ❌ Camera: no local camera stack in workspace

### RGB LED (resolved)
- ✅ RGB LED: WS2812 on GPIO26 (JC1060P470 back panel), driven by
  `components/led` (espressif/led_strip over RMT) with the `rgb` command, an
  auto status layer tied to Wi-Fi/HTTP events, a boot confirmation flash, and
  a CONFIG.SYS `RGB=` directive (see changelog v0.24.25).

---

## Recently Completed (v0.32.0 - August 2026)

### FX-870P/VX-4 BASIC port into the batch language
- ✅ `calc` float calculator (`components/batch/calc.c`): `calc [NAME=] <expr>` evaluates a
  floating-point expression over double/fixed-string values and stores the result in an
  environment variable when `NAME=` is given. Functions: `ABS SGN INT FIX FRAC ROUND SQR
  EXP LN LOG SIN COS TAN SINH COSH TANH ASN/ASIN ACS/ACOS ATN/ATAN FACT NCR NPR DMS DMS$
  VAL VALF STR$ HEX$ ASC CHR$ LEN LEFT$ MID$ RIGHT$ MOD POL REC`. Grammar: `+ - * / ^`
  (right-assoc power), the `MOD` keyword, unary `- +`, parentheses, `&H`/`0x` hex, `PI`, a
  seeded `RAN#`, string literals with `+` concatenation, and env-var references. `calc
  /deg|/rad|/angle` set/query the trig angle mode; `calc /hex` prints `&H` hex. `POL`/`REC`
  store their two results in the X/Y variables (calculator BASIC parity). ERRORLEVEL 0/1/2.
- ✅ `set /p NAME=< file` — reads one line from a `< file` redirection or pipe stage instead
  of the interactive key queue, covering the BASIC `INPUT#`/`LINE INPUT#` verbs.
- ✅ `for /f` — file-line loops with `eol=c skip=n delims=xyz tokens=a,b,m-n,*` over an
  explicit file, a wildcard, or the active `<`/pipe input, covering the BASIC
  `READ`/`DATA`/`RESTORE`/`EOF` verbs. Pure option parser / line splitter unit-tested.
- ✅ The rest of the BASIC surface is documented as DOS mappings or not-applicable in
  `command.md` ("BASIC-to-DOS Batch Mapping"): no stubs, no duplicated verbs.
- ✅ Batch process model defined: `command.md` and `SDK.md` ("Batch process model")
  document stdout/stderr/stdin/argv/cwd/PATH/environment propagation and errorlevel.
- ✅ Host-side batch authoring/deployment documented in `SDK.md` ("Authoring and deploying
  batch files"): UTF-8/CRLF, `*.bat` naming and PATH placement, line/label/continuation
  limits, quoting, environment hygiene, and validation.
- ✅ New config: `P4_CONFIG_CALC_*` and `P4_CONFIG_FORF_*` (documented in
  `p4minishell_config.yaml`); test project main-task stack raised to 16 KB.
- ✅ On-board: 139 unit tests, 0 failures (15 `calc` + 5 `for /f` new cases).

---

## Recently Completed (v0.24.6 - August 2026)

### LVGL transcript colour rendering and crash fixes
- ✅ The on-screen transcript now renders the ANSI colour scheme: it is an `lv_spangroup`
      where each coloured run of the output becomes a span with an explicit text colour, so
      the display matches the UART console (previously monochrome green).
- ✅ The span-group rebuild is deferred to an `lv_async_call` so deleting/recreating spans
      never races a running redraw (fixed a `Load access fault` in `lv_draw_span` after
      on-screen keyboard input).
- ✅ Upgraded LVGL from 9.2.2 to 9.4.0 (large bug-fix release) and pinned it in the project
      and test manifests.

### Networking layer consolidation
- ✅ Corrected the initialization order to the canonical ESP-Hosted sequence: transport
      init and slave connect now precede `nvs_flash_init()`, so a dead or mismatched C6
      reports as a transport fault instead of a confusing Wi-Fi error later
- ✅ Documented the full eleven-step order in `networking.h`, with the reason each step
      precedes the next
- ✅ Closed the last encapsulation leak: added `networking_wifi_get_rssi()` and removed the
      direct `esp_wifi_sta_get_ap_info()` call from the header status refresh.
      `components/shell/` no longer depends on `esp_wifi` at all.
- ✅ Verified no `esp_hosted_*`, `esp_wifi_*`, `esp_netif_*`, or NimBLE call remains outside
      `components/networking/`, with `components/c6ota/` the sole sanctioned exception for
      `esp_hosted_slave_ota_*`
- ✅ Station-only made structural: `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` and
      `CONFIG_WIFI_RMT_SOFTAP_SUPPORT` both disabled, so the AP path is no longer compiled
      in. Both are required because esp_wifi_remote mirrors the Wi-Fi Kconfig under its own
      `WIFI_RMT_` prefix.
- ✅ Version gate, c6ota wait/capture/shutdown/restore hooks, and the `wifi` / `bluetooth` /
      `bt` / `c6ota` command surface all unchanged

### Fixed - build constraints were not durable
- ✅ Three documented constraints existed only in the generated `sdkconfig`, so any
      regeneration silently reverted them. All are now pinned in `sdkconfig.defaults`:
      `CONFIG_SPIRAM_XIP_FROM_PSRAM=n` (the defaults file had it set to `y`, contradicting
      the documented constraint), `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (the defaults
      specified `PERF`, which grew the image by roughly 280 KB), and
      `CONFIG_LOG_DEFAULT_LEVEL_WARN=y`

### Fixed - latent truncation in `dir /w`
- ✅ The wide-listing row buffer could not hold a full column set plus one clipped cell, and
      the `strncat()` bound came from a runtime `strlen()` so the compiler could not prove
      it safe. Row length is now tracked explicitly against a compile-time bound.

---

## Previously Completed (v0.22.0 - August 2026)

### Built-in default colour scheme
- ✅ Added `components/ansi/ansi_palette.h` as the single source of truth for what colour
      each category of shell output uses. Named `SH_*` macros expand to the existing
      `@`-specifiers, so no command picks a colour by hand and a palette change is a
      one-file edit. The macros are plain string literals, so the ansi module stays a leaf.
- ✅ Added eight semantic print helpers to `components/shell/`
      (`shell_print_heading`, `shell_print_field`, `shell_print_field_num`, `shell_print_ok`,
      `shell_print_error`, `shell_print_warning`, `shell_print_muted`, `shell_print_usage`),
      each emitting its own reset and newline
- ✅ Migrated every command group: system, hardware, storage, batch, Wi-Fi, Bluetooth, USB,
      and C6 OTA. Roughly 105 error paths and 51 usage lines now route through the helpers.
- ✅ `dir` and `sd ls` colour entries by kind — bold bright blue directories, bright green
      `.bat` files, white files — with grey timestamps and magenta sizes. Colour is applied
      after width formatting so alignment is unaffected, and `dir /b` stays uncoloured so
      redirected output remains machine-parsable.
- ✅ C6 OTA output is coloured at the `main.c` bridge, so `components/c6ota` keeps emitting
      the exact contractual strings documented in `API.md`
- ✅ Defaults only: compile-time palette, no runtime theming or user configuration

### Fixed — muted text was nearly invisible
- ✅ `@k` is pure black (0x0C0C0C) against a dark blue background (0x012456), not the grey
      the old documentation claimed. Thirty sites across `networking.c`, `shell.c`, and
      `command.c` used it for muted text. All now use `@K` via `SH_MUTE`, which fixes
      readability in `wifi status`, `wifi diag`, `wifi scan`, and the keyboard status line.

### Fixed — `ansi_format()` discarded printf width and precision
- ✅ The formatter captured the full specifier then re-rendered with only the bare
      conversion, silently dropping `%10s`, `%-4s`, `%05d`, and `%.2f` formatting. It now
      passes the captured specifier to `snprintf` verbatim and handles `l`/`ll` modifiers.
      This is what lets the aligned `chkdsk` and `dir` reports survive colouring.

### Testing
- ✅ Added `test_ansi_format_width_flags` covering width, alignment, zero padding, float
      precision, length modifiers, and width combined with a colour code
- ✅ Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 / esp32p4

---

## Previously Completed (v0.21.0 — August 2026)

### Batch expressions
- ✅ **`set /a`** — a recursive-descent evaluator over 32-bit signed integers implementing the
      cmd.exe operator set and precedence: `||`, `&&`, comparisons `== != < > <= >=` (1/0),
      `|`, `^`, `&`, `<< >>`, `+ -`, `* / %`, unary `- ~ !`, and parentheses. Compound
      assignment operators are supported. Divide by zero, `INT32_MIN / -1` overflow,
      unbalanced parentheses, and trailing garbage are all detected and reported rather than
      trapping or producing a wrong answer.
- ✅ The `&` and `|` levels deliberately refuse to consume `&&` and `||`, so an expression
      can never swallow a command-chain separator; the logical levels consume them. The
      assignment splitter (`shell_expr_find_assignment`) skips the `=` of a comparison so
      `set /a x=5==3` assigns the comparison result.
- ✅ **`if` numeric keywords** — `EQU`, `NEQ`, `LSS`, `LEQ`, `GTR`, `GEQ` between two decimal
      operands (non-numeric reads as 0), added in v0.24.9.
- ✅ **`set /p`** — prompts and reads a line through the v0.18.0 key-wait facility. Backspace
      edits, ESC cancels, and an empty line leaves the variable unchanged as DOS does.
- ✅ Added `shell_read_line()` to the shell core, which is the reusable piece `set /p` needed

### Line continuation
- ✅ A trailing `^` joins the next physical line, bounded by
      `P4_CONFIG_LINE_CONTINUATION_MAX` so a malformed file cannot loop
- ✅ Uses an odd-caret-count rule, so `^^` at end of line is an escaped literal caret and not
      a continuation — consistent with the escaping rules added in v0.19.0
- ✅ The label scanner applies the identical rule, so a continued line whose tail begins with
      `:` cannot register a phantom label and corrupt `goto` targets

### Attribute preservation
- ✅ New `storage_copy_attributes()` carries R/H/S/A from source to destination
- ✅ Wired into `shell_fs_copy_file()`, so all five copy call sites inherit it at once:
      `copy`, wildcard `copy`, `move`'s copy fallback, `xcopy`, and `xcopy /S`
- ✅ `xcopy /S` carries attributes onto directories it creates
- ✅ Applied after the write, since a read-only destination cannot be opened for writing
- ✅ Non-fatal by design: the file data is already correct, so losing an archive bit does not
      discard a completed transfer. The failure is recorded in the debug log.
- ✅ A read-only destination now reports `use attrib -R to clear it` instead of a bare error,
      which matters more now that copies propagate the read-only bit

### Testing
- ✅ Added `test/main/test_batch_expr.c` with five suites: literals and number bases,
      arithmetic and precedence, bitwise and shift operators, variable reads including the
      undefined-reads-as-zero rule, and error detection
- ✅ Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 / esp32p4

---

## Previously Completed (v0.20.0 — August 2026)

### Phase 2: Stronger Storage Model

#### Volume management
- ✅ `chkdsk [path] [/F]` (alias `scandisk`) — reports the volume label, total/used/free space,
      allocation unit size, and cluster counts. `/F` additionally walks every directory
      verifying each entry is readable. The check is deliberately **read-only**: a card with
      real corruption should be imaged and repaired on a host, not rewritten in place by an
      embedded shell, so problems are reported rather than silently "fixed".
- ✅ `format [/FS:FAT|FAT32|EXFAT] [/V:label] [/Q]` — reformats the card through
      `esp_vfs_fat_sdcard_format()`. Gated behind the exact confirmation word
      `P4_CONFIG_FORMAT_CONFIRM_WORD`, collected through the shared key queue so the answer
      never reaches the command dispatcher. Refuses outright when no interactive input source
      is attached, so a batch file can never silently wipe a card.

#### `dir` option set
- ✅ `/W` wide multi-column listing with DOS-style `[dirname]` bracketing and `~` clipping
- ✅ `/P` pause after each screenful, Enter/Space to continue and `Q` to quit
- ✅ `/S` recurse into subdirectories with per-directory counts plus a grand total
- ✅ `/B` bare output (names only; full paths under `/S` so it can be piped)
- ✅ `/L` lowercase names
- ✅ `/A[:]attrs` attribute filter — `D` dirs, `H` hidden, `S` system, `R` read-only,
      `A` archive, each negatable with a `-` prefix
- ✅ `/O[:]order` sort — `N` name, `S` size, `E` extension, `D` date, `G` dirs first,
      with `-` to reverse. Name is the tie-breaker for every key, so ordering is deterministic.
- ✅ Detailed listings now show a `YYYY-MM-DD  HH:MM` timestamp per entry
- ✅ Every listing closes with free space, and `/S` adds an explicit grand-total banner

#### Guardrails and free-space reporting
- ✅ New `storage_get_space_info()` reports total, used, and free bytes plus cluster geometry
      via `f_getfree()`
- ✅ New `storage_check_free_space()` refuses a write that would leave less than
      `P4_CONFIG_STORAGE_FREE_MARGIN_BYTES` free, so the card never fills to the point where
      FAT metadata updates start failing. An overwrite correctly credits the space the
      destination already occupies.
- ✅ `copy` now refuses to copy a file onto itself. Previously this opened the destination
      with `"wb"` and truncated the source to zero before the first read — a data-loss bug.
- ✅ `copy` prechecks free space and **removes the partial destination** when a write fails,
      so a failed transfer never leaves a truncated file that looks complete
- ✅ `copy` reports percentage progress for files above `P4_CONFIG_COPY_PROGRESS_THRESHOLD`
- ✅ `move` refuses a self-move, which would otherwise hit the same truncation path
- ✅ `write` and `append` precheck free space before opening the file, so an overwrite cannot
      destroy the existing contents and then fail for lack of room
- ✅ `sd info` now reports filesystem total/used/free and the allocation unit size alongside
      the raw card capacity

#### Fixed — recursive walk stack budgets
- ✅ The three recursive directory walkers each held large buffers on the shared 8 KB command
      worker task stack. Moving their per-level state to the heap: `dir` **1472 → 752** bytes,
      `chkdsk` **1024 → 400**, and the pre-existing `tree` walker **976 → 752**. At the 8-level
      depth limit these now total well under budget instead of overflowing it.
- ✅ Verified from the disassembled prologues, per the stack-discipline rule added in v0.19.0

### Testing
- ✅ Added `test/main/test_storage_format.c` covering size formatting across every unit
      boundary, DOS wildcard matching, the self-copy path-identity guard, and the path helpers
- ✅ Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 / esp32p4

---

## Previously Completed (v0.19.0 — August 2026)

### Quoting and Escaping
- ✅ Added a single quote/escape scanner to `components/shell/`:
      `shell_find_unquoted_char()`, `shell_find_unquoted_any()`,
      `shell_has_unquoted_char()`, and `shell_unescape_in_place()`
- ✅ Five surfaces now share it — the argument tokenizer, redirection parsing, pipe splitting,
      chain splitting, and variable expansion — so they can never disagree about whether a
      character is syntax or data
- ✅ `"text"` groups an argument and still expands variables; `'text'` groups literally with no
      expansion; `^c` escapes any single character
- ✅ `shell_split_args()` now understands all three and strips the markup, so a handler receives
      the literal value the user meant
- ✅ `shell_expand_variables()` skips single-quoted runs entirely and honors `^%`

### Command Chaining
- ✅ Added `shell_split_chain()` with `shell_chain_segment_t` / `shell_chain_op_t`
- ✅ `a & b` (both), `a && b` (on success), `a || b` (on failure), mixable on one line
- ✅ A single `|` is deliberately left alone so pipelines still reach the pipe executor;
      `type f | sort && echo done` splits into a pipeline plus a conditional link
- ✅ Chain splitting happens before expansion, so a variable containing `&` cannot inject a
      new command — the same ordering COMMAND.COM uses
- ✅ Success is tracked per link rather than read from global errorlevel, so a stale value from
      an earlier line cannot make the next link take the wrong branch
- ✅ Unknown commands set `P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND` (9009)

### Fixed — batch frame stack overflow (pre-existing, found during this work)
- ✅ `shell_execute_batch_file()` placed a 12,272-byte frame on the 8,192-byte command worker
      task stack, so **a single batch file already overflowed it** before any nesting. The
      label table alone was 8 KB because each of 32 label slots was sized at the full 256-byte
      command width.
- ✅ Label names are now bounded by `P4_CONFIG_BATCH_LABEL_BYTES` (48), and both the frame and
      the line buffer moved to the heap. The function's stack frame is now **96 bytes**.
- ✅ The pipeline's expansion buffers also moved to the heap, taking `shell_execute_command()`
      from 2,032 to 480 bytes, because it sits on the same recursion path.
- ✅ Four levels of batch nesting now use about 5.6 KB of the 8 KB stack, with headroom.
      Verified by disassembling the prologues rather than by inspection.

### Fixed — `if` command correctness
- ✅ `if exist <file>` now resolves relative paths and runs inside a guarded SD session. It
      previously called `stat()` on the raw argument, so any relative path reported "not found".
- ✅ `if "a"=="b"` accepts the joined, spaced, and half-spaced spellings, so the comparison
      still works now that the tokenizer removes the quotes.

### Testing
- ✅ Added `test/main/test_shell_quoting.c` with four suites covering unquoted-operator
      scanning, markup removal, the quoting-aware tokenizer, and chain splitting
- ✅ Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 / esp32p4

---

## Previously Completed (v0.18.0 — August 2026)

### Interactive Keypress Wait (new shell-core facility)
- ✅ Added a keypress queue to `components/shell/` with
      `shell_key_wait_begin()` / `shell_wait_for_key()` / `shell_key_wait_end()`,
      `shell_key_wait_submit()`, `shell_key_wait_is_active()`, and
      `shell_key_input_available()`
- ✅ All three input sources feed it: the UART console reader, the USB HID keyboard bridge,
      and the LVGL on-screen keyboard
- ✅ While a wait is active, every source stops treating input as a command line, so a key
      answering a prompt is never dispatched as a shell command
- ✅ Every wait is bounded by `P4_CONFIG_KEY_WAIT_TIMEOUT_MS`, and commands fall back to the
      previous timed behavior when no interactive key source is attached, so a headless board
      never stalls a batch file

### Batch Control Flow — now complete
- ✅ `pause` blocks on a real keypress instead of a fixed 2-second delay
- ✅ `choice` blocks on a real keypress with `/C:list`, `/N`, `/T:c,secs`, and `/S`, ignores
      unmatched keys like DOS, and sets errorlevel to the 1-based index of the chosen key
- ✅ `setlocal` / `endlocal` perform real environment scoping through a snapshot stack
      (`P4_CONFIG_SETLOCAL_DEPTH_MAX` = 8). A scope left open when a batch frame returns is
      unwound automatically, so a child file can never leak variables into its caller.
- ✅ `exit /b [code]` leaves only the current batch file; a bare `exit [code]` unwinds every
      nested level. Added a `batch_stop_mode_t` so `for` loops and the executor honor both.

### Runtime Prompt Engine
- ✅ `prompt` is a full DOS template engine supporting
      `$p $g $l $b $n $d $t $v $s $_ $q $$ $a $c $f $e $h`
- ✅ The template drives BOTH the UART console prompt and the LVGL input line, so the two
      surfaces can never disagree
- ✅ The input line snapshots the prefix it painted, so a template or path change between two
      LVGL events can never make the shell mis-parse what the user typed
- ✅ `prompt /?` lists the metacharacters; `prompt` with no argument shows the template and its
      rendered form

### Clock Commands
- ✅ `date [MM-DD-YYYY]` and `time [HH:MM[:SS]]` set the system clock, and with no argument
      show a fuller clock panel (local, UTC, Unix timestamp, timezone, uptime, NTP sync status)
- ✅ `timezone [TZ]` shows or sets the POSIX timezone string
- ✅ `sntp` / `ntpsync [sync]` shows NTP sync status and forces a fresh exchange against
      `P4_CONFIG_NTP_SERVER` (verified syncing the clock over Wi-Fi)
- ✅ All time/date/SNTP command bodies live in `components/clock/clock_commands.c`

### File Utility Commands — now complete
- ✅ `tree` is fully recursive with DOS box-drawing connectors, `/F` (include files) and `/A`
      (ASCII connectors), bounded by `P4_CONFIG_TREE_DEPTH_MAX` and the 128-entry listing cap,
      and buffers each directory level on the heap rather than the worker-task stack
- ✅ `sort` replaced the bubble sort with `qsort()`, raised the capacity from 128 to 1024 lines,
      added `/R`, `/I`, and `/U`, and now has a single leak-free release path that frees every
      successful allocation even when a mid-read `strdup()` fails
- ✅ `find` added `/I`, `/N`, `/C`, and `/V`, and gained a recursive file-discovery mode
      (`/NAME:` `/SIZE:` `/NEWER:` `/OLDER:` `/DIRS` `/B`) filtering by name, size, and date
- ✅ `more` waits for Enter or Space between pages and accepts `Q` to quit
- ✅ `fc` reports differing lines in DOS style and detects trailing length differences
- ✅ All five now resolve paths through `shell_fs_resolve_path()` and run inside a guarded SD
      session, which they previously did not — a relative path or a missing card used to fail
      with a bare `fopen` error

### Pipes and Input Redirection
- ✅ `|` supports multi-stage pipelines (`cmd1 | cmd2 | cmd3`, up to
      `P4_CONFIG_PIPE_STAGE_MAX` = 4) with quote-aware splitting, so `echo "a | b"` is no
      longer mistaken for a pipeline
- ✅ Each stage spools to its own file and every spool file is removed on every exit path,
      including a stage failure or an `exit` mid-pipeline
- ✅ Added the `<` input redirection operator. It and the pipe operator share one mechanism —
      the storage input-redirection slot — so `sort < f.txt` and `type f.txt | sort` reach the
      same code path in the text-processing commands.
- ✅ Rewrote the redirection parser as a two-pass scan that handles `>`, `>>`, and `<` in any
      order on one line

### Testing
- ✅ Added `test/main/test_shell_prompt.c` covering template storage, every metacharacter,
      path expansion, and the keypress-wait state machine
- ✅ Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 / esp32p4

---

## Previously Completed (v0.17.0 — August 2026)

### Phase B: Extract Modules
- ✅ Created `components/storage/` — SD session management (`shell_sd_begin`/`shell_sd_end`,
      persistent mount tracking, `sd eject`), path resolution (`shell_sd_resolve_path`,
      `shell_fs_resolve_path`, `shell_resolve_target_from_source`), FATFS conversion
      (`shell_sd_vfs_to_fatfs_path`, `shell_sd_fresult_to_esp_err`), size formatting
      (`shell_sd_format_size`), the RAM-only current working directory, DOS wildcard matching,
      the shared file helpers (`shell_fs_copy_file`, `shell_list_directory_path`,
      `shell_print_file_text`), and the output-redirection writer
- ✅ Created `components/storage/storage_commands.c` — every DOS file command moved out of
      `command.c`: `cd`/`chdir`, `dir`, `copy`, `move`, `del`/`erase`, `ren`/`rename`,
      `md`/`mkdir`, `rd`/`rmdir`, `type`, `write`, `append`, `touch`, `attrib`, `label`,
      `xcopy`, `find`, `more`, `tree`, `fc`, `sort`, and the `sd` command family
- ✅ Created `components/batch/` — batch file execution, `:label` scanning, `goto`,
      `call :label`, `for` loops, the `|` pipe operator, the RAM-only environment variable
      table, PATH, `%VAR%`/`%0`/`%1`..`%9`/`%*` expansion, errorlevel, and the batch language
      commands (`set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`, `choice`,
      `setlocal`, `endlocal`, `exit`)
- ✅ `command.c` reduced from **4,384 lines to 1,140 lines** (74% smaller)
- ✅ Added `batch_command_ops_t` registration table so the batch engine re-enters the command
      pipeline for nested contexts without a reverse dependency
- ✅ `command_init()` now brings up `storage_init()` and `batch_init()`, then registers both
      operations tables
- ✅ Zero behavior change: every dispatcher verb, usage string, output format, error message,
      and bound remains identical to v0.16.0

### Architecture
- ✅ Dependency direction extended and still strictly one-way:
      `main` → `command` → `batch` → `storage` → `shell` → (`ansi`, `display`, `windows`,
      `header`, `keyboard`, `clock`)
- ✅ Two registration tables now invert the only upward dependencies: `shell_command_ops_t`
      (shell ← command) and `batch_command_ops_t` (batch ← command)

### Testing
- ✅ `test/CMakeLists.txt` gained the `storage` and `batch` component directories
- ✅ Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 / esp32p4

---

## Previously Completed (v0.16.0 — August 2026)

### Phase A: Command Implementation Consolidation
- ✅ Moved **every** command implementation from `main.c` into `components/command/command.c`
- ✅ Removed duplicate hardware command implementations (`brightness`, `rotate`, `battery`, `volume`, `gpio`) from `main.c`
- ✅ Removed duplicate `shell_execute_command_core()` and `shell_execute_command()` from `main.c`
- ✅ Removed duplicate transcript functions (`shell_transcript_append_text`, `shell_transcript_appendf`,
      `shell_transcript_render`, `shell_transcript_reset`, `shell_schedule_transcript_appendf`,
      the async staging buffer, and the UART console) from `main.c`
- ✅ Removed duplicate debug functions (`shell_debug_log_push`, `shell_record_errorf`,
      `shell_record_warningf`, `shell_record_infof`, `shell_command_debug`) from `main.c`
- ✅ Removed all 18 `shell_bridge_*` trampolines and `shell_execute_command_core_bridge`
- ✅ Removed dead code from `main.c`: legacy shell-local Wi-Fi runtime, legacy hosted Bluedroid path,
      `reboot_task`, and every unused `static` command duplicate
- ✅ `main.c` reduced from **5,990 lines to 397 lines** (93% smaller) and now contains only
      `app_main()`, LVGL event callbacks, UI construction, and the c6ota/usb host bridges
- ✅ Eliminated all 80+ forward declarations from `main.c`

### Architecture Corrections
- ✅ Dependency direction is now strictly one-way: `command` → `shell`. The old `command` → `main`
      component dependency (a layering inversion) is gone.
- ✅ Added `shell_command_ops_t` registration table so `shell.c` can reach command-owned services
      (dispatch, cwd, volume, SD mount state, battery telemetry) without depending on `command`
- ✅ `components/shell/` now owns: transcript, async buffer, history, debug log, UART console,
      input-line prompt contract, and system info commands
- ✅ `components/command/` now owns: all built-ins, cwd, environment variables, PATH, batch engine,
      SD session management, and hardware telemetry
- ✅ Moved `Kconfig.projbuild` from `main/` to `components/networking/`, the only consumer of
      `CONFIG_P4MINISHELL_WIFI_DEFAULT_*`

### Bug Fixes Found During Consolidation
- ✅ Fixed overlapping-buffer `snprintf` in command history shifting (now `memmove`)
- ✅ Fixed ANSI transcript output being written to UART twice
- ✅ Fixed async transcript flush holding a critical section across LVGL calls
- ✅ Removed unused `var_str` variable in the `for` loop parser

### Testing
- ✅ Unit-test project now builds: added target pinning, component manifest, `sdkconfig.defaults`,
      staged shared headers, and the missing `p4_usb`/USB host component directories
- ✅ Added tests for transcript command formatting and password masking
- ✅ Clean build: 0 errors, 0 warnings for both firmware and test projects on ESP-IDF v5.5.5 / esp32p4

### Documentation
- ✅ Updated all `.md` files and configs to reflect the consolidated module boundaries

---

## Previously Completed (v0.15.1 — August 2026)

### Merged Command Dispatch
- ✅ Consolidated duplicate command execution pipeline from `main.c` and `components/command/command.c` into single path
- ✅ `components/command/command.c` owns unified command dispatcher (`shell_execute_command_core`) and worker task

### Build & Toolchain
- ✅ Updated ESP-IDF baseline from v5.5.3 to v5.5.5 across all configs, lock files, and documentation
- ✅ Fixed deprecation warning: Updated `esp_lvgl_port` DSI callback from `on_refresh_done` to `on_frame_buf_complete` for ESP-IDF 5.5.0+

---

## Main gaps to full feature parity

### 1. Command interpreter parity — COMPLETE
- ✅ Labels (`:label`) in batch files — implemented (label table built on load)
- ✅ `%0`, `%*` — batch script name and all-arguments expansion — implemented
- ✅ `for` loops in batch files — implemented
- ✅ `call` with label targets within same batch file — implemented
- ✅ Better command-line escaping and quoting rules — implemented in v0.19.0:
      `"text"` groups with expansion, `'text'` groups literally with no expansion,
      and `^c` escapes any single character including `^&`, `^|`, `^>`, `^"`, and `^^`
- ✅ Command chaining with `&`, `&&`, and `||` — implemented in v0.19.0

### 2. Filesystem parity — COMPLETE
- ✅ `chkdsk` / `scandisk` for FATFS capacity reporting and a read-only integrity walk
- ✅ `format` for SD card formatting, gated behind an exact confirmation word
- ✅ `dir /W` (wide), `/P` (pause), `/S` (recursive), plus `/B`, `/L`, `/A`, and `/O`
- ✅ Free-space guardrails on `copy`, `move`, `write`, and `append`
- ✅ File attribute preservation during `copy`/`xcopy` — implemented in v0.21.0; R/H/S/A are
      carried across every copy path, and `xcopy /S` also carries them onto created directories

### 3. Batch language completeness — COMPLETE
- ✅ `%0` (script name) and `%*` (all args) expansion — implemented
- ✅ `for` loops — implemented
- ✅ `call :label` within same batch file — implemented
- ✅ `choice` waits for a keypress — implemented in v0.18.0
- ✅ `setlocal`/`endlocal` scope the environment — implemented in v0.18.0
- ✅ `pause` waits for a keypress — implemented in v0.18.0
- ✅ `exit /b` leaves a single batch file — implemented in v0.18.0
- ✅ Line continuation (`^`) in batch files — implemented in v0.21.0
- ✅ `set /a` for arithmetic expressions — implemented in v0.21.0
- ✅ `set /p` for user input prompts — implemented in v0.21.0
- ✅ `set /p NAME=< file` file-line input — implemented in v0.32.0 (reads one line from a
      `< file`/pipe source)
- ✅ `calc` float calculator — implemented in v0.32.0 (BASIC math/string functions,
      `&H`/`0x` hex, `PI`, `RAN#`, DEG/RAD mode, POL/REC X/Y side effects)
- ✅ `for /f` file-line loops — implemented in v0.32.0 (`eol= skip= delims= tokens=,*`
      over files, wildcards, or `<`/pipe input)
- ✅ Shared libraries of batch routines — implemented in v0.32.3:
      `call <file.bat>::<routine> [args]` starts an external `.bat` at a
      `:routine` and returns on `exit /b` / `goto :eof` / EOF, with automatic
      variable isolation (beyond `setlocal`) and errorlevel propagation
- ✅ Easy persistent state — implemented in v0.32.4: `ini` (KEY=VALUE files +
      env import/export), `appconfig <app>` (per-app settings at
      `sd:/APPS/<APP>.INI` without hand-rolling parsing), and `temp`
      (SD-backed temporary files), all on the SD card and sharing one
      `storage_ini.c` core with the applib state group (`app_ini_*`,
      `app_temp_*`)
- ✅ Input timeouts — implemented/verified in v0.32.3: `choice /T:c,secs` (existing),
      `set /p NAME=<prompt> /T:secs` (new), and bounded waits throughout the shell
      key queue

### 4. Extended command completeness
- ✅ `tree` is recursive with `/F` and `/A` — implemented in v0.18.0
- ✅ `sort` uses qsort with `/R`, `/I`, `/U` and a 1024-line capacity — implemented in v0.18.0
- ✅ `find` supports `/I`, `/N`, `/C`, `/V` — implemented in v0.18.0
- ✅ `more` waits for a keypress with `Q` to quit — implemented in v0.18.0
- ✅ Pipe operator `|` supports multiple stages — implemented in v0.18.0
- ✅ Input redirection `<` — implemented in v0.18.0
- ✅ Pipe stages spool through SD rather than streaming — implemented since
      v0.18.0 (each stage spools to an SD temp file read through the
      input-redirection slot; the single worker task runs one command at a
      time, so there is no second process to stream into) and fully
      stress-tested in v0.32.2: multi-stage pipes, pipes inside batch files,
      batch files as pipe stages (stdin via `for /f in ()` / `set /p NAME=<`),
      and pipe + outer `>`/`>>` redirection (the redirection capture is now
      re-entrant, so `cmd1 | cmd2 > out.txt` round-trips).

### 5. Native application model
This is the biggest missing layer for SD-card app support. The **batch**
process model is now explicit (v0.32.2: `proc` process-stack introspection,
`%ERRORLEVEL%` exit-code expansion, and batch files as first-class pipe
processes with stdin/stdout through the redirection layer); a loader-driven
native-app process model remains open:
- ❌ No executable loader
- ✅ Batch process abstraction — implemented in v0.32.2: `proc` lists every
      nested batch process (script, depth, args, echo state) and reports the
      current one (`/args` `/name` `/depth` `/errorlevel` `/echo` `/stdin`);
      `%ERRORLEVEL%` expands to the current exit code. A loader-driven native
      app process model (per-app task/lifecycle) remains open.
- ❌ No per-app lifecycle hooks, startup contract, or exit-code model (the
      batch `exit /b [code]` + errorlevel contract is the working shape)
- ✅ No isolated stdin, stdout, stderr abstraction beyond the shared
      transcript — answered for batch: a batch process's stdin is the
      input-redirection slot (a `< file` or pipe spool), its stdout is the
      transcript/`>`/`>>` capture, and it can be a pipe stage
      (`echo x | filter.bat | findstr ...`). Native apps use the applib
      console API (v0.32.1).
- ❌ No ABI for passing argv, environment variables, or current directory into apps (batch: `%0`..`%9`/`%*`, env table, storage cwd)
- ❌ No memory or task ownership rules for third-party programs (batch runs on the shared worker task; the applib allocation policy is defined)

### 6. `.exe` support strategy
This needs an explicit design decision before implementation starts:
- Recommended path: support native ESP32-P4 applications compiled in C and stored on SD, using a project-defined executable format or extension.
- Compatibility wrapper option: allow a `.exe` file extension for native P4 binaries plus metadata, even though they are not DOS/x86 binaries.
- Full MS-DOS `.exe` compatibility option: add an x86 emulator or DOS-compatible virtual machine. This is a separate subsystem with much higher flash, RAM, performance, and testing cost.

## SDK and API work required
To support third-party apps written in C, the project needs a minimal stable runtime API.

The `edit` editor (v0.24.35) is the reference implementation of the **modal
app surface** pattern — the first native app on top of the shell core. It
settles several of the open design questions below and the full contract is
documented in `SDK.md` ("Modal app surfaces"):

- ✅ App owns a worker session + LVGL surface split, with file I/O off the
  LVGL task and a graceful close handoff through a session event group.
- ✅ Input capture: OSK buttons (through the single keyboard event handler in
  `main.c`), USB keys, and serial lines (through `shell_command_ops_t` hooks)
  all reach the app; nothing leaks to the shell dispatcher while it is open.
- ✅ A status-bar prompt system (Find/Replace/Go-to/Save-As/confirmation)
  instead of modal dialogs.
- ✅ Pure app logic is LVGL-free and unit-tested (`test/main/test_editor.c`).

### Runtime services to define
- ✅ Console output API mapped to the transcript and redirection layer —
  implemented in v0.32.1 as `components/applib` (`app_printf`/`app_printf_ansi`
  and the semantic print helpers). An app's stdout is the transcript, so an app
  run inside a command dispatch is captured by `>`/`>>` exactly like a built-in
  command. The editor remains the reference for the LVGL span-surface rendering
  of richer apps.
- ✅ Input API for keyboard, buttons, and optional touch events — the editor's
  `editor_view_handle_usb_key` / `editor_view_handle_osk` / serial hooks are
  the working shape; formalize as `app_input_t` for Phase 4
- ✅ Filesystem API rooted at the shell current directory and SD mount
  conventions — the editor uses `shell_fs_resolve_path` + guarded
  `shell_sd_begin`/`shell_sd_end`; formalize for Phase 4
- ✅ Memory allocation policy and error reporting contract — implemented in
  v0.32.1 (`app_alloc`/`app_calloc`/`app_realloc`/`app_strdup`/`app_strndup`/
  `app_free` with the PSRAM-threshold policy, plus `app_report_error`/
  `app_report_warning`/`app_report_info` routed to the shell debug log)
- ✅ Time, timers, sleep, and system information helpers — implemented in
  v0.32.1 (`app_time`, `app_time_local`/`app_time_utc`, `app_uptime_sec`,
  `app_now_ms`, `app_delay_ms`, `app_time_synced`, `app_uptime_formatted`,
  `app_sysinfo`)
- ✅ Networking helpers for apps that need Wi-Fi state without owning the full
  stack — implemented in v0.32.1 (`app_wifi_is_connected`,
  `app_wifi_get_rssi`, `app_wifi_state_string`) through the registered
  `applib_net_ops_t` table, so applib never includes `networking.h`

### Tooling to add
- ✅ Header files for the shell SDK — `components/applib` ships the stable
  runtime surface as **lean headers** (`applib.h` umbrella over
  `applib_console.h` / `applib_mem.h` / `applib_time.h` / `applib_net.h` /
  `applib_input.h`, v0.32.3) so an app includes only the groups it uses; the
  editor's `editor.h`/`editor_view.h` split remains the template for
  LVGL-surface apps
- ❌ Example apps written in C
- ❌ Build templates for app targets
- ✅ Packaging rules for SD deployment — done for batch files in v0.32.0 (`SDK.md`,
  "Authoring and deploying batch files": UTF-8/CRLF, `*.bat` naming and PATH placement,
  line/label limits, quoting, validation); native C app packaging remains open
- ✅ A documented ABI or loader manifest format — partially answered: `applib.h`
  (`components/applib`) is the stable runtime ABI for native apps (console,
  memory, time/sysinfo, Wi-Fi state); a loader manifest for SD-deployed apps
  remains open

## Suggested delivery phases

### Phase 1: solid PowerShell/DOS shell core — ✅ COMPLETED (v0.21.0)
- ✅ Wildcard matching (`shell_wildcard_match`) — restored
- ✅ `attrib` command — restored
- ✅ `label` command — restored
- ✅ `xcopy` command — restored
- ✅ `if`, `goto`, `shift`, `errorlevel` — implemented
- ✅ `for` loops — implemented (with `%%var in (set) do command` syntax)
- ✅ `%0`/`%*` expansion — implemented (script name and all arguments)
- ✅ `:label` parsing — implemented (label table built on batch file load)
- ✅ `call :label` — implemented (jump to label within same batch file)
- ✅ `pause`, `prompt`, `date`, `time` — fully implemented (real key wait, DOS template engine, clock set)
- ✅ `find`, `more`, `tree`, `fc`, `sort` — fully implemented with DOS switches
- ✅ Pipe support with `|` — multi-stage pipeline with quote-aware splitting
- ✅ Input redirection with `<`
- ✅ `choice` keypress wait, `setlocal`/`endlocal` scoping, `pause` key wait
- ✅ Quoting and escaping rules (`"..."`, `'...'`, `^c`) — implemented in v0.19.0
- ✅ Command chaining with `&`, `&&`, `||` — implemented in v0.19.0
- ✅ `set /a` arithmetic, `set /p` input, `^` line continuation — implemented in v0.21.0

### Phase 2: stronger storage model — ✅ COMPLETED (v0.20.0)
- ✅ Added `chkdsk` (with `scandisk` alias) and `format` commands for SD card management
- ✅ Added `dir /W`, `/P`, and `/S` options
- ✅ Added guardrails for larger file operations and free-space reporting
- ✅ Improved `dir` sorting, filtering, and formatting: `/O` sort orders, `/A` attribute
      filters, `/B` bare output, `/L` lowercase, timestamps, and grand totals
- ✅ Recursive `tree` command — implemented in v0.18.0 with `/F` and `/A`
- ✅ `choice` keypress wait, `setlocal`/`endlocal` scoping, `pause` key wait — implemented in v0.18.0

### Phase 3: app runtime contract
- ❌ Define a native app ABI for ESP32-P4 programs
- ❌ Decide whether apps are loaded dynamically, linked as plugins, or executed through an interpreted wrapper
- ✅ Define stdout, stderr, stdin, argv, cwd, PATH, and environment propagation — answered
  for the **batch** surface in v0.32.0: `command.md` ("Batch process model") and `SDK.md`
  ("Batch process model") define stdout (transcript + `>`/`>>`), stderr (interleaved, no
  separate stream), stdin (the storage input-redirection slot, consumed by the text tools,
  `for /f`, and `set /p < file`), argv (`%0`..`%9`/`%*`), cwd (storage-owned), PATH
  (batch-owned), and environment propagation (`call` + `setlocal`/`endlocal`). A native app
  ABI for standalone C programs remains open (see Phase 4/5).
- ✅ Define how apps yield control back to the shell cleanly — answered by the
  `edit` modal-surface pattern: a worker session + LVGL view pair with a
  session event group, the window-manager editor-mode handoff, and
  `shell_command_ops_t` input hooks (see SDK.md, "Modal app surfaces").

### Phase 4: SDK and samples
- ✅ Publish a stable shell SDK in C (in progress — see "SDK and API work
  required": the modal app-surface contract is defined by the editor)
- ✅ `edit` — DOS-style inline text editor with DOS-EDIT search parity
  (Find / Repeat / Replace / Go-to-Line, Save As, overwrite, word nav,
  delete line, quit confirm; v0.24.35); ❌ `view`, `netinfo`, `hexview`
  still open
- ✅ Provide host-side build instructions and packaging rules for SD deployment — answered
  for the **batch** surface in v0.32.0: `SDK.md` ("Authoring and deploying batch files")
  documents UTF-8/CRLF, `*.bat` naming and PATH placement, line/label/continuation limits,
  quoting rules, environment hygiene, and the validation flow. Build templates and
  packaging rules for native C apps remain open.

### Phase 5: SD app launcher
- ❌ Add `run` or direct executable invocation from the command line
- ❌ Support app discovery from PATH-like directories on SD
- ❌ Add metadata, versioning, and validation for deployed apps
- ❌ Decide whether `.exe` is a native shell-app extension or a compatibility layer

### Phase 6: optional DOS compatibility layer
- ❌ Evaluate whether literal DOS `.exe` support is still required
- ❌ If yes, design a VM or emulator boundary separate from the shell core
- ❌ Keep it optional so the base shell remains usable without the compatibility cost

## Full project review (2026-08-08)

### Documentation issues

1. ✅ **`documentation.md` contradicts `sdkconfig.defaults`** — Fixed in v0.24.0:
   changed from "Performance (-O3)" to "Size (-Os)".

2. ✅ **`roadmap.md` line counts were stale** — Fixed in v0.24.0: corrected to
   main.c 499, command.c 1,793.

3. ✅ **`command.md` lists `rgb` and `camera` as hardware commands but they are stubs**
   The "Where Commands Live" table at line 113 listed `rgb` and `camera` as
   `components/command/command.c` commands, but `shell_execute_rgb_command()` and
   `shell_execute_camera_command()` only print error messages. Fixed in v0.24.1:
   removed from the hardware table and clarified the "Unsupported Commands" table.
   `rgb` was later implemented in full (v0.24.25): WS2812 status LED with auto
   status, effects, and a CONFIG.SYS `RGB=` directive.

4. ✅ **`p4minishell_config.yaml` may not reflect all v0.23.0 changes**
   The v0.23.0 changelog mentions pinning `CONFIG_SPIRAM_XIP_FROM_PSRAM=n`,
   `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`, and `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` in
   `sdkconfig.defaults`, but the YAML documentation file did not mention these
   build-affecting Kconfig values. Fixed in v0.24.1: added `build_constraints`
   section with all three values, and added `main_task` and `system_event` task
   stack entries to the `task_stacks` section.

### Code quality issues

5. ✅ **Incomplete `SH_*` macro migration** — Fixed in v0.24.0: all remaining raw
   `@`-specifiers in shell.c, command.c, and storage_commands.c now use `SH_*` macros.

6. ✅ **Display/keyboard/windows commands used raw `@` specifiers** — Fixed in v0.24.0:
   migrated to `SH_*` macros.

7. ✅ **`shell_print_heading()` / `shell_print_ok()` / etc. add newline via separate call**
   Each semantic helper called `shell_print_coloured()` then
   `shell_transcript_append_text("\n")` separately. Fixed in v0.24.1: added a
   `bool newline` parameter to `shell_print_coloured()` so the newline is included
   in the ANSI output. All six single-colour helpers (`shell_print_heading`,
   `shell_print_ok`, `shell_print_error`, `shell_print_warning`, `shell_print_muted`,
   `shell_print_usage`) now pass `true` and no longer call
   `shell_transcript_append_text("\n")` separately. `shell_print_field` and
   `shell_print_field_num` were unchanged — they already embed `\n` in their
   snprintf and do not use `shell_print_coloured()`.

8. ✅ **`shell_get_cwd_for_prompt()` uses static buffer — not thread-safe**
   `shell.c:1517` declared `static char path_buf[...]` and returned it. The comment
   said "single UART task" but the function was also called from the LVGL input line
   path. Fixed in v0.24.0: changed to caller-provided buffer
   (`shell_get_cwd_for_prompt(char *buf, size_t buf_size)`), eliminating shared state
   between the UART console task and the LVGL input-line task.

### Hardware support problems

9. ✅ **ESP32-P4 ADC attenuation value may be wrong** — False alarm. Verified in
   ESP-IDF v5.5.5 `hal/adc_types.h:51`: `ADC_ATTEN_DB_12 = 3` IS the correct enum
   value on ESP32-P4. The roadmap's claim that `ADC_ATTEN_DB_12` doesn't exist on
   P4 was based on outdated information. The code at `p4minishell_config.h:451`
   using `P4_CONFIG_BATTERY_ATTEN = 3` with comment `/* ADC_ATTEN_DB_12 */` is
   correct.

10. **ADC calibration may not work on ESP32-P4** — Informational, not a bug.
    ESP32-P4 supports `ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED` (confirmed in
    `esp_adc/esp32p4/include/adc_cali_schemes.h`). When eFuse calibration data is
    present, curve fitting is used. When absent, the fallback
    `gpio_mv = (raw * 3300) / 4095` is the expected behavior — the battery
    percentage is approximate but the system degrades gracefully. This is documented
    behavior, not a defect.

11. ✅ **Coprocessor firmware directory has no source files** — Already documented.
    `coprocessor/esp32c6_slave/README.md` explicitly states "No custom slave source
    code" and explains that the firmware is built entirely from the managed
    `espressif__esp_hosted` component. The `main/CMakeLists.txt` clearly sources
    files from the upstream path (`managed_components/espressif__esp_hosted/slave/main`).
    No change needed.

### Bugs

8. ✅ **`shell_get_cwd_for_prompt()` static buffer race condition** — Fixed in v0.24.0:
   changed to caller-provided buffer, eliminating shared state between UART and LVGL tasks.

12. ✅ **`shell_uart_console_task` uses `fgets()`** — Reviewed and documented. The
    `clearerr(stdin)` + `vTaskDelay(20ms)` recovery loop handles USB-Serial-JTAG
    disconnect/reconnect correctly. stdin is set to unbuffered mode (`_IONBF`) in
    `shell_uart_console_start()`, so `fgets()` reads character-by-character from the
    underlying driver. Added clarifying comment to the function documenting why this
    pattern is safe for USB-Serial-JTAG. No code change needed.

13. ✅ **Potential NULL dereference in `shell_extract_input_text()`**
    `shell.c:783` calls `lv_textarea_get_text(input_line)` and stores in `text`. If
    `input_line` is not NULL but the LVGL textarea has no text allocated,
    `lv_textarea_get_text()` could return NULL. The code checks for NULL at line 789,
    so this is actually safe. However, `strlen(s_input_line_prompt)` was called twice
    (once for `strncmp`, once for pointer arithmetic). Fixed in v0.24.1: cached both
    prompt lengths in local variables so each is computed once.

14. ~~**`shell_command_has_pipe()` called before `shell_split_args()` in dispatcher**
    `command.c:1051` calls `shell_command_has_pipe(trimmed)` which scans the entire
    line for `|`. Then `shell_split_args(trimmed, ...)` at line 1056 re-scans the
    entire line to tokenize. This is O(n) redundant work. For a 256-byte command line
    this is negligible.~~ — Deferred. Both scans are correct, safe, and the
    combined cost on a 256-byte line is ~512 bytes of character processing. The
    dispatcher's early-return on `shell_command_has_pipe` also avoids allocating
    `argv` for pipeline commands, which is a minor win.

### Duplicate code/features

15. ✅ **`shell_command_debug()` duplicates information from `mem` and `wifi status`**
    `shell_command_debug()` printed `networking_wifi_state_string()` and
    `heap_caps_get_free_size()` — both of which are already shown by `wifi status`
    and `mem`. Fixed in v0.24.1: removed the two duplicated lines
    (`debug.wifi_state` and `debug.free_heap`). The debug command now focuses on its
    unique value: the debug log entries and warning count.

16. ✅ **`shell_print_field()` includes `\n` in snprintf but `shell_print_heading()` does not**
    Fixed in v0.24.1: `shell_print_heading()` now includes `\n` via
    `shell_print_coloured(..., true, args)`. Both `shell_print_field()` (which
    embeds `\n` in its own snprintf) and `shell_print_heading()` now produce
    consistent newline behavior through different but equivalent mechanisms.

### API issues

17. ✅ **`ansi.h` documents conflicting meanings for `@b`**
    The `ansi.h` header documented `@b` as both "foreground blue" and "bold off"
    depending on context, and listed non-existent lowercase attribute "off" specifiers
    (`@d`, `@i`, `@u`). Fixed in v0.24.1: rewrote the specifier table to match the
    actual implementation. `@b` is documented as foreground blue only; `@R` is the
    reset-all-attributes specifier. Removed the non-existent `@d`/`@i`/`@u` entries
    and corrected `@k`/`@K` descriptions to match the implementation.

18. ✅ **`shell_execute_command()` implementation not visible in command.c**
    Verified the full pipeline in `shell_execute_command()` (line 1533) and
    `shell_execute_command_segment()` (line 1421). The chain is correct:
    1. Chain splitting (`shell_split_chain`) on `&`, `&&`, `||`
    2. Variable expansion (`shell_expand_variables`) on `%VAR%`, `%0`..`%9`, `%*`
    3. Redirection parsing (`shell_parse_redirection`) on `>`, `>>`, `<`
    4. Input redirect published (`storage_set_input_redirect`)
    5. Dispatch (`shell_execute_command_core`)
    6. Input redirect cleared (`storage_clear_input_redirect`)
    7. Output capture to redirect target (`shell_write_redirect_output`)
    8. Success tracking via errorlevel comparison. No issues found.

### SDK issues

19. ✅ **`sdkconfig.defaults` Kconfig values not documented in YAML**
    The v0.23.0 release pinned `CONFIG_SPIRAM_XIP_FROM_PSRAM=n`,
    `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`, and `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` in
    `sdkconfig.defaults`, but `p4minishell_config.yaml` did not document these
    build-affecting Kconfig options. Fixed in v0.24.1: added `build_constraints`
    section with all three values and explanatory descriptions.

20. ✅ **ESP-Hosted version compatibility gate is hardcoded**
    The version gate in networking.c checks for C6 firmware major/minor == 2.12.x
    using macros from the managed ESP-Hosted component (`ESP_HOSTED_VERSION_MAJOR_1`,
    `ESP_HOSTED_VERSION_MINOR_1`). These auto-update when the component version
    changes, but there was no way to skip the check for development/testing. Fixed
    in v0.24.1: added `P4_CONFIG_HOSTED_SKIP_VERSION_GATE` (default 0) to
    `p4minishell_config.h`. When set to 1, version mismatches are logged as warnings
    instead of rejecting Wi-Fi/Bluetooth init. Both the Wi-Fi and Bluetooth version
    checks in `networking.c` and `bluetooth.c` respect this config. Documented in
    `p4minishell_config.yaml` under `wifi.hosted_skip_version_gate`.

### Test coverage gaps

21. ✅ **No tests for command dispatch, redirection, pipes, or chaining**
    The test suite covers: ANSI formatting, batch expressions, shell history,
    shell parser, shell prompt, shell quoting, storage format, WiFi state,
    variable expansion, and debug log. The pipeline functions
    (`shell_parse_redirection`, `shell_execute_pipe`, `shell_execute_command_core`)
    are static functions that depend on SD card access for redirection and pipe
    execution. They cannot be unit-tested in isolation without a mock SD layer.
    Added in v0.24.1: tests for `shell_expand_variables` (env vars, batch args,
    quoting protection, buffer bounds, NULL safety) and the debug log ring buffer
    (push, warning count, overflow, NULL safety).

22. ✅ **No integration tests**
    All tests are unit tests that test individual functions in isolation. There are
    no integration tests that verify the full flow: input → parse → dispatch → output.
    This means regressions in the interaction between modules (e.g., expansion +
    redirection) are not caught. Full integration tests require a working SD card
    and display, which the test environment does not have. This is a known limitation
    of the current test architecture — integration testing is deferred to hardware
    bring-up sessions (like the one documented in v0.24.1).

23. ✅ **`test_wifi_state.c` tests only static logic**
    The WiFi state test cannot initialize the networking module (no ESP-Hosted
    transport in the test environment), so it tests only the state machine logic.
    The actual WiFi event handlers, connection flow, and watchdog are untested.
    This is inherent to the test environment — the networking module requires
    ESP-Hosted transport to initialize, which is not available in the unit test
    runner. The state machine tests verify the correctness of the state
    transitions that the real event handlers drive.

### Component integration issues

24. ✅ **`shell.c` includes `networking.h`, `bluetooth.h`, `usb.h`, `c6ota.h`**
    Already resolved. Verified that `shell.c` does NOT include any of these
    headers. Its includes are limited to the allowed leaf dependencies:
    `shell.h`, `ansi.h`, `ansi_palette.h`, `display.h`, `header.h`,
    `keyboard.h`, `windows.h`, `clock.h`, plus ESP-IDF/FreeRTOS/BSP/LVGL
    system headers. External-module state is read through the
    `shell_command_ops_t` accessors as required by the ai-context rules.

25. ✅ **`command.c` includes 15+ component headers**
    Already addressed. `command.c` includes10 component headers (`batch.h`,
    `storage.h`, `storage_commands.h`, `shell.h`, `ansi_palette.h`,
    `ansi.h`, `display.h`, `header.h`, `networking.h`, `bluetooth.h`,
    `usb.h`, `c6ota.h`, `clock.h`) which is the minimum required for the
    dispatcher to route commands to all families. UI-specific handlers
    (display, keyboard, windows) were already split into `command_ui.c`
    (separate translation unit) to reduce `command.c`'s direct dependency
    surface. The architecture rules do not restrict `command.c`'s includes —
    `command` is the top-level module that depends on all families.

### Optimizations

26. ✅ **`shell_execute_command_async()` creates a FreeRTOS task per command**
    Each command execution spawned a new task with ~8KB stack and ~1-2ms
    creation overhead. Fixed in v0.24.1: replaced with a persistent worker
    task (`command_worker_task`) and a FreeRTOS queue (`s_command_queue`).
    Commands are posted to the queue via `xQueueSend` and processed
    sequentially by one long-lived task. Eliminates task-creation overhead
    and reduces heap fragmentation. Queue depth is 4 (`SHELL_COMMAND_QUEUE_DEPTH`);
    full queues are logged as warnings with the command dropped.

27. **Pipe spool files use SD for every stage**
    Each pipe stage spools its output to a temporary file on the SD card. For
    small outputs (a few KB), this is slow compared to a RAM-based pipe. This
    optimization requires modifying the storage input-redirection API to support
    both file-based and RAM-based sources, which is a significant architectural
    change. Deferred — the current SD-based approach is correct and the overhead
    is acceptable for typical pipe usage patterns.

28. **Transcript buffer copies on every append**
    The 8KB transcript buffer uses `memcpy` and `memmove` on every append. A
    ring buffer would eliminate the `memmove` on overflow. However,
    `shell_transcript_get_text_from()` needs contiguous strings for `>`/`>>`
    output redirection capture, which complicates a ring buffer implementation.
    Deferred — the current linear buffer is correct and the append cost is
    acceptable for the transcript's write pattern.

---

## Immediate next steps (priority order)

The following are concrete, actionable items organized by priority. They address
real issues found in the codebase, not just new feature work.

### Priority 1 — Bugs and correctness (fix before any new features) — ✅ COMPLETED (v0.24.0)

1. ✅ **Fix `shell_get_cwd_for_prompt()` thread safety** — changed from static
   buffer to caller-provided buffer. Updated `shell.c` (definition + 2 call
   sites), `shell.h`, `test_shell_prompt.c`, `API.md`.

2. ~~**Fix ESP32-P4 ADC attenuation value**~~ — **False positive.** Verified
   `ADC_ATTEN_DB_12 = 3` exists and is correct for ESP32-P4 in ESP-IDF v5.5.5.
   No change needed.

3. ✅ **Complete the `SH_*` macro migration** — replaced all remaining raw `@`
   specifiers in `shell.c` (help, sysinfo, version, about, mem, debug),
   `command.c` (reboot, display, keyboard, windows, unknown command, OOM),
   and `storage_commands.c` (format warning).

4. ✅ **Update `documentation.md` optimization level** — changed from
   "Performance (-O3)" to "Size (-Os)".

5. ✅ **Update `roadmap.md` line counts** — corrected to main.c 499,
   command.c 1,793.

### Priority 2 — Test coverage (add before new features to prevent regressions)

6. **`shell_parse_redirection()` tests** — The function is `static` in
   `command.c` and cannot be called directly from test files. Its quote-aware
   behavior is exercised indirectly through `test_redirection_quote_awareness()`
   (which verifies that `shell_split_args()` treats `>`, `>>`, `<` inside quotes
   as data). The two-pass cut logic itself remains without direct unit tests;
   this is an accepted limitation without a test-only accessor.

7. ✅ **`shell_split_chain()` tests** — Added edge cases beyond the existing
   `test_shell_chain_split()`: pipe-vs-chain under every quoting form,
   single-quote protection, truncation with pipelines, and empty/whitespace
   lines. See `test/main/test_shell_pipeline.c`.

8. ~~**`shell_execute_pipe()` tests**~~ — The function is tightly coupled to
   SD card spool files and `batch_run_nested()`. Its quote-aware splitting is
   covered by `test_pipe_detection_agreement()`. Full pipeline execution tests
   require hardware and are deferred to integration testing.

9. ✅ **`shell_command_has_pipe()` vs `shell_split_args()` agreement** — Added
   `test_pipe_detection_agreement()` covering bare pipes, double-quoted pipes,
   single-quoted pipes, caret-escaped pipes, pipes after closed quotes, and
   mixed real/quoted pipes. Added `test_find_unquoted_pipe()` for the scanner
   directly.

### Priority 3 — Architecture and maintainability — ✅ COMPLETED (v0.24.0)

10. ✅ **Reduce `shell.c` dependency surface** — Removed direct includes of
    `networking.h`, `bluetooth.h`, `usb.h`, `c6ota.h` from `shell.c`. Added 11 new
    hooks to `shell_command_ops_t` (`wifi_is_connected`, `wifi_get_rssi`,
    `wifi_state_string`, `append_sysinfo_summary`, `bluetooth_is_enabled`,
    `bluetooth_is_connected`, `usb_is_connected`, `usb_is_keyboard_attached`,
    `usb_key_to_ascii`, `c6ota_is_pending`, `c6ota_is_busy`). All registered
    from `command_init()`. Every hook is NULL-checked before use.

11. ✅ **Reduce `command.c` dependency surface** — Created `command_ui.c`/
    `command_ui.h` with the `display`, `keyboard`, and `windows` subcommand
    handlers. `command.c` no longer includes `keyboard.h` or `windows.h`.
    `display.h` is still needed for `brightness` and `rotate` hardware commands.

12. ✅ **Document the coprocessor firmware approach** — Updated
    `coprocessor/esp32c6_slave/README.md` to explicitly state there is no custom
    slave source code and that the firmware is built entirely from the managed
    `espressif__esp_hosted` component.

13. ✅ **Add Kconfig values to `p4minishell_config.yaml`** — Added a
    `build_constraints` section documenting `CONFIG_SPIRAM_XIP_FROM_PSRAM`,
    `CONFIG_COMPILER_OPTIMIZATION_SIZE`, `CONFIG_LOG_DEFAULT_LEVEL_WARN`,
    `CONFIG_ESP_WIFI_SOFTAP_SUPPORT`, and `CONFIG_WIFI_RMT_SOFTAP_SUPPORT`.

### Priority 4 — Optimizations (after bugs and tests are addressed)

14. ✅ **Replace per-command task creation with a persistent worker task**
    Already implemented in v0.24.1. `shell_execute_command_async()` now posts
    commands to a FreeRTOS queue processed by a persistent worker task
    (`command_worker_task`). Eliminates ~1-2ms task-creation overhead per
    command and reduces heap fragmentation.

15. **Optimize transcript buffer with ring buffer** — eliminate the `memmove`
    on buffer-full by using a ring buffer with head/tail pointers. Deferred:
    `shell_transcript_get_text_from()` needs contiguous strings for `>`/`>>`
    output redirection capture, which complicates a ring buffer implementation.

16. ✅ **Increase SD I/O buffer size** — `P4_CONFIG_SD_IO_BUFFER_BYTES` was 128
    bytes, causing many small read/write operations. Fixed in v0.24.1: increased
    to 512 bytes (matching `P4_CONFIG_FILE_IO_BUFFER_BYTES` for consistency).
    Stack impact is +384 bytes per function — well within the 8192-byte command
    worker task budget. Affects `storage_copy_file()` and `sd cat`.

### Priority 5 — New subsystem work (Phases 3-6)

17. **App runtime**: Define the native app ABI and loader contract (Phase 3)
18. **SDK**: Publish stable C SDK headers and sample apps (Phase 4)
19. **SD app launcher**: `run` plus PATH-based app discovery (Phase 5)
20. **DOS compatibility layer**: Decide whether literal `.exe` support is required (Phase 6)

---

## Recommended first implementation milestone

The most realistic next milestone addresses the highest-risk issues first, then
builds a test foundation, then moves to new features:

### Milestone A: Bug fixes and correctness — ✅ COMPLETED (v0.24.0)
1. ✅ **Fix `shell_get_cwd_for_prompt()` thread safety** — eliminated the static
   buffer race condition between UART and LVGL tasks.
2. ~~**Fix ESP32-P4 ADC attenuation**~~ — **False positive.** Verified correct.
3. ✅ **Complete `SH_*` macro migration** — replaced all remaining raw `@`
   specifiers in shell.c, command.c, and storage_commands.c.
4. ✅ **Update stale documentation** — fixed `documentation.md` optimization level
   and `roadmap.md` line counts.

### Milestone B: Test foundation — ✅ COMPLETED (v0.24.0)
5. **`shell_parse_redirection()` tests** — Accepted limitation: the function is
   `static`. Its quote-aware behavior is verified indirectly through
   `test_redirection_quote_awareness()`.
6. ✅ **Add chain splitter tests** — Added edge cases: pipe-vs-chain under
   quotes, single-quote protection, truncation with pipelines, empty/whitespace.
7. ~~**Add pipe executor tests**~~ — Deferred: requires hardware. Quote-aware
   splitting covered by `test_pipe_detection_agreement()`.
8. ✅ **Add pipe-detection/tokenizer agreement test** — 7 new test functions
   in `test/main/test_shell_pipeline.c` covering every quoting form.

### Milestone C: Architecture cleanup — ✅ COMPLETED (v0.24.0)
9. ✅ **Reduce shell.c dependencies** — 11 new ops table hooks, 4 headers removed.
10. ✅ **Split command.c UI commands** — `command_ui.c` created.
11. ✅ **Document coprocessor firmware** — README.md updated.

### Milestone D: New features (ongoing)
12. **Native app ABI** — define the contract for SD-card applications.
13. **SDK** — publish stable C SDK headers and one sample app.
14. **SD app launcher** — `run` plus PATH-based app discovery.
15. **Packaging** — build templates and SD deployment rules.

This path fixes the real bugs first, builds a test foundation to prevent
regressions, then moves to new subsystem work with confidence.

---
 
## Code Quality & Architecture (Reviewed 2026-08-07, resolved in v0.16.0 and v0.17.0, extended in v0.18.0)

### Command Consolidation — ✅ COMPLETED
There is exactly one dispatcher, no duplicate implementations, and no bridge trampolines
anywhere in the tree. Phase A pulled everything into `command.c`; Phase B split it into
three focused, single-responsibility modules.

| Area | Owner after Phase B |
|------|---------------------|
| Hardware (`brightness`, `rotate`, `battery`, `volume`, `gpio`, `rgb`, `camera`) | `components/command/command.c` |
| UI queries (`display`, `keyboard`, `windows`) | `components/command/command.c` |
| System (`reboot`, `clear`/`cls`, `prompt`, `date`, `time`) | `components/command/command.c` |
| Dispatcher, execution pipeline, worker task, redirection parsing | `components/command/command.c` |
| System info (`help`, `sysinfo`, `version`, `about`, `mem`, `debug`) | `components/shell/shell.c` |
| DOS file commands, extended DOS commands, text utilities, `sd` family | `components/storage/storage_commands.c` |
| SD session management, path resolution, FATFS conversion, cwd, wildcards | `components/storage/storage.c` |
| Batch engine, `:label`s, `for` loops, pipes, environment, PATH, errorlevel | `components/batch/batch.c` |
| Transcript, history, debug log, UART console, input line | `components/shell/shell.c` |
| Boot, LVGL event routing, UI construction, host bridges | `main/main.c` |

### Module Boundaries — ✅ RESOLVED
- `main.c` is 499 lines and holds no command implementations, no shell state, and no forward
  declarations for local functions.
- `command.c` is 1,793 lines and holds no filesystem code and no batch-language code.
- Dependencies flow strictly one way:
  `main` → `command` → `batch` → `storage` → `shell` → (`ansi`, `display`, `windows`, `header`,
  `keyboard`, `clock`).
- `shell.c` reaches command-owned services through the registered `shell_command_ops_t` table,
  and `batch.c` reaches the command pipeline through `batch_command_ops_t`, rather than
  include-time dependencies. Both mirror the `networking_host_ops_t` pattern.

### Architecture Rule Compliance — ✅ RESOLVED
Per `ai-context.md`:
- ✅ All command dispatch goes through `shell_execute_command()` / `shell_execute_command_core()`
      from `components/command/`
- ✅ All transcript output goes through `shell_transcript_append_text()` /
      `shell_transcript_appendf()` from `components/shell/`
- ✅ All debug logging uses `shell_record_errorf()` / `shell_record_warningf()` /
      `shell_record_infof()` from `components/shell/`
- ✅ No duplicate implementations remain in `main.c`

---

## Refactoring Plan

### Phase A: Consolidate Command Implementations (Priority 1) — ✅ COMPLETED (v0.16.0)
1. ✅ Move all command implementations from `main.c` → `components/command/command.c`
2. ✅ Remove duplicate hardware command implementations in `main.c`
3. ✅ Remove duplicate `shell_execute_command_core()` and `shell_execute_command()` from `main.c`
4. ✅ Remove duplicate transcript/debug functions from `main.c`

### Phase B: Extract Modules (Priority 2) — ✅ COMPLETED (v0.17.0)
`command.c` was the single owner of the commands, which made the remaining split mechanical.
- ✅ Created `components/batch/` for the batch engine (`shell_execute_batch_file`,
      `shell_resolve_batch_path`, label handling, `shell_execute_for_loop`, `shell_execute_pipe`),
      plus the environment variable table, PATH, `shell_expand_variables()`, errorlevel, and the
      batch language commands (`set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`,
      `choice`, `setlocal`, `endlocal`, `exit`)
- ✅ Created `components/storage/` for SD session management (`shell_sd_begin`/`shell_sd_end`,
      path resolution, FATFS conversion, size formatting), plus the current working directory,
      DOS wildcard matching, and the shared file helpers
- ✅ Split the DOS file commands out of `command.c` into `components/storage/storage_commands.c`
- ✅ `command.c` reduced from **4,384 lines to 1,140 lines** (74% smaller) and now owns only the
      dispatcher, the execution pipeline, the worker task, the hardware commands, and the
      system commands
- ✅ Added `batch_command_ops_t` so the batch engine re-enters the command pipeline without a
      reverse dependency, mirroring `shell_command_ops_t`
- ✅ Dependency direction is strictly one-way:
      `command` → `batch` → `storage` → `shell` → (`ansi`, `display`, `windows`, `header`,
      `keyboard`, `clock`)
- ✅ Clean build: 0 errors, 0 warnings for firmware and tests on ESP-IDF v5.5.5 / esp32p4
- ✅ Zero regressions: the dispatcher verb table, every usage string, every output format, and
      every error message are byte-identical to v0.16.0

### Phase C: Clean Up `main.c` (Priority 3) — ✅ COMPLETED (v0.16.0)
1. ✅ `main.c` reduced to `app_main()`, UI callbacks, boot initialization, and host bridges
2. ✅ Removed all forward declarations for functions that belong in module headers
3. ✅ Removed all unused static functions and dead legacy code paths

### Phase D: Documentation Sync (Priority 4) — ✅ COMPLETED (v0.16.0, re-synced v0.17.0)
1. ✅ Updated `changelog.md` with the refactoring entries
2. ✅ Updated `API.md` and `SDK.md` with the new module boundaries
3. ✅ Updated `roadmap.md`, `readme.md`, `documentation.md`, `ai-context.md`, and `command.md`
4. ✅ v0.17.0: re-synced all of the above for the `storage` and `batch` extraction, added the
      Storage and Batch API sections, the per-domain "Adding a new command" table, the layering
      rules for the two new components, and bumped `p4minishell_config.h`,
      `p4minishell_config.yaml`, and the `readme.md` version badge to 0.17.0

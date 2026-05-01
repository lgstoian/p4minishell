# P4MiniShell Roadmap

## Goal
The long-term goal is to turn P4MiniShell into a practical embedded shell environment with strong PowerShell-style usability, a stable SDK for native apps written in C, and app loading from the SD card.

On this hardware, that goal needs to be interpreted carefully:
- Practical target: PowerShell-like shell behavior on UART console, DOS-style file commands on SD, native ESP32-P4 applications stored on SD, and a small C SDK/API for those apps.

## Current baseline (v0.15.0 — May 2026)
Implemented today in the checked-in firmware:

### Shell Core & UI
- ✅ PowerShell-style prompt: `PS \path> ` with ANSI-colored tokens (bright white PS, bright yellow path, bright white >)
- ✅ Windows 11 PowerShell color palette: 16 standard + 8 bright ANSI colors mapped to PS theme
- ✅ 10 PowerShell semantic format specifiers: `@P`/`@H`/`@Q`/`@X`/`@V`/`@O`/`@N`/`@T`/`@Z`/`@F`
- ✅ ANSI/VT escape sequence module (`components/ansi/`) with SGR state machine, format builder, plain-text stripping
- ✅ Touch-first LVGL shell UI with transcript, prompt, on-screen keyboard, 10-command recall
- ✅ Fixed top header bar with Wi-Fi, battery, Bluetooth, USB, SD status + MEM/CPU/BAT system panel
- ✅ Worker-task command execution to protect the LVGL event stack
- ✅ Interactive UART console bridge (stdin/stdout routed through same shell path)
- ✅ Touch-to-show-keyboard: tapping input line brings up OSK (Windows 11 behavior)
- ✅ Keyboard hide button functional (`LV_SYMBOL_KEYBOARD` → `LV_EVENT_CANCEL` → `keyboard_hide()`)
- ✅ Backspace on empty line is a no-op (prompt prefix protection)

### Component Architecture
- ✅ `components/ansi/` — ANSI/VT SGR processing, 16-color palette, format string builder
- ✅ `components/display/` — Display manager (rotation, resolution, refresh, brightness, power)
- ✅ `components/windows/` — Window manager (LVGL screen layout, dynamic scaling, rotation-aware)
- ✅ `components/keyboard/` — Keyboard manager (visibility, modes, textarea binding, external input)
- ✅ `components/header/` — Fixed top status bar (passive, display-only, async-safe)
- ✅ `components/shell/` — Shell core (transcript, history, debug log, UART, sysinfo commands)
- ✅ `components/command/` — Command dispatcher (parser, worker task, all built-ins)
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

### Environment & Batch
- ✅ RAM-only environment variables (24 max), PATH, `%1`..`%9` expansion
- ✅ `.bat` file execution with `rem`/`::` comments, `echo on/off`
- ✅ Output redirection `>` and `>>` to SD files
- ✅ Command history with password masking for `wifi connect`

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
- ✅ USB HID mouse with opt-in echo
- ✅ Full US keyboard layout (60+ HID key codes, modifier-aware)
- ✅ On-screen keyboard auto-hide when USB keyboard attached

### OTA & Hardware
- ✅ `c6ota` for validated C6 firmware updates from SD or HTTP/S
- ✅ C6 slave firmware built (v2.12.1, matching host) in `coprocessor/esp32c6_slave/`
- ✅ Hardware controls: `brightness`, `rotate`, `battery`, `volume`, `gpio`
- ✅ Display power management: `display power on|sleep|off`

### Testing
- ✅ Unity test framework in `test/`
- ✅ Tests for: shell parser, command history, Wi-Fi state machine, ANSI format
- ✅ Clean build: 0 errors, 0 warnings on ESP-IDF v5.5.3 / esp32p4

### Hardware Gaps (intentionally blocked)
- ❌ RGB LED: no authoritative wiring in JC1060 reference
- ❌ Camera: no local camera stack in workspace

---

## Recently Completed (v0.15.0 — May 2026)

### DOS Extended Commands — RESTORED
- ✅ `attrib` — FATFS file attributes (R/H/S/A) with +R/-R/+H/-H/+S/-S/+A/-A
- ✅ `label` — FATFS volume label read/set (max 11 chars, FAT 8.3 convention)
- ✅ `xcopy` — Recursive directory copy with `/S` flag
- ✅ `shell_wildcard_match()` — DOS-style `*` and `?` pattern matching

### Batch Control Flow — NEW
- ✅ `if` — Conditional execution: `if errorlevel N`, `if exist file`, `if "str"=="str"`, `if not ...`
- ✅ `goto` — Jump to `:label` within batch files
- ✅ `shift` — Shift batch arguments left (`%1`→`%0`, etc.)
- ✅ `errorlevel` — Global error code tracking (0=success, non-zero=error)

### Extended Built-in Commands — NEW
- ✅ `pause` — "Press any key to continue" with 2s embedded delay
- ✅ `choice` — Display options and default to first
- ✅ `setlocal` / `endlocal` — Environment scope markers
- ✅ `prompt` — Show/set UART prompt string
- ✅ `date` / `time` — Show current date/time from SNTP
- ✅ `exit` — Exit batch context with errorlevel

### File Utility Commands — NEW
- ✅ `find` — Search text in files or transcript
- ✅ `more` — Paginated file viewing (20 lines per page)
- ✅ `tree` — Simple recursive directory tree display
- ✅ `fc` — File comparison (line-by-line diff)
- ✅ `sort` — Line sorting with bubble sort (128 line max)

### Pipe Support — NEW
- ✅ `|` pipe operator — `command1 | command2` via temp file on SD

### Code Quality & Hardening
- ✅ All build errors fixed (implicit declarations, conflicting types)
- ✅ `p4minishell.h` extended with all public function declarations
- ✅ `command.c` includes `p4minishell.h` for proper declarations
- ✅ `main.c` includes `shell.h` for `shell_get_time_string()`
- ✅ Forward declarations added for functions used before definition
- ✅ All `static` functions needed by `command.c` made non-static
- ✅ `shell_command_sd_eject()` exported for `sdeject` command dispatch

---

## Main gaps to full feature parity

### 1. Command interpreter parity
Missing user-facing shell features compared with COMMAND.COM or PowerShell:
- ❌ Labels (`:label`) in batch files — `goto` exists but label parsing needs work
- ❌ `%0`, `%*` — batch script name and all-arguments expansion
- ❌ Better command-line escaping and quoting rules
- ❌ `for` loops in batch files
- ❌ `call` with label targets within same batch file

### 2. Filesystem parity
- ❌ No `chkdsk` / `scandisk` for FATFS integrity checks
- ❌ No `format` command for SD card formatting
- ❌ No file attribute preservation during `copy`/`xcopy`
- ❌ No `dir /w` (wide), `/p` (pause), `/s` (recursive) options

### 3. Batch language completeness
- ❌ No `%0` (script name) or `%*` (all args) expansion
- ❌ No line continuation (`^`) in batch files
- ❌ No `for` loops
- ❌ No `call :label` within same batch file
- ❌ No `set /a` for arithmetic expressions
- ❌ No `set /p` for user input prompts

### 4. Native application model
This is the biggest missing layer for SD-card app support:
- ❌ No executable loader
- ❌ No process abstraction
- ❌ No per-app lifecycle hooks, startup contract, or exit-code model
- ❌ No isolated stdin, stdout, stderr abstraction beyond the shared transcript
- ❌ No ABI for passing argv, environment variables, or current directory into apps
- ❌ No memory or task ownership rules for third-party programs

### 5. `.exe` support strategy
This needs an explicit design decision before implementation starts:
- Recommended path: support native ESP32-P4 applications compiled in C and stored on SD, using a project-defined executable format or extension.
- Compatibility wrapper option: allow a `.exe` file extension for native P4 binaries plus metadata, even though they are not DOS/x86 binaries.
- Full MS-DOS `.exe` compatibility option: add an x86 emulator or DOS-compatible virtual machine. This is a separate subsystem with much higher flash, RAM, performance, and testing cost.

## SDK and API work required
To support third-party apps written in C, the project needs a minimal stable runtime API.

### Runtime services to define
- ❌ Console output API mapped to the transcript and redirection layer
- ❌ Input API for keyboard, buttons, and optional touch events
- ❌ Filesystem API rooted at the shell current directory and SD mount conventions
- ❌ Memory allocation policy and error reporting contract
- ❌ Time, timers, sleep, and system information helpers
- ❌ Networking helpers for apps that need Wi-Fi state without owning the full stack

### Tooling to add
- ❌ Header files for the shell SDK
- ❌ Example apps written in C
- ❌ Build templates for app targets
- ❌ Packaging rules for SD deployment
- ❌ A documented ABI or loader manifest format

## Suggested delivery phases

### Phase 1: solid PowerShell/DOS shell core ✅ COMPLETE
- ✅ Wildcard matching (`shell_wildcard_match`) — restored
- ✅ `attrib` command — restored
- ✅ `label` command — restored
- ✅ `xcopy` command — restored
- ✅ `if`, `goto`, `shift`, `errorlevel` — implemented
- ✅ `pause`, `prompt`, `date`, `time` — implemented
- ✅ `find`, `more`, `tree`, `fc`, `sort` — implemented
- ✅ Pipe support with `|` — implemented
- ❌ `for` loops, `%0`/`%*` expansion, label parsing in batch files

### Phase 2: stronger storage model
- ❌ Add `chkdsk`/`format` commands for SD card management
- ❌ Add `dir /w`, `/p`, `/s` options
- ❌ Add guardrails for larger file operations and better free-space reporting
- ❌ Improve `dir` sorting, filtering, and formatting options

### Phase 3: app runtime contract
- ❌ Define a native app ABI for ESP32-P4 programs
- ❌ Decide whether apps are loaded dynamically, linked as plugins, or executed through an interpreted wrapper
- ❌ Define stdout, stderr, stdin, argv, cwd, PATH, and environment propagation
- ❌ Define how apps yield control back to the shell cleanly

### Phase 4: SDK and samples
- ❌ Publish a stable shell SDK in C
- ❌ Add sample apps such as `edit`, `view`, `netinfo`, or `hexview`
- ❌ Provide host-side build instructions and packaging rules for SD deployment

### Phase 5: SD app launcher
- ❌ Add `run` or direct executable invocation from the command line
- ❌ Support app discovery from PATH-like directories on SD
- ❌ Add metadata, versioning, and validation for deployed apps
- ❌ Decide whether `.exe` is a native shell-app extension or a compatibility layer

### Phase 6: optional DOS compatibility layer
- ❌ Evaluate whether literal DOS `.exe` support is still required
- ❌ If yes, design a VM or emulator boundary separate from the shell core
- ❌ Keep it optional so the base shell remains usable without the compatibility cost

## Immediate next steps (priority order)
1. **Batch language**: Add `for` loops, `%0`/`%*` expansion, `:label` parsing, `call :label`
2. **Storage tools**: Add `chkdsk`, `format`, `dir /w /p /s` options
3. **Code consolidation**: Merge duplicate dispatch logic between main.c and command.c
4. **App runtime**: Define native app ABI and loader contract
5. **SDK**: Publish stable C SDK headers and sample apps
2. **Wildcard integration**: Wire `shell_wildcard_match()` into `dir`, `del`, `copy` commands.
3. **Batch control flow**: Add `if`, `goto`, `errorlevel` for COMMAND.COM parity.
4. **Native app ABI**: Define the contract for SD-card applications.

## Recommended first implementation milestone
The most realistic next milestone is:
1. Restore `attrib`/`label`/`xcopy`/wildcard implementations
2. Integrate wildcards into `dir`/`del`/`copy`
3. Add `if`/`goto`/`errorlevel` batch control flow
4. Define a minimal native app ABI for ESP32-P4 C programs

That path gets the project to a usable embedded shell platform quickly.
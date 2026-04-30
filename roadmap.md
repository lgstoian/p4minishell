# P4MiniShell Roadmap

## Goal
The long-term goal is to turn P4MiniShell into a practical embedded shell environment with strong PowerShell-style usability, a stable SDK for native apps written in C, and app loading from the SD card.

On this hardware, that goal needs to be interpreted carefully:
- Practical target: PowerShell-like shell behavior on UART console, DOS-style file commands on SD, native ESP32-P4 applications stored on SD, and a small C SDK/API for those apps.

## Current baseline (v0.14.1 — April 2026)
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

## Main gaps to full feature parity

### 1. DOS attribute & volume commands (PARTIALLY DONE — implementations lost, need restoration)
- ❌ `attrib` — FATFS file attributes (R/H/S/A). **Dispatch exists in command.c; implementation was added but lost during git restore. Needs re-implementation.**
- ❌ `label` — FATFS volume label read/set. **Dispatch exists in command.c; implementation was added but lost.**
- ❌ `xcopy` — Recursive directory copy with `/S`. **Dispatch exists in command.c; implementation was added but lost.**
- ❌ Wildcard matching (`*` and `?`) for `dir`, `del`, `copy`. **`shell_wildcard_match()` was added but lost.**

### 2. Command interpreter parity
Missing user-facing shell features compared with COMMAND.COM or PowerShell:
- ❌ `if`, `goto`, `shift`, labels, `errorlevel`, and conditional batch execution
- ❌ `pause`, `choice`, `setlocal`, `endlocal`, `prompt`, `date`, `time`
- ❌ Pipe support with `|`
- ❌ Better command-line escaping and quoting rules
- ❌ Built-ins such as `find`, `more`, `tree`, `fc`, `sort`, `exit`

### 3. Filesystem parity
- ❌ No `attrib` for hidden, system, archive, or read-only flags in the shell UX
- ❌ No `label` for volume label
- ❌ No `xcopy` for recursive copy or directory tree operations
- ❌ No wildcard-aware rename, delete, or copy flows

### 4. Batch language completeness
The batch subsystem is functional but intentionally small:
- ❌ No labels or `goto`
- ❌ No `%0`, `%*`, or argument shifting
- ❌ No local variable scopes such as `setlocal`
- ❌ No return codes or structured error handling across batch invocations
- ❌ No line continuation or more advanced parser behavior

### 5. Native application model
This is the biggest missing layer for SD-card app support:
- ❌ No executable loader
- ❌ No process abstraction
- ❌ No per-app lifecycle hooks, startup contract, or exit-code model
- ❌ No isolated stdin, stdout, stderr abstraction beyond the shared transcript
- ❌ No ABI for passing argv, environment variables, or current directory into apps
- ❌ No memory or task ownership rules for third-party programs

### 6. `.exe` support strategy
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

### Phase 1: solid PowerShell/DOS shell core
- ✅ Wildcard matching function (`shell_wildcard_match`) — IMPLEMENTED BUT LOST, needs restoration
- ✅ `attrib` command — IMPLEMENTED BUT LOST, needs restoration
- ✅ `label` command — IMPLEMENTED BUT LOST, needs restoration
- ✅ `xcopy` command — IMPLEMENTED BUT LOST, needs restoration
- ❌ Add `if`, labels, `goto`, `shift`, and `errorlevel`
- ❌ Add `pause`, `prompt`, `date`, `time`
- ❌ Normalize quoting and escaping behavior

### Phase 2: stronger storage model
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

## Recent completions (v0.14.1)
- ✅ PowerShell-style prompt with ANSI color tokens and path truncation
- ✅ Windows 11 PowerShell color palette (16+8 colors, dark blue BG, light gray FG)
- ✅ 10 PowerShell semantic format specifiers in ansi.c
- ✅ Touch-to-show-keyboard behavior (Windows 11 touchscreen UX)
- ✅ Backspace-on-empty-line no-op (prompt prefix protection)
- ✅ All build warnings fixed (0 errors, 0 warnings)
- ✅ Wi-Fi mutex and persistent watchdog fully implemented
- ✅ SD/file command dispatch fixed in command.c (20+ bridged commands)
- ✅ LVGL keyboard event callback registered
- ✅ Unity test framework with 5 test suites
- ✅ C6 slave firmware built (v2.12.1, matching host)
- ✅ Deprecated VFS UART API warnings suppressed

## Immediate next steps (priority order)
1. **Restore lost implementations**: `attrib`, `label`, `xcopy`, `shell_wildcard_match` — dispatches exist in command.c, implementations were added via Python script but lost during `git checkout`. Re-implement directly in main.c.
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
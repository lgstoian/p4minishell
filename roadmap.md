# P4MiniShell Roadmap

## Goal
The long-term goal is to turn P4MiniShell into a practical embedded shell environment with strong MS-DOS-style usability, a stable SDK for native apps written in C, and app loading from the SD card.

On this hardware, that goal needs to be interpreted carefully:
- Practical target: DOS-like shell behavior, native ESP32-P4 applications stored on SD, and a small C SDK/API for those apps.

## Current baseline
Implemented today in the checked-in firmware:
- Fixed top header bar module for notifications plus passive Wi-Fi, battery, Bluetooth, USB, and SD status, integrated without changing the locked transcript shell flow
- Touch-first LVGL shell UI with transcript, prompt, keyboard, and command recall
- Worker-task command execution to protect the LVGL event stack
- Stable family-command dispatch for `wifi`, `sd`, and `c6ota`, with the original command line preserved for second-stage subcommand parsing
- Interactive serial monitor access through the configured ESP-IDF console, reusing the same shell transcript and command path as the touch UI
- Live hardware shell controls for display brightness, display rotation with GT911 remap, battery telemetry, speaker volume, and safer GPIO inspection or limited writes — all display controls now routed through `components/display/` display manager
- DOS-style file commands on SD: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
- RAM-only environment variables, PATH, `%1`..`%9` expansion, `.bat` execution, and `echo on/off`
- Transcript-backed `>` and `>>` output redirection to SD
- ESP-Hosted Wi-Fi on ESP32-P4 through the ESP32-C6 over SDIO
- `c6ota` for validated ESP32-C6 firmware updates from SD or HTTP/S

Current hardware gaps still intentionally blocked in the checked-in firmware:
- RGB LED control remains blocked until the board metadata declares a real RGB output pin and driver model
- Camera capture remains blocked until the board metadata declares a real camera device and capture path

Hosted connectivity status in the current baseline:
- Wi-Fi has been moved into `components/networking/networking.c` while preserving the working boot, command, and OTA restore behavior
- Bluetooth now has a real hosted NimBLE baseline in `components/networking/bluetooth.c` for `bluetooth status`, `bluetooth scan`, and `bluetooth advertise <on|off>`
- USB now has a dedicated `components/usb` baseline for USB MSC storage at `/usb0` plus HID keyboard or mouse attach and debug echo through the `usb` command family
- USB keyboard auto-detect is now implemented: plug in a USB keyboard to type commands; on-screen keyboard hides automatically
- Full US keyboard layout supported: 60+ USB HID key codes with modifier-aware shifted character mapping
- Keyboard external input mode API: `keyboard_set_external_input()`, `keyboard_force_visible()`, `keyboard_clear_force_visible()`
- USB keyboard CLI injection bridge: `shell_usb_keyboard_input()` with LVGL async dispatch for safe input line manipulation
- Header now has a dedicated `components/header` baseline for a fixed notification and status bar above the locked transcript
- `c6ota` has now been fully refactored into `components/c6ota` with the same shell-visible behavior and a documented public API in `API.md` and `SDK.md`
- Display now has a dedicated `components/display` baseline for centralized display hardware management: rotation, resolution, refresh rate, brightness, power state, touch handle, and diagnostics — all routed through a single public API
- Windows now has a dedicated `components/windows` baseline for LVGL screen layout management: named regions, resolution-aware scaling, rotation-aware layout, consistent styling, and clean lifecycle — working together with display.c and header.c
- Keyboard now has a dedicated `components/keyboard` baseline for LVGL keyboard management: visibility control, mode switching, textarea binding, and dynamic height scaling — with automatic UI reflow when hidden
- ANSI/VT escape sequence module now has a dedicated `components/ansi` baseline for SGR color processing: 16-color PowerShell-inspired palette, format string builder, text processing state machine, and UART pass-through
- Shell now has a dedicated `components/shell` baseline for transcript management, command history, debug logging, UART console bridge, and system info commands
- Command now has a dedicated `components/command` baseline for command parsing, dispatch, worker task execution, and all built-in command implementations
- Future Bluetooth work should build on the hosted NimBLE module rather than reviving the older Bluedroid experiment

## Main gaps to full feature parity

### 1. Command interpreter parity
Missing user-facing shell features compared with COMMAND.COM or late MS-DOS usage patterns:
- Wildcard expansion for file commands such as `copy *.txt backup\`
- `if`, `goto`, `shift`, labels, `errorlevel`, and conditional batch execution
- `pause`, `choice`, `setlocal`, `endlocal`, `prompt`, `date`, `time`, and richer `dir` options
- Pipe support with `|`
- Better command-line escaping and quoting rules
- Built-ins such as `attrib`, `find`, `more`, `tree`, `fc`, `sort`, and `exit`

### 2. Filesystem parity
The current shell can manage files, but it does not yet mirror DOS semantics closely enough:
- No attribute model for hidden, system, archive, or read-only flags in the shell UX
- No volume label support
- No recursive copy or directory tree operations
- No wildcard-aware rename, delete, or copy flows
- No user-visible handling for short-name versus long-name compatibility

### 3. Batch language completeness
The batch subsystem is functional but intentionally small:
- No labels or `goto`
- No `%0`, `%*`, or argument shifting
- No local variable scopes such as `setlocal`
- No return codes or structured error handling across batch invocations
- No line continuation or more advanced parser behavior

### 4. Native application model
This is the biggest missing layer for SD-card app support:
- No executable loader
- No process abstraction
- No per-app lifecycle hooks, startup contract, or exit-code model
- No isolated stdin, stdout, stderr abstraction beyond the shared transcript
- No ABI for passing argv, environment variables, or current directory into apps
- No memory or task ownership rules for third-party programs

### 5. `.exe` support strategy
This needs an explicit design decision before implementation starts:
- Recommended path: support native ESP32-P4 applications compiled in C and stored on SD, using a project-defined executable format or extension.
- Compatibility wrapper option: allow a `.exe` file extension for native P4 binaries plus metadata, even though they are not DOS/x86 binaries.
- Full MS-DOS `.exe` compatibility option: add an x86 emulator or DOS-compatible virtual machine. This is a separate subsystem with much higher flash, RAM, performance, and testing cost.

## SDK and API work required
To support third-party apps written in C, the project needs a minimal stable runtime API.

### Runtime services to define
- Console output API mapped to the transcript and redirection layer
- Input API for keyboard, buttons, and optional touch events
- Filesystem API rooted at the shell current directory and SD mount conventions
- Memory allocation policy and error reporting contract
- Time, timers, sleep, and system information helpers
- Networking helpers for apps that need Wi-Fi state without owning the full stack

### Tooling to add
- Header files for the shell SDK
- Example apps written in C
- Build templates for app targets
- Packaging rules for SD deployment
- A documented ABI or loader manifest format

### Recent completions
- Done: `components/shell` shell core module with transcript, history, debug log, UART console, and system info commands
- Done: `components/command` command dispatcher module with parser, worker task, and all built-in commands
- Done: `components/keyboard` keyboard manager module with visibility control, mode switching, and automatic UI reflow
- Done: `components/windows` window manager module with LVGL screen layout, dynamic scaling, rotation-aware regions, and consistent styling
- Done: `components/display` display manager module with centralized rotation, resolution, refresh rate, brightness, power management, and touch handle control
- Done: all display-related shell commands (`brightness`, `rotate`) refactored to use display manager public API
- Done: display manager provides `display_info_t` for comprehensive sysinfo diagnostics
- Done: display manager owns touch handle acquisition and rotation remapping internally
- Done: fixed `components/header` top-bar module with passive `header_init`, `header_update_status`, and `header_update_*` integration for notifications and system status
- Done: header system panel redesigned with MEM | CPU | BAT all on the far right, dynamically linked to FreeRTOS runtime statistics
- Done: CPU usage bar + percentage from FreeRTOS idle task runtime counter deltas
- Done: battery always visible — shows "BAT N/C" when ADC not connected
- Done: `sysinfo`, `version`, `mem`, and `about` commands expanded with real-time FreeRTOS data (uptime, task count, heap percentage)
- Done: fixed SD card status icon visibility and styling so it matches the other header status icons when a card is mounted
- Done: `c6ota` refactor into `components/c6ota` with stable `c6ota_init`, `c6ota_perform`, and `c6ota_register_progress_callback` documentation
- Done: initial `API.md` and `SDK.md` published for the modular OTA component
- Done: USB host refactor into `components/usb` with documented `usb_init`, `usb_handle_command`, `usb_status`, `usb_msc_mount`, `usb_msc_ls`, and HID echo controls

## Suggested delivery phases

### Phase 1: solid DOS shell core
- Add wildcard expansion
- Add `if`, labels, `goto`, `shift`, and `errorlevel`
- Add `pause`, `prompt`, `date`, `time`, and `attrib`
- Normalize quoting and escaping behavior

### Phase 2: stronger storage model
- Add recursive file operations where safe
- Expose attributes and volume information
- Improve `dir` sorting, filtering, and formatting options
- Add guardrails for larger file operations and better free-space reporting

### Phase 3: app runtime contract
- Define a native app ABI for ESP32-P4 programs
- Decide whether apps are loaded dynamically, linked as plugins, or executed through an interpreted wrapper
- Define stdout, stderr, stdin, argv, cwd, PATH, and environment propagation
- Define how apps yield control back to the shell cleanly

### Phase 4: SDK and samples
- Publish a stable shell SDK in C
- Add sample apps such as `edit`, `view`, `netinfo`, or `hexview`
- Provide host-side build instructions and packaging rules for SD deployment

### Phase 5: SD app launcher
- Add `run` or direct executable invocation from the command line
- Support app discovery from PATH-like directories on SD
- Add metadata, versioning, and validation for deployed apps
- Decide whether `.exe` is a native shell-app extension or a compatibility layer

### Phase 6: optional DOS compatibility layer
- Evaluate whether literal DOS `.exe` support is still required
- If yes, design a VM or emulator boundary separate from the shell core
- Keep it optional so the base shell remains usable without the compatibility cost

## Recommended first implementation milestone
The most realistic next milestone is not a full `.exe` runtime. It is:
- finish COMMAND.COM-style batch control flow
- add wildcard-aware file commands
- define a minimal native app ABI for ESP32-P4 C programs
- load those native apps from SD with a simple manifest or wrapper format

That path gets the project to a usable embedded DOS-like platform quickly.
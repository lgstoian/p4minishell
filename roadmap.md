# P4MiniShell Roadmap

## Goal
The long-term goal is to turn P4MiniShell into a practical embedded shell environment with strong MS-DOS-style usability, a stable SDK for native apps written in C, and app loading from the SD card.

On this hardware, that goal needs to be interpreted carefully:
- Practical target: DOS-like shell behavior, native ESP32-P4 applications stored on SD, and a small C SDK/API for those apps.

## Current baseline
Implemented today in the checked-in firmware:
- Touch-first LVGL shell UI with transcript, prompt, keyboard, and command recall
- Worker-task command execution to protect the LVGL event stack
- Stable family-command dispatch for `wifi`, `sd`, and `c6ota`, with the original command line preserved for second-stage subcommand parsing
- Live hardware shell controls for display brightness, display rotation with GT911 remap, battery telemetry, speaker volume, and safer GPIO inspection or limited writes
- DOS-style file commands on SD: `cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`, `write`, `append`, `touch`
- RAM-only environment variables, PATH, `%1`..`%9` expansion, `.bat` execution, and `echo on/off`
- Transcript-backed `>` and `>>` output redirection to SD
- ESP-Hosted Wi-Fi on ESP32-P4 through the ESP32-C6 over SDIO
- `c6ota` for validated ESP32-C6 firmware updates from SD or HTTP/S

Current hardware gaps still intentionally blocked in the checked-in firmware:
- Hosted Bluetooth remains disabled on the current ESP32-C6 baseline until a proven BLE-safe host path replaces the unstable Bluedroid experiment
- RGB LED control remains blocked until the board metadata declares a real RGB output pin and driver model
- Camera capture remains blocked until the board metadata declares a real camera device and capture path

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
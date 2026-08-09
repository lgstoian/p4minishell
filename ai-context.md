# P4MiniShell AI Context Rules

## Project Identity
- **Name**: P4MiniShell
- **Type**: Embedded DOS-style command shell
- **Target**: ESP32-P4 (host) + ESP32-C6 (co-processor over ESP-Hosted SDIO)
- **Framework**: ESP-IDF v5.5.5
- **UI**: LVGL 9.4.0 with JD9165 1024x600 display + GT911 touch

## Mandatory Reading Before Any Change
1. changelog.md - version history and recent changes
2. readme.md - project overview and current behavior
3. documentation.md - technical architecture
4. ai-context.md - this file (project rules)
5. board_config.yaml - hardware configuration
6. p4minishell_config.h - centralized config values
7. p4minishell_config.yaml - config documentation
8. command.md - command reference
9. sdkconfig - current build configuration
10. main/idf_component.yml - component dependencies
11. For roadmap work: also read roadmap.md and licence.md

## Configuration Rules
- ALL tunable values MUST live in p4minishell_config.h, never hardcoded in source
- p4minishell_config.yaml MUST be updated whenever a config value changes
- Use P4_CONFIG_* macros for new code; SHELL_* aliases exist for backward compatibility
- USB HID key codes are standard USB HID usage table values and stay local to usb.c
- board_config.h remains the hardware-pin source of truth (GPIO assignments, display timing)
- sdkconfig remains the ESP-IDF build-configuration source of truth

## Source Code Rules

### Comments
- All major sections need descriptive comments explaining purpose and behavior
- Use Doxygen-style @file, @brief, @param for public APIs
- Section markers should describe what follows, not just repeat function names
- NEVER use `// AI:` prefix in comments; use plain descriptive text
- Existing `// AI:` comments must be replaced with equivalent descriptive text

### Audit and Hardening Rules
- Before every release: scan all source files for `// AI:` comments and replace them
- Build must produce ZERO errors and ZERO warnings on target toolchain
- No build artifacts in repository; .gitignore must cover build/, sdkconfig, and generated files
- All hardcoded strings must reference p4minishell_config.h macros
- Public API functions must have Doxygen documentation in their header files
- Cross-component calls must go through documented bridge functions (p4minishell.h)
- Semaphore/mutex acquisition must always have a corresponding release path
- Buffer operations must bounds-check before memcpy/snprintf
- NULL pointer checks required before dereference in all public API functions
- All `lv_async_call` return values must be checked; free payload on failure
- All `lv_textarea_get_text()` return values must be NULL-checked before use
- Forward declarations without implementations are a critical bug; never commit them

### Header Bar Rules
- All `header_update_*()` functions MUST update internal state immediately before scheduling async render
- Header state writes (bool/int) are atomic on this platform and safe from any task context
- If LVGL async dispatch fails (lv_async_call returns error), fall back to synchronous header_render()
- If allocation fails for the async payload, fall back to synchronous header_render()
- SD indicator uses consistent HEADER_SD_SYMBOL in both mounted and unmounted states
- SD indicator shows persistent state (NO/INS/ON/ERR)
- Battery is ALWAYS visible — shows "BAT N/C" with muted styling when ADC is not connected
- System panel (MEM | CPU | BAT) is on the far right, all dynamically linked to FreeRTOS runtime stats
- header_update_battery(int percent, bool adc_ready) — pass adc_ready=false for N/C display
- header_update_mem(uint32_t free_heap, uint32_t total_heap) — real-time from heap_caps
- header_update_cpu(int percent, uint32_t task_count) — real-time from FreeRTOS runtime stats
- header_update_uptime(uint32_t seconds) — system uptime from esp_timer_get_time()
- Touch init failure must not prevent header rendering (BSP touch is optional)

### Module Layering Rules
- Component dependencies flow ONE WAY: `main` -> `command` -> `batch` -> `storage` -> `shell` -> (`ansi`, `display`, `windows`, `header`, `keyboard`, `clock`)
- `components/shell/` MUST NOT depend on `components/command/`. When the shell core needs a
  command-owned service, add it to `shell_command_ops_t` in `shell.h` and register it from
  `command_init()` via `shell_register_command_ops()`
- `components/batch/` MUST NOT depend on `components/command/`. When the batch engine needs the
  full command pipeline (nested `if`/`for`/pipe/batch lines), route it through
  `batch_command_ops_t` in `batch.h`, registered from `command_init()` via
  `batch_register_command_ops()`
- `components/storage/` MUST NOT depend on `components/batch/` or `components/command/`. It is
  the lowest shell-facing layer and only calls into `components/shell/` and `components/header/`
- Every `shell_command_ops_t` and `batch_command_ops_t` hook MUST be NULL-checked before use;
  both modules must degrade gracefully if called before `command_init()`
- No component may declare `main` as a requirement; that is a layering inversion
- Bridge trampolines (`shell_bridge_*`) are FORBIDDEN. Put the implementation in the owning
  module instead of forwarding across a layer boundary.
- `components/shell/` MUST NOT include `networking.h`, `bluetooth.h`, `usb.h`, or `c6ota.h`.
  External-module state is read through the `shell_command_ops_t` accessors (wifi_*,
  bluetooth_*, usb_*, c6ota_*). If a needed value is missing, add an accessor to the ops
  table rather than reaching into the driver.

### Shell Core Rules
- ALL transcript output MUST go through `shell_transcript_append_text()` / `shell_transcript_appendf()` from `components/shell/`
- ANSI-colored transcript output MUST go through `shell_transcript_append_ansi()` / `shell_transcript_appendf_ansi()` using `@`-prefixed format specifiers
- ANSI color palette is defined in `p4minishell_config.h` via `P4_CONFIG_ANSI_*` macros; never hardcode ANSI color values
- Command output MUST use the semantic palette in `components/ansi/ansi_palette.h`. Never pick
  a raw `@`-specifier at a call site: use `SH_HEAD`, `SH_LBL`, `SH_OK`, `SH_ERR`, `SH_WARN`,
  `SH_MUTE`, `SH_VAL`, `SH_NUM`, `SH_PATH`, and the rest.
- For the common shapes prefer the helpers over hand-built format strings:
  `shell_print_heading()`, `shell_print_field()`, `shell_print_field_num()`,
  `shell_print_ok()`, `shell_print_error()`, `shell_print_warning()`,
  `shell_print_muted()`, `shell_print_usage()`. Each emits its own reset and newline.
- `@k` is pure BLACK and is nearly invisible on the default background. Muted text MUST use
  `SH_MUTE` (`@K`, bright black). This was a real readability bug fixed in v0.22.0.
- Other specifier traps: `@B` is bold not blue, `@E` is bright red, `@L` is bright blue
- Apply colour AFTER any width or padding calculation. Escape bytes count toward `strlen()`,
  so colouring before padding silently breaks column alignment in listings and reports.
- Machine-readable output (`dir /b`, redirected pipeline stages) MUST stay uncoloured
- Adding a new colour category means adding a macro to `ansi_palette.h`, never inlining a
  specifier. The palette is compile-time only: no runtime theming or user configuration.
- Plain transcript appends mirror to UART; ANSI appends must NOT mirror the stripped copy or
  every colored line prints twice on the serial console
- The on-screen transcript is an LVGL span group (`lv_spangroup`), not a textarea. Coloured
  output reaches it through `windows_set_transcript_text()` (in `components/windows/`), which
  parses the raw ANSI text into per-colour spans. That rebuild deletes/recreates spans, so it
  is DEFERRED to an `lv_async_call` to avoid a use-after-free when the rebuild is triggered from
  an LVGL event or during a redraw pass. Never call LVGL textarea APIs on the transcript, and
  never rebuild the span group synchronously from an LVGL event context.
- ALL debug logging MUST use `shell_record_errorf()` / `shell_record_warningf()` / `shell_record_infof()`
- Command history MUST use `shell_store_command_history()` / `shell_recall_history()`
- UART console MUST use `shell_uart_console_start()` / `shell_uart_console_write_text()`
- Input line text MUST go through `shell_input_line_set_text()` / `shell_input_line_reset()` /
  `shell_extract_input_text()`; never manipulate the prompt prefix directly
- System info commands (help, sysinfo, version, about, mem, debug) live in `components/shell/`
- Interactive keypress waits MUST use `shell_key_wait_begin()` / `shell_wait_for_key()` /
  `shell_key_wait_end()`. Every `begin` needs a matching `end` on every return path.
- Every keypress wait MUST be bounded. Pass `P4_CONFIG_KEY_WAIT_TIMEOUT_MS` unless the command
  defines its own timeout, and check `shell_key_input_available()` first so a headless board
  falls back to a timed path instead of stalling.
- Input sources (UART reader, USB HID bridge, LVGL input line) MUST check
  `shell_key_wait_is_active()` and route to `shell_key_wait_submit()` instead of the command
  line while a wait is running
- The prompt is a runtime template owned by `components/shell/`. Never hardcode the prompt
  string in a new surface; call `shell_prompt_render_plain()` (plain) or let
  `shell_uart_console_print_prompt()` handle the colored form.
- `shell_input_line_set_text()` snapshots the prefix it paints. Extraction and repair compare
  against that snapshot, never against a freshly rendered prompt.
- The async transcript buffer MUST be drained into a local copy before appending; never hold
  `s_async_transcript_lock` across an LVGL call
- Overlapping array slots MUST use `memmove()`, never `snprintf()` (triggers `-Werror=restrict`)
- `shell_transcript_append_internal()`, `shell_transcript_reset()`,
  `shell_history_transcript_scroll_to_end()`, and `shell_input_line_set_text()` all call LVGL
  functions internally. These functions MUST acquire `lvgl_port_lock(0)` around their LVGL
  sections when called from non-LVGL tasks. The mutex is recursive, so the LVGL event path,
  async transcript flush callback, and UART-console command path — which already hold the
  lock — nest without deadlock. The lock is only taken when the LVGL port is initialized
  (guarded by transcript/input-line non-NULL checks).

### Command Rules
- `main.c` must contain NO command implementations
- There is exactly ONE dispatcher: `shell_execute_command_core()` in `components/command/command.c`.
  Never add a second dispatch table.
- Command implementations live with the module that owns their domain:
  - Filesystem verbs (`cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`,
    `write`, `append`, `touch`, `attrib`, `label`, `xcopy`, `find`, `more`, `tree`, `fc`,
    `sort`, `sd`) -> `components/storage/storage_commands.c`
  - Batch language verbs (`set`, `path`, `echo`, `call`, `if`, `goto`, `shift`, `pause`,
    `choice`, `setlocal`, `endlocal`, `exit`) -> `components/batch/batch.c`
  - System info verbs (`help`, `sysinfo`, `version`, `about`, `mem`, `debug`) -> `components/shell/shell.c`
  - Hardware, UI-query, and remaining system verbs -> `components/command/command.c`
- ALL command dispatch MUST go through `shell_execute_command()` (full pipeline) or
  `shell_execute_command_core()` (dispatch only) from `components/command/`
- Variable expansion (`%VAR%`, `%0`, `%1`..`%9`, `%*`) is implemented in `components/batch/` and
  called by `shell_execute_command()`; redirection (`>`, `>>`, `<`) is parsed in command.c, the
  output side is written by `components/storage/`, and the input side is published to the storage
  input-redirection slot
- Pipe stages and the `<` operator MUST publish through `storage_set_input_redirect()`. Never add
  a per-command input argument; the text-processing commands resolve their source through
  `storage_resolve_input_source()` so all three spellings share one code path.
- ANY code that looks for an operator character in a command line MUST use
  `shell_find_unquoted_char()` / `shell_find_unquoted_any()` / `shell_has_unquoted_char()`.
  Never hand-roll a quote check: five surfaces depend on agreeing about what is syntax, and a
  local scan will miss single quotes and caret escapes.
- Quoting rules are `"text"` (groups, expands), `'text'` (groups, literal), `^c` (escapes one
  character). Strip markup with `shell_unescape_in_place()`, never by hand.
- Chain separators MUST be split before variable expansion, and expansion MUST run per segment.
  Expanding first would let a variable containing `&` inject a command. This ordering is a
  security property, not a style choice.
- A single `|` is the pipe operator and MUST NOT be treated as a chain separator, or pipelines
  stop reaching the pipe executor.
- Chain success MUST be tracked per link from the segment executor's return value, never
  re-read from global errorlevel, which may hold a stale value from an earlier line.
- The input-redirection slot belongs to exactly one command. Clear it after dispatch on every
  path, including failure.
- Nested execution contexts (`if`, `for`, pipes, batch lines) MUST re-enter the full pipeline via
  `batch_command_ops_t.execute_command` so they inherit expansion and redirection
- The batch executor and the label scanner MUST agree on where a logical line ends. Both apply
  the same odd-trailing-caret continuation rule; changing one without the other lets a
  continued line register a phantom `:label` and silently corrupt `goto` targets.
- `set /a` operator parsing MUST NOT consume `&&` or `||`. A lone `&` is bitwise and, a lone
  `|` is bitwise or, but the doubled forms belong to command chaining.
- Command execution from the LVGL path MUST use `shell_execute_command_async()` to protect the LVGL stack
- Module-routed commands (wifi, bluetooth, usb, c6ota, sd) receive the original unsplit command text
- State ownership: cwd and SD mount tracking belong to `components/storage/` (reset by
  `storage_init()`); environment, PATH, batch frame, and errorlevel belong to `components/batch/`
  (reset by `batch_init()`). `command_init()` calls both before registering the ops tables.

### Keyboard Manager Rules
- ALL keyboard operations MUST go through `components/keyboard/` — never call LVGL keyboard APIs directly from main.c
- `keyboard_init()` MUST be called after `display_init()` and from the LVGL task context
- Keyboard visibility changes trigger automatic UI reflow via the window manager callback
- When keyboard is hidden, the transcript area expands to fill the freed space
- Keyboard height is configurable via `P4_CONFIG_KEYBOARD_*` macros
- Keyboard modes (text_lower, text_upper, number, symbols) are managed by the keyboard component

### Window Manager Rules
- ALL LVGL screen layout MUST go through `components/windows/` — never create screen-level widgets directly in main.c
- `windows_init()` MUST be called after `display_init()` and from the LVGL task context (inside `bsp_display_lock`)
- `windows_deinit()` MUST be called before rebuilding the UI after rotation changes
- Window objects MUST be accessed via `windows_get_*()` accessors, never stored as static variables in main.c
- All window region dimensions MUST use config macros (`P4_CONFIG_WINDOW_*`), never hardcoded
- Colors MUST be accessed through `windows_get_color()` with semantic names, never raw hex values
- The window manager delegates header rendering to `components/header/` via `header_init()`/`header_deinit()`
- The window manager queries display resolution from `components/display/` via `display_get_width()`/`display_get_height()`

### Display and Touch
- Display init MUST use `display_init()` (which wraps `bsp_display_start_with_config()` with BOARD_CFG_* values)
- ALL display operations MUST go through `components/display/` public API — never call BSP display functions directly from main.c
- Touch init MUST use the managed GT911 driver through BSP (handled internally by display manager)
- Never replace the BSP display/touch path with raw driver calls
- After `display_set_rotation()`, the display manager schedules a UI rebuild via the registered callback
- `shell_build_ui()` MUST call `header_deinit()` before `lv_obj_clean()` for clean header re-init
- Touch rotation mapping is handled automatically by the display manager on rotation change
- Keyboard height scales dynamically (~35% of vertical resolution, clamped 180-280px)
- Input row height scales dynamically (~8% of vertical resolution, clamped 40-56px)
- Header height is rotation-aware (uses correct resolution axis for 90/270)
- Display state variables (rotation, brightness, power) are owned by the display manager and accessed through its public API
- `display_register_ui_rebuild_callback()` MUST be called after `display_init()` to register the shell's UI rebuild function
- The display manager's `display_info_t` struct is the canonical source for sysinfo display diagnostics

### Wi-Fi Rules
- ALL ESP-Hosted, esp_wifi_remote, esp_netif, and NimBLE calls MUST live inside
  `components/networking/`. The only sanctioned exception is `components/c6ota/`, which
  drives `esp_hosted_slave_ota_*` because co-processor update is its purpose.
- When another layer needs networking state, add a status accessor to `networking.h`.
  Never call `esp_wifi_*` from `shell/`, `command/`, `storage/`, `batch/`, or `main/`.
- Only the official path is permitted: `espressif/esp_hosted` + `espressif/esp_wifi_remote`.
  Never introduce custom RPC, an alternative transport, or a re-implemented control plane.
- The initialization order in `networking_wifi_start_runtime()` is load-bearing and MUST
  be preserved: hosted init -> connect to slave -> version gate -> NVS -> netif ->
  event loop -> default STA netif -> esp_wifi_init -> handlers -> STA mode -> start.
  Transport before NVS matters: a dead C6 must report as a transport fault, not as a
  confusing Wi-Fi init error later.
- Station-only is enforced twice: the code always calls `esp_wifi_set_mode(WIFI_MODE_STA)`,
  AND both `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` and `CONFIG_WIFI_RMT_SOFTAP_SUPPORT` are
  disabled. Both symbols are required — esp_wifi_remote mirrors the Wi-Fi Kconfig under
  its own `WIFI_RMT_` prefix, and on the hosted path that mirror is authoritative.
- Wi-Fi startup MUST follow sdkconfig only
- Auto-start in background task on normal boot
- Restore after successful c6ota
- ESP-Hosted + esp_wifi_remote for C6 SDIO path
- Version compatibility gate: refuse init if C6 firmware major/minor != host 2.12.x
- NVS initialized before esp_wifi_init() with erase-and-retry recovery
- Station-only profile; no SoftAP, WPA3, or enterprise
- Password masking in transcript and command history
- ALL shared Wi-Fi state (s_wifi_state, s_wifi_connected, s_wifi_target_ssid, etc.) MUST be accessed through wifi_lock()/wifi_unlock() mutex
- Wi-Fi init task claiming MUST use wifi_try_claim_init_task()/wifi_release_init_task() (TOCTOU-safe atomic)
- Persistent watchdog (networking_wifi_watchdog_task) retries disconnected Wi-Fi with exponential backoff (1s→30s cap, 120s total timeout)
- Watchdog starts automatically on WIFI_EVENT_STA_DISCONNECTED; stops on successful connection or timeout

### Bluetooth Rules
- Hosted NimBLE on C6 over ESP-Hosted VHCI (not Bluedroid)
- Stateful lifecycle: enable once, reuse for scan/advertise
- bt is alias for bluetooth
- Supported: status, scan, advertise on|off

### SD Card Rules
- All SD access goes through `components/storage/`. No other module may call `bsp_sdcard_mount()`
  or `bsp_sdcard_unmount()` directly.
- All SD commands use the shared guarded session: `shell_sd_begin()` / `shell_sd_end()`. Every
  `shell_sd_begin()` MUST have a matching `shell_sd_end()` on every return path.
- The mount is persistent; only `sd eject` / `sdeject` unmounts
- Any command that WRITES a file MUST precheck capacity with `storage_check_free_space()`
  before opening the destination, so a truncating overwrite cannot destroy the existing
  contents and then fail for lack of room
- Any command that copies MUST reject a self-copy via `storage_paths_are_same()`. Opening a
  destination with `"wb"` truncates it, so `copy a.txt a.txt` destroys the source.
- Any command that copies MUST carry attributes with `storage_copy_attributes()`, and MUST do
  so AFTER writing the data — a read-only destination cannot be opened for writing. Treat a
  failure as non-fatal: the data is already correct.
- A failed write MUST remove its partial destination. A truncated file that looks complete is
  worse than no file.
- Capacity queries go through `storage_get_space_info()`; never call `f_getfree()` directly
- `chkdsk` and any future integrity tool MUST stay read-only. This firmware does not rewrite
  FAT structures: report a problem, never attempt an in-place repair.
- Destructive volume operations (`format`) MUST require the exact confirmation word through
  the shell key queue AND refuse to run when `shell_key_input_available()` is false, so they
  can never execute unattended from a batch file
- FATFS LFN enabled with heap-backed buffers, MAX_LFN=255, UTF-8 encoding
- Bounded output: 128 entries max for listings, 8192 bytes max for sd cat
- SD VO4 LDO explicitly acquired at 3300 mV before mounts
- Path resolution handles sd:/, /sdcard/, and relative paths

### USB Rules
- USB Host Library with MSC and HID class drivers
- MSC mounts at /usb0 via VFS/FATFS
- HID echo is opt-in (keyboard/mouse on/off)
- Follows same bounded transcript style as SD commands
- USB keyboard auto-detect: automatically hides on-screen keyboard when USB keyboard attached
- USB keystrokes injected into shell CLI input line via shell_usb_keyboard_input() bridge
- Full US keyboard layout supported (60+ HID key codes with modifier-aware mapping)
- Auto-detect runs in periodic header refresh timer; state transitions trigger notifications
- keyboard_set_external_input() / keyboard_clear_force_visible() for manual control

### OTA Rules (c6ota)
- Lives in components/c6ota with stable public API
- Exact confirmation: WARNING: This will reboot the C6. Type YES to continue
- Validates ESP-IDF app magic 0xE9 + ESP32-C6 chip ID 0x000D
- 1500-byte chunks, progress every 5%
- Wi-Fi stopped before transfer, kept alive during (no esp_hosted_deinit())
- Factory v2.3.0 needs one-time standalone tool first

### Stack Discipline
- The command worker task stack is `P4_CONFIG_COMMAND_TASK_STACK` (8192 bytes) and is shared by
  the whole dispatch path
- `shell_execute_batch_file()`, `shell_execute_command()`, and `shell_execute_command_core()`
  form a RECURSIVE cycle: a batch file re-enters the pipeline for every line it runs. Their
  stack frames are multiplied by `P4_CONFIG_BATCH_DEPTH_MAX`.
- NEVER add a line-sized or larger buffer as a local variable in any function on that cycle.
  Allocate it on the heap and free it on every exit path. A 12 KB batch frame on the 8 KB stack
  was a real crash fixed in v0.19.0.
- When changing anything on that path, verify the frame size from the disassembly
  (`riscv32-esp-elf-objdump -d <obj>`, read the `addi sp,sp,-N` prologue), not by inspection
- Structures stored per batch frame (label table, argument copies) must be sized deliberately;
  a field sized at the full command width multiplies by the slot count
- The recursive directory walkers (`shell_dir_list_one`, `shell_tree_walk`,
  `shell_chkdsk_walk`) are subject to the same rule, multiplied by
  `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`. Keep their per-level state in ONE heap block and free it
  before descending, so only one level's buffer exists at a time.
- Never declare a `FF_DIR`, `FILINFO`, or path-sized array as a local in a recursive walker;
  those three alone are over 700 bytes

### Command Execution
- Heavy commands run on dedicated worker task (not LVGL input callback stack)
- Preserve original unsplit command text for family handlers (wifi, sd, c6ota)
- LV_EVENT_READY on input line is the confirmed submission path
- Serial console reuses same shell path (stdin to submit, stdout from transcript)

### Shell State
- RAM-only: current working directory, environment variables, PATH, batch args
- Current working directory owned by `components/storage/storage.c`; exposed read-only via
  `shell_get_cwd()` and mutated only through `storage_set_cwd()`
- Environment variables, PATH, batch frame stack, and errorlevel owned by
  `components/batch/batch.c`
- No persistence across boots
- Environment variables: max 24, names alphanumeric + underscore
- Batch depth: max 4 nested calls
- Batch labels: max 32 per file (`P4_CONFIG_BATCH_LABEL_MAX`)
- `set /a` parenthesis nesting: max 16 (`P4_CONFIG_SET_EXPR_DEPTH_MAX`)
- `set /p` input: max 128 bytes (`P4_CONFIG_SET_PROMPT_INPUT_BYTES`)
- Line continuations: max 8 joined lines (`P4_CONFIG_LINE_CONTINUATION_MAX`)
- setlocal scopes: max 8 nested (`P4_CONFIG_SETLOCAL_DEPTH_MAX`). Every scope a batch frame
  leaves open MUST be unwound when that frame returns, or the snapshot allocation leaks and the
  caller's environment is corrupted.
- Chained commands: max 8 (`P4_CONFIG_CHAIN_SEGMENT_MAX`); truncation MUST be reported, never
  silent
- Batch label names: max 48 bytes (`P4_CONFIG_BATCH_LABEL_BYTES`). This is deliberately small
  because the label table is `LABEL_MAX * LABEL_BYTES` and lives in the batch frame.
- Pipeline stages: max 4 (`P4_CONFIG_PIPE_STAGE_MAX`). Every spool file MUST be removed on
  every exit path, including a stage failure.
- Prompt template: max 64 bytes (`P4_CONFIG_PROMPT_TEMPLATE_BYTES`)

### Kconfig Rules
- `Kconfig.projbuild` MUST live with the component that consumes its options, not in `main/`.
  This keeps the symbols available to any project that includes the component, including `test/`.
- `CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID` / `_PASSWORD` are defined in `components/networking/`

### Unit Test Rules
- `test/` is a standalone ESP-IDF project and MUST build with zero errors and zero warnings
- `test/CMakeLists.txt` pins `IDF_TARGET` to `esp32p4` and stages `board_config.h` and
  `p4minishell_config.h` into the generated config directory
- `test/main/idf_component.yml` MUST pin the same managed component versions as `main/idf_component.yml`
- `test/sdkconfig.defaults` MUST mirror the firmware's build-affecting options (FATFS LFN,
  ESP-Hosted, NimBLE, USB host) or the shared components will not compile
- Adding a component dependency to `shell`, `storage`, `batch`, or `command` requires adding its
  directory to `EXTRA_COMPONENT_DIRS` in `test/CMakeLists.txt`
- A new component under `components/` must be added to BOTH the root `CMakeLists.txt`
  `EXTRA_COMPONENT_DIRS` and `test/CMakeLists.txt`

### Header Bar
- Passive, display-only module in components/header
- Non-scrollable, resolution-scaled height
- Status icons: Wi-Fi, battery, Bluetooth, USB, SD
- SD icon hidden when no card mounted
- Notification area transient (blank when idle)

### Build Constraints
- Every build constraint MUST be pinned in `sdkconfig.defaults`, not only in the
  generated `sdkconfig`. A setting present only in the generated file is silently
  reverted whenever the config is regenerated. Three constraints were lost this way
  before v0.23.0.
- After changing `sdkconfig.defaults`, delete `sdkconfig` and rebuild to confirm the
  intended values actually survive regeneration
- LVGL examples MUST stay disabled (image budget)
- Station-only Wi-Fi profile: `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n` AND
  `CONFIG_WIFI_RMT_SOFTAP_SUPPORT=n`
- Newlib nano formatting
- Warn-level compile-time logging (`CONFIG_LOG_DEFAULT_LEVEL_WARN=y`)
- Size optimization (`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`); `OPTIMIZATION_PERF` grows
  the image by roughly 280 KB
- PSRAM XIP instruction/rodata mapping MUST stay disabled
  (`CONFIG_SPIRAM_XIP_FROM_PSRAM=n`) or the flash/PSRAM budget overflows at link time
- ESP-Hosted reset: SLAVE_RESET_ON_EVERY_HOST_BOOTUP

### Documentation Updates
After every task, update: changelog.md, readme.md, documentation.md, ai-context.md, board_config.yaml, command.md
For roadmap work, also update: roadmap.md, API.md, SDK.md
When bumping the version, update all three: `p4minishell_config.h` version macros,
`p4minishell_config.yaml` `config_version`, and the `readme.md` version badge

### Hardware Gaps (Do NOT implement)
- RGB LED: no authoritative wiring in JC1060 reference
- Camera: no local camera stack in workspace
- Both should fail explicitly with honest messages

### Error Handling
- Friendly transcript messages for all failures
- 5-entry debug history buffer via debug command
- Healthy boot recorded in debug history, not as warning
- ESP-IDF error names in messages: esp_err_to_name()

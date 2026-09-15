# P4MiniShell AI Context Rules

## Project Identity
- **Name**: P4MiniShell
- **Type**: Embedded DOS-style command shell
- **Target**: ESP32-P4 (host) + ESP32-C6 (co-processor over ESP-Hosted SDIO)
- **Framework**: ESP-IDF v5.5.5
- **UI**: LVGL 9.5.0 (esp_lvgl_port 2.9.0) with JD9165 1024x600 display + GT911 touch
- **Version**: v0.38.2 (`p4minishell_config.h:54-59`). **0.38.2:** fixed the recurrent "BSOD" full-screen blue flash (a MIPI-DSI bridge underrun caused by the header telemetry calling `uxTaskGetSystemState()` on the LVGL task; it now samples on a pinned `sheltlm` background task with an idle-counter CPU estimate) and the boot first-mount regression (`security_init()` mounted the SD during `command_init()` before the first-mount hook, so the welcome/defaults/font/CJK/history work was skipped; the CONFIG.SYS reads moved to `security_load_saved()` and `networking_init()` now runs after the SD mounts so the C6 hosted transport does not starve on the shared SDMMC/DMA buffers). Added `tools/display_glitch_watch.py` and `display stress on|off`. **0.38.1:** verified the `wifi throughput` bench (host↔device, same subnet) and raised the lwIP TCP window (`CONFIG_LWIP_TCP_WND_DEFAULT`/`SND_BUF_DEFAULT` 5760→32768, `RECVMBOX_SIZE` 6→32) — the window, not the SDIO clock, had capped a single stream (~2.4 Mbit/s); host→device rose 2.45→~11 Mbit/s and 40 MHz measured faster than 10 MHz in both directions. **0.38.0:** adopted the 40 MHz SDIO clock after a soak gate, added the `wifi throughput` bench (+ `tools/wifi_bench.py`) and `tools/regression.py`. **0.37.1:** fixed O3 (driver-API UART mirror), O4 (no-reset `open_port()`), O5 (I2C bus recovery). **0.37.0:** moved the app-owned flash-safe task stacks (USB, c6ota, audio, alarm, led, shell UART) to PSRAM, which lifted the tightest boot-time internal DMA headroom from ~1 KB to ~10 KB free / 8.7 KB largest block (see `bugs.md` O8); the boot script now runs on a dedicated short-lived `bootscript` task instead of the Wi-Fi event task. **0.36.1:** fixed an LVGL timer-list heap-corruption panic (a `header_schedule()` async payload double free; see changelog `[0.36.1]`), boot SD scripting now runs on the SD first mount so CONFIG.SYS/AUTOEXEC are no longer skipped, and USB host bring-up retries. Including: `components/gfx/` (RGB565 raster + BMP parser/decoder, plus the B2 toolkit: `hline`/`vline`/`triangle`/`ellipse`/`polygon`/`flood_fill`/`text` and the committed 8x8 ASCII font `gfx_font.c`), `gfx`/`crc32`/`asset` verbs, packaged SD apps (`pkg list|info|verify|check|install|remove` over `APPS/<APP>.APPINFO` + `APPS/<APP>.ASSETS`, installed from CRC-checked `PKGS/<APP>/` bundles), `start`/`taskkill` background jobs, `draw table`/`draw list` with cursor/selection, UI themes (B3: `theme list|show|set [/save]` over `default`/`amber`/`ice`/`mono` with live re-apply + `SHELL.INI` persistence), dead-code cleanup (B4: deleted the `storage_commands.c` stub and the `p4_usb` CMake twin, removed dead statics, de-duplicated `help /all`), plot/graph layer (`plot` world-coordinate verbs over the `gfx` canvas or TUI via `gfx_view.c`, sampling `calc`), RAM-loaded batch execution (`P4_CONFIG_BATCH_FILE_MAX_BYTES` 131072), O(1) transcript appends + `windows_set_transcript_text_len()`, off-console mirror suppression, and the TCMD/SNAKE/ELITE/BOUNCE/GFXTOOL/PLOT reference apps. Verified baseline: unit 281/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21, pkg 15/15, gfx toolkit 17/17, theme 11/11, plot 25/25, header OK (COM3).

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

### Hard Rules
- **Always fix any issue or bug that is found, even if it is unrelated to the
  current task.** A known failure is never acceptable to leave behind: never
  ignore or suppress it, never mark it "out of scope" and move on. Fix it in
  the same change, or, if it genuinely cannot be fixed now, report it
  explicitly and land a clearly-scoped follow-up — but the default is to fix it
  immediately. This includes failing unit tests, build warnings, crashes,
  memory/documentation drift, and latent use-after-free/dangling-pointer bugs
  spotted while working nearby.
- Every change is finished only when: firmware **and** `test/` build with zero
  errors and zero warnings, the on-board unit suite is green
  (`tools/unit_run.py COM3`), and the affected hardware behaviour is verified
  on the board. Reflash the main firmware after any test-app run.

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
- SD indicator shows persistent state (NO/INS/ON/ERR)
- Status classification (glyph, tone, Wi-Fi label, and the memory/CPU/battery
  healthy/warn/critical thresholds) MUST live ONLY in `header_status.c` (pure,
  unit-tested). Never re-derive a threshold, re-classify a state, or pick a raw
  color in `header.c` or any command file; `header.c` maps the tone to
  `theme.text`/`warn`/`err`/`text_muted`. `P4_CONFIG_HEADER_STATUS_STYLE` selects
  words vs the compact colored glyphs; both styles share the same classification.
- Tap/long-press detail: the header owns the gestures and self-summarizes on tap
  from its cached state; long-press calls the shell handler registered through
  `header_register_status_action()` (registered by `command_init()`). The header
  stays a command-free leaf — never include command/batch headers in it.
- Notifications MUST go through the pure `header_notify_queue.c` FIFO (single
  source of order/overflow/clear). `header_notify()` takes a severity
  (info/warn/error) that colors the message; `header_set_notification()` is the
  INFO wrapper. Never add a second notification slot or timer: the header owns
  ONE persistent, paused display timer (`repeat_count=-1`, never auto-freed, so
  the O7 freed-timer hazard cannot recur) that `header_init()` creates and
  `header_deinit()` deletes. Blank text flushes the queue (`notify -`).
- The idle center shows the local clock from `time_format_hm()`; a displayed
  notification always takes precedence. Do not add a second clock widget.
- The `A` activity indicator is conditional (hidden when nothing runs) and fed
  by `c6ota_busy`/`bg_jobs_running` in `header_batch_t`; hidden items MUST
  contribute zero width in `header_measure_sides()` (mirror LVGL flex, which
  skips hidden children).
- The header poll cadence MUST come from the pure `header_refresh.c` policy
  (`header_refresh_interval_ms()`), assembled by the shell and applied by main's
  single timer. Expensive telemetry (heap/CPU task snapshot/battery) MUST stay
  throttled to `P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS` AND MUST run off the LVGL
  task (the pinned `sheltlm` sampler) — never call `uxTaskGetSystemState()` on
  the LVGL/render task at any rate. That full task-list walk suspends scheduling
  long enough to delay the MIPI-DSI bridge DMA refill ISR, which underruns the
  panel and paints the whole screen blue (the "BSOD"). `shell_sample_cpu_percent()`
  uses `ulTaskGetIdleRunTimeCounter()` + `esp_timer` deltas instead; only the
  on-demand `ps`/`top` commands use the full snapshot. `uxTaskGetSystemState()`
  is also unsafe during an OTA (PSRAM unavailable). The idle interval MUST be
  bounded by the idle-display-off deadline (`shell_power_ms_until_idle_off()`)
  and may not skip the clock minute.
- Battery is ALWAYS visible — shows "BAT N/C" with muted styling when ADC is not connected
- System panel (MEM | CPU | BAT) is on the far right, all dynamically linked to FreeRTOS runtime stats
- header_update_battery(int percent, bool adc_ready) — pass adc_ready=false for N/C display
- header_update_mem(uint32_t free_heap, uint32_t total_heap) — real-time from heap_caps
- header_update_cpu(int percent, uint32_t task_count) — real-time from FreeRTOS runtime stats
- header_update_uptime(uint32_t seconds) — system uptime from esp_timer_get_time()
- Touch init failure must not prevent header rendering (BSP touch is optional)
- Header layout is responsive and measurement-driven (`header_layout.c` pure policy + `header.c` widgets): never hardcode panel widths — measure live label widths, run the policy, set explicit absolute panel geometry; the notification label must be pinned to its container width; prefer `header_get_height()` over local height math
- `header_render()` must release the LVGL port lock on EVERY exit path

### Image Support Rules
- There is exactly ONE BMP implementation: `gfx_bmp_parse_header_ex` /
  `gfx_bmp_decode_scaled_565` / `gfx_bmp_fit` / `gfx_surface_blit_scaled` in
  `components/gfx/`. Never add a second BMP parser, decoder, or scaler, and
  never re-derive aspect math at a call site. The strict sprite wrapper
  `gfx_bmp_parse_header` (24-bit bottom-up, `GFX_SPR_MAX`) MUST keep its exact
  behavior — `gfx load` and `test_gfx.c` depend on it.
- Supported ingest: 24-bit and 32-bit BI_RGB, bottom-up or top-down. Reject
  RLE, other bit depths, and dimensions beyond `GFX_IMAGE_MAX_W/H` or
  `P4_CONFIG_IMAGE_MAX_BYTES`. Decode straight to the target size; never
  materialize a large native surface.
- Routing is by the `components/filetype/` registry (`.bmp`/`.dib` =
  `FILETYPE_IMAGE`): `view`/`open` route images to the shared `imageview`
  modal surface; do not hand-roll extension checks.
- The viewer is a `modal_surface_t` on the shared runtime (`imageview` in
  `modal_surf.c`), not a private loop. The TUI path is `tui_draw_image`
  (single cell-map); the canvas path is `gfx image`. All three call the same
  gfx decoder.
- New image surfaces/verbs are additive; never remove `gfx load`/`blit`/`save`
  or the screenshot BMP path.

### Icon / Boot Splash Rules
- The author's master icon is `icon/icon.png` (16384x16384, ~70 MB) and is
  **git-ignored**; never commit it. Two committed derivatives are generated by
  `tools/make_icon.py` (re-run after replacing the master):
  `icon/icon-512.png` (referenced by `readme.md`) and
  `main/assets/icon_splash.c` + `.h` (a 256x256 RGB565 `lv_image_dsc_t`).
  Generated assets are committed (like `components/gfx/gfx_font.c`); do not
  hand-edit them.
- The boot splash is an opaque overlay (no alpha format) flattened onto the
  default theme screen colour by the generator. It is shown once by
  `main.c`'s `shell_splash_maybe_show()` from `shell_build_ui()`, dismissed by
  a one-shot timer (`P4_CONFIG_SPLASH_MS`) or a tap, and survives UI rebuilds.
- The overlay MUST live on `lv_screen_active()` (as its topmost child), not
  `lv_layer_top()`: the streaming `screenshot` snapshots the active screen only,
  so a top-layer splash would be invisible to it. `windows_deinit()` calls
  `lv_obj_clean(screen)` on every rebuild, so the splash code clears its
  overlay pointer there and re-creates the overlay while the window is open
  (never leave a dangling overlay pointer for the timer).
- Splash tunables live in `p4minishell_config.h` (`P4_CONFIG_SPLASH_ENABLE`,
  `P4_CONFIG_SPLASH_MS`) and `p4minishell_config.yaml`.

### Module Layering Rules
- Component dependencies flow ONE WAY: `main` -> `command` -> `batch` -> `storage` -> `shell` -> (`ansi`, `display`, `windows`, `header`, `keyboard`, `clock`). `components/modal/` is a shared runtime used by `editor` and by the batch `dialog`/`list`/`ask` commands; it is reached through `command`/`batch`, never from `shell`.
- Leaf components below the shell: `components/tui/` (80x25 cell buffer + draw primitives, reached by `draw`/`tui` in `components/command/tui_commands.c` and by `windows`), `components/gfx/` (pure RGB565 raster + general 24/32-bit BMP parser/decoder + scaled decode + nearest scaling + aspect-fit + blit + B2 toolkit primitives + 8x8 font `gfx_font.c`, no LVGL; reached by `components/command/gfx_commands.c`, `image_commands.c`, `components/tui/`, and `components/modal/`), `components/filetype/` (extension→kind registry), `components/markdown/`, `components/font/` (registry + SD TTF + CJK + theme registry `theme.c`), `components/db/`, `components/alarm/`, `components/audio/`, `components/boot/`, `components/clock/`.
- `components/applib/` is the native-app runtime library (the shell SDK
  surface). It is a leaf: REQUIRES only `shell`, `clock`, `storage`, `db`, `tui` (for the
  shared INI / temp-file state mechanics, the same guarded-SD pattern the
  `edit` app uses), and the FreeRTOS/heap/esp_timer IDF components. It MUST
  NOT include `networking.h`, `command.h`, or `batch.h`. Its public API is
  split into lean headers (`applib.h` is an umbrella over `applib_console.h`,
  `applib_mem.h`, `applib_time.h`, `applib_net.h`, `applib_input.h`,
  `applib_state.h`, `applib_ui.h`, `applib_app.h`, `applib_env.h`, `applib_db.h`, `applib_tui.h`) so an app
  includes only the groups it uses; declare each
  function in exactly one header. Wi-Fi state is read through the registered
  `applib_net_ops_t` table, which `command_init()` populates with
  `networking_*` wrappers; every hook is NULL-checked so the helpers degrade
  gracefully before registration. The environment table and cwd are read
  through the registered `applib_env_ops_t` table (`command_init()` registers
  `shell_env_get`/`shell_env_set`/`shell_get_cwd`) — applib never includes
  `batch.h` and never reaches into storage internals. The native-app ABI
  (`applib_app.h`) is the entry/registration contract: an `app_main_t` app is
  `app_register`ed, the shell dispatcher runs it via `app_dispatch` (after
  built-ins and `.bat` lookup), and its return value becomes ERRORLEVEL. New
  apps MUST go through `app_register`, never a private dispatcher branch.
- `.bat` app discovery (`launch`, `components/command/command.c`) scans the
  command PATH + `sd:/APPS` for `*.bat` and reads optional APPINFO metadata
  from `sd:/APPS/<name>.APPINFO`. The discovery table MUST be heap-allocated —
  a stack-resident table overflows the command-worker stack once the launched
  batch re-enters the dispatcher per line (a real crash found in bring-up). A
  batch app is made discoverable by placing it in a PATH dir or `sd:/APPS` and
  (optionally) adding an APPINFO file; `launch` never shadows a built-in or a
  PATH-resolved `.bat`.
  Input helpers (`app_wait_key`,
  `app_read_line`) wrap the shell key queue with a caller timeout and MUST
  return false on a headless board rather than stalling. The persistent-state
  helpers (`app_ini_*`, `app_temp_*`) wrap the storage core and keep ALL
  storage on the SD card. UI helpers (`app_mode_enter`, `app_mode_exit`,
  `app_notify`) live in `applib_ui.h` and route through the shell-core
  primitives. New runtime services for native apps belong in the matching
  `applib_*.h` (or an ops table when the owner lives higher in the stack). A new component under `components/` must be added to BOTH the root
  `CMakeLists.txt` `EXTRA_COMPONENT_DIRS` and `test/CMakeLists.txt`.
- `components/db/` is the Palm-OS-style SD record store (`db.{h,c}`). It is a
  LEAF below `storage`: it REQUIRES only `storage`, `esp_timer`, `freertos`
  (plus the board header for `BSP_SD_MOUNT_POINT`), never includes
  `batch.h`/`command.h`, and never prints or parses commands — the `db`
  command lives in `components/command/db_commands.c`. ALL database data lives
  on the SD card (`sd:/DBS/<name>.DB/`: HEADER.INI, CATEGORIES.INI,
  INDEX.TXT, RECORDS/R<id>.DAT). Every operation MUST: open its own guarded
  SD session; pre-check free space (reclaiming the old file's size on
  overwrite); write atomically (temp + rename, removing the partial on
  failure); and heap-allocate every record/command-sized buffer — never a
  large stack local (the batch path re-enters the dispatcher per line). Read
  payload sizes from `stat`, NOT `fseek(SEEK_END)+ftell` (the FATFS VFS does
  not report a correct size that way). Index lines use a `-` sentinel for an
  empty key so the parser is unambiguous. The `db` command prints `/b` output
  without ANSI colour and sets ERRORLEVEL 0/1/2 so `for /f` and `if errorlevel`
  work. The applib surface (`applib_db.h`) wraps the core with NULL-checked
  `app_db_*` bool wrappers.
- `components/alarm/` is the SD-persisted alarm/event store + one background
  checker task. It is a LEAF: REQUIRES only `shell`, `clock`, `storage`,
  `led`, `audio`, `freertos`, `esp_timer` (plus the board header). It NEVER
  includes `command.h`/`batch.h`; the `/run:` batch action is queued onto the
  command worker through `alarm_host_ops_t.execute_async` (registered by
  `command_init()`), and the `alarm`/`cal` commands live in
  `components/command/alarm_commands.c`. ALL event data lives on the SD card
  (`sd:/ALARMS/`), every write is atomic behind a free-space pre-check, and
  the checker posts ONLY to the existing surfaces (`shell_header_notify`,
  `led_notify`, `audio_play_tone`, the queued `call`). There is no private
  notification loop. The checker task stack MUST stay generous
  (P4_CONFIG_ALARM_TASK_STACK 8192): newlib's `snprintf` frame used to render
  event files/notifications is ~1.5 KB and combined with the tick's
  event/path locals a smaller stack overflows (a real crash found in
  bring-up). Event-file names are `E<6 digits>.INI` (11 chars — the directory
  scan checks this exactly). `alarm del all` wipes the store and resets the
  id space; individual deletes never reuse ids.
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
- The transcript span group's per-span overhead lives in the internal DMA-capable heap. If the
  internal heap runs low (`P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES`), the shell auto-trims the
  oldest scrollback — keeping the newest three quarters (half only under severe pressure) — and
  frees the spans, via `shell_transcript_guard_internal()` at
  every command start and before every append — this is REQUIRED to prevent stdio-lock OOM
  aborts on long sessions. Do not bypass the trim; if you add a large internal-heap consumer,
  keep it out of the internal heap (PSRAM) or the sweep will abort.
- The SDMMC SD-card host MUST carry a cached DMA buffer: `storage_sd_ensure_dma_buffer()` sets
  `bsp_sdcard->host.dma_aligned_buffer` at mount (from `P4_CONFIG_SD_DMA_BUFFER_BYTES`). Without
  it, per-transaction DMA allocation fails (`allocate_dma_buf: not enough mem`) once the internal
  heap fragments — every SD command goes down. Keep `shell_sd_begin()` calling it on every path.
- Boot-time internal RAM is the scarce resource, and PSRAM is NOT `MALLOC_CAP_DMA` on this P4 build
  (`dma_spi=0`), so `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA` requests fall back to internal RAM. New
  tasks whose work never runs with the flash cache disabled SHOULD use
  `xTaskCreate*WithCaps(..., MALLOC_CAP_SPIRAM)` (and `vTaskDeleteWithCaps` if they self-delete) to
  keep the boot peak low. Tasks that may touch host flash/NVS (the command worker, the Wi-Fi
  bring-up task) or are timing-critical (LVGL) MUST stay on internal stacks. See `bugs.md` O8.
- `header_schedule()` (components/header/header.c) does NOT free the async payload: the caller owns
  it and frees it if scheduling fails, and the async callback frees it on success. Freeing in the
  helper double-frees at every caller and corrupts the heap free list (the O7 LVGL timer-list
  panic). Keep the ownership rule one-sided when touching header async paths.
- The UART transcript mirror (`shell_uart_console_write_text`) MUST write through the driver API
  (`usb_serial_jtag_write_bytes`), not `printf`/VFS: the IDF VFS write drops the whole line when the
  SOF connection monitor reports a false disconnect under load (the O3 single-output loss). It must
  also expand LF to CRLF itself (the VFS did) and only stop on a *persistent* disconnect
  (`P4_CONFIG_UART_MIRROR_DISCONNECT_GRACE_MS`) so on-device no-host use does not block. Binary
  transfers already bypass this path via `serial_commands.c`.
- Host tooling: `shell_session.open_port()` pre-sets `dtr=False`/`rts=False` before `open()` so it
  never resets the board (setting DTR after open is too late — O4). Anything that needs a fresh boot
  calls `shell_session.hard_reset()`; do not reintroduce raw `serial.Serial(...)` opens.
- `CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ=40000` is the adopted hosted SDIO clock (v0.38.0). The
  10 MHz value it replaced was a conservative early-bring-up default, not a signal-integrity
  requirement: the trial that reverted it had crashed on the O7 header double free, which the higher
  clock only made more likely. Re-adopting was soak-gated (20 clean boots + 20-min concurrent
  Wi-Fi+SD soak, 0 stalls + full suite). Measured with `wifi throughput tx|rx [port=N] [mb=N] [udp]`
  (device) + `tools/wifi_bench.py` (host, same subnet): 40 MHz beats 10 MHz in both directions
  (~11 vs 7.9 Mbit/s host→device, 4.4 vs ~1.6 device→host). `CONFIG_LWIP_TCP_WND_DEFAULT`/
  `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` are 32768 (raised from 5760 in v0.38.1) so the window is not the
  bottleneck. `tools/regression.py` runs the whole host suite in one command.
- The transcript is a scrollable CONTAINER holding the span group. The span group is sized to
  its exact wrapped content height (`windows_transcript_update_content_size()`), never
  `LV_SIZE_CONTENT`: a content-sized child inside a scrollable container makes
  `lv_obj_update_layout()` loop forever (the `scr->scr_layout_inv` while-loop), freezing the
  LVGL task. An explicit pixel height keeps the widget in `LV_SPAN_MODE_FIXED` so layout
  converges in one pass while the container still scrolls.
- Transcript follow policy: new output pins the view to the bottom only when the view is within
  `P4_CONFIG_TRANSCRIPT_SCROLL_FOLLOW_PX` of the bottom. Submitting a command
  (`shell_force_transcript_scroll_to_end()`) sets a one-shot force-follow flag so the command's
  output is always visible; the flag is consumed by the next apply so later background output
  reverts to near-bottom following. Scrolling is available from the input-row `Up`/`Dn` buttons,
  USB keyboard PageUp/PageDown (`windows_scroll_transcript_by()`), and the USB mouse wheel
  (routed to the LVGL task through the `usb_host_scroll_transcript()` bridge in main.c).
- ALL debug logging MUST use `shell_record_errorf()` / `shell_record_warningf()` / `shell_record_infof()`
- Command history MUST use `shell_store_command_history()` / `shell_recall_history()`
- UART console MUST use `shell_uart_console_start()` / `shell_uart_console_write_text()`
- `shell_uart_console_write_text()` drops the console mirror (and the prompt) when
  `usb_serial_jtag_is_connected()` is false, so an untethered USB-Serial-JTAG TX path can never
  block the command worker on backpressure. The on-screen transcript is unaffected, and the mirror
  still works when a host is attached.
- Transcript length is TRACKED, never rescanned: `s_transcript_len` / `s_transcript_ansi_len` are
  updated by every append/reset/trim and returned by `shell_transcript_get_length()` /
  `shell_transcript_get_ansi_length()` (O(1)). The label path calls
  `windows_set_transcript_text_len()` so no `strlen` / `snprintf("%s")` scans the 64 KB buffer.
  Any new writer of these buffers MUST keep the tracked length in sync.
- The UART console task assembles partial reads: USB-Serial-JTAG delivers one logical line across
  several reads (its RX FIFO is 64 bytes), so a read without a line terminator is NOT a complete
  command. Buffer the fragment and keep reading. During a key wait, forward the first newly-read
  character immediately (do not wait for the terminator).
- A modal surface blocks the command worker, so the streaming `screenshot` MUST be handled by the
  console-reader task via `shell_command_ops_t.modal_console_command` (tried BEFORE
  `modal_handle_serial_line`, only when `modal_is_active()`), not dispatched to the worker. The
  handler claims only the exact bare `screenshot`/`scr`/`capture` tokens and calls the ONE
  `shell_command_screenshot` implementation with `argc == 1`; never add a second capture path.
  Because the reader is the caller, `shell_uart_console_rx_begin/end` MUST skip `vTaskSuspend/Resume`
  when called from the console task (`xTaskGetCurrentTaskHandle()`), or it self-suspends forever;
  the worker-side `receive`/`send` suspend path is unchanged.
- `surf_create_container()` (components/modal) MUST `lv_obj_scroll_to_view()` its panel: modal
  surfaces alias the scrollable transcript container, so a transcript scrolled to its newest output
  would otherwise leave the panel off-screen above the viewport (invisible on the device and in a
  screenshot). The editor renders into the span group and does not use this helper.
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
- The UART console reader forwards the WHOLE freshly-read line into the key queue during a
  wait (not just its first character), so confirmation words (`YES`) and `set /p` values can
  be typed on one serial line. This is safe because `shell_key_wait_begin()`/`end()` reset the
  queue, so strays from one-key waits (`pause`/`choice`/`more`) are flushed before the next
  prompt. Keep the forwarding bounded by `P4_CONFIG_KEY_QUEUE_DEPTH`.
- The prompt is a runtime template owned by `components/shell/`. Never hardcode the prompt
  string in a new surface; call `shell_prompt_render_plain()` (plain) or let
  `shell_uart_console_print_prompt()` handle the colored form.
- `shell_input_line_set_text()` snapshots the prefix it paints. Extraction and repair compare
  against that snapshot, never against a freshly rendered prompt.
- The async transcript buffer MUST be drained into a local copy before appending; never hold
  `s_async_transcript_lock` across an LVGL call
- The async transcript flush scratch is heap-allocated (it can be up to
  `P4_CONFIG_ASYNC_TRANSCRIPT_BYTES`, which is too large for the main task stack). Before the UI
  exists (`windows_get_transcript()` returns NULL — unit tests, very early boot) the flush runs
  synchronously instead of via `lv_async_call`, so an uninitialized LVGL heap is never touched.
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
- The command line is up to `P4_CONFIG_COMMAND_BYTES` (4096). NEVER declare a
  `SHELL_COMMAND_BYTES`-sized local in a function on the worker/UART/LVGL tasks — heap-allocate
  transient command-sized buffers (see the UART console line, submit copies,
  `shell_input_line_set_text`/repair, the mask/recall buffers, and the worker-queue
  `command_request_t`). The batch-line buffer (`P4_CONFIG_BATCH_LINE_BYTES`) is a separate,
  smaller surface.
- Tab completion routes through `shell_command_ops_t.complete_line` (registered by
  `command_init`): the provider parses the whole line — first token completes help-table command
  names, aliases, installed apps and `.bat` files; later tokens complete the command's usage
  tokens, app/bundle names and SD paths, capped by `P4_CONFIG_COMPLETION_MAX_MATCHES`. The help
  table is the single source of command names (there is no `shell_builtin_commands[]`); a new
  command becomes completable as soon as it has a help entry. Keep the provider in
  `components/command/` (it touches the SD via storage); the shell only calls the hook.
- Inline ghost completion uses the separate SD-free `ghost_line` op (`P4_CONFIG_COMPLETION_GHOST`)
  and draws the muted suffix after the caret on the shell input line only; Right/Tab accepts it.
- Recall history is heap-backed (`char **` of `strdup`'d lines) with depth
  `P4_CONFIG_COMMAND_HISTORY_DEPTH` and a total-byte cap `P4_CONFIG_HISTORY_TOTAL_BYTES`; never
  revert to a fixed `[depth][COMMAND_BYTES]` grid. `history /save`/`/load`/`/search` write/read the
  `P4_CONFIG_HISTORY_PROFILE` through the guarded storage session; with
  `P4_CONFIG_HISTORY_AUTOSAVE` it is auto-loaded at the first SD mount and auto-saved (5 s
  debounced on `shell_history_generation()`) with a flush on `reboot`. The Ctrl+R reverse search
  and `history /search` share `shell_history_search_matches()`; `history /search` skips its own
  command line so the query cannot self-match.
- Command implementations live with the module that owns their domain:
  - Filesystem verbs (`cd`, `dir`, `copy`, `move`, `del`, `ren`, `mkdir`, `rmdir`, `type`,
    `write`, `append`, `touch`, `attrib`, `label`, `xcopy`, `find`, `findstr`, `more`, `tree`,
    `fc`, `comp`, `sort`, `sd`, `undelete`, `restore`, `trash`, `recycle`) ->
    `components/storage/` (`storage_nav.c` / `storage_files.c` / `storage_text.c` /
    `storage_disk.c` / `storage_fam.c` / `trash.c`; shared declarations in
    `storage_commands.h`)
  - `xcopy` implements the full DOS 6.x switch set (`/S /E /I /Y /-Y /D[:date] /H /R /K /C /Q
    /T /F /L /A /M /U /P /W /N /V`); its recursive walker keeps each level's state in ONE heap
    block and must never re-enter the command. `findstr` and `comp` are separate commands
    (never split `find`); their pure matcher helpers (`shell_fsre_search`,
    `shell_findstr_match_line`, `shell_comp_first_diff`) are exposed in `storage_commands.h`
    for the unit tests. All of `find`, `findstr`, `more`, `fc`, `comp`, `sort`, and `xcopy`
    return an int ERRORLEVEL (0 ok/found, 1 not found/different, 2 usage) that the dispatcher
    records with `batch_set_errorlevel()`.
  - `del`/`erase` and `rd /s` move entries into the hidden `.trash` recycle bin
    (`components/storage/trash.c`) by default; `/p`/`/f` delete permanently. All of
    `del`, `rd`, `format`, `disk`, `undelete`, `trash` return an int ERRORLEVEL (0/1/2)
    that the dispatcher records with `batch_set_errorlevel()`. Recursive and
    volume-destructive operations are gated by `shell_confirm_destructive()`
    (`P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD`, default `YES`); the gate refuses when
    `shell_key_input_available()` is false so batch files can never trigger it.
  - `find` is ONE command with two modes: the classic text search (`/I /N /C /V`) and a
    recursive file-discovery mode selected automatically by any discovery switch
    (`/NAME:`, `/SIZE:`, `/NEWER:`, `/OLDER:`, `/DIRS`, `/B`, `/S`). Never split it into
    separate commands. The `/SIZE:` primary syntax MUST use `N-M`/`N-`/`-M`/`N` ranges
    (redirection-safe) because bare `>`/`<` are the shell's input/output operators. The
    discovery walker reuses the `dir /s` FATFS primitives, keeps each recursion level's state
    in one heap block, respects `P4_CONFIG_DIR_RECURSE_DEPTH_MAX`, and caps output at
    `P4_CONFIG_FIND_MATCH_MAX`.
  - Batch language verbs (`set`, `calc`, `path`, `echo`, `call`, `if`, `for`
    including `for /f`, `goto`, `shift`, `pause`, `choice`, `setlocal`,
    `endlocal`, `exit`, `proc`, `ini`, `appconfig`, `temp`, `ansi`, `menu`,
    `appmode`) ->
    `components/batch/batch.c` (+ `components/batch/calc.c` for the `calc`
    float evaluator).
  - Alias verbs (`alias`, `unalias`) -> `components/batch/batch.c` (the alias table, the
    `alias`/`unalias` commands, `shell_alias_get`/`set`, and
    `batch_alias_expand_command()` all live here; the command module only dispatches and
    calls the expander at the top of `shell_execute_command()`). Aliases expand ONLY at the
    interactive prompt (`s_active_batch_frame == NULL`), never in batch files. `alias /save`
    writes the profile via the guarded storage session; boot.c auto-runs it after CONFIG.SYS.
    The profile path is `P4_CONFIG_ALIAS_PROFILE` (`ALIASES.BAT`), values are quoted on save,
    and values containing `"` are skipped.
- System info verbs (`help`, `sysinfo`, `version`, `about`, `mem`, `debug`) -> `components/shell/shell.c`
- Task introspection verbs (`ps`, `tasks`, `top`) -> `components/shell/shell.c` (`shell_command_ps`).
  They support `dir /O:`-style sorting (`/O:N|C|S|P|T`, `-` reverses; `top` defaults to CPU
  descending), return an int ERRORLEVEL (0 ok / 2 usage) the dispatcher records, and must stay
  strictly read-only. The pure `shell_task_row_compare()` comparator is exposed in shell.h for
  the unit tests.
- Time / SNTP verbs (`date`, `time`, `timezone`, `sntp`/`ntpsync`) -> `components/clock/clock_commands.c`
- Command verbs live in the split `components/command/*.c` files (never one monolith):
  - TUI/modal verbs (`draw`, `tui`, `color`, `locate`, `anchor`, `dialog`, `list`, `ask`,
    `browse`, `view`, `hexview`) -> `components/command/tui_commands.c`
- GFX canvas verbs (`gfx init/close/status/clear/pixel/line/rect/circle/hline/vline/triangle/
ellipse/polygon/fill/text/show/load/blit/free/slots/save`) -> `components/command/gfx_commands.c`;
the raster core + 8x8 font are `components/gfx/`
  - Checksum/manifest (`crc32`, `asset check|list`) -> `components/command/asset_commands.c`; packaged SD apps (`pkg`) -> `components/command/pkg_commands.c`
    (the ONE CRC-32 primitive is `shell_crc32_update`, shared with `receive`)
  - Database (`db ...`) -> `components/command/db_commands.c`
  - Alarm / calendar (`alarm ...`, `cal ...`) -> `components/command/alarm_commands.c`
  - Fonts/theme/cursor (`font ...`, `theme show`, `cursor ...`) -> `components/command/font_commands.c`
    and `components/command/command_ui.c` (keyboard/windows/cursor)
  - Markdown (`markdown`) -> `components/command/md_commands.c` (renderer `components/markdown/`)
  - JSON (`json validate|pretty`) -> `components/command/json_commands.c`
  - Power/idle/sleep/battery/volume -> `components/command/power_commands.c`
  - Peripheral toolkit (`gpio`, `pwm`, `freq`, `adc`, `i2c`, `spi`, `rgb`) ->
    `components/command/periph_commands.c`
  - Audio verbs (`beep`, `tone`, `wavplay`, `audio`, `volume`) parse in
    `components/command/audio_commands.c`; ALL audio implementation lives in `components/audio/`
  - Screenshot/serial (`screenshot`/`scr`/`capture`, `receive`, `send`) ->
    `components/command/serial_commands.c`
  - Config (`config`) -> `components/command/config_cmd.c`; `gfind` -> `gfind_commands.c`
  - CSV grid (`csv rows|cols|cell|eval`) -> `components/command/csv_commands.c`; the ONE
    RFC-4180-subset parser is `csv_split_line` in `components/storage/storage_csv.c`, and the ONE
    field formatter is `csv_format_field` in `csv_commands.c` (shared with `export`). `=EXPR`
    evaluation reuses `calc_evaluate`; `R<row>C<col>` substitution is the pure `csv_substitute_refs`.
  - Portable interchange (`export <db|alarms> <csv|json|txt> <file>`) ->
    `components/command/export_commands.c`; it MUST render through the existing `db_find`/`db_get`
    and `alarm_list` APIs (no parallel readers) and write via `storage_write_text_file` (atomic).
  - Password encryption (`crypt lock|unlock`) -> `components/command/crypt_commands.c`; the ONE
    AES-256-GCM/PBKDF2 core is the `crypt_*_mem`/`crypt_derive_key` set there (mbedTLS). The key and
    password buffers MUST be zeroed after every run; `/p:` passwords are masked by the shell core.
  - `db` field queries (`/field:`, `/sort:`, `db get /field:`) use the ONE pure parser
    `db_field_get` in `components/db/db.c`; the payload `k=v;k=v` convention is documented in
    command.md and never changes the index format.
  - USB CDC-ACM serial (`usb userial ...`) -> `components/command/userial_commands.c` over the byte
    API in `components/usb/userial.c`. `usb` stays a LEAF: it owns the class driver + RX ring only;
    the verbs (key-queue pump, SD input sourcing) live in `command`, reached from command.c's `usb`
    arm. The CDC driver installs LAZILY on first `userial_open` — NEVER in
    `usb_install_host_stack()`, which would consume the boot internal RAM the hosted-SDIO bring-up
    needs (M48; same class as M38/O8).
  - TCP terminal (`tcpterm`) -> `components/networking/tcpterm.c` (owned with ping/dns/http;
    `components/networking/` stays the sole owner of the lwIP socket surface).
  - Stopwatch (`timer`/`stopwatch`) -> `components/clock/clock_timer.c` (pure slot core) + the
    command surface in `clock_commands.c`; results reach the environment only through the
    registered `clock_host_ops_t.set_env` hook (the clock component stays a leaf).
  - F-key binds (`bind`/`unbind`) -> the table + commands in `components/batch/batch.c`; the shell
    invokes it through `shell_command_ops_t.bind_lookup_fkey` at the non-printable USB-key path
    (after the modal and key-wait guards). Persisted to `P4_CONFIG_BIND_PROFILE` (`BIND.BAT`),
    auto-run by boot.c after the alias profile.
  - Background jobs (`start`, `taskkill`), the dispatcher/pipeline, and the worker task ->
    `components/command/command.c`
- Background jobs: `start <line>` runs on a pooled worker (`bg0`; pool `P4_CONFIG_BG_TASKS`)
  created suspended at init with PSRAM stacks via `xTaskCreateStatic` (needs
  `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`). `taskkill <job>` sets a cooperative kill flag
  polled per batch line and every 100 ms of `delay`. Each job owns a private batch ctx,
  transcript-defer slot, and storage redirect slot; display/modal/key-wait verbs refuse in a bg
  job. `start` is refused while a C6 OTA is pending and `c6ota` while a job runs (PSRAM is
  inaccessible during flash writes).
- Binary serial output MUST use `serial_write_raw()` (a chunked
  `usb_serial_jtag_write_bytes`) — NEVER `fwrite(stdout)`, whose CRLF
  translation corrupts payloads (every `0x0A` becomes `0x0D 0x0A`).
  `screenshot`, `send`, and the `receive` ACKs share this path and the
  `serial_write_frame_header()` framing (4-byte magic + 4-byte LE size).
- Every binary stream (`receive`, `send`, `screenshot`) MUST suspend the
  console reader (`shell_uart_console_rx_begin` / `_rx_end`) so the raw bytes
  are never read as command lines and no echo interleaves. `rx_end` MUST be
  reached on every return path after `rx_begin`.
- `receive` and `send` MUST set ERRORLEVEL (0 ok / 1 IO|CRC / 2 usage) and stay
  usable from batch files. Tunables live in `P4_CONFIG_SERIAL_*`.
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
- Batch argument mapping is COMMAND.COM-style: the frame stores the script path in `args[0]` and
  the caller's arguments in `args[1]..`, so `%0` = script name, `%1`..`%9` = the arguments, and
  `%*` = the arguments from `%1` onward (never the script name). Any change to the frame layout
  MUST update `shell_expand_variables()` in lockstep.
- `goto :eof` is the implicit end-of-file label and ends only the current batch frame. A pending
  `goto`/`goto :eof` MUST be cleared when a frame returns so it cannot leak into the caller's
  line loop when issued from inside a `for` or `if` body.
- `for` loop sets support literal token lists, a single wildcard pattern, and the `for /f`
  file-line form (`for /f "eol=c skip=n delims=xyz tokens=a,b,m-n" %%v in (file-set) do cmd`).
  All three re-enter the pipeline per iteration, so the substituted body buffer MUST be
  heap-allocated (the loop is on the recursive batch path). `for /f` sources are an explicit
  file, a wildcard, or the active `< file`/pipe input when the set is empty; `tokens=` replaces
  the default token 1 rather than appending, and a trailing `*` binds the rest of the line.
- The `calc` command (`components/batch/calc.c`) is a batch language verb and MUST keep its
  evaluator frames small: `calc_value_t` (double + fixed string) lives in parser locals across
  the additive/multiplicative/unary/power/primary/function-call recursion, so a deeply nested
  expression on the 8 KB worker stack must not blow the budget. Keep string payloads at
  `P4_CONFIG_CALC_STR_BYTES`; `POL`/`REC` set the X/Y environment variables as their documented
  side effect; an expression using shell syntax (`^ & | < >`) must be quoted at the prompt, and
  the dispatcher feeds `shell_command_calc_line()` the raw unsplit line so quoted string
  arguments survive the tokenizer.
- The batch process model is defined once in `command.md` / `documentation.md` / `SDK.md`
  ("Batch process model"): stdout = transcript delta captured by `>`/`>>`; stderr = interleaved
  (no separate stream); stdin = the storage input-redirection slot, consumed by the text tools,
  `for /f` over an empty set, and `set /p NAME=< file` (the interactive key queue is the
  fallback); argv = `%0`..`%9`/`%*`; cwd = storage-owned; PATH = batch-owned; environment =
  the shared 24-slot RAM table, scoped by `setlocal`/`endlocal`. `%ERRORLEVEL%` expands to
  the current errorlevel, and `proc` introspects the active batch process stack (`/args`
  `/name` `/depth` `/errorlevel` `/echo` `/stdin`). Batch files are first-class pipe
  processes: a `.bat` stage reads the pipe spool via `for /f in ()` / `set /p <` and its
  output flows to the next stage. Shared libraries of batch routines use
  `call <file.bat>::<routine> [args]`, which starts an external `.bat` at `:routine` and
  MUST push an automatic environment scope for the callee (variable isolation beyond
  `setlocal` — the caller does not write setlocal) that the frame-return unwinding restores;
  a whole-file `call <file.bat>` keeps the shared-environment behavior. `set /p` accepts an
  optional `/T:secs` timeout (default `P4_CONFIG_KEY_WAIT_TIMEOUT_MS`), stripped from the
  displayed prompt. Keep new batch verbs and file-input forms consistent with this model,
  and document authoring/deployment rules in `SDK.md` ("Authoring and deploying batch
  files") when adding a limit that affects script authors.
- The output-redirection capture (`shell_redirect_capture_begin/end/get/reset`) is
  RE-ENTRANT: a redirected command that internally runs another redirected command (a
  pipeline whose stages spool to their own files inside an outer `>` / `>>`) nests one
  capture per level up to `P4_CONFIG_REDIRECT_CAPTURE_MAX_DEPTH`. The inner stage's capture
  is written and popped, and the outer capture resumes so the outer file still receives the
  whole pipeline output. Never flatten it back to a single buffer: `cmd1 | cmd2 > out.txt`
  would silently write an empty file.
- Persistent state (DOS-style env + temp files + simple INI files) MUST live on the SD card.
  The shared core is `components/storage/storage_ini.c`: the pure line editors
  (`storage_ini_get_value` / `storage_ini_upsert` / `storage_ini_remove`) are the single
  implementation of `KEY=VALUE` editing (the `config` command's `config_directive_*` are
  thin wrappers), the file-level operations go through guarded SD sessions with atomic
  temp+rename writes, and `storage_temp_*` keep temporary files under `sd:/tmp`. The batch
  `ini`/`appconfig`/`temp` commands and the applib state group (`app_ini_*`, `app_temp_*`)
  call this core — never re-implement file parsing or temp-file management in another
  layer. `appconfig <app>` writes `sd:/APPS/<APP>.INI` (the APPS dir is created on demand);
  app names MUST be validated (no `/`, `\`, `.` or `..`) before they become part of a path.
- Menu/form primitives follow the DOS pattern (CHOICE + ANSI). The `ansi` batch command and
  `app_print_styled` wrap text in SGR codes (`ESC[<codes>m text ESC[0m`); both MUST accept
  the DOS `7m` spelling and the bare `7` spelling and MUST validate the codes (digits and
  semicolons only) to avoid emitting arbitrary escape sequences. The `menu` batch command
  and `app_menu` render a numbered form and read a numeric choice, returning the 1-based
  index (0 on cancel); `choice` remains the single-key primitive. All menu/form output is
  rendered in the transcript (the shell display area), never on a hidden surface.
- Variable expansion (`shell_expand_variables`) is the SINGLE place `%VAR%` is resolved.
  It handles `%0`..`%9`/`%*`, `%%` (literal `%`), the dynamic pseudo-variables `%DATE%`
  (`MM-DD-YYYY`), `%TIME%` (`HH:MM:SS`), `%RANDOM%` (`0..32767`, `esp_random() & 0x7FFF`),
  `%CD%` (cwd) and `%ERRORLEVEL%` (decimal string), and — cmd.exe parity — an **undefined
  `%VAR%` expands to the empty string** so `if "%var%"==""` works. Single-quoted text stays
  literal; `^%` is an escaped literal percent. Do not add variable resolution anywhere else.
- `if` supports `errorlevel N` (>=), `exist <path>`, `defined <name>` (cmd.exe parity),
  the numeric keywords (`EQU NEQ LSS LEQ GTR GEQ`), and `==` string tests, with optional
  `[not] [/i]`. An undefined variable in a numeric operand reads as 0 (DOS parity).
- The `delay <ms>` command (`components/command/command.c`) is a PURE deterministic wait
  clamped to `P4_CONFIG_DELAY_MAX_MS`; `sleep` remains light-sleep (blanks display, tears
  down Wi-Fi). Use `delay` for melodies/animations, never `sleep`.
- Native modal surfaces (the `edit` editor, `dialog`, `list`, `ask`) run on the shared
  runtime in `components/modal/` — ONE session loop + event group + USB/serial input
  routing. New full-screen surfaces MUST be `modal_surface_t` descriptors on that runtime,
  never a private loop, and MUST reach the shell core only through the generic
  `shell_command_ops_t.modal_*` hooks. Ready-made surfaces take `/t:secs` (auto-cancel via
  a FreeRTOS timer), `/v:NAME` (store a result variable), and `/p` (password mask) options.
- Password (no-echo) input shares ONE reader: `shell_read_line_hidden()` in the shell core
  (with `shell_read_line` a thin wrapper over the shared mode with echo on). The batch
  `set /p NAME=<prompt> /P` command and `app_read_password` MUST call it — never re-implement
  hidden line reading. `/P` and `/T:secs` are stripped from the displayed prompt.
- App mode (save/restore screen + optional full-screen) shares ONE implementation in the
  shell core: `shell_screen_save/restore/discard` (transcript, ANSI colours preserved) and
  `shell_app_mode_enter/exit/active` (full-screen via `windows_enter_app_mode` /
  `windows_exit_app_mode`). The batch `appmode` command and `applib` `app_mode_enter`/
  `app_mode_exit` MUST call these — never re-implement screen capture or widget hiding.
  The shell app-mode LVGL calls MUST be guarded by a `windows_get_transcript() != NULL`
  check (the window manager may not be up in unit tests / early boot). Each batch frame
  tracks whether IT entered app mode (`frame->app_mode`) and the frame-return cleanup MUST
  restore the screen automatically so `exit /b` never leaves the shell in app mode.
- The batch executor and the label scanner MUST agree on where a logical line ends. Both apply
  the same odd-trailing-caret continuation rule; changing one without the other lets a
  continued line register a phantom `:label` and silently corrupt `goto` targets.
- `set /a` operator parsing MUST NOT consume a shell chain separator. A lone `&` is bitwise and,
  a lone `|` is bitwise or, and the doubled forms `&&`/`||` belong to the logical level of the
  expression grammar (and to command chaining at the shell level). The bitwise levels refuse
  `&&`/`||`; the logical levels consume them. From the shell, an expression using `&&`/`||`/`&`/
  `|`/`<`/`>` must be quoted (unquoted they are split as chain/pipe/redirection before the
  command runs). Comparisons `== != < > <= >=` yield 1/0 and sit between the logical and bitwise
  levels; `shell_expr_find_assignment()` splits `NAME[OP]=expr` while skipping the `=` of a
  comparison so `set /a x=5==5` assigns x=1 rather than splitting on the `==`.
- Command execution from the LVGL path MUST use `shell_execute_command_async()` to protect the LVGL stack
- Module-routed commands (wifi, bluetooth, usb, c6ota, sd) receive the original unsplit command text
- State ownership: cwd and SD mount tracking belong to `components/storage/` (reset by
  `storage_init()`); environment, PATH, batch frame, and errorlevel belong to `components/batch/`
  (reset by `batch_init()`). `command_init()` calls both before registering the ops tables.

### Power / Idle Rules
- The power module lives in `components/command/command.c` (`power`, `sleep`, `deepsleep`,
  `power idle`). `power idle <seconds|off>` and the CONFIG.SYS `DISPLAY_TIMEOUT=` directive
  configure the idle display-off timeout; the shared accessors are
  `shell_power_set_idle_timeout()` / `shell_power_get_idle_timeout()`.
- Idle-off MUST only drop the backlight (`display_set_power_state(DISPLAY_POWER_SLEEP)`), never
  halt the panel/touch/USB or touch shell state — wake is a clean
  `DISPLAY_POWER_ON` + header notification. `shell_power_idle_tick()` runs on the LVGL task
  (the header-refresh timer) and scans `lv_indev_get_next()` for a pressed touch to wake.
- Every user input MUST reset the idle clock via `shell_power_notify_activity()`: the UART
  reader routes through the `shell_command_ops_t.pm_notify_activity` hook (NULL-checked), USB
  keyboard/mouse through main.c, and command dispatch through the top of
  `shell_execute_command_core()`. Guard the idle state (timeout, last-activity timestamp,
  off-by-idle flag) with a `portMUX` critical section; it is touched from several tasks.
- Sleep wake: timer always; `P4_CONFIG_POWER_WAKE_GPIO` (default `GPIO_NUM_NC`) enables
  `gpio_wakeup_enable()` + `esp_sleep_enable_gpio_wakeup()` and is disabled after wake. The
  GT911 INT line is NOT wired on this board (`BOARD_CFG_LCD_TOUCH_INT_GPIO = GPIO_NUM_NC`), so
  touch cannot wake light sleep — report that honestly in `sleep`/`power` and point at the
  alternatives instead of pretending touch wake works.

### Clock Rules
- The clock component (`components/clock/`) owns ALL time/SNTP behaviour: the
  C-library clock, timezone, SNTP client, AND the `date`, `time`, `timezone`,
  and `sntp`/`ntpsync` command bodies (`clock_commands.c`). Never implement a
  time command in `command.c` — dispatch there only.
- The clock component is a LEAF (shell -> clock). Its commands must NOT call
  shell helpers directly; they render through `clock_host_ops_t`
  (`clock_register_host_ops()`), registered by `command_init()` with wrappers
  that map onto `shell_print_*` / `shell_record_*` / `shell_text_equals_ignore_case`.
  Pass clock-supplied text to the wrappers as a `%s` argument so a literal
  `%`/`@` stays data. NULL-check every op before use.
- SNTP server hostname is `P4_CONFIG_NTP_SERVER`; the timezone buffer size is
  `P4_CONFIG_TIMEZONE_BYTES`. `time_start_sntp()` is intended to run once lwIP
  is ready; `sntp sync` calls `time_force_resync()` for an immediate exchange.
- The header-clock format is owned by the clock component too: use
  `time_format_hm()` (writes `--:--` until the clock is set) and its pure,
  unit-tested `clock_format_hm_snapshot()`. `time_is_set()` is the single
  "clock is valid" test — true after SNTP OR a manual `date`/`time` set, not
  only after SNTP. Never format a clock string or test the epoch at a call site.
- The timezone is AUTO-DETECTED from the network, never hardcoded: the command
  layer's `timesync` task calls `networking_time_detect()` (components/networking
  owns ALL HTTP) then `time_set_utc_offset(seconds, label)`, and starts SNTP.
  `command_time_auto_sync()` (a `shell_command_ops_t` hook) only kicks that task
  off when Wi-Fi associates; it is cheap and idempotent. The blocking HTTP probe
  MUST NOT run on the LVGL task. Detection re-runs every
  `P4_CONFIG_TIMEZONE_RESYNC_SECS` so DST/travel stays correct, and never runs
  while `c6ota_is_busy()`. Add new zones via the offset path, not a table.

### Keyboard Manager Rules
- ALL keyboard operations MUST go through `components/keyboard/` — never call LVGL keyboard APIs directly from main.c
- `keyboard_init()` MUST be called after `display_init()` and from the LVGL task context
- Keyboard visibility changes trigger automatic UI reflow via the window manager callback
- When keyboard is hidden, the transcript area expands to fill the freed space
- Keyboard height is configurable via `P4_CONFIG_KEYBOARD_*` macros
- Keyboard modes (`text_lower`, `text_upper`, `number`, `symbols`, `nav`, `nav2`)
  are managed by the keyboard component. `keyboard_mode_name()` /
  `keyboard_mode_parse()` are the stable registry, and `keyboard_set_mode()`
  takes the LVGL port lock itself so the worker task may call it. The
  `keyboard mode [page]` command exposes both.
- `keyboard_register_event_callback()` MUST leave exactly ONE handler on the
  widget: `lv_obj_remove_event_cb(widget, NULL)` is a NO-OP in LVGL (it only
  removes callbacks whose cb pointer equals NULL), so the LVGL default keyboard
  handler survives and double-processes every button. Registration removes every
  callback descriptor explicitly (see `keyboard_remove_all_callbacks`) and logs
  the post-registration count via `keyboard_event_callback_count()`; a count
  other than one is a double-input bug.
- The shell routes every OSK button through `keyboard_osk_accept(btn_id)` before
  acting on it; the dedup guard drops a re-fire of the same button within
  `P4_CONFIG_OSK_DEBOUNCE_MS`. Never bypass it.
- `keyboard_bind_textarea(NULL)` must be called to hand the OSK to a modal
  surface; it clears the LVGL widget binding so no stray handler can type into a
  hidden textarea. `keyboard_is_textarea_bound()` lets callers assert the state.
- Shell transcript/input-line LVGL callbacks MUST no-op while the editor is open
  (`windows_editor_mode_active()`), so the shared surface is never re-bound or
  keyboard-summoned underneath the editor.
- The editor MUST be fully usable from the touch keyboard and MUST open on the
  Nav page: two navigation pages (KEYBOARD_MODE_NAV -> LVGL USER_1,
  KEYBOARD_MODE_NAV2 -> LVGL USER_2) cover every editor command (nav: arrows,
  Tab, Ins, Del, Home/End, PgUp/PgDn, Find, Next, Rep, All, Case, Goto, Undo,
  Redo, Save, SaveAs, Quit, Prev, Open, Comment, Match, Wrap, Reload; edit:
  Copy, Cut, Paste, SelAll, WdL/WdR, DocH/DocE, DelLn, DelE). Mode buttons
  `Nav`/`Nav1`/`Nav2` switch pages in main.c's keyboard callback; `abc` returns
  to letters. Both `editor_view_open()` and `editor_view_close()` set the page
  (Nav on open, letters on close), and text prompts switch to letters /
  restore Nav on commit or cancel. When adding a new editor feature, a touch
  button MUST be added to one of these pages (or a new one), and the label MUST
  go through the pure `editor_osk_key_from_label()` table in `editor_view.c`
  (the single mapping shared by the handler and the unit test).
- **Situational capability keys.** Some OSK keys are only meaningful in certain
  contexts; they MUST be greyed out (disabled) where useless, never left looking
  active. `components/keyboard/` owns a capability bitmask
  (`keyboard_capability_t`, currently `KEYBOARD_CAP_NAV` for the symbols/edit
  page `Nav` key). Availability is `context callback OR reference-counted
  requests`: `keyboard_register_capabilities_callback()` supplies the
  shell-owned context (main returns `editor_view_is_open() ? KEYBOARD_CAP_NAV :
  0`), and any modal/batch app requests one via
  `keyboard_request_capability(KEYBOARD_CAP_NAV, on|off)` — exposed to batch as
  `keyboard nav on|off` (command_ui.c). `keyboard_apply_capabilities_locked()`
  re-runs on every `keyboard_apply_mode()`/`keyboard_show()` (a mode change
  reinstalls the page's ctrl map, so per-button ctrl must be re-applied) and
  sets/clears `LV_BUTTONMATRIX_CTRL_DISABLED` on every capability-governed key.
  LVGL ignores clicks on disabled buttons; the disabled look is the
  `LV_PART_ITEMS|LV_STATE_DISABLED` style (muted text, `LV_OPA_30` bg) set in
  keyboard_init/refresh_theme. Never hardwire a capability to one consumer:
  keep `keyboard_button_required_capability()` as the pure, unit-tested
  label→capability map. `ui state` reports `nav=on|off|na` for tests.
- Keep context predicates cheap and LVGL-free: the capabilities callback runs
  with the LVGL lock held (from `keyboard_apply_mode`) and must only read state.

### Editor Rules (components/editor)
- The `edit` command lives in `components/editor/`: the byte-preserving document
  model plus the LVGL surface and the worker-task session. Since v0.33.0 it runs
  on the shared modal runtime in `components/modal/`; `command.c` only dispatches
  and maps the errorlevel.
- **Everything large is PSRAM-backed.** Line text, the line array, undo
  snapshots, the view's width/wrap caches, span scratch, and preview buffers go
  through `editor_mem_alloc/realloc/free` (PSRAM first, internal fallback). Do
  not use raw `malloc` for per-document buffers.
- **Never hand a PSRAM pointer to `fread`/`fwrite`.** PSRAM is not
  `MALLOC_CAP_DMA` on this P4 build, so `editor_doc_load`/`editor_doc_save`
  stream through a small internal `MALLOC_CAP_DMA` bounce buffer
  (`editor_dma_alloc`, `EDITOR_SD_CHUNK_BYTES`). Load carries a trailing `\r`
  across chunk boundaries so CRLF detection is chunk-exact.
- **Rendering is windowed.** Only `P4_CONFIG_EDITOR_RENDER_ROWS` (256) rows are
  materialized as spans; a full-height invisible `spacer` child holds the scroll
  range, and `editor_render_follow_cursor`/`editor_scroll_event_cb` move the
  window. Overlays stay in document coordinates. Keep the window small: the span
  group is rebuilt on every edit, and a whole-document rebuild is quadratic in
  row count and trips the LVGL task watchdog (verified panic). Wrapping renders
  the full document (row pitch changes) and is a per-session toggle.
- **Undo is byte-budgeted.** `P4_CONFIG_EDITOR_UNDO_MAX_BYTES` evicts the oldest
  snapshots; above `P4_CONFIG_EDITOR_UNDO_MAX_SNAPSHOT_BYTES` undo is disabled
  for the session so each edit does not serialize the whole document. Snapshots
  store the serialized document length for accounting.
- `P4_CONFIG_EDITOR_MAX_BYTES` (1 MB) and `P4_CONFIG_EDITOR_MAX_LINES` (65536)
  are the load caps; the unit test that exercises the line cap must build it
  with one multi-line insert, not a per-Enter loop.
- Document mutators run on the LVGL task; SD load/save runs on the command
  worker inside a guarded `shell_sd_begin`/`shell_sd_end` session. A failed
  save MUST remove its partial destination.
- The editor renders through a DEDICATED span group inside the transcript
  container (not the shell's span group, which is hidden for the session and
  restored on close). Long lines are clipped in FIXED span mode, never
  wrapped; the per-row width table keeps the cursor and touch mapping aligned
  with the rendered rows.
- The line-number gutter is a render-only prefix run (via the pure
  `editor_format_line_number()` helper) — NEVER part of the document. The
  cursor x, selection overlay x, and touch x-mapping MUST all be offset by the
  same `editor_gutter_width()`, or the caret/tap/select will drift from the
  text. The current-line highlight is a background bar moved behind the text
  (`lv_obj_move_to_index(..., 0)`).
- Serial console verbs: `\q` quit, `\s` save, `\f` find, `\g` go-to-line,
  `\o` save-as, `\open` open another file, `\u` undo, `\r` redo,
  `\all` replace-all, `\c` case, `\b` match-jump, `\co` comment, `\w` wrap,
  `\l` reload, `\p` preview, `\a` select-all; any other serial line
  is typed text + Enter. Keep this set documented in command.md/editor.md.
- All editor tunables live in `P4_CONFIG_EDITOR_*` (documented in
  p4minishell_config.yaml). New syntax modes extend `editor_syntax_t`,
  `editor_doc_pick_syntax()`, and the lexer — never special-case file
  extensions in the view.
- The status-bar prompt system (Find / Replace / Go-to-Line / Save-As / Open /
  quit confirmation) is the DOS EDIT search surface. Keep prompt strings in
  the status bar, never in the document.
- File > Open swaps the document contents in place on the worker
  (`editor_doc_replace_contents`) so the view only rebuilds; the open path is
  resolved with `shell_fs_resolve_path` and a missing path opens an empty
  buffer bound to it (`editor_file_missing`). A dirty buffer is guarded by the
  `Open without saving? (Y/N)` confirm first. A stale Save-As target MUST NOT
  follow into the newly opened file.
- Editing an existing file that cannot be loaded is a hard error (refuse with
  a message) — never open an empty buffer over an existing file.
- Rotation while a session is open MUST close the editor view and wake the
  worker (main.c `shell_build_ui()` does this) rather than leaving dangling
  LVGL widgets.
- Keyboard mode switching (`abc`/`ABC`/`1#`/`Nav`) is owned by the single
  keyboard `LV_EVENT_VALUE_CHANGED` handler in main.c; the LVGL default
  handler is replaced by that callback, so all button routing (shell input
  line or editor) lives there.
- The editor unit tests (`test/main/test_editor.c`) cover the pure document
  model and the batch lexer only; the LVGL surface and session are
  hardware-bound and are verified on the board.

### Touch Automation Rules (components/uitest + command/ui_commands.c)
- `components/uitest/` owns a second LVGL pointer indev (`ui_test.c`) whose
  read callback reports a scripted point/press state. `ui tap/press/move/
  release/longpress/swipe` block until the script finishes (EventGroup), so
  they are deterministic and batch-safe. The 5 ms script timer also calls
  `lv_indev_read()` so a short tap is not lost between the indev's own samples.
- The `ui` verbs live in `components/command/ui_commands.c`; `command.c`
  dispatches them, the help table (via the completion provider) completes them,
  and `s_shell_help_entries[]` documents them. `command_modal_console_command()`
  claims `ui …` on the console-reader task while a modal blocks the worker.
- `ui targets` walks the active screen for visible `CLICKABLE` widgets and
  expands buttonmatrices into `kbd:<label>` rows. The **name is printed last**
  so names with spaces (list rows) parse. Button areas from
  `lv_buttonmatrix_get_button_area()` are object-local: add the widget's
  `lv_obj_get_coords()` origin for absolute coordinates.
- Keep long `ui` output OFF the LVGL transcript (`ui_out_raw` → serial):
  flooding the transcript starves the LVGL port lock and makes `ui targets`
  return "not ready". `ui target <id>` activates a widget directly (modal
  panels sit in the auto-scrolling transcript where coordinate taps are racy);
  `ui tap` is the raw-coordinate path.
- The LVGL button-area getter is a managed patch (tracked in
  `tools/managed_patches.patch`); re-apply with the existing script after an
  `idf.py update-dependencies`.

### Window Manager Rules
- ALL LVGL screen layout MUST go through `components/windows/` — never create screen-level widgets directly in main.c
- `windows_init()` MUST be called after `display_init()` and from the LVGL task context (inside `bsp_display_lock`)
- `windows_deinit()` MUST be called before rebuilding the UI after rotation changes
- Window objects MUST be accessed via `windows_get_*()` accessors, never stored as static variables in main.c
- All window region dimensions MUST use config macros (`P4_CONFIG_WINDOW_*`), never hardcoded
- Colors MUST be accessed through `windows_get_color()` with semantic names, never raw hex values
- The window manager delegates header rendering to `components/header/` via `header_init()`/`header_deinit()`
- The window manager queries display resolution from `components/display/` via `display_get_width()`/`display_get_height()`
- The modal editor surface IS the transcript container and MUST stay VISIBLE in
  editor mode: LVGL flex skips `LV_OBJ_FLAG_HIDDEN` children, so hiding it
  collapses the editor area to ~one line and drags the keyboard up under it.
  The editor hides the shell span group inside the container instead.
- The editor surface height MUST always equal `windows_get_rect(WINDOW_REGION_TRANSCRIPT).height`
  via `windows_refresh_editor_surface()` (called on entry and keyboard
  show/hide); `windows_editor_surface_height_ok()` and
  `windows_debug_editor_layout()` guard against a collapse.

### TUI Rules (v0.38.1 - 80x25 grid in the live transcript rect, `components/tui/`, font 384 glyphs, `draw`/`gfx`, fullscreen, screenshot)

- The TUI logical grid is `P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` (`80×25` `p4minishell_config.h:325`, DOS parity) — a heap cell buffer (`tui_cell_t utf8[4]` `components/tui/tui.h:35`, `tui_cell_set` `components/tui/tui.c:129`) with fg/bg/attribute per cell. It is CLAMPED to the logical grid, never to pixels; the pixel rect is the live transcript region `1024x510` (`tui status`) via `windows_enter_tui_mode()` / `windows_refresh_tui_surface()` / `windows_notify_keyboard_visibility` (`components/windows/windows.c:312`). That rect follows rotation and on-screen-keyboard visibility — `P4_CONFIG_TUI_*` maps to it, not to a fixed screen size. `utf8[4]` is REQUIRED (3-byte box UTF-8 `SH_BOX_*` + NUL); `utf8[2]` truncated box draws.
- Font: extended `unscii_16` in-place (`managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c`, 384 glyphs U+2500-U+257F + U+2600-U+26FF, cmaps 3, no duplication, `sdkconfig.defaults:33` `CONFIG_LV_FONT_UNSCII_16=y`) via `windows_get_terminal_font()` for `s_tui_label` (`lv_label_set_recolor true`). `SH_BOX_*` UTF-8 sequences are the ONLY box source.
- Drawing primitives MUST honor style and title via `tui_cell_set`: `tui_draw_box` (`components/tui/tui.c:241`) selects `SH_BOX_TL`/`H`/`V` vs `TL2`/`H2`/`V2` vs `TLR`/`TRR`/`BLR`/`BRR` per `single`/`double`/`rounded` and centers title with spaces; `tui_draw_line` (`components/tui/tui.c:296`) selects `SH_BOX_H`/`V` vs `H2`/`V2` vs `HL`/`VL` per `single`/`double`/`heavy`; `tui_flush` (`components/tui/tui.c:620`) coalesces by fg and emits `#RRGGBB ` per run via `ansi_get_palette_color` PowerShell palette (no duplicate palette) into `s_tui_label`. Default fg 16 emits no tag.
- Fullscreen: `draw fullscreen on|off` (global) + `tui fullscreen on|off` (per-app) both route to `tui_enter_fullscreen`/`tui_exit_fullscreen` (`components/tui/tui.c:417`) which call `windows_set_fullscreen`/`header_set_visible` (`components/windows/windows.c:418`) — header hidden completely when fullscreen, kept visible by default. Dynamic keyboard scaling via `windows_notify_keyboard_visibility` MUST be kept; `tui_refresh_surface` (`components/tui/tui.c:408`) on rotation/keyboard. `tui status` reports `rect 1024x510 cols 80 rows 25` + fullscreen + font.
- Prompt: all inputs MUST honor `shell_prompt_render_plain()` (`components/shell/shell.c:412`): `main.c:112` input line echo `SHELL_PROMPT` → `shell_prompt_render_plain()`, `modal_surf.c:412` `ask` placeholder `shell_prompt_render_plain()` + `keyboard_bind_textarea`, shell echo situational `SH_PROMPT` color.
- Screenshot debug loop (`grab_screenshot.py --port COM11 --out out.png --crop-transcript` + `capture_tui.py`) crops to transcript rect for pixel-perfect verification; use it during hardware bug hunting.
- The cell buffer is heap-allocated (PSRAM `MALLOC_CAP_SPIRAM`); every `draw`/`locate`/`color` call clamps to `80×25` and `tui_flush` diffs into LVGL recolor runs. CSI sequences are handled by `components/ansi/ansi.c` (`ansi_process_text` / `ansi_vformat`), so SGR codes in batch `draw`/`color`/`ansi` share one parser. Alt-screen `ESC[?1049h/l` save/restore (`tui_alt_enter`/`tui_alt_leave` `components/tui/tui.c:327`) is unconditional. `draw` auto-enters TUI (`tui_init` `components/tui/tui.c:56`) when no TUI/modal surface is active.
- New full-screen TUI surfaces MUST be `modal_surface_t` descriptors on the shared modal runtime (`components/modal/modal_surf.c`), never a private loop. They MUST accept `/t:secs` (auto-cancel via FreeRTOS one-shot timer firing `MODAL_EVENT_CLOSE_REQUEST`) and `/v:NAME` (store result variable; dialog/list/ask/browse all do), and MUST be routed through the generic `shell_command_ops_t.modal_*` hooks. `dialog`/`list`/`ask` dispatcher was missing from `command.c` — now wired; `browse`/`view`/`hexview` were `return -1` stubs — now restored on the same runtime. Companion testing (incl. `SVC.BAT`/`AGENDA.BAT`) is hardware-verified: deep 8/8. Window stack via nested `tui_draw_box` with title is the essential feature — not a second window manager.

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
- ALL ESP-Hosted, esp_wifi_remote, esp_netif, NimBLE, lwIP-connectivity (ping/DNS), and
  esp_http_client/mbedTLS/TLS calls MUST live inside `components/networking/`. The only
  sanctioned exceptions are `components/c6ota/`, which drives `esp_hosted_slave_ota_*` and its
  own esp_http_client download because co-processor update is its purpose.
- The persistent known-network list (`wifi_known.c` / `wifi_known.h`) belongs to
  `components/networking/`. ALL its SD I/O goes through the guarded storage session API
  (`shell_sd_begin` / `shell_sd_end`) and must tolerate failure at every step: no card,
  missing/corrupt file, read-only or full disk → empty list, no crash, no freeze, no infinite
  retry. The file (`P4_CONFIG_WIFI_KNOWN_FILE`, default `sd:/WIFI.KNOWN`) is written
  atomically (temp + rename) with the storage free-space pre-check and partial-file cleanup;
  FATFS `f_rename` refuses to overwrite, so remove the target and retry before giving up.
  Passwords are stored in the file but NEVER printed to transcript, history, debug log, or
  UART. Keep large buffers off the command-worker / event-task stacks (a ~2 KB array in the
  loader overflowed the 8192-byte worker stack).
- Boot auto-connect runs in the background task after the STA runtime is up, only when
  `WIFI_AUTOCONNECT=ON` (the CONFIG.SYS master switch): load the known list, scan, connect to
  the best visible known network (preferred / highest priority / strongest RSSI), then fall
  back to the classic single-credential path (sdkconfig default or CONFIG.SYS target). The
  chosen target feeds the existing watchdog so retries stay bounded.
- `WIFI_SSID=` + `WIFI_PASSWORD=` in CONFIG.SYS must assemble the pair: the boot-credential
  setter preserves the other field when one argument is empty and seeds the known list when a
  target is configured. Do not break the existing directives.
- `ping` and `dns`/`nslookup` are implemented in `components/networking/networking.c` over the
  lwIP `esp_ping` session and `getaddrinfo`; `components/command/command.c` only dispatches
  them and maps the returned `esp_err_t` onto ERRORLEVEL (0 success, 1 failure, 2 usage). They
  MUST set errorlevel and MUST stay redirectable/pipable like every other command. The ping
  worker-side wait is always bounded by `count * (timeout + interval) + margin`.
- `httpget` / `wget` is implemented as `networking_http_get()` in
  `components/networking/networking.c` over the same `esp_http_client` stack c6ota uses. The
  command layer only dispatches and writes the returned body to SD through the storage write
  path (free-space precheck, partial-destination cleanup). It MUST set ERRORLEVEL (0 = HTTP
  2xx, 1 = failure, 2 = usage), MUST be redirectable/pipable, MUST require an active
  connection (clear DOS-style error when disconnected), MUST buffer the body in PSRAM capped by
  `P4_CONFIG_HTTP_MAX_BODY_BYTES`, and MUST be timeout-bounded so the worker task never hangs.
  Never call `esp_http_client` / mbedTLS / socket APIs from shell/, command/, batch/, or main/.
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

### Network Services Rules (httpd / netstat / ipconfig)
- The HTTP file server (`components/networking/http_server.c`) is the sole owner of the
  `esp_http_server` surface, exactly as `networking.c` owns `esp_http_client`. It serves the
  SD card via guarded sessions (`shell_sd_begin`/`shell_sd_end`) and VFS `opendir`/`fopen`/
  `fread` chunk streaming (the same SD pattern c6ota uses), never touching FATFS internals
  directly.
- Server lifecycle is tied to Wi-Fi events: auto-start on `IP_EVENT_STA_GOT_IP` (gated by
  `P4_CONFIG_HTTPD_AUTOSTART`) and stop on `WIFI_EVENT_STA_DISCONNECTED`. Every request is
  bounded: auth check first (optional Basic auth, constant-time compare), `..` path segments
  rejected with 400, directory listings capped at `P4_CONFIG_HTTPD_LISTING_MAX`, file reads
  through a heap buffer with `recv`/`send` timeouts. All limits come from `P4_CONFIG_HTTPD_*`.
- `httpget` sends HTTP Basic auth when the URL carries a `user:pass@` prefix
  (`networking_http_url_has_userinfo()` sets `HTTP_AUTH_TYPE_BASIC`), so authenticated
  endpoints can be fetched from the shell.
- `netstat`/`ipconfig` (`components/networking/netdiag.c`) read lwIP state only: `netif_list`,
  `dns_getserver`, and the TCP/UDP PCB lists. The PCB globals are declared locally (not via
  `lwip/priv/*` headers) and traversed read-only under `LOCK_TCPIP_CORE()` when
  `LWIP_TCPIP_CORE_LOCKING` is enabled (no-op otherwise), capped by
  `P4_CONFIG_NETSTAT_ROW_MAX`. Never iterate PCBs without the core lock in a context that
  could deadlock, and never modify them.

### Bluetooth Rules
- Hosted NimBLE on C6 over ESP-Hosted VHCI (not Bluedroid)
- Stateful lifecycle: enable once, reuse for scan/advertise
- bt is alias for bluetooth
- Supported: status, scan [limit], advertise on [name]|off
- A scan run MUST be bounded by P4_CONFIG_BT_SCAN_DURATION_MS (passed as the ble_gap_disc
  duration), so `bluetooth scan` always terminates and the worker task never hangs
- `bluetooth advertise on [name]` stores the name in s_bluetooth_state.session_advertise_name;
  it is session-only and must never be persisted across boots

### SD Card Rules
- All SD access goes through `components/storage/`. No other module may call `bsp_sdcard_mount()`
  or `bsp_sdcard_unmount()` directly.
- All SD commands use the shared guarded session: `shell_sd_begin()` / `shell_sd_end()`. Every
  `shell_sd_begin()` MUST have a matching `shell_sd_end()` on every return path.
- The mount is persistent; only `sd eject` / `sdeject` unmounts
- The one-shot first-mount hook (`storage_register_sd_first_mount_callback`) MUST be
  registered before anything can mount the card. `storage_sd_mark_mounted()` leaves the
  one-shot armed when no handler is set and fires the deferred work on registration, so a
  mount that beats the registration is not lost. Do NOT add init-time SD reads that mount the
  card ahead of the hook (a regression source: `security_init()` reading CONFIG.SYS at init —
  now deferred to `security_load_saved()` from the boot script).
- The SD card (slot 0) and the ESP-Hosted C6 transport (slot 1) share the SDMMC controller and
  its DMA-capable internal buffers. The card MUST mount before `networking_init()` starts the
  C6 bring-up (main runs `boot_run_startup()` first); bringing the C6 up first races the mount
  and makes the C6 SDIO card init retry `sdmmc_allocate_aligned_buf: not enough mem`.
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
- The command worker task stack is `P4_CONFIG_COMMAND_TASK_STACK` (32768 bytes) and is shared by
  the whole dispatch path
- `shell_execute_batch_file()`, `shell_execute_command()`, and `shell_execute_command_core()`
  form a RECURSIVE cycle: a batch file re-enters the pipeline for every line it runs. Their
  stack frames are multiplied by `P4_CONFIG_BATCH_DEPTH_MAX`.
- NEVER add a line-sized or larger buffer as a local variable in any function on that cycle.
  Allocate it on the heap and free it on every exit path. A 12 KB batch frame on the 8 KB stack
  was a real crash fixed in v0.19.0.
- This includes the batch command handlers themselves: `shell_resolve_batch_path`,
  `shell_command_set`/`set /a`/`set /p`, `shell_command_path`, `shell_command_echo`,
  `shell_command_if`, and `shell_command_choice` all run on the recursive batch path and keep
  their SD-path/line-sized scratch on the heap. A nested `call` whose callee runs `set`/`echo`
  stacked these frames and overflowed the worker stack (fixed in v0.24.8).
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
- Batch files execute from a RAM image: `shell_batch_run_internal` loads the file (up to
  `P4_CONFIG_BATCH_FILE_MAX_BYTES`, 131072) into a PSRAM buffer and reads it through
  `shell_frame_fgets`/`shell_frame_tell`/`shell_frame_seek` (byte-for-byte `fgets` semantics, so
  line continuation, `call :label` resume, and label positions are unchanged), keeping
  `goto`-heavy loops off the SD card; larger files stream from the SD. Free the image on every
  frame-return path.

### Command Execution
- Heavy commands run on dedicated worker task (not LVGL input callback stack)
- Preserve original unsplit command text for family handlers (wifi, sd, c6ota)
- LV_EVENT_READY on input line is the confirmed submission path
- Serial console reuses same shell path (stdin to submit, stdout from transcript)
- Background jobs run on a separate pooled worker: `start <line>` resumes one of
  `P4_CONFIG_BG_TASKS` workers (`bg0`) that were created suspended at init with PSRAM stacks via
  `xTaskCreateStatic` (needs `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`). `taskkill <job>` sets a
  COOPERATIVE kill flag polled per batch line and every 100 ms of `delay`; a job inside one long
  native command runs to completion. `start` is refused while a C6 OTA is pending and `c6ota`
  while a job runs (PSRAM is inaccessible during flash writes).

### Task Introspection (ps / tasks / top)
- `ps`, `tasks`, and `top` are READ-ONLY: they only read `uxTaskGetSystemState()`
  snapshots and must never call `vTaskSuspend` / `vTaskDelete` / priority changes.
  Do not add kill/suspend verbs.
- The `TaskStatus_t` snapshot array MUST be heap-allocated (an entry is ~40 bytes;
  `P4_CONFIG_TASK_SNAPSHOT_MAX` entries can exceed the 8 KB worker stack) and freed on
  every path. Cap the count with `P4_CONFIG_TASK_SNAPSHOT_MAX` so a task-creation burst
  cannot blow the allocation or flood the transcript.
- Per-task CPU% comes from diffing `ulRunTimeCounter` against the previous sample keyed by
  `xTaskNumber` (unsigned arithmetic handles counter wrap; a new/deleted task starts fresh).
  Unpinned tasks report `tskNO_AFFINITY`; display that as `-1`. Guard `xCoreID` access with
  `#if configTASKLIST_INCLUDE_COREID`.
- `/O:` sorting normalizes the snapshot into lightweight heap `shell_task_row_t` rows and
  `qsort`s them with `shell_task_row_compare()` (pure; keep it in shell.h for the unit tests).
  `top` defaults to CPU-descending; `ps`/`tasks` keep FreeRTOS order unless `/O:` is given.

### Shell State
- RAM-only: current working directory, environment variables, PATH, batch args
- The RAM clipboard is shell-core state owned by `components/shell/` (`s_clipboard`,
  `shell_clipboard_*`, `shell_input_line_paste`). The `clip`/`paste` COMMANDS live in
  `components/command/command.c` and only call the `shell.h` clipboard API; the file-side work
  (`clip read`, `paste <dest>` copy, `clip file` resolution) uses storage helpers there. Keep
  clipboard state out of command.c; add it to `components/shell/` and keep the verbs
  batch-safe, redirectable, and ERRORLEVEL-setting.
- Background jobs hold per-task state owned by their own worker: a batch context
  (`batch_bg_alloc`/`batch_bg_bind`/`batch_bg_release`), a transcript defer slot
  (`shell_register_bg_task`), and a storage redirect slot. Env/alias writes stay shared behind the
  env lock (copy-out discipline). Display, modal, and key-wait verbs REFUSE in a bg job instead of
  blocking the shared UI.
- Current working directory owned by `components/storage/storage.c`; exposed read-only via
  `shell_get_cwd()` and mutated only through `storage_set_cwd()`
- Environment variables, PATH, batch frame stack, and errorlevel owned by
  `components/batch/batch.c`
- No persistence across boots
- Environment variables: max 24, names alphanumeric + underscore
- Batch depth: max 4 nested calls
- Batch labels: max 32 per file (`P4_CONFIG_BATCH_LABEL_MAX`)
- `set /a` parenthesis nesting: max 16 (`P4_CONFIG_SET_EXPR_DEPTH_MAX`)
- `set /p` input: max 128 bytes (`P4_CONFIG_SET_PROMPT_INPUT_BYTES`); the same cap bounds a
  line read from a `< file`/pipe source (`set /p NAME=< file`)
- `calc` string results/arguments: max `P4_CONFIG_CALC_STR_BYTES`; nesting max
  `P4_CONFIG_CALC_MAX_DEPTH`; `for /f` `tokens=` list max `P4_CONFIG_FORF_TOKEN_MAX`, source
  lines max `P4_CONFIG_FORF_LINE_MAX`, `delims=` set max `P4_CONFIG_FORF_DELIMS_BYTES`
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

### Boot Scripting (CONFIG.SYS / AUTOEXEC.BAT)
- The firmware looks for `CONFIG.SYS` and `AUTOEXEC.BAT` on the SD card root at every boot.
- When either file is missing and `P4_CONFIG_BOOT_GENERATE_DEFAULTS` is set, default files are written once.
- `CONFIG.SYS` directives are parsed line-by-line by `components/boot/boot.c`. Supported: `SET`,
  `PATH=`, `PROMPT=`, `ECHO ON|OFF`, `ROTATE=`, `BRIGHTNESS=`, `DISPLAY_POWER=`, `VOLUME=`,
  `RGB=` (WS2812 LED: `<r>,<g>,<b>` / `#RRGGBB` / `<effect>[,speed]` / `OFF` / `AUTO,<ON|OFF>`),
  `OSK=ON|OFF`, `HEADER=ON|OFF`,
  `WIFI_SSID=`, `WIFI_PASSWORD=`, `WIFI_AUTOCONNECT=`, `WIFI=ON|OFF`, `BLUETOOTH=ON|OFF`,
  `BT_ADVERTISE=ON|OFF`, `USB_KEYBOARD=ON|OFF`, `USB_MOUSE=ON|OFF`, `GPIO <n> = OUT [HIGH|LOW]`.
- Any unrecognized `NAME=VALUE` line is applied as a batch environment variable (same effect as
  `SET`), so CONFIG.SYS can carry project variables. Only unknown keywords *without* a value warn.
- Hardware directives are applied by executing their command-line equivalent through the batch
  pipeline (`batch_boot_execute_command()`), reusing existing validation. State-only directives
  (Wi-Fi credentials, autoconnect policy, echo default) use accessors exposed by the owning modules.
- `OSK=` maps to `keyboard_show()` / `keyboard_hide()`; `HEADER=` maps to
  `header_set_visible()` + `display_schedule_ui_rebuild()` (the UI is already built when
  CONFIG.SYS runs). The header visibility flag survives deinit/init, so a hidden bar stays hidden
  across the rebuild.
- The `config` command (`components/command/config_cmd.c`) is the ONLY runtime writer of
  CONFIG.SYS. It tracks BRIGHTNESS/ROTATE/VOLUME/PROMPT/WIFI_AUTOCONNECT/DISPLAY_TIMEOUT/OSK/
  HEADER and rewrites CONFIG.SYS with a guarded atomic temp+rename write, preserving comments and
  unknown directives. `config factory` is the only destructive reset; it MUST use
  `shell_confirm_destructive()` and delete CONFIG.SYS, AUTOEXEC.BAT, WIFI.KNOWN, ALIASES.BAT, and
  HISTORY.TXT. New persistent settings MUST be added to the `config_settings[]` table with a
  getter/render + apply pair, never by adding a second CONFIG.SYS writer.
- `AUTOEXEC.BAT` runs through the normal batch pipeline (`shell_execute_batch_file()`), with cwd =
  SD root. Non-zero errorlevel is a warning only; the shell continues.
- Unknown or malformed directives produce a single muted warning and are never fatal.
- With no SD card the boot path prints a muted "No SD card detected" transcript line + header
  notification instead of being silent (the old `ESP_LOGI` is compiled out at WARN level).
- First mount of each boot fires the one-shot hook `storage_register_sd_first_mount_callback`
  (wired in main to `boot_on_sd_first_mount`), which generates default CONFIG.SYS/AUTOEXEC.BAT
  when missing and prints the "SD card ready" welcome. New default-file generation MUST go
  through `boot_ensure_default_files()` (idempotent, never overwrites an existing user file).
- The boot script also loads the persisted security/owner state: `boot_script_apply()` calls
  `security_load_saved()` (the CONFIG.SYS reads) immediately before `security_engage_boot_lock()`,
  so boot-lock / auto-lock / conceal take effect at boot. `security_init()` MUST NOT touch the SD
  card at init — an init-time mount beats the first-mount hook and skips the boot work (O10).
- `main` runs the boot script BEFORE `networking_init()`: the SD card must mount before the
  ESP-Hosted C6 transport claims the shared SDMMC host/DMA buffers (see SD Card Rules).
- Wi-Fi password from `WIFI_PASSWORD=` is never echoed to transcript, history, or debug log.
- GPIO directives are delegated to the existing `gpio set` safety check; reserved pins are refused.
- All tunable values live in `p4minishell_config.h` and are documented in `p4minishell_config.yaml`
  under `boot_scripting`.

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
- Status icons: Wi-Fi, Bluetooth, USB, SD (left panel); MEM, CPU, BAT (right)
- Two styles via `P4_CONFIG_HEADER_STATUS_STYLE`: `words` (WiFi HI, USB ON) or
  `glyph` (compact colored `W BT U S` / `M C B`); classification is shared and
  lives once in the pure `header_status.c`
- Status colors: green healthy, amber degraded, red off/failed, muted absent (SD none)
- Conditional `A` activity indicator while a C6 OTA or background job runs
- Tap shows a one-line detail in the notification area; long-press runs the
  subsystem status command via the shell handler
- Center shows the local clock when idle; notifications queue FIFO with a
  severity color and take precedence over the clock
- Poll cadence is adaptive (`header_refresh.c`), telemetry throttled to
  `P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS` and sampled on the `sheltlm` background
  task, never `uxTaskGetSystemState()` on the LVGL task (DSI-underrun "BSOD")

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
`p4minishell_config.yaml` `config_version`, and the `readme.md` version badge, then add
a new section to `changelog.md`.

### Hardware Gaps (Do NOT implement)
- Camera: no local camera stack in workspace
- Should fail explicitly with honest messages

### LED Rules (components/led + `rgb`)
- `components/led/led.c` is the single owner of the WS2812 status LED on GPIO26
  (`BOARD_CFG_RGB_LED_GPIO`, `BOARD_CFG_RGB_LED_IS_WS2812`). It creates the strip via the
  `espressif/led_strip` managed component over RMT and drives ALL strip I/O from one small
  animation task; public API calls only mutate a mutex-protected state snapshot, so the
  command worker and the Wi-Fi event handler never block on RMT.
- Consumers push state/events; the module never queries other subsystems, so it is a leaf
  that `command`, `networking`, and `main` can all depend on (no layering cycle). The `rgb`
  command body stays in `components/command/command.c` (hardware verbs); `networking` pushes
  Wi-Fi/HTTP events via `led_notify()`; `main` fires the boot confirmation flash.
- Auto status follows Wi-Fi state (connecting=amber pulse, connected=green, disconnected=red
  blink, watchdog timeout=red pulse); HTTP server start flashes blue. In manual mode events
  are transient (`P4_CONFIG_LED_NOTIFY_MS`). GPIO26 is a reserved critical line in the board
  pin table, so `pwm`/`freq`/`adc`/`i2c`/`spi` refuse it.
- All tunables live in `P4_CONFIG_LED_*` (documented in `p4minishell_config.yaml`).

### Audio Rules (beep / tone / wavplay / volume)
- ALL audio logic lives in `components/audio/` (`audio.h` / `audio.c`): the ES8311 codec handle,
  speaker volume (`audio_set_volume` / `audio_get_volume`), and the background playback engine.
  The codec is mono 16-bit at 22050 Hz (the BSP default) — `esp_codec_dev_open({22050,1,16})` /
  `write` / `close` is the whole output path, driven through `bsp_audio_codec_speaker_init()`.
- The command layer (`components/command/command.c`) only dispatches `beep` / `tone` /
  `wavplay` / `audio` / `volume`, parses arguments, and calls the `audio.h` API. Keep audio
  logic out of command.c; add it to the `audio` component instead.
- `beep`, `tone`, and `wavplay` MUST play in the background: they post a request to the
  component's `audio_play` task and return immediately, so batch files never block. One sound
  at a time (a second request is refused with `audio busy`); `audio stop` sets a stop flag the
  play loop checks; all audio commands return an ERRORLEVEL (0 started / 1 busy|io / 2 usage).
- Tone PCM is generated in heap chunks (`P4_CONFIG_TONE_CHUNK_SAMPLES`) with a short
  fade-in/out; WAVs (16-bit PCM mono/stereo at 22050/44100 Hz) are streamed from SD, stereo
  mixed to mono and 44100 decimated to 22050, bounded by `P4_CONFIG_WAV_MAX_BYTES`. Keep all
  chunk buffers on the heap, never on the audio task's stack. Call `audio_stop()` before
  `sleep`/`deepsleep` so playback cannot continue into sleep.

### Peripheral Toolkit Rules (pwm / freq / adc / i2c / spi)
- The `pwm`, `freq`, `adc`, `i2c`, and `spi` commands live in `components/command/command.c`
  (hardware verbs belong to the command module). All of them gate every pin through
  `shell_pin_is_reserved()` (the `critical` entries of `s_gpio_pins`), so the board's active
  I2C/I2S/SDIO/display/SD lines can never be repurposed.
- PWM/square waves use the legacy LEDC API (`driver/ledc.h`). The toolkit must use LEDC
  timers 0/2/3 and channels excluding `BOARD_CFG_DISPLAY_BRIGHTNESS_LEDC_CH` (the backlight
  owns channel 1 / timer 1). It MUST use the same global LEDC clock the backlight
  auto-selects (`P4_CONFIG_PWM_CLK_SOURCE` = SOC_MOD_CLK_XTAL, 40 MHz) and pick the duty
  resolution from `P4_CONFIG_PWM_SRC_CLK_HZ` (40 MHz); a mismatch or an 80 MHz assumption
  fails with "timer clock conflict" / `div_param=0`.
- `adc <pin>` opens a fresh one-shot unit per read and deletes it (plus calibration) on every
  path. `adc status` enumerates pins from the SOC channel map (`soc/adc_channel.h`), never by
  probing arbitrary GPIOs with `adc_oneshot_io_to_channel` (that logs spurious errors), and
  live-samples each candidate so a busy ADC unit is reported unavailable.
- `i2c` reuses the board's shared bus handle (`bsp_i2c_get_handle()`) when pins match the BSP
  I2C pair, otherwise it creates a temporary `i2c_master` bus on a free port. Scans probe via
  normal device transactions (add device + one-byte transmit), NEVER `i2c_master_probe()`,
  which reprograms the shared controller's timing/interrupts and disrupts the GT911 touch.
- SPI is a reported hardware gap on this board: `spi status` reports the toolkit
  configuration, but the `loopback`/`peek`/`poke` verbs return an honest
  "unavailable on this board" error and MUST NOT call the SPI driver — initializing
  the SPI host on this P4 with the ESP-Hosted SDIO link active stalls the chip and
  drops USB-Serial-JTAG off the bus (verified with both SPI2 and SPI3, DMA on and
  off). Do not re-wire SPI transactions without first proving the host init does
  not stall the chip.

### Error Handling
- Friendly transcript messages for all failures
- 5-entry debug history buffer via debug command
- Healthy boot recorded in debug history, not as warning
- ESP-IDF error names in messages: esp_err_to_name()

### Hardware Bring-Up Rules (learned 2026-09-06, COM11 + CH340 UART)

- **Two USB paths, different powers.** The shell console is USB-Serial-JTAG (native USB, ex-COM11). The CH340 port shows ONLY the ROM bootloader + 2nd-stage logs, never the shell (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`). If the native device is absent from Device Manager, the app may be running fine with no visible console — check cables before assuming a boot failure.
- **Opening the serial port resets the board** (`rst:0x17 CHIP_USB_UART_RESET`). Every fresh `serial.Serial()` open reboots; always settle 10-12 s, `reset_input_buffer()`, and never infer state across separate opens. Long sessions MUST hold one open handle (see `tools/shell_session.py`).
- **Bootloader silence is config, not failure.** `CONFIG_ESP_CONSOLE_UART_NUM=-1` means no 2nd-stage logs on UART. ROM-only output after `entry` does NOT mean a dead flash — verify with `esptool read_flash` before tearing anything down.
- **Screenshot framing is byte-exact.** `screenshot` (no args) suspends the console reader and streams `BMPX` + LE32 size + raw BMP via `usb_serial_jtag_write_bytes` (no CRLF translation). Host: wait for `streaming`, read magic, size, exactly size bytes. A modal on screen blocks the worker, so the bare streaming `screenshot` is handled by the console-reader task via `modal_console_command` and DOES capture an open modal (the `screenshot <file>` form still runs on the worker). `tools/harness/grab_screenshot.py --port --out --crop-transcript` is the reference loop; convert BMP→PNG with Pillow to view.
- **`send`/`receive` protocol.** `receive <path> <size> [/crc]`: device prints `=== RX READY ===`, host streams chunks, device echoes `RX <cumulative>`, optional LE32 CRC-32 trailer, closes with `=== RX DONE ===`. `send`: `SDFX` + LE32 + raw + CRC-32 + `=== TX DONE ===`. `tools/harness` + `apps/companion/push_sd.py` implement the host side; do not reimplement it per task.
- **Panic triage in order.** (1) Capture the full `Backtrace:` line. (2) Decode with `riscv32-esp-elf-addr2line -e build/p4minishell.elf -f -C <pcs>` (rebuild first — PCs are only valid for the flashed ELF). (3) The worker task (`shell_cmd`) in `lv_obj_update_layout` means an unlocked LVGL call from a non-LVGL task — see M39. Panic markers to scan logs for: `Guru Meditation`, `Backtrace`, `assert failed`, `abort()`, `Task watchdog`, `Stack protection`.
- **Modal input routing over serial.** While a modal is open, serial lines go to `modal_handle_serial_line`, never the dispatcher: `dialog y`→0, `list 2`→index, `ask myname`→`ASK_RESULT`. `ask` needs the `serial_answered` guard (M37) so close does not clobber the answer with the empty textarea. The console reader (`shell.c`) MUST NOT queue a line while `modal_is_active()`: the worker is blocked in the modal, so a queued line cannot run until it closes, and a burst overflows the command queue and drops input (including the modal's own quit line) — route it to the surface and drop it there. The bare streaming `screenshot` is claimed first by `modal_console_command`.
- **Command queue payloads live in PSRAM.** `command_request_t` is one `P4_CONFIG_COMMAND_BYTES` (4096) block per queue slot; `shell_execute_command_async` allocates it with `heap_caps_malloc(MALLOC_CAP_SPIRAM)` (internal fallback) and the worker frees it with `heap_caps_free`, so a burst never fragments the internal DMA-capable heap. Queue depth `P4_CONFIG_COMMAND_QUEUE_DEPTH` (32). A full queue still drops loudly after `P4_CONFIG_COMMAND_QUEUE_SEND_TIMEOUT_MS`; the modal rule above is what keeps that from happening in practice.
- **Alarm checker is lazy.** `alarm_init()` runs on the first `alarm`/`cal` command, NOT in `command_init()` — eager start fragments the DMA heap before `usb_init()` and breaks USB HCD bring-up (M38, A/B-verified). Never move it earlier.
- **Port discipline for agents.** One holder per port: check `Get-Process python*` + command lines before opening; never fight another session for COM11. Detached builds go via `Start-Process idf.py build` with log redirect + poll; never `idf.py monitor` (it grabs the port).
- **Generated `sdkconfig` goes stale.** If a `sdkconfig.defaults` symbol shows "not set" in generated `sdkconfig` (e.g. `CONFIG_LV_FONT_UNSCII_16`), delete the generated file and rebuild — but back it up first; machine-local tweaks live only there. This repo sets `git config core.autocrlf false` (mixed LF/CRLF tree); do not re-enable it or every file churns.
- **Managed-component patches evaporate** on `idf.py update-dependencies`. The BSP + font diffs live in `tools/managed_patches.patch`; re-apply with `tools/reapply_managed_patches.ps1` (`-Check` dry-runs). The font file previously hid syntax errors behind a disabled guard — a newly enabled managed file MUST get a clean full build before anything else.

### Settings / Security / Form Rules (v0.38.x Preferences pass)
- **CONFIG.SYS is the ONE scalar-preference store.** The only writer is the `config`
  machinery (`config_directive_upsert` + `config_write_file`, exposed as
  `config_persist_set()` in `config_cmd.h`). Every owning command's `/save`
  (theme, font, header mode, cursor, timezone, owner, security). The OSK page is
  deliberately NOT persisted: the touch keyboard always starts in the letters
  page (`KEYBOARD_MODE_TEXT_LOWER`) on boot, and every ready-made modal resets it
  to letters in `surf_create_container()` (the editor owns the Nav page). Never
  restore a saved OSK page at boot.
  MUST persist through `config_persist_set`, never a private file. `sd:/APPS/SHELL.INI`
  is legacy: read once as a fallback (`config_get_saved` first), never written.
  `config /b` (and `config <KEY> /b`) is the machine-readable listing for the
  Preferences app. Do NOT add a second settings registry or a native Preferences GUI.
- **The Preferences GUI is a batch app.** `apps/companion/SET.BAT` drives the
  owning shell commands only; it owns no settings file. `LIB.BAT :load_settings`
  is a no-op (boot restores from CONFIG.SYS). Add settings by adding a directive
  in `boot.c` + a `config_persist_set` call in the owning command, then a SET.BAT
  page. Batch menu indices are 0-based (list ERRORLEVEL); modal **serial**
  selection is 1-based (index+1) � a test-harness gotcha.
- **`form`** (`modal_surf.c`) is a general multi-field modal primitive
  (`text|password|check|select|range`, values bound to env vars, `/t:secs`).
  It is a shell primitive, not a Preferences special case. New full-screen
  surfaces stay `modal_surface_t` on the shared runtime.
- **Device security** (`security_commands.c`): owner + PBKDF2-SHA256 salted
  passcode hash + conceal policy all live in CONFIG.SYS (`OWNER_*`/`SECURITY_*`).
  A passcode + `SECURITY_BOOTLOCK=on` locks the dispatcher; the gate
  (`security_command_allowed`) is checked at the top of `shell_execute_command_core`
  with a tiny allowlist. Boot scripting runs before the lock engages, so
  CONFIG.SYS/AUTOEXEC are never blocked. `db /reveal`, `gfind`, and `export` must
  consult `security_can_reveal_private()`/`security_conceal_mode()`. Recovery is
  deleting the `SECURITY_*` lines on the SD card.
- **Passcode/owner config hooks**: `security_init()` runs in `command_init()`;
  `security_engage_boot_lock()` is called at the end of `boot_script_apply()`.
  Passcode entry uses `modal_ask_run(..., password=true)` for the masked field +
  on-screen keyboard (touch-first); `shell_read_line_hidden` also works (fixed,
  see the console CRLF rule below).
- **Console CRLF artifact (fixed).** The USB-Serial-JTAG console VFS maps CR to
  LF, so a host `...\r\n` arrives as TWO LFs: the first ends the command line,
  the second is a stray empty line. That stray LF used to be forwarded into the
  next key wait and satisfy it immediately — `set /p`, `set /p /P`, `pause`,
  `choice`, `crypt /ask`, and `shell_read_line_hidden` would all return an empty
  line. The console reader now timestamps the last completed line
  (`line_done_us`) and drops any leading LF that arrives within
  `line_artifact_grace_us` (100 ms), in BOTH the command and key-wait paths.
  Keep that guard when touching `shell_uart_console_task()`; do not "simplify"
  it away. Verified on COM3: 5× echoed and 5× hidden `set /p` loops capture,
  and `crypt lock|unlock /ask` (hidden) round-trips.

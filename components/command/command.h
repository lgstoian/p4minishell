/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file command.h
 * @brief Command parser and dispatcher for P4MiniShell.
 *
 * Owns the command execution pipeline — variable-expansion entry, output
 * redirection, parsing, dispatch, and the worker task — plus the commands
 * that are not tied to the filesystem or the batch language. Uses shell.c for
 * transcript output and debug logging.
 *
 * Features:
 *   - Command dispatch table with family routing (wifi, bluetooth, usb, c6ota, sd)
 *   - Dedicated worker task for command execution
 *   - Hardware commands: brightness, rotate, battery, volume, gpio, display
 *   - System commands: reboot, clear/cls, prompt, date, time
 *   - Output redirection (> / >>) applied around every dispatch
 *
 * Delegated ownership:
 *   - `components/storage/` — SD sessions, path resolution, current working
 *     directory, and every DOS file command
 *   - `components/batch/` — batch engine, environment variables, PATH,
 *     variable expansion, errorlevel, and the batch language commands
 *   - `components/shell/` — transcript, history, debug log, UART console,
 *     input line, and the system info commands
 *
 * Architecture:
 *   This module is the only dispatcher. main.c orchestrates boot and routes
 *   LVGL input events; it contains no command implementations.
 */

#ifndef P4MINISHELL_COMMAND_H
#define P4MINISHELL_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "esp_sleep.h"
#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * COMMAND EXECUTION
 * ======================================================================== */

/**
 * Execute a command line: expand variables, apply output redirection, and
 * dispatch to the matching built-in. Runs synchronously on the calling task.
 *
 * @param command  Null-terminated command string (copied internally).
 */
void shell_execute_command(char *command);

/**
 * Dispatch an already-expanded command line to its built-in handler.
 * Does not perform variable expansion or output redirection.
 *
 * @param command  Null-terminated command string (modified in-place).
 * @return true if a command was recognized and executed.
 */
bool shell_execute_command_core(char *command);

/**
 * Queue a command for execution on the dedicated worker task.
 * Used by the LVGL input path so heavy commands never run on the LVGL
 * event-callback stack.
 *
 * @param command  Null-terminated command string (copied into the request).
 */
void shell_execute_command_async(char *command);

/**
 * Check if the C6 OTA module has a pending confirmation.
 * @return true if OTA confirmation is pending.
 */
bool shell_command_ota_is_pending(void);

/* ========================================================================
 * SHELL STATE ACCESSORS
 * ======================================================================== */

/**
 * Get the last speaker volume applied through the `volume` command.
 * @return Volume percentage in the range 0..100.
 */
int command_get_volume_percent(void);

/**
 * Set the speaker volume directly (boot CONFIG.SYS VOLUME= directive).
 *
 * Clamps @p percent to 0..100 and mirrors the validation in the interactive
 * `volume` command. No transcript output is produced.
 *
 * @param percent  Volume percentage in the range 0..100.
 */
void command_set_volume(int percent);

/**
 * Read the battery ADC and convert it to millivolts and a charge percentage.
 * Any output pointer may be NULL when that value is not needed.
 *
 * @param battery_mv_out  Scaled battery voltage in millivolts.
 * @param percent_out     Charge estimate in the range 0..100.
 * @param raw_out         Raw ADC sample.
 * @param gpio_mv_out     Voltage measured at the ADC pin before divider scaling.
 * @return ESP_OK on success, or the ESP-IDF error from ADC setup/read.
 */
esp_err_t command_battery_read(int *battery_mv_out, int *percent_out, int *raw_out, int *gpio_mv_out);

/** True when the pack is on external charge power (gauge boards). */
bool command_battery_is_charging(void);

/* ========================================================================
 * POWER / IDLE
 * ======================================================================== */

/**
 * Configure the idle display-off timeout (seconds; 0 disables). The display
 * backlight turns off after this long without user input and wakes on the
 * next touch, USB keyboard/mouse, or serial command.
 */
void shell_power_set_idle_timeout(int seconds);

/** Current idle display-off timeout in seconds (0 = disabled). */
int shell_power_get_idle_timeout(void);

/**
 * Report user input: resets the idle clock and wakes the display if the idle
 * timer had switched it off. Safe from any task.
 */
void shell_power_notify_activity(void);

/**
 * Idle timer tick, called on the LVGL task by the periodic header refresh.
 * Switches the display off once the idle timeout elapses and wakes it on a
 * touch press while it is off.
 */
void shell_power_idle_tick(void);

/**
 * Milliseconds until the idle display-off deadline. Returns 0 when idle-off is
 * disabled or the display is already off. Used by the adaptive header refresh
 * scheduler so it never sleeps past the deadline.
 */
int shell_power_ms_until_idle_off(void);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/**
 * Initialize the command module. Brings up `storage` and `batch`, registers
 * the batch pipeline hook and the shell operations table. Call once at boot.
 */
void command_init(void);

/** Check if the command module is initialized. */
bool command_is_initialized(void);

/** Load the recall history profile from SD (boot, first SD mount). */
void command_history_autoload(void);

/** Write the recall history profile to SD now (reboot/shutdown flush). */
void command_history_save_now(void);

/* ========================================================================
 * APP DISCOVERY / LAUNCH
 * ======================================================================== */

/**
 * Resolve and run a `.bat` app by name (PATH resolution, then the conventional
 * `sd:/APPS` directory). Used by the `launch` command and by the boot-time
 * "offer to launch" hook. Runs synchronously on the calling task.
 *
 * @param name  App name, with or without the `.bat` extension.
 * @return true when the app was found and launched.
 */
bool shell_launch_app(const char *name);

/* ========================================================================
 * AUDIO (`volume`, `beep`, `tone`, `wavplay`, `audio`)
 * ======================================================================== */

/**
 * Audio verbs: argument parsing and transcript output only. All codec,
 * volume, and background-playback logic lives in components/audio
 * (`audio.h`). Each sets ERRORLEVEL 0 (ok/started) / 1 (busy/failure) /
 * 2 (usage).
 */
void shell_command_volume(int argc, char **argv);
void shell_command_beep(int argc, char **argv);
void shell_command_tone(int argc, char **argv);
void shell_command_wavplay(int argc, char **argv);
void shell_command_audio(int argc, char **argv);

/* ========================================================================
 * TUI AND MODAL-SURFACE VERBS (`draw`, `anchor`, `browse`, `dialog`,
 * `list`, `ask`, `view`, `hexview`, `color`, `locate`, `tui`)
 * ======================================================================== */

/**
 * TUI/modal verbs, implemented in tui_commands.c (moved out of command.c
 * and batch.c in v0.35.3). `browse_batch` is the `/t:`/`/v:` form of
 * `browse` used by batch files. Each sets ERRORLEVEL per command.md.
 */
bool shell_command_draw(int argc, char **argv);
bool shell_command_gfx(int argc, char **argv);
bool shell_command_plot(int argc, char **argv);

/** Shared `draw` helpers reused by the `plot` coordinate layer
 * (implemented in tui_commands.c): background-job refusal, DOS color
 * parse (0..16 passthrough, larger quantized), hold-aware TUI flush. */
bool draw_require_foreground(const char *verb);
uint8_t draw_color_arg(const char *s, uint8_t fallback);
void draw_maybe_flush(void);

/** Shared `gfx` canvas accessors reused by `plot` (gfx_commands.c).
 * The `gfx` verbs keep owning allocation, display glue, and sprites. */
uint16_t gfx_canvas_parse_color(const char *s, uint16_t fallback);
bool gfx_canvas_is_open(void);
gfx_surface_t *gfx_canvas_surface(void);

/** Force-close the gfx canvas and restore the transcript if open. Used to tear
 *  down a foreground surface left behind by an aborted batch (Stop / Ctrl+C),
 *  so the shell prompt becomes visible again. Safe to call when none is open. */
void gfx_force_close(void);

/** Tear down any foreground surface (gfx canvas, TUI cell buffer, app mode)
 *  left open by an aborted batch (Stop / Ctrl+C) or a display-rotation rebuild,
 *  so the shell prompt becomes visible again. Idempotent. */
void command_close_foreground_surfaces(void);

/** Frame-coalescing flag for `draw hold on|off` (tui_commands.c). True
 * while per-verb flushes are suppressed; headless-safe unit-test hook. */
bool draw_hold_active(void);
bool shell_command_anchor(int argc, char **argv);
void shell_command_browse(int argc, char **argv);
void shell_command_dialog(int argc, char **argv);
void shell_command_list(int argc, char **argv);
void shell_command_ask(int argc, char **argv);
void shell_command_form(int argc, char **argv);
void shell_command_browse_batch(int argc, char **argv);
void shell_command_view(int argc, char **argv);
void shell_command_open(int argc, char **argv);
void shell_command_hexview(int argc, char **argv);
void shell_command_image(int argc, char **argv);

/**
 * Load a whole file into a PSRAM-first heap buffer under a guarded SD
 * session. The single shared loader behind `image info`, `draw image`,
 * `gfx image`, and `gfx load`; the caller decodes and frees the buffer.
 *
 * @param path_arg  User-supplied path (resolved internally).
 * @param verb      Error-message prefix, e.g. "gfx image".
 * @param max_bytes Upper bound on the file size.
 * @param out_buf   Receives the heap buffer (caller heap_caps_free()s it).
 * @param out_size  Receives the byte count.
 * @return 0 on success, otherwise an ERRORLEVEL (1 I/O, 2 usage/path) after
 *         printing the reason.
 */
int command_load_file_psram(const char *path_arg, const char *verb,
                            uint32_t max_bytes, uint8_t **out_buf,
                            size_t *out_size);

/** Bounded string copy with explicit truncation (never -Wformat-truncation).
 *  Shared by the `export`/`import` interchange writers. */
void command_copy_trunc(char *dst, size_t dst_size, const char *src);

void shell_command_color(int argc, char **argv);
void shell_command_locate(int argc, char **argv);
void shell_command_tui(int argc, char **argv);

/* ========================================================================
 * FONT VERBS (`font info|coverage|list|set|size`; TTF sizes in Phase 2)
 * ======================================================================== */

/**
 * Font verbs, implemented in font_commands.c. `set` switches a role live
 * (+/save to SHELL.INI). Each sets ERRORLEVEL per command.md.
 */
void shell_command_font(int argc, char **argv);

/** Best-effort boot restore of the saved font choice (silent without SD).
 * Called by boot_on_sd_first_mount(), where the mount is guaranteed.
 * @return true when the restore completed (or there was nothing saved);
 *         false when the SD was not readable, so the caller retries on the
 *         next mount instead of skipping the restore for the whole boot. */
bool font_restore_saved(void);

/** `theme` verb: list / show / set the active UI theme (font_commands.c). */
void shell_command_theme(int argc, char **argv);

/** `header` verb: layout mode + visibility + status (header_commands.c). */
void shell_command_header(int argc, char **argv);
/** Best-effort boot restore of the saved header mode (CONFIG.SYS). */
void header_restore_saved(void);

/** Load persisted security/owner state from CONFIG.SYS (security_commands.c).
 *  Deferred to the boot script so security_init() never mounts the SD card
 *  during init (which would beat the shell's first-mount hook). Idempotent. */
void security_load_saved(void);

/** Engage the boot passcode lock once the boot script has finished
 * (security_commands.c). Called by boot_script_apply(). */
void security_engage_boot_lock(void);

/** Markdown rendering (`markdown <file> | -e <text> | on | off`). */
void shell_command_markdown(int argc, char **argv);

/** JSON validate/pretty (`json validate|pretty <file>`). */
void shell_command_json(int argc, char **argv);

/** CSV grid verbs (`csv rows|cols|cell|eval <file>`, csv_commands.c).
 *  Parsing is the pure csv_split_line() core in components/storage;
 *  =EXPR evaluation reuses the calc engine here in the command layer.
 *  Returns an ERRORLEVEL: 0 ok, 1 none/error, 2 usage. */
int shell_command_csv(int argc, char **argv);

/** Portable interchange (`export <db NAME | alarms> <csv|json|txt> <file>`,
 *  export_commands.c). Renders the structured stores through the existing
 *  db/alarm APIs and writes atomically. Returns 0 exported, 1 empty,
 *  2 usage/I-O. */
int shell_command_export(int argc, char **argv);

/** Portable interchange in (`import <db NAME | alarms> <file> <fmt>`,
 *  import_commands.c). db takes csv|json|vcf, alarms takes csv|json|ics;
 *  shapes match `export` output for round-trips, ids are always fresh.
 *  Returns 0 imported, 1 empty, 2 usage/I-O. */
int shell_command_import(int argc, char **argv);

/** USTAR backup archives (`archive create|extract|list|verify` and the
 *  `backup` alias; archive_commands.c over components/archive).
 *  argv[0] selects the family. Returns 0 ok, 1 nothing/mismatch,
 *  2 usage/I-O. */
int shell_command_archive(int argc, char **argv);
/** Split a vCard/iCalendar content line ("NAME;params:value") into the
 *  property name (group prefix stripped, params dropped) and the raw value.
 *  Pure, unit-tested. */
bool import_vcf_prop_split(const char *line, char *name_out, size_t name_size,
                           const char **value_out);

/** Unescape a JSON string body (no quotes) into @p dst. Handles \" \\ \/
 *  \b \f \n \r \t and \uXXXX (BMP as UTF-8, lone surrogates as '?').
 *  Pure, unit-tested. @return bytes written excluding NUL. */
size_t import_json_unescape(const char *src, size_t len, char *dst, size_t dst_size);

/** Parse an iCalendar DATE-TIME ("YYYYMMDDTHHMMSS", date-only, trailing Z
 *  accepted as device-local) into @p out with full range checks (no mktime,
 *  so TZ-independent). Pure, unit-tested. */
bool import_ics_datetime(const char *text, struct tm *out);

/** Password file encryption (`crypt lock|unlock <src> <dst> [/p:pass|/ask]`,
 *  crypt_commands.c). AES-256-GCM with a PBKDF2-SHA256 key; secrets never
 *  print. Returns 0 ok, 1 crypto/IO failure, 2 usage. */
int shell_command_crypt(int argc, char **argv);

/** USB CDC-ACM serial verbs (`usb userial ...`, userial_commands.c) over the
 *  byte API in components/usb. Expects argv[0]=="usb", argv[1]=="userial",
 *  argv[2]=verb. Returns 0 ok, 1 state/IO failure, 2 usage. */
int shell_command_userial(int argc, char **argv);

/** Derive the 32-byte file key from @p pass and @p salt (PBKDF2-SHA256).
 *  Pure, unit-tested. @return 0 on success. */
int crypt_derive_key(const char *pass, const uint8_t *salt, uint8_t *key_out);

/** One-shot whole-buffer envelope (`P4CRYPT1` magic + salt + nonce +
 *  ciphertext + tag). Pure, unit-tested. @p out must hold the plaintext
 *  length plus the 45-byte envelope overhead. */
int crypt_encrypt_mem(const uint8_t *in, size_t len, const char *pass,
                      uint8_t *out, size_t out_size, size_t *out_len);

/** Inverse of crypt_encrypt_mem. @return 0 on success, 1 on a bad password,
 *  corrupt input, or a short buffer (never distinguished). */
int crypt_decrypt_mem(const uint8_t *in, size_t len, const char *pass,
                      uint8_t *out, size_t out_size, size_t *out_len);

/** Substitute `R<row>C<col>` references (case-insensitive) in @p expr with
 *  the numeric values of @p cells (flat rows x cols grid of heap strings;
 *  a non-numeric or out-of-range reference reads as 0). Pure, unit-tested. */
void csv_substitute_refs(const char *expr, char **cells, int rows, int cols,
                         char *out, size_t out_size);

/** Format one CSV field with RFC-4180 quoting. Pure, unit-tested, shared
 *  with `export` (single quoting implementation). With @p out NULL returns
 *  the bytes needed including the terminator. */
size_t csv_format_field(const char *text, char *out, size_t out_size);
/** Validate JSON text (testable core): true when structurally valid.
 * @p err receives "msg at line L col C" on failure (may be NULL). */
bool json_validate_text(const char *text, size_t len, char *err, size_t err_size);

/** Pretty-print JSON text with 2-space indent (testable core).
 * @return bytes written excluding NUL (0 = invalid; @p err set). */
size_t json_pretty_text(const char *text, size_t len, char *out, size_t out_size,
                        char *err, size_t err_size);

/* ========================================================================
 * POWER / DISPLAY / BATTERY (`brightness`, `rotate`, `battery`, `power`,
 * `sleep`, `deepsleep`)
 * ======================================================================== */

/**
 * Power verbs, implemented in power_commands.c (moved out of command.c in
 * v0.35.4). Owns the battery ADC state and the idle display-off state. The
 * ops-table backings (command_battery_read, shell_power_* API) live there too.
 */
void shell_command_brightness(int argc, char **argv);
void shell_command_rotate(int argc, char **argv);
void shell_command_battery(int argc, char **argv);
void shell_command_power(int argc, char **argv);
void shell_command_sleep(int argc, char **argv);
void shell_command_deepsleep(int argc, char **argv);
void shell_command_shutdown(int argc, char **argv);

/**
 * Parse `sleep`/`deepsleep` seconds (pure, unit-tested): no argument yields
 * the default, values clamp to the max, negatives and garbage fail.
 */
bool shell_power_parse_seconds(int argc, char **argv, uint32_t *seconds_out);

/**
 * Wake-cause enum to short string (pure, unit-tested, "none" by default).
 */
const char *shell_power_wake_cause_string(esp_sleep_wakeup_cause_t cause);

/* ========================================================================
 * PERIPHERAL TOOLKIT (`gpio`, `pwm`, `freq`, `adc`, `i2c`, `spi`, `rgb`,
 * `camera`)
 * ======================================================================== */

/**
 * Peripheral toolkit verbs, implemented in periph_commands.c (moved out of
 * command.c in v0.35.4). Every verb gates its pins through the board GPIO
 * table, so active I2C/I2S/SDIO/display/SD lines can never be repurposed.
 */
void shell_execute_gpio_command(int argc, char **argv);
void shell_execute_pwm_command(int argc, char **argv);
void shell_execute_freq_command(int argc, char **argv);
void shell_execute_adc_command(int argc, char **argv);
void shell_execute_i2c_command(int argc, char **argv);
void shell_execute_spi_command(int argc, char **argv);
void shell_execute_rgb_command(int argc, char **argv);
void shell_execute_imu_command(int argc, char **argv);
void shell_execute_camera_command(int argc, char **argv);

/* ========================================================================
 * SERIAL TRANSFER AND SCREENSHOT (`screenshot`, `receive`, `send`)
 * ======================================================================== */

/**
 * Serial verbs, implemented in serial_commands.c (moved out of command.c in
 * v0.35.4). Binary host<->device transfer over USB-Serial-JTAG plus LVGL
 * screen capture as BMP. Each sets ERRORLEVEL 0 (ok) / 1 (I/O) / 2 (usage).
 */
void shell_command_screenshot(int argc, char **argv);
void shell_command_receive(int argc, char **argv);
void shell_command_send(int argc, char **argv);

/**
 * Fill 54 BMP header bytes for a WxH 24-bit image (pure, unit-tested).
 */
int screenshot_write_bmp_headers(uint8_t *buf, uint32_t width, uint32_t height);

/**
 * Incremental CRC-32 update (IEEE 802.3, reflected poly 0xEDB88320;
 * implemented in serial_commands.c). The firmware's single CRC primitive:
 * `receive` transfer verification and the `crc32`/`asset` verbs share it.
 * Start the accumulator at 0xFFFFFFFF and invert at the end (zlib parity).
 */
uint32_t shell_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* ========================================================================
 * ASSET MANIFESTS (`crc32`, `asset`)
 * ======================================================================== */

/**
 * One-shot CRC-32 over a memory buffer (pure, unit-tested).
 */
uint32_t asset_crc32_data(const uint8_t *data, size_t len);

/**
 * Parse one `APPS/<APP>.ASSETS` manifest line (`path=HEXCRC`, `#`/`;`
 * comments and blanks skipped). Pure, unit-tested. @return true with the
 * SD-relative path and expected CRC; false for comments/blanks/malformed
 * lines (caller skips comments, fails malformed ones).
 */
bool asset_parse_line(const char *line, char *path_out, size_t path_size,
                      uint32_t *crc_out);

/**
 * `crc32 <path>` — print the file's CRC-32. `asset check|list <app>` —
 * verify/list an app's manifest. Implemented in asset_commands.c.
 * Sets ERRORLEVEL 0 (ok/all match) / 1 (I/O or any mismatch) / 2 (usage).
 */
void shell_command_crc32(int argc, char **argv);
void shell_command_asset(int argc, char **argv);

/**
 * Shared asset helpers (asset_commands.c), reused by `pkg` so there is exactly
 * one manifest/CRC implementation:
 *  - asset_app_ok(): validate an app name ([A-Za-z0-9_-]+).
 *  - asset_crc_file(): CRC-32 of one resolved file.
 *  - asset_verify_app(): verify/list `APPS/<app>.ASSETS`; sets + returns
 *    ERRORLEVEL 0 ok / 1 missing|mismatch|empty / 2 usage|no SD.
 */
bool asset_app_ok(const char *app);
bool asset_crc_file(const char *resolved, uint32_t *crc_out);
int  asset_verify_app(const char *tag, const char *app, bool list_only);

/**
 * `pkg` — SD app packages over `APPS/<APP>.APPINFO`
 * (title/description/version, plus type=/abi= for native bundles — see
 * docs/native_packaging.md) + `APPS/<APP>.ASSETS` (contents manifest),
 * installed from a `PKGS/<APP>/` bundle. Verbs: list, info, verify, check,
 * install, remove. Implemented in pkg_commands.c; ERRORLEVEL 0 ok / 1 some
 * failure / 2 usage.
 */
void shell_command_pkg(int argc, char **argv);

/**
 * Pure helper (pkg_commands.c, unit-tested): true when @p filename is a
 * `NAME.APPINFO` file, writing the uppercased NAME to @p out.
 */
bool pkg_app_name_from_appinfo(const char *filename, char *out, size_t size);

/** App-name buffers for the completion providers (pkg_commands.c). */
#define COMMAND_APP_NAME_BYTES 32
#define COMMAND_APP_MAX        48

/**
 * Completion sources (pkg_commands.c): installed app names from the
 * APPS directory APPINFO files, and available bundle names from the
 * PKGS bundle directories.
 * @return the number written (0 when no SD / none).
 */
int pkg_list_installed(char names[][COMMAND_APP_NAME_BYTES], int max);
int pkg_list_available(char names[][COMMAND_APP_NAME_BYTES], int max);

/**
 * Tab-completion provider bound to the shell `complete_line` op. Fills @p out
 * with the @p match_index-th match for the last word of @p line and returns
 * the total number of matches. The candidate set derives from the shell help
 * table (command names + usage tokens), aliases, installed apps, and paths.
 */
int command_complete_line(const char *line, int match_index, char *out,
                          size_t out_size);

/**
 * SD-free best-match provider bound to the shell `ghost_line` op. Fills @p out
 * with the single best completion for @p line (no paths, no I/O) and returns
 * the number of matches found.
 */
int command_ghost_line(const char *line, char *out, size_t out_size);

/* ========================================================================
 * DATABASE (`db`)
 * ======================================================================== */

/**
 * `db` — Palm-OS-style SD-backed record store. argv-verb dispatcher over the
 * `db` core (components/db). See command.md for the full verb surface.
 * Sets ERRORLEVEL 0 (ok/found) / 1 (not found / empty) / 2 (usage / I/O).
 */
void shell_command_db(int argc, char **argv);

/**
 * `alarm` — SD-persisted alarms (components/alarm). argv-verb dispatcher:
 * add / list / del / enable / disable / status / purge. Sets ERRORLEVEL
 * 0 (ok) / 1 (not found / none due) / 2 (usage / I/O).
 */
void shell_command_alarm(int argc, char **argv);

/**
 * `cal` — thin calendar view over the alarm store: `today`, `next`, or
 * `YYYY-MM`. Sets ERRORLEVEL 0/1/2.
 */
void shell_command_cal(int argc, char **argv);

/**
 * `gfind` — Global Find across the structured "apps" (Palm-style): db records
 * and alarm/calendar events, searched through the existing db_find / alarm_list
 * APIs (no parallel search logic). `/b` = bare machine-readable lines,
 * `/i` = case-insensitive, `/db:name` restricts to one database,
 * `/noalarms` / `/nodb` exclude a store. ERRORLEVEL 0 (matches) / 1 (none) /
 * 2 (usage / I/O).
 */
void shell_command_gfind(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_COMMAND_H */

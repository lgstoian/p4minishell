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
#include "esp_err.h"
#include "esp_sleep.h"

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
bool shell_command_anchor(int argc, char **argv);
void shell_command_browse(int argc, char **argv);
void shell_command_dialog(int argc, char **argv);
void shell_command_list(int argc, char **argv);
void shell_command_ask(int argc, char **argv);
void shell_command_browse_batch(int argc, char **argv);
void shell_command_view(int argc, char **argv);
void shell_command_hexview(int argc, char **argv);
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
 * Called by boot_on_sd_first_mount(), where the mount is guaranteed. */
void font_restore_saved(void);

/** Theme stub (`theme show` prints the active table; switching later). */
void shell_command_theme(int argc, char **argv);

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

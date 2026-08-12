/**
 * @file boot.c
 * @brief DOS-style boot scripting: CONFIG.SYS parser and AUTOEXEC.BAT runner.
 *
 * On every boot the firmware looks for CONFIG.SYS and AUTOEXEC.BAT on the SD
 * card root. When either is missing and P4_CONFIG_BOOT_GENERATE_DEFAULTS is
 * set, default files are written once. CONFIG.SYS directives are parsed and
 * applied, then AUTOEXEC.BAT is run through the normal batch pipeline.
 *
 * Hardware directives are applied by executing their command-line equivalent
 * through the batch pipeline, so every existing validation path is reused and
 * no private state is reached into. State-only directives (Wi-Fi target
 * credentials, autoconnect policy, the echo default) use the small set of
 * accessors the owning modules expose.
 */

#include "boot.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "bsp/esp-bsp.h"

#include "p4minishell_config.h"
#include "storage.h"
#include "batch.h"
#include "command.h"
#include "display.h"
#include "networking.h"
#include "usb.h"
#include "shell.h"
#include "ansi_palette.h"

/* ========================================================================
 * DEFAULT TEMPLATES
 * ======================================================================== */

#define BOOT_CFG_HEADER \
    "; P4MiniShell configuration file.\n" \
    "; Lines starting with ; or REM are comments. Keywords are case-insensitive.\n" \
    "; Classic DOS directives:\n" \
    ";   SET name=value            Set an environment variable\n" \
    ";   PATH=dir1;dir2;...        Set the command search PATH\n" \
    ";   PROMPT=template           Set the prompt template ($p $g etc.)\n" \
    ";   ECHO ON|OFF               Set the batch echo default\n" \
    ";   NAME=VALUE                Any other KEY=VALUE sets an environment variable\n"

#define BOOT_CFG_DISPLAY \
    "\n; Display & audio:\n" \
    ";   ROTATE=0|90|180|270       Set display rotation\n" \
    ";   BRIGHTNESS=0-100          Set backlight brightness\n" \
    ";   DISPLAY_POWER=ON|OFF|SLEEP  Set display power state\n" \
    ";   VOLUME=0-100              Set speaker volume\n" \
    ";   RGB=<r>,<g>,<b>|#RRGGBB|<effect>[,speed]|OFF|AUTO,<ON|OFF>  Set the WS2812 status LED\n"

#define BOOT_CFG_NETWORK \
    "\n; Wi-Fi (station only):\n" \
    ";   WIFI_SSID=...              Target SSID for auto-connect\n" \
    ";   WIFI_PASSWORD=...          Target password (never echoed/logged)\n" \
    ";   WIFI_AUTOCONNECT=ON|OFF    Control watchdog auto-retry\n" \
    "\n; Bluetooth:\n" \
    ";   BLUETOOTH=ON|OFF           Enable/disable hosted BLE\n" \
    ";   BT_ADVERTISE=ON|OFF        Start/stop advertising\n"

#define BOOT_CFG_USB_GPIO \
    "\n; USB host:\n" \
    ";   USB_KEYBOARD=ON|OFF        Enable/disable HID keyboard\n" \
    ";   USB_MOUSE=ON|OFF           Enable/disable HID mouse echo\n" \
    "\n; GPIO (output-only, safe pins only):\n" \
    ";   GPIO <n> = OUT [HIGH|LOW]   Set initial level at a safe pin\n" \
    "; Unknown KEY=VALUE lines set an environment variable; unknown keywords\n" \
    "; without a value produce a single muted warning and are skipped.\n"

#define BOOT_CFG_DEFAULT BOOT_CFG_HEADER BOOT_CFG_DISPLAY BOOT_CFG_NETWORK \
    BOOT_CFG_USB_GPIO

#define BOOT_BAT_HEADER \
    "@echo off\n" \
    "REM P4MiniShell startup batch file.\n" \
    "REM Runs with full batch power: if/for/goto/call, pipes, redirection.\n"

#define BOOT_BAT_DEFAULT BOOT_BAT_HEADER \
    "echo P4MiniShell boot complete.\n"

/* ========================================================================
 * TAG
 * ======================================================================== */

#define BOOT_TAG    P4_CONFIG_SHELL_TAG

/* ========================================================================
 * HELPERS
 * ======================================================================== */

#define BOOT_CMD_LEN 48

static void boot_warn_unknown(const char *directive)
{
    shell_transcript_appendf_ansi(SH_MUTE "config: unknown directive \"%s\", skipped\n" SH_RST,
                                  directive != NULL ? directive : "(null)");
    shell_record_warningf("boot", "Unknown CONFIG.SYS directive: %s", directive != NULL ? directive : "(null)");
}

/** Trim leading/trailing whitespace in place. */
static char *boot_trim(char *text)
{
    char *end;

    if (text == NULL) {
        return text;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        end--;
    }
    *(end + 1) = '\0';
    return text;
}

/** Case-insensitive prefix match: does @p text start with @p kw? */
static bool boot_starts_with_ci(const char *text, const char *kw)
{
    size_t i;

    if (text == NULL || kw == NULL) {
        return false;
    }
    for (i = 0; kw[i] != '\0'; i++) {
        if (text[i] == '\0' || tolower((unsigned char)text[i]) != tolower((unsigned char)kw[i])) {
            return false;
        }
    }
    return true;
}

/** Apply a hardware directive by executing its command-line equivalent. */
static void boot_exec(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') {
        return;
    }
    batch_boot_execute_command(cmd);
}

/* ========================================================================
 * FILE EXISTENCE / GENERATION
 * ======================================================================== */

/**
 * Ensure a boot file exists, generating a default when configured to.
 * @return true when the file exists (or was created), false otherwise.
 */
static bool boot_ensure_file(const char *filename, const char *default_contents)
{
    FILE *file;

    if (filename == NULL || default_contents == NULL) {
        return false;
    }

    file = fopen(filename, "r");
    if (file != NULL) {
        fclose(file);
        ESP_LOGI(BOOT_TAG, "Boot file exists: %s", filename);
        return true;
    }

    if (!P4_CONFIG_BOOT_GENERATE_DEFAULTS) {
        ESP_LOGI(BOOT_TAG, "Boot file missing, generation disabled: %s", filename);
        return false;
    }

    if (!storage_check_free_space(strlen(default_contents) + 64, 0, filename)) {
        shell_print_warning("boot: cannot generate %s (storage full or unavailable)", filename);
        shell_record_warningf("boot", "Cannot generate %s (storage issue)", filename);
        return false;
    }

    file = fopen(filename, "w");
    if (file == NULL) {
        shell_print_warning("boot: cannot create %s", filename);
        shell_record_warningf("boot", "Cannot create %s", filename);
        return false;
    }

    if (fwrite(default_contents, 1, strlen(default_contents), file) != strlen(default_contents)) {
        fclose(file);
        shell_print_warning("boot: write failed for %s", filename);
        shell_record_warningf("boot", "Write failed for %s", filename);
        remove(filename);
        return false;
    }

    fclose(file);
    ESP_LOGI(BOOT_TAG, "Generated default boot file: %s", filename);
    return true;
}

/* ========================================================================
 * DIRECTIVE HANDLERS
 * ======================================================================== */

static bool boot_handle_set(char *statement)
{
    char *equals;

    if (statement == NULL || *statement == '\0') {
        boot_warn_unknown("SET");
        return false;
    }

    equals = strchr(statement, '=');
    if (equals == NULL || equals == statement) {
        boot_warn_unknown("SET");
        return false;
    }

    *equals = '\0';
    statement = boot_trim(statement);
    if (statement[0] == '\0') {
        return false;
    }

    shell_env_set(statement, boot_trim(equals + 1));
    return true;
}

static bool boot_handle_path(const char *value)
{
    if (value == NULL || *value == '\0') {
        boot_warn_unknown("PATH");
        return false;
    }
    shell_env_set("PATH", value);
    return true;
}

static bool boot_handle_prompt(const char *value)
{
    if (value == NULL || *value == '\0') {
        boot_warn_unknown("PROMPT");
        return false;
    }
    shell_prompt_set_template(value);
    return true;
}

static bool boot_handle_echo(const char *value)
{
    if (value == NULL || *value == '\0') {
        boot_warn_unknown("ECHO");
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        batch_set_default_echo(true);
        return true;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        batch_set_default_echo(false);
        return true;
    }
    boot_warn_unknown("ECHO");
    return false;
}

static bool boot_handle_rotate(const char *value)
{
    char cmd[BOOT_CMD_LEN];

    if (value == NULL || *value == '\0') {
        boot_warn_unknown("ROTATE");
        return false;
    }
    snprintf(cmd, sizeof(cmd), "rotate %s", value);
    boot_exec(cmd);
    return true;
}

/**
 * Apply the `RGB=` directive: set the WS2812 status LED at boot.
 *
 * The value is passed through to the `rgb` command with commas mapped to
 * argument separators, so all of these work:
 *   RGB=OFF
 *   RGB=255,0,0          (r,g,b)
 *   RGB=#00FF00
 *   RGB=rainbow,5        (effect[,speed])
 *   RGB=AUTO,ON|OFF
 */
static bool boot_handle_rgb(const char *value)
{
    char cmd[BOOT_CMD_LEN];
    char normalized[BOOT_CMD_LEN - 4];
    size_t i;

    if (value == NULL || *value == '\0') {
        boot_warn_unknown("RGB");
        return false;
    }
    for (i = 0; value[i] != '\0' && i < sizeof(normalized) - 1; i++) {
        normalized[i] = (value[i] == ',') ? ' ' : value[i];
    }
    normalized[i] = '\0';

    snprintf(cmd, sizeof(cmd), "rgb %s", normalized);
    boot_exec(cmd);
    return true;
}

static bool boot_handle_brightness(const char *value)
{
    char cmd[BOOT_CMD_LEN];

    if (value == NULL || *value == '\0') {
        boot_warn_unknown("BRIGHTNESS");
        return false;
    }
    snprintf(cmd, sizeof(cmd), "brightness %s", value);
    boot_exec(cmd);
    return true;
}

static bool boot_handle_display_power(const char *value)
{
    char cmd[BOOT_CMD_LEN];
    const char *state;

    if (value == NULL || *value == '\0') {
        boot_warn_unknown("DISPLAY_POWER");
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        state = "on";
    } else if (shell_text_equals_ignore_case(value, "OFF")) {
        state = "off";
    } else if (shell_text_equals_ignore_case(value, "SLEEP")) {
        state = "sleep";
    } else {
        boot_warn_unknown("DISPLAY_POWER");
        return false;
    }
    snprintf(cmd, sizeof(cmd), "display power %s", state);
    boot_exec(cmd);
    return true;
}

static bool boot_handle_volume(const char *value)
{
    char cmd[BOOT_CMD_LEN];

    if (value == NULL || *value == '\0') {
        boot_warn_unknown("VOLUME");
        return false;
    }
    snprintf(cmd, sizeof(cmd), "volume %s", value);
    boot_exec(cmd);
    return true;
}

static bool boot_handle_gpio(char *statement)
{
    /* statement points past the leading "GPIO" whitespace. Format:
     *   "<n> = OUT [HIGH|LOW]"  (input direction is not exposed). */
    char *equals;
    char *end = NULL;
    long gpio_num;
    char *dir_str;
    char *level_str;
    char cmd[BOOT_CMD_LEN];

    while (*statement != '\0' && isspace((unsigned char)*statement)) {
        statement++;
    }
    gpio_num = strtol(statement, &end, 10);
    if (end == statement) {
        boot_warn_unknown("GPIO");
        return false;
    }

    /* The remaining text is " = <DIR> [LEVEL]". Split on '=': the token
     * before it is the pin (already read), the token after is the rest. */
    equals = strchr(end, '=');
    if (equals == NULL) {
        boot_warn_unknown("GPIO");
        return false;
    }
    *equals = '\0';
    level_str = boot_trim(equals + 1);   /* e.g. "OUT HIGH" */

    /* Split the direction expression into keyword + optional level. */
    {
        char *space = strchr(level_str, ' ');
        if (space != NULL) {
            *space = '\0';
            dir_str = level_str;
            level_str = boot_trim(space + 1);
        } else {
            dir_str = level_str;
            level_str = "";
        }
    }

    if (shell_text_equals_ignore_case(dir_str, "OUT") ||
        shell_text_equals_ignore_case(dir_str, "OUTPUT")) {
        int level = 1;
        if (level_str[0] != '\0') {
            if (shell_text_equals_ignore_case(level_str, "LOW") || strcmp(level_str, "0") == 0) {
                level = 0;
            } else if (!shell_text_equals_ignore_case(level_str, "HIGH") && strcmp(level_str, "1") != 0) {
                boot_warn_unknown("GPIO");
                return false;
            }
        }
        snprintf(cmd, sizeof(cmd), "gpio set %ld %d", gpio_num, level);
        boot_exec(cmd);
        return true;
    }

    if (shell_text_equals_ignore_case(dir_str, "IN") || shell_text_equals_ignore_case(dir_str, "INPUT")) {
        shell_print_muted("boot: GPIO %ld input direction not exposed by shell, skipped\n", gpio_num);
        return true;
    }

    boot_warn_unknown("GPIO");
    return false;
}

static bool boot_handle_usb_keyboard(const char *value)
{
    if (value == NULL || *value == '\0') {
        boot_warn_unknown("USB_KEYBOARD");
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        usb_hid_keyboard_enable();
        return true;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        usb_hid_keyboard_disable();
        return true;
    }
    boot_warn_unknown("USB_KEYBOARD");
    return false;
}

static bool boot_handle_usb_mouse(const char *value)
{
    if (value == NULL || *value == '\0') {
        boot_warn_unknown("USB_MOUSE");
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        usb_hid_mouse_enable();
        return true;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        usb_hid_mouse_disable();
        return true;
    }
    boot_warn_unknown("USB_MOUSE");
    return false;
}

static bool boot_handle_wifi_autoconnect(const char *value)
{
    if (value == NULL || *value == '\0') {
        boot_warn_unknown("WIFI_AUTOCONNECT");
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        networking_wifi_set_boot_autoconnect(true);
        return true;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        networking_wifi_set_boot_autoconnect(false);
        return true;
    }
    boot_warn_unknown("WIFI_AUTOCONNECT");
    return false;
}

static bool boot_handle_bluetooth(const char *value)
{
    if (value == NULL || *value == '\0') {
        boot_warn_unknown("BLUETOOTH");
        return false;
    }
    if (shell_text_equals_ignore_case(value, "ON")) {
        boot_exec("bt enable");
        return true;
    }
    if (shell_text_equals_ignore_case(value, "OFF")) {
        /* No public disable once enabled; report and continue. */
        shell_print_muted("boot: BLUETOOTH=OFF has no effect once BLE is initialized\n");
        return true;
    }
    boot_warn_unknown("BLUETOOTH");
    return false;
}

static bool boot_handle_bt_advertise(const char *value)
{
    char cmd[BOOT_CMD_LEN];

    if (value == NULL || *value == '\0') {
        boot_warn_unknown("BT_ADVERTISE");
        return false;
    }
    snprintf(cmd, sizeof(cmd), "bluetooth advertise %s",
             shell_text_equals_ignore_case(value, "ON") ? "on" : "off");
    boot_exec(cmd);
    return true;
}

/* ========================================================================
 * PUBLIC ENTRY POINT
 * ======================================================================== */

void boot_run_startup(void)
{
    shell_sd_session_t session;
    char path[64];
    FILE *file = NULL;
    unsigned int directive_count = 0;

    if (shell_sd_begin(&session) != ESP_OK) {
        ESP_LOGI(BOOT_TAG, "Boot scripting skipped: no SD card");
        return;
    }

    ESP_LOGI(BOOT_TAG, "Boot scripting: checking for CONFIG.SYS / AUTOEXEC.BAT");

    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_BOOT_CONFIG_SYS_NAME);
    boot_ensure_file(path, BOOT_CFG_DEFAULT);

    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_BOOT_AUTOEXEC_BAT_NAME);
    boot_ensure_file(path, BOOT_BAT_DEFAULT);

        /* ---- Parse and apply CONFIG.SYS ---- */
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_BOOT_CONFIG_SYS_NAME);
    file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGI(BOOT_TAG, "No CONFIG.SYS to parse");
        goto run_autoexec;
    }

    char line[P4_CONFIG_BOOT_LINE_BYTES];

    while (fgets(line, sizeof(line), file) != NULL) {
        size_t len = strlen(line);
        char *trimmed;

        /* Strip trailing newline/carriage-return. */
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        trimmed = boot_trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == ';' || boot_starts_with_ci(trimmed, "REM")) {
            continue;
        }

        if (directive_count >= P4_CONFIG_BOOT_MAX_DIRECTIVES) {
            shell_print_warning("boot: CONFIG.SYS directive limit (%u) reached, ignoring remaining lines",
                                P4_CONFIG_BOOT_MAX_DIRECTIVES);
            shell_record_warningf("boot", "CONFIG.SYS directive limit reached");
            break;
        }
        directive_count++;

        /* GPIO has no '='; dispatch before any '=' split. */
        if (boot_starts_with_ci(trimmed, "GPIO")) {
            char *cursor = trimmed + 4;
            while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            (void)boot_handle_gpio(cursor);
            continue;
        }

        /* SET is special: "SET name=value" — the keyword is "SET", the rest is
         * the assignment statement. Dispatch the whole remainder as one. */
        if (boot_starts_with_ci(trimmed, "SET ")) {
            (void)boot_handle_set(boot_trim(trimmed + 4));
            continue;
        }
        if (shell_text_equals_ignore_case(trimmed, "SET")) {
            boot_warn_unknown("SET");
            continue;
        }

        /* ECHO is special: "ECHO ON|OFF" (no '=') or "ECHO message". Dispatch
         * the remainder after "ECHO " so the argument is never lost. */
        if (boot_starts_with_ci(trimmed, "ECHO ")) {
            (void)boot_handle_echo(boot_trim(trimmed + 5));
            continue;
        }
        if (shell_text_equals_ignore_case(trimmed, "ECHO")) {
            batch_set_default_echo(true);   /* bare ECHO == echo on */
            continue;
        }

        /* Split everything else on its first '='. */
        {
            char *equals = strchr(trimmed, '=');
            char *value = NULL;

            if (equals != NULL && equals != trimmed) {
                *equals = '\0';
                value = boot_trim(equals + 1);
            }

            {
                char *keyword = boot_trim(trimmed);

                if (shell_text_equals_ignore_case(keyword, "ECHO")) {
                    (void)boot_handle_echo(value);
                } else if (boot_starts_with_ci(keyword, "PATH") && equals != NULL) {
                    (void)boot_handle_path(value);
                } else if (boot_starts_with_ci(keyword, "PROMPT")) {
                    (void)boot_handle_prompt(value);
                } else if (boot_starts_with_ci(keyword, "ROTATE")) {
                    (void)boot_handle_rotate(value);
                } else if (boot_starts_with_ci(keyword, "BRIGHTNESS")) {
                    (void)boot_handle_brightness(value);
                } else if (boot_starts_with_ci(keyword, "DISPLAY_POWER") ||
                           (equals != NULL && strncasecmp(keyword, "DISPLAY", 7) == 0)) {
                    (void)boot_handle_display_power(value);
                } else if (boot_starts_with_ci(keyword, "VOLUME")) {
                    (void)boot_handle_volume(value);
                } else if (boot_starts_with_ci(keyword, "RGB")) {
                    (void)boot_handle_rgb(value);
                } else if (boot_starts_with_ci(keyword, "WIFI_SSID")) {
                    networking_wifi_set_boot_credentials(value != NULL ? value : "", "");
                } else if (boot_starts_with_ci(keyword, "WIFI_PASSWORD")) {
                    networking_wifi_set_boot_credentials("", value != NULL ? value : "");
                } else if (shell_text_equals_ignore_case(keyword, "WIFI")) {
                    if (shell_text_equals_ignore_case(value, "ON")) {
                        networking_wifi_set_boot_autoconnect(true);
                    } else if (shell_text_equals_ignore_case(value, "OFF")) {
                        networking_wifi_set_boot_autoconnect(false);
                    } else {
                        boot_warn_unknown("WIFI");
                    }
                } else if (boot_starts_with_ci(keyword, "WIFI_AUTOCONNECT")) {
                    (void)boot_handle_wifi_autoconnect(value);
                } else if (shell_text_equals_ignore_case(keyword, "BLUETOOTH")) {
                    (void)boot_handle_bluetooth(value);
                } else if (boot_starts_with_ci(keyword, "BT_ADVERTISE")) {
                    (void)boot_handle_bt_advertise(value);
                } else if (boot_starts_with_ci(keyword, "USB_KEYBOARD")) {
                    (void)boot_handle_usb_keyboard(value);
                } else if (boot_starts_with_ci(keyword, "USB_MOUSE")) {
                    (void)boot_handle_usb_mouse(value);
                } else if (boot_starts_with_ci(keyword, "FILES") ||
                           boot_starts_with_ci(keyword, "BUFFERS") ||
                           boot_starts_with_ci(keyword, "LASTDRIVE") ||
                           boot_starts_with_ci(keyword, "DEVICE") ||
                           boot_starts_with_ci(keyword, "DOS") ||
                           boot_starts_with_ci(keyword, "SHELL")) {
                    boot_warn_unknown(keyword);
                } else if (equals != NULL) {
                    /* Generic KEY=VALUE fallback: any unrecognized setting is
                     * applied as an environment variable, so CONFIG.SYS can
                     * carry project variables without a separate SET line. */
                    shell_env_set(keyword, value);
                } else {
                    boot_warn_unknown(keyword);
                }
            }
        }
    }

    fclose(file);
    file = NULL;

    ESP_LOGI(BOOT_TAG, "CONFIG.SYS processed (%u directives)", directive_count);

run_autoexec:
    /* ---- Run the alias profile (if present) ----
     * `alias /save` writes the alias table to this batch file, so persisting
     * the current macros and having them restored every boot needs no
     * AUTOEXEC.BAT edit. Safe when the file is absent. */
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_ALIAS_PROFILE);
    file = fopen(path, "r");
    if (file != NULL) {
        fclose(file);
        file = NULL;
        ESP_LOGI(BOOT_TAG, "Loading alias profile: %s", path);
        (void)shell_execute_batch_file(path, 0, NULL);
    }

    /* ---- Run AUTOEXEC.BAT ---- */
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, P4_CONFIG_BOOT_AUTOEXEC_BAT_NAME);
    file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGI(BOOT_TAG, "No AUTOEXEC.BAT to run");
        shell_sd_end(&session, "boot");
        return;
    }
    fclose(file);
    file = NULL;

    if (!P4_CONFIG_BOOT_RUN_ON_STARTUP) {
        ESP_LOGI(BOOT_TAG, "AUTOEXEC.BAT skipped (P4_CONFIG_BOOT_RUN_ON_STARTUP=0)");
        shell_sd_end(&session, "boot");
        return;
    }

    ESP_LOGI(BOOT_TAG, "Running AUTOEXEC.BAT: %s", path);
    shell_transcript_appendf_ansi(SH_HEAD "Boot Script" SH_RST ": running " SH_PATH "%s" SH_RST "\n",
                                  P4_CONFIG_BOOT_AUTOEXEC_BAT_NAME);

    {
        esp_err_t result = shell_execute_batch_file(path, 0, NULL);

        if (result != ESP_OK) {
            shell_print_warning("boot: AUTOEXEC.BAT failed (%s, errorlevel=%d)", esp_err_to_name(result), batch_get_errorlevel());
            shell_record_warningf("boot", "AUTOEXEC.BAT failed: %s (errorlevel=%d)", esp_err_to_name(result), batch_get_errorlevel());
        } else if (batch_get_errorlevel() != 0) {
            shell_print_muted("boot: AUTOEXEC.BAT finished with errorlevel=%d\n", batch_get_errorlevel());
        }
    }

    shell_sd_end(&session, "boot");
    ESP_LOGI(BOOT_TAG, "Boot scripting complete");
}

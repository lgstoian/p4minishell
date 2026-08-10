/**
 * @file command.c
 * @brief Command parser and dispatcher implementation for P4MiniShell.
 *
 * Owns the command execution pipeline and the commands that are not tied to
 * the filesystem or the batch language: variable-expansion entry, output
 * redirection parsing, dispatch, the worker task, hardware commands, system
 * commands, and the GPIO/RGB/camera families.
 *
 * Delegated ownership:
 *   - `components/storage/` owns SD sessions, path resolution, the current
 *     working directory, and every DOS file command
 *   - `components/batch/` owns the batch engine, environment variables, PATH,
 *     variable expansion, errorlevel, and the batch language commands
 *   - `components/shell/` owns transcript, history, debug log, UART console,
 *     the input line, and the system info commands
 *
 * State owned here:
 *   - Battery ADC handles and calibration state
 *   - Speaker codec handle and last applied volume
 *   - Light-sleep request tracking
 */

#include "command.h"
#include "command_ui.h"
#include "batch.h"
#include "storage.h"
#include "storage_commands.h"
#include "shell.h"
#include "ansi_palette.h"
#include "ansi.h"
#include "display.h"
#include "header.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "networking.h"
#include "bluetooth.h"
#include "c6ota.h"
#include "clock.h"
#include "usb.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_codec_dev.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <time.h>

/* Backward-compatibility aliases */
#define COMMAND_TAG                     P4_CONFIG_SHELL_TAG
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_COMMAND_TASK_STACK_BYTES  P4_CONFIG_COMMAND_TASK_STACK
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_C6_HOST_RESET_GPIO        P4_CONFIG_C6_HOST_RESET_GPIO
#define SHELL_BATTERY_ATTEN             P4_CONFIG_BATTERY_ATTEN
#define SHELL_BATTERY_MIN_SLEEP_FREQ_MHZ P4_CONFIG_BATTERY_MIN_SLEEP_FREQ_MHZ
#define SHELL_ARGV_MAX                  P4_CONFIG_COMMAND_ARGV_MAX
#define SHELL_REBOOT_DELAY_MS           P4_CONFIG_REBOOT_DELAY_MS

/** Maximum redirection operators parsed from one command line. */
#define SHELL_REDIRECT_TOKEN_MAX        4
#define SHELL_CHAIN_SEGMENT_MAX         P4_CONFIG_CHAIN_SEGMENT_MAX

/* ========================================================================
 * TYPES
 * ======================================================================== */

/** One queued command line for the worker task. */
typedef struct {
    char command[SHELL_COMMAND_BYTES];
} command_request_t;

/** One entry in the exposed board GPIO table. */
typedef struct {
    const char *name;
    gpio_num_t gpio;
    const char *role;
    bool allow_output;
    bool critical;
} shell_gpio_pin_desc_t;

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/** Persistent worker task and its command queue. Replaces the per-command
 *  task creation pattern: commands are posted to the queue and processed
 *  sequentially by a single long-lived task, eliminating task-creation
 *  overhead (~1-2ms per command) and reducing heap fragmentation. */
#define SHELL_COMMAND_QUEUE_DEPTH  4
static QueueHandle_t s_command_queue = NULL;
static TaskHandle_t s_command_worker_handle = NULL;

/* Hardware handles */
static esp_codec_dev_handle_t s_speaker_dev;
static int s_volume_percent = P4_CONFIG_VOLUME_DEFAULT_PCT;
static bool s_light_sleep_requested;
static adc_oneshot_unit_handle_t s_battery_adc_unit;
static adc_channel_t s_battery_adc_channel;
static bool s_battery_adc_ready;
static adc_cali_handle_t s_battery_cali_handle;
static bool s_battery_cali_ready;

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

/* Hardware helpers */
static esp_err_t shell_audio_ensure_speaker(void);
static esp_err_t shell_battery_ensure_adc(void);
static const shell_gpio_pin_desc_t *shell_find_gpio_pin(int gpio_num);
static void shell_battery_print_usage(void);

/* Pipeline helpers */
static bool shell_parse_redirection(char *command,
                                    char **command_part,
                                    char **redirect_target,
                                    bool *append_mode,
                                    char **input_source);
static bool shell_command_has_pipe(const char *command);

/* Command implementations owned by this module */
static void shell_command_brightness(int argc, char **argv);
static void shell_command_rotate(int argc, char **argv);
static void shell_command_battery(int argc, char **argv);
static void shell_command_volume(int argc, char **argv);
static void shell_command_reboot(void);
static void shell_command_clear(void);
static void shell_command_gpio_status(void);
static void shell_execute_gpio_command(int argc, char **argv);
static void shell_execute_rgb_command(int argc, char **argv);
static void shell_execute_camera_command(int argc, char **argv);
static void shell_command_prompt_cmd(int argc, char **argv);
static void shell_command_date(int argc, char **argv);
static void shell_command_time_cmd(int argc, char **argv);

/* ========================================================================
 * BOARD GPIO TABLE
 * ======================================================================== */

static const shell_gpio_pin_desc_t s_gpio_pins[] = {
    {"i2c_sda", BSP_I2C_SDA, "Shared control bus for GT911 touch and onboard peripherals", false, true},
    {"i2c_scl", BSP_I2C_SCL, "Shared control clock for GT911 touch and onboard peripherals", false, true},
    {"i2s_dout", BSP_I2S_DOUT, "Audio codec data from the ESP32-P4 to the speaker path", false, true},
    {"i2s_lclk", BSP_I2S_LCLK, "Audio codec word-select clock", false, true},
    {"i2s_dsin", BSP_I2S_DSIN, "Audio codec data into the ESP32-P4", false, true},
    {"i2s_sclk", BSP_I2S_SCLK, "Audio codec bit clock", false, true},
    {"i2s_mclk", BSP_I2S_MCLK, "Audio codec master clock", false, true},
    {"power_amp", BSP_POWER_AMP_IO, "Speaker amplifier enable line", true, false},
    {"backlight", BSP_LCD_BACKLIGHT, "JD9165 panel backlight control", false, true},
    {"lcd_reset", BSP_LCD_RST, "JD9165 panel hardware reset", false, true},
    {"battery_adc", BOARD_CFG_BATTERY_ADC_GPIO, "Battery divider sense input", false, true},
    {"hosted_sdio_d0", GPIO_NUM_14, "ESP32-C6 hosted SDIO data lane D0", false, true},
    {"hosted_sdio_d1", GPIO_NUM_15, "ESP32-C6 hosted SDIO data lane D1", false, true},
    {"hosted_sdio_d2", GPIO_NUM_16, "ESP32-C6 hosted SDIO data lane D2", false, true},
    {"hosted_sdio_d3", GPIO_NUM_17, "ESP32-C6 hosted SDIO data lane D3", false, true},
    {"hosted_sdio_clk", GPIO_NUM_18, "ESP32-C6 hosted SDIO clock", false, true},
    {"hosted_sdio_cmd", GPIO_NUM_19, "ESP32-C6 hosted SDIO command", false, true},
    {"c6_host_reset", (gpio_num_t)SHELL_C6_HOST_RESET_GPIO, "ESP32-C6 hosted reset or enable control", false, true},
    {"sd_d0", BSP_SD_D0, "MicroSD data lane D0", false, true},
    {"sd_d1", BSP_SD_D1, "MicroSD data lane D1", false, true},
    {"sd_d2", BSP_SD_D2, "MicroSD data lane D2", false, true},
    {"sd_d3", BSP_SD_D3, "MicroSD data lane D3", false, true},
    {"sd_clk", BSP_SD_CLK, "MicroSD clock", false, true},
    {"sd_cmd", BSP_SD_CMD, "MicroSD command", false, true},
};

static const shell_gpio_pin_desc_t *shell_find_gpio_pin(int gpio_num)
{
    size_t index;

    for (index = 0; index < sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]); index++) {
        if ((int)s_gpio_pins[index].gpio == gpio_num) {
            return &s_gpio_pins[index];
        }
    }

    return NULL;
}

/* ========================================================================
 * AUDIO AND BATTERY HARDWARE
 * ======================================================================== */

static esp_err_t shell_audio_ensure_speaker(void)
{
    if (s_speaker_dev != NULL) {
        return ESP_OK;
    }

    s_speaker_dev = bsp_audio_codec_speaker_init();
    if (s_speaker_dev == NULL) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t shell_battery_ensure_adc(void)
{
    esp_err_t error;
    adc_unit_t unit_id;
    adc_oneshot_unit_init_cfg_t unit_cfg = {0};
    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = SHELL_BATTERY_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    if (s_battery_adc_ready) {
        return ESP_OK;
    }

    error = adc_oneshot_io_to_channel(BOARD_CFG_BATTERY_ADC_GPIO, &unit_id, &s_battery_adc_channel);
    if (error != ESP_OK) {
        return error;
    }

    unit_cfg.unit_id = unit_id;
    error = adc_oneshot_new_unit(&unit_cfg, &s_battery_adc_unit);
    if (error != ESP_OK) {
        return error;
    }

    error = adc_oneshot_config_channel(s_battery_adc_unit, s_battery_adc_channel, &channel_cfg);
    if (error != ESP_OK) {
        return error;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .chan = s_battery_adc_channel,
            .atten = SHELL_BATTERY_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_battery_cali_handle) == ESP_OK) {
            s_battery_cali_ready = true;
        }
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .atten = SHELL_BATTERY_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_battery_cali_handle) == ESP_OK) {
            s_battery_cali_ready = true;
        }
    }
#endif

    s_battery_adc_ready = true;
    return ESP_OK;
}

esp_err_t command_battery_read(int *battery_mv_out, int *percent_out, int *raw_out, int *gpio_mv_out)
{
    esp_err_t error;
    int raw = 0;
    int gpio_mv = 0;
    int battery_mv;
    int percent;

    error = shell_battery_ensure_adc();
    if (error != ESP_OK) {
        return error;
    }

    error = adc_oneshot_read(s_battery_adc_unit, s_battery_adc_channel, &raw);
    if (error != ESP_OK) {
        return error;
    }

    if (s_battery_cali_ready) {
        error = adc_cali_raw_to_voltage(s_battery_cali_handle, raw, &gpio_mv);
        if (error != ESP_OK) {
            return error;
        }
    } else {
        gpio_mv = (raw * 3300) / 4095;
    }

    battery_mv = (gpio_mv * BOARD_CFG_BATTERY_DIVIDER_NUMERATOR) / BOARD_CFG_BATTERY_DIVIDER_DENOMINATOR;
    if (battery_mv <= BOARD_CFG_BATTERY_EMPTY_MV) {
        percent = 0;
    } else if (battery_mv >= BOARD_CFG_BATTERY_FULL_MV) {
        percent = 100;
    } else {
        percent = ((battery_mv - BOARD_CFG_BATTERY_EMPTY_MV) * 100) /
                  (BOARD_CFG_BATTERY_FULL_MV - BOARD_CFG_BATTERY_EMPTY_MV);
    }

    if (battery_mv_out != NULL) {
        *battery_mv_out = battery_mv;
    }
    if (percent_out != NULL) {
        *percent_out = percent;
    }
    if (raw_out != NULL) {
        *raw_out = raw;
    }
    if (gpio_mv_out != NULL) {
        *gpio_mv_out = gpio_mv;
    }

    return ESP_OK;
}

static void shell_battery_print_usage(void)
{
    shell_print_usage("Usage: battery or battery sleep <on|off|status>");
}

/* ========================================================================
 * REDIRECTION PARSING
 * ========================================================================
 * Handles the three DOS redirection operators on one pass:
 *   >   truncate output to a file
 *   >>  append output to a file
 *   <   read input from a file
 *
 * Quote state is tracked so an operator inside a quoted argument is treated
 * as data. The scan walks the whole line rather than stopping at the first
 * operator, so `sort < in.txt > out.txt` works in either order.
 */

/**
 * Remove quoting and escape markup from a redirection target.
 *
 * A target such as `"my file.txt"`, `'my file.txt'`, or `my^ file.txt` all
 * reach the filesystem layer as the literal path.
 */
static char *shell_redirect_unquote(char *target)
{
    return shell_unescape_in_place(target);
}

/**
 * Split a command line into its command text and redirection targets.
 *
 * @param command        Line to parse, modified in place.
 * @param command_part   Receives the trimmed command text.
 * @param redirect_target Receives the `>` / `>>` target, or NULL.
 * @param append_mode    Receives true for `>>`.
 * @param input_source   Receives the `<` source, or NULL.
 * @return true when any redirection operator was found.
 */
/**
 * Find the next redirection operator that is neither quoted nor escaped.
 *
 * Delegates to the shared shell-core scanner so redirection, pipes, and
 * chaining all agree on what counts as syntax.
 *
 * @return Pointer to the operator character, or NULL when none remains.
 */
static char *shell_redirect_find_operator(char *cursor)
{
    return shell_find_unquoted_any(cursor, "><");
}

/** One redirection operator located during the scanning pass. */
typedef struct {
    char *position;   /**< The operator character within the line. */
    char *target;     /**< First character of the target text. */
    bool is_output;   /**< true for `>` / `>>`, false for `<`. */
    bool is_append;   /**< true for `>>`. */
} shell_redirect_token_t;

/**
 * Parse `>`, `>>`, and `<` out of a command line.
 *
 * Works in two passes so the operator characters can be located before any
 * of them is overwritten. The first pass records every unquoted operator and
 * where its target begins; the second pass writes a terminator over each
 * operator, which simultaneously ends the text that preceded it. Because
 * every operator becomes a NUL, each target is naturally terminated by the
 * next operator without any byte having to be restored.
 *
 * The last occurrence of each direction wins, matching COMMAND.COM.
 */
static bool shell_parse_redirection(char *command,
                                    char **command_part,
                                    char **redirect_target,
                                    bool *append_mode,
                                    char **input_source)
{
    shell_redirect_token_t tokens[SHELL_REDIRECT_TOKEN_MAX];
    size_t token_count = 0;
    char *cursor;
    size_t index;

    if (command_part == NULL || redirect_target == NULL ||
        append_mode == NULL || input_source == NULL) {
        return false;
    }

    *command_part = command;
    *redirect_target = NULL;
    *append_mode = false;
    *input_source = NULL;

    if (command == NULL) {
        return false;
    }

    /* Pass 1: locate every unquoted operator. */
    cursor = shell_redirect_find_operator(command);
    while (cursor != NULL && token_count < SHELL_REDIRECT_TOKEN_MAX) {
        shell_redirect_token_t *token = &tokens[token_count++];

        token->position = cursor;
        token->is_output = (*cursor == '>');
        token->is_append = token->is_output && (cursor[1] == '>');
        token->target = cursor + (token->is_append ? 2 : 1);

        cursor = shell_redirect_find_operator(token->target);
    }

    if (token_count == 0) {
        *command_part = shell_trim(command);
        return false;
    }

    /* Pass 2: cut the line at every operator. A `>>` needs both characters
     * blanked so the extra '>' cannot leak into the preceding text. */
    for (index = 0; index < token_count; index++) {
        tokens[index].position[0] = '\0';
        if (tokens[index].is_append) {
            tokens[index].position[1] = '\0';
        }
    }

    /* Pass 3: publish the targets. */
    for (index = 0; index < token_count; index++) {
        char *target = shell_redirect_unquote(shell_trim(tokens[index].target));

        if (tokens[index].is_output) {
            *redirect_target = target;
            *append_mode = tokens[index].is_append;
        } else {
            *input_source = target;
        }
    }

    *command_part = shell_trim(command);
    return true;
}

/**
 * Report whether a line contains a `|` pipe separator that is real syntax.
 *
 * Quote- and escape-aware, so `echo "a | b"`, `echo 'a | b'`, and `echo a^|b`
 * are not mistaken for pipelines.
 */
static bool shell_command_has_pipe(const char *command)
{
    return shell_has_unquoted_char(command, '|');
}

/* ========================================================================
 * HARDWARE COMMANDS: brightness, rotate, battery, volume
 * ======================================================================== */

static void shell_command_brightness(int argc, char **argv)
{
    esp_err_t error;
    int percent;

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_print_usage("Usage: brightness <0-100>");
        shell_record_warningf("brightness", "Usage error for brightness command");
        return;
    }

    error = display_set_brightness(percent);
    if (error != ESP_OK) {
        shell_print_error("brightness: failed to set backlight (%s)", esp_err_to_name(error));
        shell_record_errorf("brightness", error, "Failed to set brightness to %d%%", percent);
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "brightness set to" SH_RST " " SH_NUM "%d%%" SH_RST "\n", percent);
}

static void shell_command_rotate(int argc, char **argv)
{
    display_rotation_t rotation;
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: rotate <0|90|180|270>");
        shell_record_warningf("rotate", "Usage error for rotate command");
        return;
    }

    error = display_rotation_parse(argv[1], &rotation);
    if (error != ESP_OK) {
        shell_print_usage("Usage: rotate <0|90|180|270>");
        shell_record_warningf("rotate", "Invalid rotation angle: %s", argv[1]);
        return;
    }

    error = display_set_rotation(rotation);
    if (error != ESP_OK) {
        shell_print_error("rotate: failed to apply display rotation (%s)", esp_err_to_name(error));
        shell_record_errorf("rotate", error, "Failed to apply rotation %s", argv[1]);
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "rotation set to" SH_RST " " SH_NUM "%s" SH_RST " " SH_LBL "degrees and GT911 remap updated" SH_RST "\n", argv[1]);
}

static void shell_command_battery(int argc, char **argv)
{
    int battery_mv;
    int percent;
    int raw;
    int gpio_mv;
    esp_err_t error;

    if (argc == 1) {
        error = command_battery_read(&battery_mv, &percent, &raw, &gpio_mv);
        if (error != ESP_OK) {
            shell_print_error("battery: failed to read ADC on GPIO %d (%s)",
                                     BOARD_CFG_BATTERY_ADC_GPIO,
                                     esp_err_to_name(error));
            shell_record_errorf("battery", error, "Failed to read battery ADC");
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "battery:" SH_RST " " SH_NUM "%d%%" SH_RST ", " SH_NUM "%d.%03d V" SH_RST "\n", percent, battery_mv / 1000, battery_mv % 1000);
        shell_transcript_appendf_ansi(SH_LBL "battery.detail:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "gpio_mv=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "scaled_mv=" SH_RST SH_NUM "%d" SH_RST "\n",
                                 BOARD_CFG_BATTERY_ADC_GPIO,
                                 raw,
                                 gpio_mv,
                                 battery_mv);
        shell_transcript_appendf_ansi(SH_LBL "calibrated=" SH_RST "%s" SH_RST "\n",
                                 s_battery_cali_ready ? "yes" : "no");
#if CONFIG_PM_ENABLE
        if (s_light_sleep_requested) {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_OK "yes" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_MUTE "no" SH_RST "\n");
        }
#else
        shell_print_muted("battery.sleep: unavailable because CONFIG_PM_ENABLE is off in sdkconfig");
#endif
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "sleep") &&
        shell_text_equals_ignore_case(argv[2], "status")) {
        if (s_light_sleep_requested) {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_OK "yes" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep requested=" SH_RST SH_MUTE "no" SH_RST "\n");
        }
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "sleep")) {
        if (!shell_text_equals_ignore_case(argv[2], "on") && !shell_text_equals_ignore_case(argv[2], "off")) {
            shell_battery_print_usage();
            shell_record_warningf("battery", "Invalid battery sleep argument: %s", argv[2]);
            return;
        }
#if CONFIG_PM_ENABLE
        esp_pm_config_t pm_config = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = SHELL_BATTERY_MIN_SLEEP_FREQ_MHZ,
            .light_sleep_enable = shell_text_equals_ignore_case(argv[2], "on"),
        };

        error = esp_pm_configure(&pm_config);
        if (error != ESP_OK) {
            shell_transcript_appendf("battery.sleep: failed to apply power management (%s)\n", esp_err_to_name(error));
            shell_record_errorf("battery", error, "Failed to update light sleep request");
            return;
        }

        s_light_sleep_requested = pm_config.light_sleep_enable;
        if (s_light_sleep_requested) {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep" SH_RST " " SH_OK "enabled" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "battery.sleep:" SH_RST " " SH_LBL "light sleep" SH_RST " " SH_MUTE "disabled" SH_RST "\n");
        }
#else
        shell_print_muted("battery.sleep: unavailable because CONFIG_PM_ENABLE is off in sdkconfig");
#endif
        return;
    }

    shell_battery_print_usage();
    shell_record_warningf("battery", "Usage error for battery command");
}

static void shell_command_volume(int argc, char **argv)
{
    int percent;
    int result;
    esp_err_t error;

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_print_usage("Usage: volume <0-100>");
        shell_record_warningf("volume", "Usage error for volume command");
        return;
    }

    error = shell_audio_ensure_speaker();
    if (error != ESP_OK) {
        shell_print_error("volume: failed to initialize the ES8311 speaker path (%s)", esp_err_to_name(error));
        shell_record_errorf("volume", error, "Failed to initialize speaker device");
        return;
    }

    result = esp_codec_dev_set_out_vol(s_speaker_dev, percent);
    if (result != ESP_CODEC_DEV_OK) {
        shell_print_error("volume: failed to set speaker volume (codec=%d)", result);
        shell_record_warningf("volume", "Failed to set speaker volume to %d%% (codec=%d)", percent, result);
        return;
    }

    s_volume_percent = percent;
    shell_transcript_appendf_ansi(SH_LBL "volume set to" SH_RST " " SH_NUM "%d%%" SH_RST "\n", percent);
}

/* ========================================================================
 * SYSTEM COMMANDS: reboot, clear, prompt, date, time
 * ======================================================================== */

static void shell_command_reboot(void)
{
    shell_transcript_appendf_ansi(SH_ERR "Rebooting..." SH_RST "\n");

    /* Give the transcript and UART console time to flush the message
     * before the reset takes effect. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_REBOOT_DELAY_MS));
    esp_restart();
}

static void shell_command_clear(void)
{
    shell_transcript_reset();
    shell_record_infof("shell", "Transcript cleared");
}

/**
 * `prompt` — show or set the DOS prompt template.
 *
 * Usage:
 *   prompt              Show the active template and its rendered form
 *   prompt <template>   Set the template ($p path, $g >, $t time, ...)
 *   prompt /?           List the supported metacharacters
 *
 * The template drives both the UART console prompt and the LVGL input line,
 * so the two surfaces can never disagree. Rendering lives in the shell core.
 */
static void shell_command_prompt_cmd(int argc, char **argv)
{
    char template_text[P4_CONFIG_PROMPT_TEMPLATE_BYTES];

    if (argc >= 2 && strcmp(argv[1], "/?") == 0) {
        shell_print_usage("Usage: prompt [template]");
        shell_transcript_append_text("  $p  current path      $g  >              $l  <\n");
        shell_transcript_append_text("  $n  drive letter      $b  |              $q  =\n");
        shell_transcript_append_text("  $d  date              $t  time           $v  version\n");
        shell_transcript_append_text("  $a  &                 $c  (              $f  )\n");
        shell_transcript_append_text("  $s  space             $_  newline        $$  $\n");
        shell_transcript_append_text("  $h  backspace         $e  escape\n");
        shell_transcript_append_text("  prompt with no argument restores nothing; use 'prompt $p$g' for the default style\n");
        return;
    }

    if (argc == 1) {
        shell_print_field("prompt: template is", "%s", shell_prompt_get_template());
        shell_print_field("prompt: renders as", "%s", shell_prompt_render_plain());
        return;
    }

    /* Join the remaining arguments so an unquoted template with spaces,
     * such as `prompt $p $g`, is preserved the way DOS accepts it. */
    shell_join_args(argv, 1, argc, template_text, sizeof(template_text));

    shell_prompt_set_template(template_text);

    /* Repaint the input line immediately so the change is visible without
     * waiting for the next command. */
    shell_input_line_reset();

    shell_print_ok("prompt: template set to '%s'", shell_prompt_get_template());
    shell_print_field("prompt: renders as", "%s", shell_prompt_render_plain());
}

/**
 * `date` — show or set the system date.
 *
 * Usage: date [MM-DD-YYYY]
 * Setting the date adjusts the C library clock. When SNTP later synchronizes,
 * the network time wins, which matches how DOS-era boxes behaved against an
 * authoritative source.
 */
static void shell_command_date(int argc, char **argv)
{
    struct tm now;
    struct timeval tv;
    time_t stamp;
    int month = 0;
    int day = 0;
    int year = 0;

    if (argc == 1) {
        const char *text = shell_get_time_string();

        shell_print_field("The current date is:", "%s", text != NULL ? text : "unknown");
        if (!shell_time_is_synced()) {
            shell_print_muted("date: clock is not NTP-synchronized yet");
        }
        return;
    }

    if (argc != 2) {
        shell_print_usage("Usage: date [MM-DD-YYYY]");
        return;
    }

    if (sscanf(argv[1], "%d-%d-%d", &month, &day, &year) != 3 &&
        sscanf(argv[1], "%d/%d/%d", &month, &day, &year) != 3) {
        shell_print_usage("Usage: date [MM-DD-YYYY]");
        shell_record_warningf("date", "Unparsable date argument: %s", argv[1]);
        return;
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 1970 || year > 2099) {
        shell_print_usage("Usage: date [MM-DD-YYYY]");
        shell_transcript_append_text("date: value out of range (months 1-12, days 1-31, years 1970-2099)\n");
        shell_record_warningf("date", "Out of range or wrong order (expected MM-DD-YYYY): %s", argv[1]);
        return;
    }

    now = time_get_local();
    now.tm_mon = month - 1;
    now.tm_mday = day;
    now.tm_year = year - 1900;
    now.tm_isdst = -1;

    stamp = mktime(&now);
    if (stamp == (time_t)-1) {
        shell_print_error("date: could not apply that date");
        shell_record_warningf("date", "mktime rejected %s", argv[1]);
        return;
    }

    tv.tv_sec = stamp;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0) {
        shell_print_error("date: failed to update the system clock");
        shell_record_errorf("date", ESP_FAIL, "settimeofday failed");
        return;
    }

    shell_print_field("The current date is:", "%s", shell_get_time_string());
}

/**
 * `time` — show or set the system time.
 *
 * Usage: time [HH:MM[:SS]]
 * Setting the time adjusts the C library clock; a later SNTP sync overrides it.
 */
static void shell_command_time_cmd(int argc, char **argv)
{
    struct tm now;
    struct timeval tv;
    time_t stamp;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int parsed;

    if (argc == 1) {
        const char *text = shell_get_time_string();

        shell_print_field("The current time is:", "%s", text != NULL ? text : "unknown");
        if (!shell_time_is_synced()) {
            shell_print_muted("time: clock is not NTP-synchronized yet");
        }
        return;
    }

    if (argc != 2) {
        shell_print_usage("Usage: time [HH:MM[:SS]]");
        return;
    }

    parsed = sscanf(argv[1], "%d:%d:%d", &hour, &minute, &second);
    if (parsed < 2) {
        shell_print_usage("Usage: time [HH:MM[:SS]]");
        shell_record_warningf("time", "Unparsable time argument: %s", argv[1]);
        return;
    }
    if (parsed == 2) {
        second = 0;
    }

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        shell_transcript_append_text("time: value out of range (hours 0-23, minutes and seconds 0-59)\n");
        return;
    }

    now = time_get_local();
    now.tm_hour = hour;
    now.tm_min = minute;
    now.tm_sec = second;
    now.tm_isdst = -1;

    stamp = mktime(&now);
    if (stamp == (time_t)-1) {
        shell_print_error("time: could not apply that time");
        shell_record_warningf("time", "mktime rejected %s", argv[1]);
        return;
    }

    tv.tv_sec = stamp;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0) {
        shell_print_error("time: failed to update the system clock");
        shell_record_errorf("time", ESP_FAIL, "settimeofday failed");
        return;
    }

    shell_print_field("The current time is:", "%s", shell_get_time_string());
}

/* ========================================================================
 * GPIO COMMANDS
 * ======================================================================== */

static const char *shell_gpio_access_label(const shell_gpio_pin_desc_t *pin)
{
    if (pin == NULL) {
        return "unknown";
    }

    if (pin->allow_output && !pin->critical) {
        return "shell-settable";
    }

    return "monitor-only";
}

static void shell_print_gpio_entry(const shell_gpio_pin_desc_t *pin)
{
    int level;

    if (pin == NULL || pin->gpio < 0) {
        return;
    }

    level = gpio_get_level(pin->gpio);
    shell_transcript_appendf_ansi(SH_LBL "gpio.list:" SH_RST " " SH_LBL "name=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "level=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "access=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "critical=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "role=" SH_RST SH_DESC "%s" SH_RST "\n",
                             pin->name,
                             pin->gpio,
                             level,
                             shell_gpio_access_label(pin),
                             pin->critical ? "yes" : "no",
                             pin->role != NULL ? pin->role : "unspecified");
}

static esp_err_t shell_gpio_set_safe_level(int gpio_num, int level)
{
    const shell_gpio_pin_desc_t *pin = shell_find_gpio_pin(gpio_num);
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (pin == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!pin->allow_output || pin->critical) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (gpio_config(&config) != ESP_OK) {
        return ESP_FAIL;
    }

    return gpio_set_level((gpio_num_t)gpio_num, level);
}

static void shell_command_gpio_status(void)
{
    size_t index;

    for (index = 0; index < sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]); index++) {
        const shell_gpio_pin_desc_t *pin = &s_gpio_pins[index];
        int level = gpio_get_level(pin->gpio);

        shell_transcript_appendf_ansi(SH_LBL "gpio.status:" SH_RST " " SH_LBL "name=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "level=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "access=" SH_RST SH_VAL "%s" SH_RST " " SH_LBL "role=" SH_RST SH_DESC "%s" SH_RST "\n",
                                 pin->name,
                                 pin->gpio,
                                 level,
                                 shell_gpio_access_label(pin),
                                 pin->role != NULL ? pin->role : "unspecified");
    }
}

static void shell_execute_gpio_command(int argc, char **argv)
{
    size_t index;
    char *end = NULL;
    long gpio_num;
    long level;
    esp_err_t error;

    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        shell_command_gpio_status();
        return;
    }

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "list")) {
        for (index = 0; index < sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]); index++) {
            shell_print_gpio_entry(&s_gpio_pins[index]);
        }
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "read")) {
        const shell_gpio_pin_desc_t *pin;

        gpio_num = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: gpio read <pin>");
            shell_record_warningf("gpio", "Usage error for gpio read command");
            return;
        }
        pin = shell_find_gpio_pin((int)gpio_num);
        if (pin != NULL) {
            shell_transcript_appendf("gpio read: name=%s pin=%ld level=%d role=%s\n",
                                     pin->name,
                                     gpio_num,
                                     gpio_get_level((gpio_num_t)gpio_num),
                                     pin->role != NULL ? pin->role : "unspecified");
        } else {
            shell_transcript_appendf("gpio read: pin=%ld level=%d\n", gpio_num, gpio_get_level((gpio_num_t)gpio_num));
        }
        return;
    }

    if (argc == 4 && shell_text_equals_ignore_case(argv[1], "set")) {
        gpio_num = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: gpio set <pin> <0|1>");
            shell_record_warningf("gpio", "Usage error for gpio set pin argument");
            return;
        }

        end = NULL;
        level = strtol(argv[3], &end, 10);
        if (end == NULL || *end != '\0' || (level != 0 && level != 1)) {
            shell_print_usage("Usage: gpio set <pin> <0|1>");
            shell_record_warningf("gpio", "Usage error for gpio set level argument");
            return;
        }

        error = shell_gpio_set_safe_level((int)gpio_num, (int)level);
        if (error == ESP_ERR_NOT_SUPPORTED) {
            shell_transcript_appendf("gpio set: pin %ld is reserved for active board functions and is read-only from the shell\n", gpio_num);
            shell_record_warningf("gpio", "Rejected unsafe gpio set on pin %ld", gpio_num);
            return;
        }
        if (error == ESP_ERR_NOT_FOUND) {
            shell_transcript_appendf("gpio set: pin %ld is not in the exposed board pin list\n", gpio_num);
            shell_record_warningf("gpio", "Unknown gpio set pin %ld", gpio_num);
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("gpio set: failed to drive pin %ld (%s)", gpio_num, esp_err_to_name(error));
            shell_record_errorf("gpio", error, "Failed to set gpio %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "gpio set:" SH_RST " " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST " " SH_LBL "level=" SH_RST SH_NUM "%ld" SH_RST "\n", gpio_num, level);
        return;
    }

    shell_print_usage("Usage: gpio list | gpio status | gpio read <pin> | gpio set <pin> <0|1>");
    shell_record_warningf("gpio", "Usage error for gpio command");
}

/* ========================================================================
 * UNSUPPORTED HARDWARE COMMANDS (rgb, camera)
 * ======================================================================== */

static void shell_execute_rgb_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (BOARD_CFG_RGB_LED_GPIO == GPIO_NUM_NC) {
        shell_print_error("rgb: unsupported because the JC1060 reference repo does not expose authoritative onboard RGB LED wiring and this workspace still has no declared RGB driver");
        shell_record_warningf("rgb", "RGB LED command requested without a configured RGB LED pin");
        return;
    }

    shell_transcript_append_text("rgb: RGB LED control is reserved until the board metadata declares the exact driver mode\n");
}

static void shell_execute_camera_command(int argc, char **argv)
{
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "init")) {
        shell_print_error("camera: unsupported because the JC1060 reference repo shows a camera add-on path, but this workspace still does not declare the sensor, CSI pin map, or local esp_video camera stack needed to initialize it");
        shell_record_warningf("camera", "Camera init requested without camera metadata in the workspace");
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "snap")) {
        shell_transcript_appendf("camera: snap unavailable for %s because the workspace still lacks the declared sensor, CSI pin map, and local camera stack that the JC1060 examples depend on\n",
                                 argv[2]);
        shell_record_warningf("camera", "Camera snap requested without camera metadata in the workspace");
        return;
    }

    shell_print_usage("Usage: camera init | camera snap <filename>");
    shell_record_warningf("camera", "Usage error for camera command");
}

/* ========================================================================
 * HTTPGET / WGET
 * ========================================================================
 * `httpget <url> [localfile]` — the HTTP engine lives in
 * components/networking (the sole owner of the esp_http_client surface); this
 * file only dispatches, renders the body, and saves it to SD through the
 * storage write path with the usual free-space guardrails. ERRORLEVEL is 0 on
 * an HTTP 2xx, 1 on any failure, 2 on a usage error.
 */

/**
 * Print an httpget response body to the transcript, bounded and sanitized so a
 * binary or control-character payload cannot corrupt the transcript or inject
 * ANSI sequences. Plain text keeps `httpget url > file` redirectable.
 */
static void shell_print_http_body(const uint8_t *body, size_t size)
{
    char chunk[160];
    size_t limit = size;
    size_t index;
    size_t used = 0;

    if (body == NULL || size == 0) {
        return;
    }
    if (limit > P4_CONFIG_HTTP_PRINT_BODY_BYTES) {
        limit = P4_CONFIG_HTTP_PRINT_BODY_BYTES;
    }

    for (index = 0; index < limit; index++) {
        char ch = (char)body[index];

        /* Keep printable ASCII and the common text separators; render
         * everything else (including ESC and NUL) as a harmless dot. */
        if ((unsigned char)ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
            ch = '.';
        }
        chunk[used++] = ch;
        if (used >= sizeof(chunk) - 1) {
            chunk[used] = '\0';
            shell_transcript_append_text(chunk);
            used = 0;
        }
    }
    if (used > 0) {
        chunk[used] = '\0';
        shell_transcript_append_text(chunk);
    }
    if (limit < size) {
        shell_print_muted("httpget: body truncated at %u bytes (use httpget <url> <file> for the full body)",
                          (unsigned int)limit);
    }
}

/* ========================================================================
 * COMMAND DISPATCH
 * ======================================================================== */

bool shell_execute_command_core(char *command)
{
    char *argv[SHELL_ARGV_MAX];
    int argc;
    char *trimmed;
    char *family_command = NULL;

    if (command == NULL) {
        return false;
    }

    trimmed = shell_trim(command);
    if (trimmed[0] == '\0') {
        return false;
    }

    /* A pending C6 OTA confirmation swallows the line before any command
     * lookup so a stray "YES" cannot be dispatched as a shell command. */
    if (c6ota_try_handle_input(trimmed)) {
        return true;
    }

    /* An unquoted pipe operator splits the line into pipeline stages. */
    if (shell_command_has_pipe(trimmed)) {
        shell_execute_pipe(trimmed);
        return true;
    }

    /* Module-routed family handlers (wifi, bluetooth/bt, usb, sd, disk) parse
     * the full command line themselves, so they need the original text. The
     * shell_split_args() call below writes token terminators into the buffer
     * in place — after it runs, `command` would be truncated to the first
     * token ("wifi status" -> "wifi"). Preserve a heap copy of the trimmed
     * line for those branches. The copy is only made for family prefixes, and
     * every family branch frees it, so normal commands never allocate. */
    if (strncmp(trimmed, "wifi", 4) == 0 &&
        (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) {
        family_command = strdup(trimmed);
    } else if (strncmp(trimmed, "bluetooth", 9) == 0 &&
               (trimmed[9] == '\0' || isspace((unsigned char)trimmed[9]))) {
        family_command = strdup(trimmed);
    } else if (strncmp(trimmed, "bt", 2) == 0 &&
               (trimmed[2] == '\0' || isspace((unsigned char)trimmed[2]))) {
        family_command = strdup(trimmed);
    } else if (strncmp(trimmed, "usb", 3) == 0 &&
               (trimmed[3] == '\0' || isspace((unsigned char)trimmed[3]))) {
        family_command = strdup(trimmed);
    } else if (strncmp(trimmed, "sd", 2) == 0 &&
               (trimmed[2] == '\0' || isspace((unsigned char)trimmed[2]))) {
        family_command = strdup(trimmed);
    } else if (strncmp(trimmed, "disk", 4) == 0 &&
               (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) {
        family_command = strdup(trimmed);
    }

    argc = shell_split_args(trimmed, argv, SHELL_ARGV_MAX);
    if (argc == 0) {
        free(family_command);
        return false;
    }

    /* ---- System commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "help")) {
        shell_command_help();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "cls") || shell_text_equals_ignore_case(argv[0], "clear")) {
        shell_command_clear();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "reboot")) {
        shell_command_reboot();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "version") || shell_text_equals_ignore_case(argv[0], "ver")) {
        shell_command_version();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "about")) {
        shell_command_about();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sysinfo")) {
        shell_command_sysinfo();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "mem")) {
        shell_command_mem();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "debug")) {
        shell_command_debug();
        return true;
    }

    /* ---- Hardware commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "brightness")) {
        shell_command_brightness(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rotate")) {
        shell_command_rotate(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "battery")) {
        shell_command_battery(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "volume")) {
        shell_command_volume(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "gpio")) {
        shell_execute_gpio_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rgb")) {
        shell_execute_rgb_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "camera")) {
        shell_execute_camera_command(argc, argv);
        return true;
    }

    /* ---- Display/keyboard/windows commands ----
     * Implemented in command_ui.c to keep display.h, keyboard.h, and windows.h
     * out of this translation unit. */
    if (shell_text_equals_ignore_case(argv[0], "display")) {
        return shell_command_display(argc, argv);
    }

    if (shell_text_equals_ignore_case(argv[0], "keyboard")) {
        return shell_command_keyboard(argc, argv);
    }

    if (shell_text_equals_ignore_case(argv[0], "windows")) {
        return shell_command_windows(argc, argv);
    }

    /* ---- Module-routed commands ----
     * These receive the original unsplit line because their own parsers
     * need the full text (for example `wifi connect <ssid> <password>`). */
    if (shell_text_equals_ignore_case(argv[0], "wifi")) {
        networking_handle_wifi_command(family_command);
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "bluetooth") || shell_text_equals_ignore_case(argv[0], "bt")) {
        bluetooth_handle_command(family_command);
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "usb")) {
        usb_handle_command(family_command);
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "c6ota")) {
        free(family_command);
        c6ota_perform(argc >= 2 ? argv[1] : NULL);
        return true;
    }

    /* ---- SD tools ---- */
    if (shell_text_equals_ignore_case(argv[0], "sd")) {
        shell_command_sd(family_command);
        free(family_command);
        return true;
    }

    /* ---- Disk and partition tools (diskpart style) ---- */
    if (shell_text_equals_ignore_case(argv[0], "disk")) {
        shell_command_disk(family_command);
        free(family_command);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sdeject")) {
        free(family_command);
        shell_command_sd_eject();
        return true;
    }

    /* ---- DOS-style file commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "cd") || shell_text_equals_ignore_case(argv[0], "chdir")) {
        shell_command_cd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dir")) {
        shell_command_dir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "copy")) {
        shell_command_copy(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "move")) {
        shell_command_move(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "del") || shell_text_equals_ignore_case(argv[0], "erase")) {
        shell_command_del(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ren") || shell_text_equals_ignore_case(argv[0], "rename")) {
        shell_command_rename(argc, argv, argv[0]);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "md") || shell_text_equals_ignore_case(argv[0], "mkdir")) {
        shell_command_mkdir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rd") || shell_text_equals_ignore_case(argv[0], "rmdir")) {
        shell_command_rmdir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "type")) {
        shell_command_type_file(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "write")) {
        shell_command_write_file(argc, argv, false);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "append")) {
        shell_command_write_file(argc, argv, true);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "touch")) {
        shell_command_touch(argc, argv);
        return true;
    }

    /* ---- Extended DOS commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "attrib")) {
        shell_command_attrib(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "label")) {
        shell_command_label(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "xcopy")) {
        shell_command_xcopy(argc, argv);
        return true;
    }

    /* ---- Volume management ---- */
    if (shell_text_equals_ignore_case(argv[0], "chkdsk") ||
        shell_text_equals_ignore_case(argv[0], "scandisk")) {
        shell_command_chkdsk(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "format")) {
        shell_command_format(argc, argv);
        return true;
    }

    /* ---- Environment and batch commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "set")) {
        shell_command_set(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "path")) {
        shell_command_path(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "echo")) {
        shell_command_echo(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "call")) {
        shell_command_call(argc, argv);
        return true;
    }

    /* Batch comment lines are accepted interactively as no-ops. */
    if (shell_text_equals_ignore_case(argv[0], "rem") || strncmp(argv[0], "::", 2) == 0) {
        return true;
    }

    /* ---- Batch control flow ---- */
    if (shell_text_equals_ignore_case(argv[0], "if")) {
        shell_command_if(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "goto")) {
        shell_command_goto(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "shift")) {
        shell_command_shift(argc, argv);
        return true;
    }

    /* ---- Extended built-in commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "pause")) {
        shell_command_pause(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "choice")) {
        shell_command_choice(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "setlocal")) {
        shell_command_setlocal(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "endlocal")) {
        shell_command_endlocal(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "prompt")) {
        shell_command_prompt_cmd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "date")) {
        shell_command_date(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "time")) {
        shell_command_time_cmd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "exit")) {
        shell_command_exit(argc, argv);
        return true;
    }

    /* ---- File utility commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "find")) {
        shell_command_find(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "more")) {
        shell_command_more(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tree")) {
        shell_command_tree(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "fc")) {
        shell_command_fc(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sort")) {
        shell_command_sort(argc, argv);
        return true;
    }

    /* ---- Network connectivity commands: ping, dns ----
     * These live in components/networking (the sole owner of the lwIP /
     * esp_ping surface) and return an esp_err_t that the dispatcher maps onto
     * ERRORLEVEL, so `ping host && echo up`, `ping host || echo down`, and the
     * equivalent batch-file forms work exactly like DOS. Redirection and pipes
     * capture their transcript output automatically. */
    if (shell_text_equals_ignore_case(argv[0], "ping")) {
        int count = 0;
        esp_err_t error;

        if (argc < 2) {
            shell_print_usage("Usage: ping <host-or-ip> [count]");
            batch_set_errorlevel(2);
            return true;
        }
        if (argc > 3) {
            shell_print_usage("Usage: ping <host-or-ip> [count]");
            batch_set_errorlevel(2);
            return true;
        }
        if (argc == 3) {
            char *end = NULL;
            long parsed = strtol(argv[2], &end, 10);

            if (end == NULL || *end != '\0' || parsed < 1) {
                shell_print_usage("Usage: ping <host-or-ip> [count]  (count 1..%d)", P4_CONFIG_PING_COUNT_MAX);
                batch_set_errorlevel(2);
                return true;
            }
            count = (int)parsed;
        }

        error = networking_wifi_ping(argv[1], count);
        batch_set_errorlevel(error == ESP_OK ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dns") || shell_text_equals_ignore_case(argv[0], "nslookup")) {
        esp_err_t error;

        if (argc != 2) {
            shell_print_usage("Usage: dns <hostname>");
            batch_set_errorlevel(2);
            return true;
        }

        error = networking_wifi_dns_lookup(argv[1]);
        batch_set_errorlevel(error == ESP_OK ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "httpget") || shell_text_equals_ignore_case(argv[0], "wget")) {
        networking_http_result_t result;
        esp_err_t error;

        if (argc < 2 || argc > 3) {
            shell_print_usage("Usage: httpget <url> [localfile]");
            batch_set_errorlevel(2);
            return true;
        }

        error = networking_http_get(argv[1], &result);
        if (error != ESP_OK) {
            networking_http_result_free(&result);
            batch_set_errorlevel(1);
            return true;
        }

        if (argc == 3) {
            /* Save the exact body to the SD card through the storage write
             * path: cwd-relative resolution, guarded session, free-space
             * precheck, and partial-destination cleanup on failure. */
            char resolved[SHELL_SD_PATH_BYTES];
            shell_sd_session_t session;
            FILE *file = NULL;

            if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("httpget: invalid path %s", argv[2]);
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            if (shell_sd_begin(&session) != ESP_OK) {
                shell_print_error("httpget: SD card not present - insert and retry");
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            /* Refuse before opening so an overwrite cannot destroy the existing
             * file and then fail for lack of room. */
            {
                uint64_t reclaim = storage_get_file_size(resolved);

                if (!storage_check_free_space(result.body_size, reclaim, "httpget")) {
                    shell_sd_end(&session, "httpget");
                    networking_http_result_free(&result);
                    batch_set_errorlevel(1);
                    return true;
                }
            }

            file = fopen(resolved, "wb");
            if (file == NULL) {
                shell_print_error("httpget: failed to open %s", resolved);
                shell_sd_end(&session, "httpget");
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            if (result.body_size > 0 &&
                fwrite(result.body, 1, result.body_size, file) != result.body_size) {
                fclose(file);
                (void)remove(resolved);
                shell_print_error("httpget: write failed - removed partial %s", resolved);
                shell_sd_end(&session, "httpget");
                networking_http_result_free(&result);
                batch_set_errorlevel(1);
                return true;
            }

            fclose(file);
            shell_sd_end(&session, "httpget");
            shell_print_ok("httpget: saved %u bytes to %s",
                           (unsigned int)result.body_size, resolved);
        } else {
            /* Print the body to the transcript (bounded and sanitized). A
             * trailing newline keeps the next prompt on its own line. */
            shell_print_http_body(result.body, result.body_size);
            shell_transcript_append_text("\n");
        }

        networking_http_result_free(&result);
        batch_set_errorlevel(0);
        return true;
    }

    /* ---- Batch file direct execution ---- */
    {
        char batch_path[SHELL_SD_PATH_BYTES];

        if (shell_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
            shell_execute_batch_file(batch_path, argc - 1, &argv[1]);
            return true;
        }
    }

    free(family_command);
    return false;
}

/* ========================================================================
 * COMMAND EXECUTION PIPELINE
 * ========================================================================
 * A command line goes through five stages before dispatch:
 *   1. Chain splitting on unquoted &, &&, and || — one segment at a time
 *   2. Variable expansion (%VAR%, %0..%9, %*) — owned by components/batch
 *   3. Redirection parsing (>, >>, and <)
 *   4. Input redirection published to components/storage for the stage
 *   5. Dispatch, then capture of the transcript delta for output redirection.
 *
 * Expansion happens per segment rather than once for the whole line. That way
 * a variable whose value contains an `&` cannot inject a new command, which
 * is the same reason COMMAND.COM parses separators before expanding.
 */

/**
 * Run one command with expansion, redirection, and dispatch.
 *
 * Sets errorlevel so conditional chaining has something to test: a recognized
 * command that does not set it explicitly reports success, and an unrecognized
 * one reports P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND.
 *
 * @return true when the command was recognized and executed.
 */
static bool shell_execute_command_segment(char *command)
{
    /* Expansion needs two line-sized buffers. They live on the heap because
     * this function sits on the batch recursion path: a batch file calls back
     * into the pipeline for every line, and four nested levels of two 768-byte
     * stack buffers would overflow the command worker task's stack. */
    const size_t work_size = SHELL_BATCH_LINE_BYTES * 2;
    char *expanded = NULL;
    char *command_buffer = NULL;
    char *command_part = NULL;
    char *redirect_target = NULL;
    char *input_source = NULL;
    bool append_mode = false;
    bool input_redirect_set = false;
    bool recognized;
    int errorlevel_before;
    size_t transcript_len_before;

    if (command == NULL) {
        return false;
    }

    expanded = malloc(work_size);
    command_buffer = malloc(work_size);
    if (expanded == NULL || command_buffer == NULL) {
        free(expanded);
        free(command_buffer);
        shell_transcript_append_text("shell: out of memory expanding the command line\n");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory expanding a command line");
        batch_set_errorlevel(1);
        return false;
    }

    shell_expand_variables(command, expanded, work_size);
    snprintf(command_buffer, work_size, "%s", expanded);
    shell_parse_redirection(command_buffer, &command_part, &redirect_target, &append_mode, &input_source);

    /* Publish the `<` source so the text-processing commands can pick it up
     * when the user gave no filename argument. A pipeline stage sets the same
     * slot, so both spellings reach one code path. */
    if (input_source != NULL && input_source[0] != '\0') {
        char resolved_input[SHELL_SD_PATH_BYTES];
        esp_err_t error = shell_fs_resolve_path(input_source, resolved_input, sizeof(resolved_input));

        if (error != ESP_OK) {
            shell_print_error("redirection: invalid input path %s", input_source);
            shell_record_warningf("shell", "Invalid input redirection path %s", input_source);
            batch_set_errorlevel(1);
            free(expanded);
            free(command_buffer);
            return false;
        }

        storage_set_input_redirect(resolved_input);
        input_redirect_set = true;
    }

    /* Remember where the transcript ends so the redirection layer can copy
     * exactly the output this command produced. */
    transcript_len_before = shell_transcript_get_length();

    /* Snapshot errorlevel rather than clearing it. Clearing would destroy the
     * value that the very next `if errorlevel N` is meant to read, and DOS
     * only changes errorlevel when a command actually reports a status. A
     * command counts as failed for chaining purposes when it leaves a new
     * non-zero errorlevel behind. */
    errorlevel_before = batch_get_errorlevel();

    recognized = shell_execute_command_core(command_part);
    if (!recognized) {
        shell_transcript_appendf_ansi(SH_ERR "Unknown command:" SH_RST " %s\n", command_part);
        shell_record_warningf("shell", "Unknown command: %s", command_part);
        batch_set_errorlevel(P4_CONFIG_ERRORLEVEL_UNKNOWN_COMMAND);
    }

    /* The input slot belongs to exactly one command. Clear it here so a
     * failed dispatch cannot leak the source into the next line. */
    if (input_redirect_set) {
        storage_clear_input_redirect();
    }

    if (redirect_target != NULL && redirect_target[0] != '\0') {
        const char *captured = shell_transcript_get_text_from(transcript_len_before);
        esp_err_t error = shell_write_redirect_output(redirect_target,
                                                      captured != NULL ? captured : "",
                                                      append_mode);
        if (error != ESP_OK) {
            shell_print_error("redirection: failed to write %s (%s)",
                                     redirect_target,
                                     esp_err_to_name(error));
            shell_record_warningf("shell", "Failed to redirect command output to %s", redirect_target);
            batch_set_errorlevel(1);
        }
    }

    free(expanded);
    free(command_buffer);

    /* A command "succeeded" when it was recognized and did not raise a new
     * non-zero errorlevel. Comparing against the snapshot means a stale value
     * from an earlier line cannot make this command look failed. */
    if (!recognized) {
        return false;
    }

    {
        int errorlevel_after = batch_get_errorlevel();

        return (errorlevel_after == 0) || (errorlevel_after == errorlevel_before);
    }
}

void shell_execute_command(char *command)
{
    shell_chain_segment_t segments[SHELL_CHAIN_SEGMENT_MAX];
    const size_t chain_size = SHELL_BATCH_LINE_BYTES * 2;
    char *chain_buffer;
    bool truncated = false;
    bool previous_succeeded;
    int segment_count;
    int index;

    if (command == NULL) {
        return;
    }

    /* Chain splitting works on a private copy: shell_split_chain() writes
     * terminators in place, and the caller's buffer may be a batch line that
     * the executor still needs intact. Heap-allocated because this function
     * is on the batch recursion path. */
    chain_buffer = malloc(chain_size);
    if (chain_buffer == NULL) {
        shell_transcript_append_text("shell: out of memory splitting the command line\n");
        shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory splitting a command chain");
        return;
    }

    snprintf(chain_buffer, chain_size, "%s", command);

    segment_count = shell_split_chain(chain_buffer,
                                      segments,
                                      SHELL_CHAIN_SEGMENT_MAX,
                                      &truncated);
    if (segment_count <= 0) {
        free(chain_buffer);
        return;
    }

    if (truncated) {
        shell_transcript_appendf("chain: at most %d chained commands are supported\n",
                                 SHELL_CHAIN_SEGMENT_MAX);
        shell_record_warningf("shell", "Command chain truncated at %d segments", SHELL_CHAIN_SEGMENT_MAX);
    }

    /* Tracks the outcome of the most recently executed link. A skipped link
     * leaves it untouched, so `a && b && c` correctly skips c when a failed. */
    previous_succeeded = true;

    for (index = 0; index < segment_count; index++) {
        char *segment = segments[index].command;

        /* Decide whether this link runs, based on the previous outcome. */
        switch (segments[index].op) {
        case SHELL_CHAIN_ON_SUCCESS:
            if (!previous_succeeded) {
                continue;
            }
            break;
        case SHELL_CHAIN_ON_FAILURE:
            if (previous_succeeded) {
                continue;
            }
            break;
        case SHELL_CHAIN_FIRST:
        case SHELL_CHAIN_ALWAYS:
        default:
            break;
        }

        if (segment == NULL || segment[0] == '\0') {
            /* An empty link is only an error when a separator implied one. */
            if (segments[index].op != SHELL_CHAIN_FIRST) {
                shell_transcript_append_text("chain: empty command between separators\n");
                batch_set_errorlevel(1);
                previous_succeeded = false;
            }
            continue;
        }

        previous_succeeded = shell_execute_command_segment(segment);
    }

    free(chain_buffer);
}

/** Persistent worker task: waits on the command queue and executes commands
 *  sequentially. This replaces the per-command task creation pattern. */
static void command_worker_task(void *arg)
{
    command_request_t request;

    (void)arg;

    while (true) {
        if (xQueueReceive(s_command_queue, &request, portMAX_DELAY) == pdTRUE) {
            shell_execute_command(request.command);
        }
    }
}

void shell_execute_command_async(char *command)
{
    command_request_t request;

    if (command == NULL || command[0] == '\0') {
        return;
    }

    if (s_command_queue == NULL) {
        shell_print_error("shell: command worker not initialized");
        shell_record_errorf("shell", ESP_FAIL, "Command worker not initialized");
        return;
    }

    snprintf(request.command, sizeof(request.command), "%s", command);

    if (xQueueSend(s_command_queue, &request, 0) != pdTRUE) {
        shell_print_error("shell: command queue full, command dropped");
        shell_record_warningf("shell", "Command queue full, dropped: %s", command);
    }
}

bool shell_command_ota_is_pending(void)
{
    return c6ota_is_confirmation_pending();
}

/* ========================================================================
 * SHELL STATE ACCESSORS
 * ======================================================================== */

int command_get_volume_percent(void)
{
    return s_volume_percent;
}

void command_set_volume(int percent)
{
    esp_err_t error;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    error = shell_audio_ensure_speaker();
    if (error != ESP_OK) {
        ESP_LOGW(COMMAND_TAG, "command_set_volume: speaker init failed (%s)", esp_err_to_name(error));
        return;
    }

    if (esp_codec_dev_set_out_vol(s_speaker_dev, percent) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(COMMAND_TAG, "command_set_volume: codec set failed");
        return;
    }

    s_volume_percent = percent;
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void command_init(void)
{
    static const shell_command_ops_t shell_ops = {
        .execute_command    = shell_execute_command,
        .execute_command_async = shell_execute_command_async,
        .get_cwd            = shell_get_cwd,
        .get_volume_percent = command_get_volume_percent,
        .sd_is_mounted      = storage_sd_is_mounted,
        .battery_read       = command_battery_read,

        /* External-module accessors. These let the shell core read state from
         * the networking, Bluetooth, USB, and C6 OTA modules without including
         * their headers, keeping the dependency direction one-way. */
        .wifi_is_connected      = networking_wifi_is_connected,
        .wifi_get_rssi          = networking_wifi_get_rssi,
        .wifi_state_string      = networking_wifi_state_string,
        .append_sysinfo_summary = networking_append_sysinfo_summary,
        .bluetooth_is_enabled   = bluetooth_is_enabled,
        .bluetooth_is_connected = bluetooth_is_connected,
        .usb_is_connected       = usb_is_connected,
        .usb_is_keyboard_attached = usb_is_keyboard_attached,
        .usb_key_to_ascii       = usb_key_to_ascii_full,
        .c6ota_is_pending       = c6ota_is_confirmation_pending,
        .c6ota_is_busy          = c6ota_is_busy,
    };
    static const batch_command_ops_t batch_ops = {
        .execute_command = shell_execute_command,
    };

    if (s_initialized) {
        return;
    }

    /* Bring the owned-state modules up before anything can dispatch:
     * storage owns the current working directory and SD mount tracking,
     * batch owns the environment table, PATH, and errorlevel. */
    storage_init();
    batch_init();

    /* Create the persistent command queue and worker task. The queue
     * replaces per-command task creation: commands are posted to the
     * queue and processed sequentially by one long-lived task, eliminating
     * task-creation overhead and heap fragmentation. */
    s_command_queue = xQueueCreate(SHELL_COMMAND_QUEUE_DEPTH, sizeof(command_request_t));
    if (s_command_queue == NULL) {
        ESP_LOGE(COMMAND_TAG, "Failed to create command queue");
    } else if (xTaskCreate(command_worker_task,
                           "shell_cmd",
                           SHELL_COMMAND_TASK_STACK_BYTES,
                           NULL,
                           tskIDLE_PRIORITY + 2,
                           &s_command_worker_handle) != pdPASS) {
        ESP_LOGE(COMMAND_TAG, "Failed to create command worker task");
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
    }

    /* Publish the command pipeline to the batch engine so nested contexts
     * (if bodies, for bodies, pipe stages, batch lines) inherit variable
     * expansion and output redirection. */
    batch_register_command_ops(&batch_ops);

    /* Publish this module's services to the shell core. Keeping the
     * dependency one-way (command -> shell) avoids a component cycle. */
    shell_register_command_ops(&shell_ops);

    s_initialized = true;
    ESP_LOGI(COMMAND_TAG, "Command module initialized");
}

bool command_is_initialized(void)
{
    return s_initialized;
}

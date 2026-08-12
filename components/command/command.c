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
#include "http_server.h"
#include "netdiag.h"
#include "led.h"
#include "bluetooth.h"
#include "c6ota.h"
#include "clock.h"
#include "usb.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_codec_dev.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#if SOC_ADC_SUPPORTED
#include "soc/adc_channel.h"
#endif
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
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
#define SHELL_POWER_SLEEP_DEFAULT_SECS  P4_CONFIG_POWER_SLEEP_DEFAULT_SECS
#define SHELL_POWER_SLEEP_MAX_SECS      P4_CONFIG_POWER_SLEEP_MAX_SECS
#define SHELL_POWER_SLEEP_PRE_DELAY_MS  P4_CONFIG_POWER_SLEEP_PRE_DELAY_MS
#define SHELL_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI P4_CONFIG_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI
#define SHELL_PWM_FREQ_MAX_HZ           P4_CONFIG_PWM_FREQ_MAX_HZ
#define SHELL_PWM_SRC_CLK_HZ            P4_CONFIG_PWM_SRC_CLK_HZ
#define SHELL_PWM_CLK_SOURCE            P4_CONFIG_PWM_CLK_SOURCE
#define SHELL_PWM_CHANNEL_MAX           P4_CONFIG_PWM_CHANNEL_MAX
#define SHELL_PWM_DUTY_DEFAULT_PCT      P4_CONFIG_PWM_DUTY_DEFAULT_PCT
#define SHELL_ADC_DEFAULT_SAMPLES       P4_CONFIG_ADC_DEFAULT_SAMPLES
#define SHELL_ADC_MAX_SAMPLES           P4_CONFIG_ADC_MAX_SAMPLES
#define SHELL_ADC_ATTEN                 P4_CONFIG_ADC_ATTEN
#define SHELL_I2C_TOOL_TIMEOUT_MS       P4_CONFIG_I2C_TOOL_TIMEOUT_MS
#define SHELL_I2C_SCAN_PROBE_TIMEOUT_MS P4_CONFIG_I2C_SCAN_PROBE_TIMEOUT_MS
#define SHELL_I2C_SCAN_FIRST_ADDR       P4_CONFIG_I2C_SCAN_FIRST_ADDR
#define SHELL_I2C_SCAN_LAST_ADDR        P4_CONFIG_I2C_SCAN_LAST_ADDR
#define SHELL_I2C_TOOL_CLK_HZ           P4_CONFIG_I2C_TOOL_CLK_HZ
#define SHELL_SPI_TOOL_CLK_HZ           P4_CONFIG_SPI_TOOL_CLK_HZ
#define SHELL_SPI_TOOL_TIMEOUT_MS       P4_CONFIG_SPI_TOOL_TIMEOUT_MS
#define SHELL_SPI_TOOL_BUFFER_BYTES     P4_CONFIG_SPI_TOOL_BUFFER_BYTES
#define SHELL_SPI_TOOL_HOST             P4_CONFIG_SPI_TOOL_HOST

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
static void shell_command_power(int argc, char **argv);
static void shell_command_sleep(int argc, char **argv);
static void shell_command_deepsleep(int argc, char **argv);
static void shell_command_reboot(void);
static void shell_command_clear(void);
static void shell_command_gpio_status(void);
static void shell_execute_gpio_command(int argc, char **argv);
static void shell_execute_pwm_command(int argc, char **argv);
static void shell_execute_freq_command(int argc, char **argv);
static void shell_execute_adc_command(int argc, char **argv);
static void shell_execute_i2c_command(int argc, char **argv);
static void shell_execute_spi_command(int argc, char **argv);
static void shell_execute_rgb_command(int argc, char **argv);
static void shell_execute_camera_command(int argc, char **argv);
static void shell_command_prompt_cmd(int argc, char **argv);

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
    {"led_status", (gpio_num_t)BOARD_CFG_RGB_LED_GPIO, "WS2812 RGB status LED (LED1, back panel)", false, true},
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

/**
 * Pin-safety gate for the peripheral toolkit (pwm, freq, adc, i2c, spi).
 *
 * A pin is reserved when it is missing from the exposed table (untracked),
 * out of the GPIO range, or listed in the table as critical (active board
 * lines such as I2C, I2S, SDIO, display, SD, and the battery ADC). Every
 * toolkit command refuses reserved pins, so a peripheral never disturbs a
 * live board function.
 */
static bool shell_pin_is_reserved(int gpio_num)
{
    const shell_gpio_pin_desc_t *pin;

    if (gpio_num < 0 || gpio_num > GPIO_NUM_MAX || gpio_num == GPIO_NUM_NC) {
        return true;
    }
    pin = shell_find_gpio_pin(gpio_num);
    return pin != NULL && pin->critical;
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

/* ========================================================================
 * POWER COMMANDS: power, sleep, deepsleep
 *
 * `power`     reports power-management state (PM, light sleep request,
 *             display, Wi-Fi, battery, last wake cause).
 * `sleep`     enters light sleep: RAM is retained and the shell resumes
 *             on wake with all state intact.
 * `deepsleep` enters deep sleep: RAM is lost and the device reboots on
 *             wake (same path as `reboot`).
 *
 * Battery telemetry goes through command_battery_read() (the same ADC path
 * the `battery` command and header use), and Wi-Fi/hosted state is torn down
 * through networking_wifi_shutdown() (the same path C6 OTA uses).
 * ======================================================================== */

static const char *shell_power_wake_cause_string(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:             return "ext0";
        case ESP_SLEEP_WAKEUP_EXT1:             return "ext1";
        case ESP_SLEEP_WAKEUP_TIMER:            return "timer";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:         return "touchpad";
        case ESP_SLEEP_WAKEUP_ULP:              return "ulp";
        case ESP_SLEEP_WAKEUP_GPIO:             return "gpio";
        case ESP_SLEEP_WAKEUP_UART:             return "uart";
        case ESP_SLEEP_WAKEUP_UART1:            return "uart1";
        case ESP_SLEEP_WAKEUP_UART2:            return "uart2";
        case ESP_SLEEP_WAKEUP_WIFI:             return "wifi";
        case ESP_SLEEP_WAKEUP_COCPU:            return "cocpu";
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:  return "cocpu-trap";
        case ESP_SLEEP_WAKEUP_BT:               return "bluetooth";
        case ESP_SLEEP_WAKEUP_USB:              return "usb";
        default:                                return "none";
    }
}

/** Read and print the battery through the shared ADC path. */
static bool shell_power_report_battery(const char *label)
{
    int battery_mv;
    int percent;
    int raw;
    int gpio_mv;
    esp_err_t error;

    error = command_battery_read(&battery_mv, &percent, &raw, &gpio_mv);
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_LBL "%s:" SH_RST " " SH_LBL "battery" SH_RST " " SH_MUTE "unavailable" SH_RST
                                 " (" SH_WARN "%s" SH_RST ")\n",
                                 label, esp_err_to_name(error));
        return false;
    }
    shell_transcript_appendf_ansi(SH_LBL "%s:" SH_RST " " SH_LBL "battery" SH_RST " " SH_NUM "%d%%" SH_RST
                             " (" SH_NUM "%d.%03d V" SH_RST ")\n",
                             label, percent, battery_mv / 1000, battery_mv % 1000);
    return true;
}

/**
 * Parse an optional duration argument in seconds. With no argument the
 * configured default is used; out-of-range values are clamped so a typo
 * cannot put the board to sleep for days.
 */
static bool shell_power_parse_seconds(int argc, char **argv, uint32_t *seconds_out)
{
    char *end;
    long value;

    if (argc < 2) {
        *seconds_out = SHELL_POWER_SLEEP_DEFAULT_SECS;
        return true;
    }
    value = strtol(argv[1], &end, 10);
    if (*end != '\0' || value < 0) {
        return false;
    }
    if (value > SHELL_POWER_SLEEP_MAX_SECS) {
        value = SHELL_POWER_SLEEP_MAX_SECS;
    }
    *seconds_out = (uint32_t)value;
    return true;
}

/** Tear down Wi-Fi/hosted state so the radio cannot keep the SoC awake. */
static esp_err_t shell_power_shutdown_wifi(void)
{
    esp_err_t error = networking_wifi_shutdown();

    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_LBL "power:" SH_RST " " SH_LBL "wifi shutdown failed" SH_RST
                                 " (" SH_WARN "%s" SH_RST ")\n",
                                 esp_err_to_name(error));
    }
    return error;
}

static void shell_command_power(int argc, char **argv)
{
    esp_sleep_wakeup_cause_t wake_cause;
    display_power_state_t display_state;

    if (argc >= 2 && !shell_text_equals_ignore_case(argv[1], "status")) {
        shell_print_usage("Usage: power [status]");
        shell_record_warningf("power", "Usage error for power command");
        return;
    }

    shell_power_report_battery("power");

    shell_transcript_appendf_ansi(SH_LBL "power.pm:" SH_RST " ");
#if CONFIG_PM_ENABLE
    shell_transcript_appendf_ansi(SH_OK "enabled" SH_RST " ");
    if (s_light_sleep_requested) {
        shell_transcript_appendf_ansi(SH_LBL "light sleep" SH_RST " " SH_OK "requested" SH_RST "\n");
    } else {
        shell_transcript_appendf_ansi(SH_LBL "light sleep" SH_RST " " SH_MUTE "off" SH_RST "\n");
    }
#else
    shell_transcript_appendf_ansi(SH_MUTE "disabled" SH_RST " (light sleep needs CONFIG_PM_ENABLE)\n");
#endif

    display_state = display_get_power_state();
    shell_transcript_appendf_ansi(SH_LBL "power.display:" SH_RST " ");
    if (display_state == DISPLAY_POWER_ON) {
        shell_transcript_appendf_ansi(SH_OK "on" SH_RST "\n");
    } else if (display_state == DISPLAY_POWER_SLEEP) {
        shell_transcript_appendf_ansi(SH_WARN "sleep" SH_RST "\n");
    } else {
        shell_transcript_appendf_ansi(SH_ERR "off" SH_RST "\n");
    }

    shell_transcript_appendf_ansi(SH_LBL "power.wifi:" SH_RST " " SH_NUM "%s" SH_RST "\n",
                             networking_wifi_is_connected() ? "connected" : "down");

    wake_cause = esp_sleep_get_wakeup_cause();
    shell_transcript_appendf_ansi(SH_LBL "power.wake:" SH_RST " " SH_NUM "%s" SH_RST "\n",
                             shell_power_wake_cause_string(wake_cause));
    shell_transcript_appendf_ansi(SH_MUTE "Tip: `battery sleep on` enables automatic light sleep when idle.\n");
}

static void shell_command_sleep(int argc, char **argv)
{
    uint32_t seconds;
    esp_err_t error;

    if (!shell_power_parse_seconds(argc, argv, &seconds)) {
        shell_print_usage("Usage: sleep [seconds]");
        shell_record_warningf("sleep", "Usage error for sleep command");
        return;
    }

    shell_power_report_battery("sleep");

    if (seconds == 0) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        shell_transcript_appendf_ansi(SH_WARN "sleep: no timer set; wake only from an external wake source" SH_RST "\n");
    } else {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
        shell_transcript_appendf_ansi(SH_LBL "sleep:" SH_RST " timer wake in " SH_NUM "%u" SH_RST " s\n",
                                 (unsigned)seconds);
    }

#if SHELL_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI
    shell_power_shutdown_wifi();
#else
    shell_transcript_appendf_ansi(SH_MUTE "sleep: keeping Wi-Fi state (light sleep wifi shutdown disabled)\n");
#endif

    display_set_power_state(DISPLAY_POWER_OFF);

    /* Give the transcript and LVGL task time to paint before sleeping. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_POWER_SLEEP_PRE_DELAY_MS));

    error = esp_light_sleep_start();
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "sleep: light sleep failed" SH_RST " (" SH_WARN "%s" SH_RST ")\n",
                                 esp_err_to_name(error));
    } else {
        shell_transcript_appendf_ansi(SH_LBL "sleep:" SH_RST " woke up (" SH_LBL "cause" SH_RST "=" SH_NUM "%s" SH_RST ")\n",
                                 shell_power_wake_cause_string(esp_sleep_get_wakeup_cause()));
    }

    display_set_power_state(DISPLAY_POWER_ON);

#if SHELL_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI
    shell_transcript_appendf_ansi(SH_MUTE "sleep: Wi-Fi was shut down; use `wifi connect` to reconnect.\n");
#endif
}

static void shell_command_deepsleep(int argc, char **argv)
{
    uint32_t seconds;

    if (!shell_power_parse_seconds(argc, argv, &seconds)) {
        shell_print_usage("Usage: deepsleep [seconds]");
        shell_record_warningf("deepsleep", "Usage error for deepsleep command");
        return;
    }

    shell_power_report_battery("deepsleep");

    shell_transcript_appendf_ansi(SH_WARN "deepsleep: RAM state (env, aliases, cwd, variables) is lost on wake" SH_RST "\n");
    if (seconds == 0) {
        shell_transcript_appendf_ansi(SH_WARN "deepsleep: no timer set; wake requires an external wake source" SH_RST "\n");
    } else {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
        shell_transcript_appendf_ansi(SH_LBL "deepsleep:" SH_RST " timer wake in " SH_NUM "%u" SH_RST " s\n",
                                 (unsigned)seconds);
    }

    shell_power_shutdown_wifi();
    display_set_power_state(DISPLAY_POWER_OFF);

    /* Let the transcript and LVGL task paint before the chip resets. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_POWER_SLEEP_PRE_DELAY_MS));
    shell_transcript_appendf_ansi(SH_LBL "deepsleep:" SH_RST " entering deep sleep\n");
    vTaskDelay(pdMS_TO_TICKS(SHELL_REBOOT_DELAY_MS));

    esp_deep_sleep_start();
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
 * `prompt` â€” show or set the DOS prompt template.
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
 * PERIPHERAL TOOLKIT: pwm, freq, adc, i2c, spi
 * ========================================================================
 * A richer GPIO/peripheral surface built on the LEDC, ADC one-shot, I2C
 * master, and SPI master drivers. Every command gates its pins through
 * shell_pin_is_reserved() so active board lines (I2C, I2S, SDIO, display,
 * SD, battery ADC) can never be repurposed by a peripheral.
 */

/* ---- PWM and square-wave generation (LEDC) ----
 * The display backlight owns LEDC channel 1 / timer 1 on this board, so the
 * toolkit allocates channels and timers from the remaining set and never
 * touches the backlight path. */

#define SHELL_PWM_BACKLIGHT_CHANNEL      BOARD_CFG_DISPLAY_BRIGHTNESS_LEDC_CH
#define SHELL_PWM_BACKLIGHT_TIMER        BOARD_CFG_LCD_BACKLIGHT_PWM_TIMER

/** LEDC timers available to the toolkit (P4 has 4 timers, timer 1 is used). */
static const ledc_timer_t s_pwm_timers[] = {
    LEDC_TIMER_0, LEDC_TIMER_2, LEDC_TIMER_3
};

/** LEDC channels available to the toolkit (channel 1 is used by backlight). */
static const ledc_channel_t s_pwm_channels[] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_2, LEDC_CHANNEL_3,
    LEDC_CHANNEL_4, LEDC_CHANNEL_5, LEDC_CHANNEL_6, LEDC_CHANNEL_7
};

typedef struct {
    bool in_use;
    int gpio_num;
    ledc_channel_t channel;
    ledc_timer_t timer;
    uint32_t freq_hz;
    uint32_t duty_pct;
} shell_pwm_slot_t;

static shell_pwm_slot_t s_pwm_slots[SHELL_PWM_CHANNEL_MAX];

static shell_pwm_slot_t *shell_pwm_find_slot_by_gpio(int gpio_num)
{
    size_t index;

    for (index = 0; index < SHELL_PWM_CHANNEL_MAX; index++) {
        if (s_pwm_slots[index].in_use && s_pwm_slots[index].gpio_num == gpio_num) {
            return &s_pwm_slots[index];
        }
    }
    return NULL;
}

static shell_pwm_slot_t *shell_pwm_find_free_slot(void)
{
    size_t index;

    for (index = 0; index < SHELL_PWM_CHANNEL_MAX; index++) {
        if (!s_pwm_slots[index].in_use) {
            return &s_pwm_slots[index];
        }
    }
    return NULL;
}

/** Claim a free timer and channel that no other active slot is using. */
static esp_err_t shell_pwm_claim(ledc_timer_t *timer_out, ledc_channel_t *channel_out)
{
    size_t i;
    size_t j;

    for (i = 0; i < sizeof(s_pwm_timers) / sizeof(s_pwm_timers[0]); i++) {
        bool used = false;
        for (j = 0; j < SHELL_PWM_CHANNEL_MAX; j++) {
            if (s_pwm_slots[j].in_use && s_pwm_slots[j].timer == s_pwm_timers[i]) {
                used = true;
                break;
            }
        }
        if (!used) {
            *timer_out = s_pwm_timers[i];
            break;
        }
    }
    if (i == sizeof(s_pwm_timers) / sizeof(s_pwm_timers[0])) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < sizeof(s_pwm_channels) / sizeof(s_pwm_channels[0]); i++) {
        bool used = false;
        for (j = 0; j < SHELL_PWM_CHANNEL_MAX; j++) {
            if (s_pwm_slots[j].in_use && s_pwm_slots[j].channel == s_pwm_channels[i]) {
                used = true;
                break;
            }
        }
        if (!used) {
            *channel_out = s_pwm_channels[i];
            break;
        }
    }
    if (i == sizeof(s_pwm_channels) / sizeof(s_pwm_channels[0])) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/**
 * Pick the LEDC duty resolution that can represent the requested frequency
 * given the assumed timer source clock, capped at the SOC timer width.
 */
static ledc_timer_bit_t shell_pwm_pick_resolution(uint32_t freq_hz)
{
    int bits = 20; /* SOC_LEDC_TIMER_BIT_WIDTH */

    while (bits > 1 && ((uint64_t)freq_hz << bits) > (uint64_t)SHELL_PWM_SRC_CLK_HZ) {
        bits--;
    }
    return (ledc_timer_bit_t)bits;
}

/** Configure or update an LEDC output for a pin. */
static esp_err_t shell_pwm_apply(int gpio_num, uint32_t freq_hz, uint32_t duty_pct)
{
    shell_pwm_slot_t *slot;
    ledc_timer_t timer;
    ledc_channel_t channel;
    ledc_timer_bit_t resolution;
    uint32_t max_duty;
    uint32_t duty_count;
    esp_err_t error;

    slot = shell_pwm_find_slot_by_gpio(gpio_num);
    if (slot == NULL) {
        error = shell_pwm_claim(&timer, &channel);
        if (error != ESP_OK) {
            return error;
        }
        slot = shell_pwm_find_free_slot();
        if (slot == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        slot->in_use = true;
        slot->gpio_num = gpio_num;
        slot->timer = timer;
        slot->channel = channel;
    }

    resolution = shell_pwm_pick_resolution(freq_hz);
    {
        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = resolution,
            .timer_num = slot->timer,
            .freq_hz = freq_hz,
            .clk_cfg = (ledc_clk_cfg_t)SHELL_PWM_CLK_SOURCE,
        };
        error = ledc_timer_config(&timer_cfg);
        if (error != ESP_OK) {
            goto fail;
        }
    }

    max_duty = (1UL << (int)resolution) - 1;
    duty_count = (uint32_t)(((uint64_t)max_duty * duty_pct) / 100);
    {
        ledc_channel_config_t channel_cfg = {
            .gpio_num = gpio_num,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = slot->channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = slot->timer,
            .duty = duty_count,
            .hpoint = 0,
        };
        error = ledc_channel_config(&channel_cfg);
        if (error != ESP_OK) {
            goto fail;
        }
        error = ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->channel);
        if (error != ESP_OK) {
            goto fail;
        }
    }

    slot->freq_hz = freq_hz;
    slot->duty_pct = duty_pct;
    return ESP_OK;

fail:
    ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
    gpio_reset_pin((gpio_num_t)gpio_num);
    slot->in_use = false;
    return error;
}

/** Stop and release the LEDC output on a pin. */
static esp_err_t shell_pwm_stop_pin(int gpio_num)
{
    shell_pwm_slot_t *slot = shell_pwm_find_slot_by_gpio(gpio_num);

    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
    gpio_reset_pin((gpio_num_t)gpio_num);
    slot->in_use = false;
    return ESP_OK;
}

/** Shared status listing for `pwm status` / `freq status`. */
static void shell_pwm_print_status(void)
{
    size_t index;
    bool any = false;

    for (index = 0; index < SHELL_PWM_CHANNEL_MAX; index++) {
        if (!s_pwm_slots[index].in_use) {
            continue;
        }
        any = true;
        shell_transcript_appendf_ansi(SH_LBL "pwm.status:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST
                                 " " SH_LBL "freq=" SH_RST SH_NUM "%lu Hz" SH_RST " " SH_LBL "duty=" SH_RST SH_NUM "%lu%%" SH_RST "\n",
                                 s_pwm_slots[index].gpio_num,
                                 (unsigned long)s_pwm_slots[index].freq_hz,
                                 (unsigned long)s_pwm_slots[index].duty_pct);
    }
    if (!any) {
        shell_transcript_appendf_ansi(SH_LBL "pwm.status:" SH_RST " " SH_MUTE "no active outputs" SH_RST "\n");
    }
}

/** Shared `stop <pin>` handling for `pwm` and `freq`. */
static void shell_pwm_cmd_stop(const char *label, const char *pin_text)
{
    char *end = NULL;
    long gpio_num = strtol(pin_text, &end, 10);

    if (end == NULL || *end != '\0') {
        shell_print_usage("Usage: %s stop <pin>", label);
        shell_record_warningf(label, "Usage error for %s stop", label);
        return;
    }
    if (shell_pin_is_reserved((int)gpio_num)) {
        shell_print_warning("%s: pin %ld is reserved for active board functions", label, gpio_num);
        shell_record_warningf(label, "Rejected reserved pin %ld", gpio_num);
        return;
    }
    if (shell_pwm_stop_pin((int)gpio_num) == ESP_ERR_NOT_FOUND) {
        shell_transcript_appendf("%s: no active output on pin %ld\n", label, gpio_num);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "%s stop:" SH_RST " " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST " " SH_OK "stopped" SH_RST "\n",
                             label, gpio_num);
}

/** Parse an optional duty argument; defaults apply when omitted. */
static bool shell_pwm_parse_duty_arg(const char *text, long *duty_out)
{
    char *end = NULL;
    long value;

    if (text == NULL) {
        *duty_out = SHELL_PWM_DUTY_DEFAULT_PCT;
        return true;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0' || value < 0 || value > 100) {
        return false;
    }
    *duty_out = value;
    return true;
}

static void shell_execute_pwm_command(int argc, char **argv)
{
    char *end = NULL;
    long gpio_num;
    long freq;
    long duty;
    esp_err_t error;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_pwm_print_status();
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "stop")) {
        shell_pwm_cmd_stop("pwm", argv[2]);
        return;
    }

    if (argc == 4) {
        gpio_num = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: pwm <pin> <freq_hz> <duty_pct>");
            shell_record_warningf("pwm", "Usage error for pwm pin argument");
            return;
        }
        end = NULL;
        freq = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || freq <= 0 || freq > SHELL_PWM_FREQ_MAX_HZ) {
            shell_print_usage("Usage: pwm <pin> <freq_hz> <duty_pct>  (freq 1..%d Hz)",
                              (int)SHELL_PWM_FREQ_MAX_HZ);
            shell_record_warningf("pwm", "Invalid pwm frequency argument");
            return;
        }
        if (!shell_pwm_parse_duty_arg(argv[3], &duty)) {
            shell_print_usage("Usage: pwm <pin> <freq_hz> <duty_pct>  (duty 0..100)");
            shell_record_warningf("pwm", "Invalid pwm duty argument");
            return;
        }

        if (shell_pin_is_reserved((int)gpio_num)) {
            shell_print_warning("pwm: pin %ld is reserved for active board functions", gpio_num);
            shell_record_warningf("pwm", "Rejected reserved pin %ld", gpio_num);
            return;
        }

        error = shell_pwm_apply((int)gpio_num, (uint32_t)freq, (uint32_t)duty);
        if (error == ESP_ERR_INVALID_STATE) {
            shell_print_error("pwm: no free PWM channel (stop one first)");
            shell_record_errorf("pwm", error, "PWM channel pool exhausted");
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("pwm: failed to configure LEDC on pin %ld (%s)", gpio_num, esp_err_to_name(error));
            shell_record_errorf("pwm", error, "LEDC config failed on pin %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "pwm set:" SH_RST " " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST
                                 " " SH_LBL "freq=" SH_RST SH_NUM "%ld Hz" SH_RST " " SH_LBL "duty=" SH_RST SH_NUM "%ld%%" SH_RST "\n",
                                 gpio_num, freq, duty);
        return;
    }

    shell_print_usage("Usage: pwm status | pwm stop <pin> | pwm <pin> <freq_hz> <duty_pct>");
    shell_record_warningf("pwm", "Usage error for pwm command");
}

static void shell_execute_freq_command(int argc, char **argv)
{
    char *end = NULL;
    long gpio_num;
    long freq;
    esp_err_t error;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_pwm_print_status();
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "stop")) {
        shell_pwm_cmd_stop("freq", argv[2]);
        return;
    }

    if (argc == 3) {
        gpio_num = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: freq <pin> <hz>");
            shell_record_warningf("freq", "Usage error for freq pin argument");
            return;
        }
        end = NULL;
        freq = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || freq <= 0 || freq > SHELL_PWM_FREQ_MAX_HZ) {
            shell_print_usage("Usage: freq <pin> <hz>  (1..%d Hz)", (int)SHELL_PWM_FREQ_MAX_HZ);
            shell_record_warningf("freq", "Invalid freq argument");
            return;
        }

        if (shell_pin_is_reserved((int)gpio_num)) {
            shell_print_warning("freq: pin %ld is reserved for active board functions", gpio_num);
            shell_record_warningf("freq", "Rejected reserved pin %ld", gpio_num);
            return;
        }

        /* A square wave is PWM at 50% duty; reuse the single LEDC engine. */
        error = shell_pwm_apply((int)gpio_num, (uint32_t)freq, SHELL_PWM_DUTY_DEFAULT_PCT);
        if (error == ESP_ERR_INVALID_STATE) {
            shell_print_error("freq: no free PWM channel (stop one first)");
            shell_record_errorf("freq", error, "PWM channel pool exhausted");
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("freq: failed to generate %ld Hz on pin %ld (%s)",
                              freq, gpio_num, esp_err_to_name(error));
            shell_record_errorf("freq", error, "Square wave config failed on pin %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "freq:" SH_RST " square wave on " SH_LBL "pin=" SH_RST SH_NUM "%ld" SH_RST
                                 " at " SH_NUM "%ld Hz" SH_RST "\n", gpio_num, freq);
        return;
    }

    shell_print_usage("Usage: freq status | freq stop <pin> | freq <pin> <hz>");
    shell_record_warningf("freq", "Usage error for freq command");
}

/* ---- ADC reads on arbitrary pins ---- */

static void shell_adc_delete_cali(adc_cali_handle_t cali)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(cali);
#else
    (void)cali;
#endif
}

/**
 * Read an arbitrary ADC-capable GPIO with a fresh one-shot unit. The unit
 * and any calibration handle are deleted before returning, so repeated
 * reads never leak ADC resources.
 */
static esp_err_t shell_adc_read_pin(int gpio_num, int samples, int *mv_out, int *raw_out)
{
    esp_err_t error;
    adc_unit_t unit_id;
    adc_channel_t channel;
    adc_oneshot_unit_init_cfg_t unit_cfg = {0};
    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = (adc_atten_t)SHELL_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_unit_handle_t unit = NULL;
    adc_cali_handle_t cali = NULL;
    int64_t raw_sum = 0;
    int i;

    error = adc_oneshot_io_to_channel(gpio_num, &unit_id, &channel);
    if (error != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    unit_cfg.unit_id = unit_id;
    error = adc_oneshot_new_unit(&unit_cfg, &unit);
    if (error != ESP_OK) {
        return error;
    }

    error = adc_oneshot_config_channel(unit, channel, &channel_cfg);
    if (error != ESP_OK) {
        adc_oneshot_del_unit(unit);
        return error;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .chan = channel,
            .atten = (adc_atten_t)SHELL_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        (void)adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali);
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = unit_id,
            .atten = (adc_atten_t)SHELL_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        (void)adc_cali_create_scheme_line_fitting(&cali_cfg, &cali);
    }
#endif

    if (samples < 1) {
        samples = 1;
    }
    for (i = 0; i < samples; i++) {
        int raw = 0;
        error = adc_oneshot_read(unit, channel, &raw);
        if (error != ESP_OK) {
            break;
        }
        raw_sum += raw;
    }

    if (error != ESP_OK) {
        if (cali != NULL) {
            shell_adc_delete_cali(cali);
        }
        adc_oneshot_del_unit(unit);
        return error;
    }

    *raw_out = (int)(raw_sum / samples);
    if (cali != NULL) {
        if (adc_cali_raw_to_voltage(cali, *raw_out, mv_out) != ESP_OK) {
            *mv_out = (int)(((int64_t)*raw_out * 3300) / 4095);
        }
        shell_adc_delete_cali(cali);
    } else {
        *mv_out = (int)(((int64_t)*raw_out * 3300) / 4095);
    }

    adc_oneshot_del_unit(unit);
    return ESP_OK;
}

/**
 * List the ADC-capable, non-reserved GPIOs that currently read back a value.
 *
 * The candidate pins come from the SOC channel map (no io_to_channel probing
 * of arbitrary GPIOs, which would log spurious errors), and each candidate is
 * actually sampled once so a busy ADC unit or a pin claimed by another driver
 * is reported as unavailable instead of listed as usable.
 */
static void shell_adc_print_capable_pins(void)
{
    size_t index;
    int count = 0;

#if SOC_ADC_SUPPORTED
    static const int adc1_gpios[] = {
        ADC1_CHANNEL_0_GPIO_NUM, ADC1_CHANNEL_1_GPIO_NUM, ADC1_CHANNEL_2_GPIO_NUM,
        ADC1_CHANNEL_3_GPIO_NUM, ADC1_CHANNEL_4_GPIO_NUM, ADC1_CHANNEL_5_GPIO_NUM,
        ADC1_CHANNEL_6_GPIO_NUM, ADC1_CHANNEL_7_GPIO_NUM,
    };
    static const int adc2_gpios[] = {
        ADC2_CHANNEL_0_GPIO_NUM, ADC2_CHANNEL_1_GPIO_NUM, ADC2_CHANNEL_2_GPIO_NUM,
        ADC2_CHANNEL_3_GPIO_NUM, ADC2_CHANNEL_4_GPIO_NUM, ADC2_CHANNEL_5_GPIO_NUM,
    };

    for (index = 0; index < sizeof(adc1_gpios) / sizeof(adc1_gpios[0]); index++) {
        int mv;
        int raw;
        int gpio = adc1_gpios[index];
        if (shell_pin_is_reserved(gpio)) {
            continue;
        }
        if (shell_adc_read_pin(gpio, 1, &mv, &raw) == ESP_OK) {
            count++;
            shell_transcript_appendf_ansi(SH_LBL "adc.status:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "unit=" SH_RST SH_NUM "1" SH_RST " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "voltage=" SH_RST SH_NUM "%d.%03d V" SH_RST "\n",
                                     gpio, raw, mv / 1000, mv % 1000);
        }
    }
    for (index = 0; index < sizeof(adc2_gpios) / sizeof(adc2_gpios[0]); index++) {
        int mv;
        int raw;
        int gpio = adc2_gpios[index];
        if (shell_pin_is_reserved(gpio)) {
            continue;
        }
        if (shell_adc_read_pin(gpio, 1, &mv, &raw) == ESP_OK) {
            count++;
            shell_transcript_appendf_ansi(SH_LBL "adc.status:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "unit=" SH_RST SH_NUM "2" SH_RST " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST
                                     " " SH_LBL "voltage=" SH_RST SH_NUM "%d.%03d V" SH_RST "\n",
                                     gpio, raw, mv / 1000, mv % 1000);
        }
    }
#endif

    if (count == 0) {
        shell_transcript_appendf_ansi(SH_LBL "adc.status:" SH_RST " " SH_MUTE "no usable ADC pins" SH_RST "\n");
    }
}

static void shell_execute_adc_command(int argc, char **argv)
{
    char *end = NULL;
    long gpio_num;
    long samples = SHELL_ADC_DEFAULT_SAMPLES;
    int mv;
    int raw;
    esp_err_t error;

    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        shell_adc_print_capable_pins();
        return;
    }

    if (argc == 2 || argc == 3) {
        gpio_num = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0') {
            shell_print_usage("Usage: adc <pin> [samples]");
            shell_record_warningf("adc", "Usage error for adc pin argument");
            return;
        }
        if (argc == 3) {
            end = NULL;
            samples = strtol(argv[2], &end, 10);
            if (end == NULL || *end != '\0' || samples < 1 || samples > SHELL_ADC_MAX_SAMPLES) {
                shell_print_usage("Usage: adc <pin> [samples]  (samples 1..%d)", (int)SHELL_ADC_MAX_SAMPLES);
                shell_record_warningf("adc", "Invalid adc samples argument");
                return;
            }
        }

        if (shell_pin_is_reserved((int)gpio_num)) {
            shell_print_warning("adc: pin %ld is reserved for active board functions", gpio_num);
            shell_record_warningf("adc", "Rejected reserved pin %ld", gpio_num);
            return;
        }

        error = shell_adc_read_pin((int)gpio_num, (int)samples, &mv, &raw);
        if (error == ESP_ERR_NOT_SUPPORTED) {
            shell_print_error("adc: pin %ld is not an ADC-capable GPIO", gpio_num);
            shell_record_warningf("adc", "Pin %ld is not ADC-capable", gpio_num);
            return;
        }
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("adc: the ADC unit for pin %ld is in use by another driver", gpio_num);
            shell_record_warningf("adc", "ADC unit for pin %ld is busy", gpio_num);
            return;
        }
        if (error != ESP_OK) {
            shell_print_error("adc: read failed on pin %ld (%s)", gpio_num, esp_err_to_name(error));
            shell_record_errorf("adc", error, "ADC read failed on pin %ld", gpio_num);
            return;
        }

        shell_transcript_appendf_ansi(SH_LBL "adc:" SH_RST " " SH_LBL "gpio=" SH_RST SH_NUM "%ld" SH_RST
                                 " " SH_LBL "raw=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "voltage=" SH_RST SH_NUM "%d.%03d V" SH_RST "\n",
                                 gpio_num, raw, mv / 1000, mv % 1000);
        return;
    }

    shell_print_usage("Usage: adc status | adc <pin> [samples]");
    shell_record_warningf("adc", "Usage error for adc command");
}

/* ---- I2C scan / peek / poke (i2c_master driver) ---- */

typedef struct {
    bool using_shared_bus;
    uint32_t clk_hz;
    i2c_master_bus_handle_t bus;
} shell_i2c_session_t;

/**
 * Open an I2C session. The board's shared bus (pins 7/8) is reused through
 * the BSP handle so a scan never conflicts with the touch controller; any
 * other pin pair gets a temporary master bus on a free port.
 */
static esp_err_t shell_i2c_open(int sda, int scl, shell_i2c_session_t *session)
{
    i2c_master_bus_config_t cfg;

    if (sda == (int)BSP_I2C_SDA && scl == (int)BSP_I2C_SCL) {
        session->bus = bsp_i2c_get_handle();
        if (session->bus != NULL) {
            session->using_shared_bus = true;
            session->clk_hz = (uint32_t)BOARD_CFG_I2C_CLK_SPEED_HZ;
            return ESP_OK;
        }
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)sda;
    cfg.scl_io_num = (gpio_num_t)scl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.trans_queue_depth = 1;
    cfg.flags.enable_internal_pullup = true;

    session->using_shared_bus = false;
    session->clk_hz = SHELL_I2C_TOOL_CLK_HZ;
    return i2c_new_master_bus(&cfg, &session->bus);
}

static void shell_i2c_close(shell_i2c_session_t *session)
{
    if (session->bus != NULL && !session->using_shared_bus) {
        i2c_del_master_bus(session->bus);
    }
    session->bus = NULL;
}

/** Parse optional `sda=` / `scl=` pin tokens; defaults to the board bus. */
static bool shell_i2c_parse_pins(int argc, char **argv, int *sda, int *scl)
{
    int i;

    *sda = (int)BSP_I2C_SDA;
    *scl = (int)BSP_I2C_SCL;

    for (i = 0; i < argc; i++) {
        if (argv[i] == NULL) {
            continue;
        }
        if (strncasecmp(argv[i], "sda=", 4) == 0) {
            char *end = NULL;
            long value = strtol(argv[i] + 4, &end, 10);
            if (end == NULL || *end != '\0' || value < 0) {
                return false;
            }
            *sda = (int)value;
        } else if (strncasecmp(argv[i], "scl=", 4) == 0) {
            char *end = NULL;
            long value = strtol(argv[i] + 4, &end, 10);
            if (end == NULL || *end != '\0' || value < 0) {
                return false;
            }
            *scl = (int)value;
        } else {
            return false;
        }
    }

    return *sda >= 0 && *scl >= 0;
}

/** The board's own I2C pins are the intended scan target; any other pair
 *  must be made of free (non-reserved) GPIOs. */
static bool shell_i2c_pins_allowed(int sda, int scl)
{
    bool is_shared = (sda == (int)BSP_I2C_SDA && scl == (int)BSP_I2C_SCL);

    return is_shared || (!shell_pin_is_reserved(sda) && !shell_pin_is_reserved(scl));
}

/**
 * Probe one 7-bit address for an ACK using a normal device transaction.
 *
 * Unlike i2c_master_probe(), this path never touches the shared controller's
 * bus timing or interrupt mask, so a scan cannot disrupt the GT911 touch that
 * runs on the same bus. A one-byte write to a device that NACKs the address
 * fails immediately; a device that ACKs the address replies with a NACK only
 * on the data byte, which the driver surfaces as a transaction error too, so
 * only addresses that fully ACK count as present.
 */
static bool shell_i2c_probe_address(const shell_i2c_session_t *session, int addr)
{
    i2c_master_dev_handle_t dev = NULL;
    uint8_t dummy = 0;
    esp_err_t error;

    error = i2c_master_bus_add_device(session->bus,
                                      &(i2c_device_config_t){
                                          .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                          .device_address = (uint16_t)addr,
                                          .scl_speed_hz = session->clk_hz,
                                      },
                                      &dev);
    if (error != ESP_OK) {
        return false;
    }

    error = i2c_master_transmit(dev, &dummy, 1, SHELL_I2C_SCAN_PROBE_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    return error == ESP_OK;
}

static void shell_i2c_print_scan(const shell_i2c_session_t *session)
{
    int addr;
    int found = 0;

    shell_transcript_appendf_ansi(SH_LBL "i2c.scan:" SH_RST " probing " SH_NUM "0x%02X..0x%02X" SH_RST "\n",
                             SHELL_I2C_SCAN_FIRST_ADDR, SHELL_I2C_SCAN_LAST_ADDR);
    for (addr = SHELL_I2C_SCAN_FIRST_ADDR; addr <= SHELL_I2C_SCAN_LAST_ADDR; addr++) {
        if (shell_i2c_probe_address(session, addr)) {
            found++;
            shell_transcript_appendf_ansi("  " SH_NUM "0x%02X" SH_RST " " SH_MUTE "(0x%02X with R/W bit)" SH_RST "\n",
                                     addr, (addr << 1));
        }
    }
    if (found == 0) {
        shell_transcript_appendf_ansi(SH_LBL "i2c.scan:" SH_RST " " SH_MUTE "no devices found" SH_RST "\n");
    } else {
        shell_transcript_appendf_ansi(SH_LBL "i2c.scan:" SH_RST " " SH_NUM "%d" SH_RST " " SH_LBL "device(s) found" SH_RST "\n", found);
    }
}

static esp_err_t shell_i2c_add_device(const shell_i2c_session_t *session, int addr,
                                      i2c_master_dev_handle_t *dev_out)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = session->clk_hz,
    };

    return i2c_master_bus_add_device(session->bus, &dev_cfg, dev_out);
}

static bool shell_parse_hex_long(const char *text, long *value_out)
{
    char *end = NULL;
    long value;

    if (text == NULL) {
        return false;
    }
    value = strtol(text, &end, 0);
    if (end == NULL || *end != '\0' || value < 0 || value > 0xFFFF) {
        return false;
    }
    *value_out = value;
    return true;
}

static void shell_execute_i2c_command(int argc, char **argv)
{
    shell_i2c_session_t session = {0};
    esp_err_t error;
    int sda;
    int scl;
    long addr;
    long reg;
    long value;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_transcript_appendf_ansi(SH_LBL "i2c.status:" SH_RST " " SH_LBL "shared bus" SH_RST " "
                                 SH_LBL "sda=" SH_RST SH_NUM "%d" SH_RST " " SH_LBL "scl=" SH_RST SH_NUM "%d" SH_RST
                                 " " SH_LBL "clk=" SH_RST SH_NUM "%u Hz" SH_RST " " SH_LBL "timeout=" SH_RST SH_NUM "%d ms" SH_RST "\n",
                                 (int)BSP_I2C_SDA, (int)BSP_I2C_SCL,
                                 (unsigned)BOARD_CFG_I2C_CLK_SPEED_HZ, (int)SHELL_I2C_TOOL_TIMEOUT_MS);
        return;
    }

    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "scan")) {
        if (!shell_i2c_parse_pins(argc - 2, argv + 2, &sda, &scl)) {
            shell_print_usage("Usage: i2c scan [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c scan");
            return;
        }
        if (!shell_i2c_pins_allowed(sda, scl)) {
            shell_print_warning("i2c: pin pair %d/%d includes a reserved board line", sda, scl);
            shell_record_warningf("i2c", "Rejected reserved scan pins %d/%d", sda, scl);
            return;
        }
        error = shell_i2c_open(sda, scl, &session);
        if (error != ESP_OK) {
            shell_print_error("i2c: bus open failed on %d/%d (%s)", sda, scl, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C bus open failed");
            return;
        }
        shell_i2c_print_scan(&session);
        shell_i2c_close(&session);
        return;
    }

    if (argc >= 4 && shell_text_equals_ignore_case(argv[1], "peek")) {
        i2c_master_dev_handle_t dev = NULL;
        uint8_t reg_byte;
        uint8_t data = 0;

        if (!shell_parse_hex_long(argv[2], &addr) || !shell_parse_hex_long(argv[3], &reg)) {
            shell_print_usage("Usage: i2c peek <addr> <reg> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c peek");
            return;
        }
        if (addr < 0 || addr > 0x7F) {
            shell_print_usage("Usage: i2c peek <addr> <reg>  (addr 0x03..0x77)");
            shell_record_warningf("i2c", "i2c peek address out of range");
            return;
        }
        if (!shell_i2c_parse_pins(argc - 4, argv + 4, &sda, &scl) || !shell_i2c_pins_allowed(sda, scl)) {
            shell_print_usage("Usage: i2c peek <addr> <reg> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c peek pins");
            return;
        }
        error = shell_i2c_open(sda, scl, &session);
        if (error != ESP_OK) {
            shell_print_error("i2c: bus open failed on %d/%d (%s)", sda, scl, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C bus open failed");
            return;
        }
        error = shell_i2c_add_device(&session, (int)addr, &dev);
        if (error == ESP_OK) {
            reg_byte = (uint8_t)reg;
            error = i2c_master_transmit_receive(dev, &reg_byte, 1, &data, 1, SHELL_I2C_TOOL_TIMEOUT_MS);
            i2c_master_bus_rm_device(dev);
        }
        shell_i2c_close(&session);

        if (error != ESP_OK) {
            shell_print_error("i2c peek: read failed at addr 0x%02lX reg 0x%02lX (%s)",
                              addr, reg, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C peek failed");
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "i2c.peek:" SH_RST " " SH_LBL "addr=" SH_RST SH_NUM "0x%02lX" SH_RST
                                 " " SH_LBL "reg=" SH_RST SH_NUM "0x%02lX" SH_RST " " SH_LBL "value=" SH_RST SH_NUM "0x%02X" SH_RST "\n",
                                 addr, reg, data);
        return;
    }

    if (argc >= 5 && shell_text_equals_ignore_case(argv[1], "poke")) {
        i2c_master_dev_handle_t dev = NULL;
        uint8_t buf[2];

        if (!shell_parse_hex_long(argv[2], &addr) || !shell_parse_hex_long(argv[3], &reg) ||
            !shell_parse_hex_long(argv[4], &value)) {
            shell_print_usage("Usage: i2c poke <addr> <reg> <value> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c poke");
            return;
        }
        if (addr < 0 || addr > 0x7F || value > 0xFF) {
            shell_print_usage("Usage: i2c poke <addr> <reg> <value>  (addr 0x03..0x77, value 0x00..0xFF)");
            shell_record_warningf("i2c", "i2c poke argument out of range");
            return;
        }
        if (!shell_i2c_parse_pins(argc - 5, argv + 5, &sda, &scl) || !shell_i2c_pins_allowed(sda, scl)) {
            shell_print_usage("Usage: i2c poke <addr> <reg> <value> [sda=<pin> scl=<pin>]");
            shell_record_warningf("i2c", "Usage error for i2c poke pins");
            return;
        }
        error = shell_i2c_open(sda, scl, &session);
        if (error != ESP_OK) {
            shell_print_error("i2c: bus open failed on %d/%d (%s)", sda, scl, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C bus open failed");
            return;
        }
        error = shell_i2c_add_device(&session, (int)addr, &dev);
        if (error == ESP_OK) {
            buf[0] = (uint8_t)reg;
            buf[1] = (uint8_t)value;
            error = i2c_master_transmit(dev, buf, 2, SHELL_I2C_TOOL_TIMEOUT_MS);
            i2c_master_bus_rm_device(dev);
        }
        shell_i2c_close(&session);

        if (error != ESP_OK) {
            shell_print_error("i2c poke: write failed at addr 0x%02lX reg 0x%02lX (%s)",
                              addr, reg, esp_err_to_name(error));
            shell_record_errorf("i2c", error, "I2C poke failed");
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "i2c.poke:" SH_RST " " SH_LBL "addr=" SH_RST SH_NUM "0x%02lX" SH_RST
                                 " " SH_LBL "reg=" SH_RST SH_NUM "0x%02lX" SH_RST " " SH_LBL "value=" SH_RST SH_NUM "0x%02lX" SH_RST " " SH_OK "written" SH_RST "\n",
                                 addr, reg, value);
        return;
    }

    shell_print_usage("Usage: i2c status | i2c scan [sda=.. scl=..] | i2c peek <addr> <reg> [sda=.. scl=..] | i2c poke <addr> <reg> <value> [sda=.. scl=..]");
    shell_record_warningf("i2c", "Usage error for i2c command");
}

/* ---- SPI status (transactions unavailable on this board) ----
 * `spi status` reports the toolkit's SPI configuration. The loopback / peek /
 * poke verbs are deliberately NOT wired to the SPI master driver: initializing
 * the SPI host on this P4 with the ESP-Hosted SDIO link active stalls the chip
 * and drops USB-Serial-JTAG off the bus, so any SPI transaction command would
 * freeze the shell. They fail with an honest message instead, following the
 * rgb/camera unsupported-hardware pattern.
 */

static void shell_spi_print_unavailable(const char *verb)
{
    shell_print_error("spi: %s is unavailable on this board - initializing the SPI host "
                      "here stalls the chip (clock-domain conflict with the ESP-Hosted "
                      "SDIO link). `spi status` reports the toolkit configuration.",
                      verb);
    shell_record_warningf("spi", "SPI %s requested but SPI host init stalls this board", verb);
}

static void shell_execute_spi_command(int argc, char **argv)
{
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
        shell_transcript_appendf_ansi(SH_LBL "spi.status:" SH_RST " " SH_LBL "host" SH_RST " " SH_NUM "SPI3" SH_RST
                                 " " SH_LBL "mode" SH_RST " " SH_NUM "0" SH_RST " " SH_LBL "clk=" SH_RST SH_NUM "%u Hz" SH_RST
                                 " " SH_LBL "timeout=" SH_RST SH_NUM "%d ms" SH_RST "\n",
                                 (unsigned)SHELL_SPI_TOOL_CLK_HZ, (int)SHELL_SPI_TOOL_TIMEOUT_MS);
        shell_transcript_appendf_ansi(SH_MUTE "spi.transactions: " SH_ERR "unavailable on this board" SH_RST
                                 " - SPI host init stalls the chip with the ESP-Hosted SDIO link active.\n");
        return;
    }

    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "loopback")) {
        shell_spi_print_unavailable("loopback");
        return;
    }
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "peek")) {
        shell_spi_print_unavailable("peek");
        return;
    }
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "poke")) {
        shell_spi_print_unavailable("poke");
        return;
    }

    shell_print_usage("Usage: spi status | spi loopback <sclk> <mosi> <miso> | spi peek <sclk> <mosi> <miso> <cs> <reg> | spi poke <sclk> <mosi> <miso> <cs> <reg> <value>");
    shell_record_warningf("spi", "Usage error for spi command");
}


/* ========================================================================
 * UNSUPPORTED HARDWARE COMMAND (camera)
 * ======================================================================== */

/** Parse a hex colour "#RRGGBB" or "RRGGBB" into 0xRRGGBB. */
static bool shell_rgb_parse_hex(const char *text, uint32_t *rgb_out)
{
    const char *cursor = text;
    uint32_t value = 0;
    size_t digits = 0;

    if (text == NULL || rgb_out == NULL) {
        return false;
    }
    if (*cursor == '#') {
        cursor++;
    }
    while (digits < 6 && cursor[digits] != '\0') {
        char c = cursor[digits];
        int nibble;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else {
            return false;
        }
        value = (value << 4) | (uint32_t)nibble;
        digits++;
    }
    if (digits != 6 || cursor[digits] != '\0') {
        return false;
    }
    *rgb_out = value;
    return true;
}

/**
 * `rgb` — control the WS2812 status LED (LED1, GPIO26).
 *
 * Usage:
 *   rgb status                  Show state (mode, colour, effect, brightness)
 *   rgb off                     Turn the LED off
 *   rgb <r> <g> <b>             Solid colour, each channel 0-255
 *   rgb #RRGGBB                 Solid colour from a hex value
 *   rgb <effect> [speed]        rainbow | breath | pulse | blink (speed 1..10)
 *   rgb auto <on|off>           Enable/disable the status-driven colour layer
 *
 * ERRORLEVEL: 0 success, 1 failure, 2 usage. Works in batch files and is
 * redirectable/pipable like every other command.
 */
static void shell_execute_rgb_command(int argc, char **argv)
{
    esp_err_t error = ESP_OK;

    /* rgb / rgb status */
    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        led_state_t state;

        if (!led_is_initialized()) {
            shell_print_error("rgb: WS2812 LED driver is not initialized (check GPIO%d)",
                              (int)BOARD_CFG_RGB_LED_GPIO);
            shell_record_warningf("rgb", "RGB LED driver not initialized");
            batch_set_errorlevel(1);
            return;
        }
        led_get_state(&state);
        shell_transcript_appendf_ansi(SH_HEAD "RGB LED" SH_RST "\n");
        shell_transcript_appendf_ansi("  " SH_LBL "driver:" SH_RST " WS2812 on " SH_NUM "GPIO%d" SH_RST "\n",
                                      (int)BOARD_CFG_RGB_LED_GPIO);
        shell_transcript_appendf_ansi("  " SH_LBL "mode:" SH_RST " %s\n",
                                      state.auto_status ? SH_OK "auto status" SH_RST : SH_VAL "manual" SH_RST);
        shell_transcript_appendf_ansi("  " SH_LBL "effect:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                      led_effect_name(state.effect));
        shell_transcript_appendf_ansi("  " SH_LBL "colour:" SH_RST " " SH_NUM "#%02X%02X%02X" SH_RST "\n",
                                      state.red, state.green, state.blue);
        shell_transcript_appendf_ansi("  " SH_LBL "speed:" SH_RST " " SH_NUM "%u" SH_RST " " SH_MUTE "(1..10)" SH_RST "\n",
                                      (unsigned int)state.speed);
        shell_transcript_appendf_ansi("  " SH_LBL "brightness:" SH_RST " " SH_NUM "%u%%" SH_RST "\n",
                                      (unsigned int)state.brightness_pct);
        batch_set_errorlevel(0);
        return;
    }

    /* rgb off */
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "off")) {
        error = led_off();
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " LED " SH_ERR "off" SH_RST "\n");
        }
        goto done;
    }

    /* rgb auto <on|off> */
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "auto")) {
        if (shell_text_equals_ignore_case(argv[2], "on")) {
            error = led_set_auto_status(true);
            if (error == ESP_OK) {
                shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " auto status " SH_OK "enabled" SH_RST "\n");
            }
            goto done;
        }
        if (shell_text_equals_ignore_case(argv[2], "off")) {
            error = led_set_auto_status(false);
            if (error == ESP_OK) {
                shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " auto status " SH_MUTE "disabled" SH_RST "\n");
            }
            goto done;
        }
        shell_print_usage("Usage: rgb auto <on|off>");
        shell_record_warningf("rgb", "Invalid rgb auto argument");
        batch_set_errorlevel(2);
        return;
    }

    /* rgb #RRGGBB */
    if (argc == 2 && argv[1][0] == '#') {
        uint32_t rgb;

        if (!shell_rgb_parse_hex(argv[1], &rgb)) {
            shell_print_usage("Usage: rgb #RRGGBB");
            shell_record_warningf("rgb", "Invalid rgb hex colour");
            batch_set_errorlevel(2);
            return;
        }
        error = led_set_hex(rgb);
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " colour set to " SH_NUM "#%06lX" SH_RST "\n",
                                          (unsigned long)rgb);
        }
        goto done;
    }

    /* rgb <r> <g> <b> */
    if (argc == 4) {
        char *end = NULL;
        long red = strtol(argv[1], &end, 10);
        if (end == NULL || *end != '\0' || red < 0 || red > 255) {
            shell_print_usage("Usage: rgb <r> <g> <b>  (0-255 each)");
            shell_record_warningf("rgb", "Invalid rgb red argument");
            batch_set_errorlevel(2);
            return;
        }
        end = NULL;
        long green = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || green < 0 || green > 255) {
            shell_print_usage("Usage: rgb <r> <g> <b>  (0-255 each)");
            shell_record_warningf("rgb", "Invalid rgb green argument");
            batch_set_errorlevel(2);
            return;
        }
        end = NULL;
        long blue = strtol(argv[3], &end, 10);
        if (end == NULL || *end != '\0' || blue < 0 || blue > 255) {
            shell_print_usage("Usage: rgb <r> <g> <b>  (0-255 each)");
            shell_record_warningf("rgb", "Invalid rgb blue argument");
            batch_set_errorlevel(2);
            return;
        }
        error = led_set_color((uint8_t)red, (uint8_t)green, (uint8_t)blue);
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " colour set to " SH_NUM "r=%ld g=%ld b=%ld" SH_RST "\n",
                                          red, green, blue);
        }
        goto done;
    }

    /* rgb <effect> [speed] */
    if (argc == 2 || argc == 3) {
        led_effect_t effect = led_effect_from_name(argv[1]);
        uint8_t speed = 0;

        if (effect == LED_EFFECT_SOLID && !shell_text_equals_ignore_case(argv[1], "solid")) {
            shell_print_usage("Usage: rgb <effect> [speed]  (rainbow|breath|pulse|blink|solid)");
            shell_record_warningf("rgb", "Unknown rgb effect %s", argv[1]);
            batch_set_errorlevel(2);
            return;
        }
        if (argc == 3) {
            char *end = NULL;
            long parsed = strtol(argv[2], &end, 10);
            if (end == NULL || *end != '\0' || parsed < 1 || parsed > 10) {
                shell_print_usage("Usage: rgb <effect> [speed]  (speed 1..10)");
                shell_record_warningf("rgb", "Invalid rgb speed argument");
                batch_set_errorlevel(2);
                return;
            }
            speed = (uint8_t)parsed;
        }
        error = led_set_effect(effect, speed);
        if (error == ESP_OK) {
            shell_transcript_appendf_ansi(SH_LBL "rgb:" SH_RST " effect " SH_NUM "%s" SH_RST
                                          " at speed " SH_NUM "%u" SH_RST "\n",
                                          led_effect_name(effect),
                                          (unsigned int)(speed ? speed : P4_CONFIG_LED_EFFECT_SPEED_DEFAULT));
        }
        goto done;
    }

    shell_print_usage("Usage: rgb status | rgb off | rgb <r> <g> <b> | rgb #RRGGBB | rgb <effect> [speed] | rgb auto <on|off>");
    shell_record_warningf("rgb", "Usage error for rgb command");
    batch_set_errorlevel(2);
    return;

done:
    if (error != ESP_OK) {
        shell_print_error("rgb: LED control failed (%s)", esp_err_to_name(error));
        shell_record_errorf("rgb", error, "RGB LED control failed");
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
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
 * `httpget <url> [localfile]` â€” the HTTP engine lives in
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
 * SCREENSHOT / SCR / CAPTURE
 * ========================================================================
 * Captures the current LVGL screen as a BMP image and either streams it
 * over the UART/USB-Serial-JTAG console with magic markers, or writes
 * it to an SD card file. Uses the LVGL snapshot API for pixel-perfect
 * capture with the LVGL lock held for thread safety.
 */

/**
 * Write a standard 14-byte BMP file header + 40-byte info header for an
 * RGB888 image into the output buffer, then return the header size (54).
 *
 * BMP format (bottom-up, 24-bit RGB888):
 *   - File header: signature 'BM', file size, reserved, pixel data offset
 *   - Info header: header size (40), width, height, planes (1), bpp (24),
 *     compression (0=BI_RGB), image size, resolution, color count
 */
static int screenshot_write_bmp_headers(uint8_t *buf, uint32_t width, uint32_t height)
{
    uint32_t row_bytes = width * 3;  /* 24-bit RGB */
    uint32_t img_size = row_bytes * height;
    uint32_t file_size = 54 + img_size;
    uint32_t bpp = 24;
    uint32_t planes = 1;
    uint32_t compression = 0;
    int32_t xppm = 3780;  /* 96 DPI */
    int32_t yppm = 3780;

    memset(buf, 0, 54);

    /* File header */
    buf[0] = 'B';
    buf[1] = 'M';
    buf[2] = (uint8_t)(file_size);
    buf[3] = (uint8_t)(file_size >> 8);
    buf[4] = (uint8_t)(file_size >> 16);
    buf[5] = (uint8_t)(file_size >> 24);
    buf[10] = 54;  /* offset to pixel data */

    /* Info header */
    buf[14] = 40;  /* header size */
    buf[18] = (uint8_t)(width);
    buf[19] = (uint8_t)(width >> 8);
    buf[20] = (uint8_t)(width >> 16);
    buf[21] = (uint8_t)(width >> 24);
    buf[22] = (uint8_t)(height);
    buf[23] = (uint8_t)(height >> 8);
    buf[24] = (uint8_t)(height >> 16);
    buf[25] = (uint8_t)(height >> 24);
    buf[26] = (uint8_t)(planes);       /* planes = 1 */
    buf[28] = (uint8_t)(bpp);          /* bits per pixel */
    buf[30] = (uint8_t)(compression);
    buf[34] = (uint8_t)(img_size);
    buf[35] = (uint8_t)(img_size >> 8);
    buf[36] = (uint8_t)(img_size >> 16);
    buf[37] = (uint8_t)(img_size >> 24);
    buf[38] = (uint8_t)(xppm);
    buf[39] = (uint8_t)(xppm >> 8);
    buf[40] = (uint8_t)(xppm >> 16);
    buf[41] = (uint8_t)(xppm >> 24);
    buf[42] = (uint8_t)(yppm);
    buf[43] = (uint8_t)(yppm >> 8);
    buf[44] = (uint8_t)(yppm >> 16);
    buf[45] = (uint8_t)(yppm >> 24);

    return 54;
}

/**
 * Convert a single RGB565 pixel (uint16_t, little-endian) to 3 bytes of
 * RGB888 in the output buffer. BMP bottom-up format expects BGR ordering.
 */
static inline void rgb565_to_bmp_row(uint8_t *dst, const uint16_t *src, uint32_t width)
{
    uint32_t i;
    for (i = 0; i < width; i++) {
        uint16_t px = src[i];
        uint8_t r = (uint8_t)(((px >> 11) & 0x1F) << 3);
        uint8_t g = (uint8_t)(((px >> 5) & 0x3F) << 2);
        uint8_t b = (uint8_t)((px & 0x1F) << 3);
        *dst++ = b;
        *dst++ = g;
        *dst++ = r;
    }
}

/**
 * `screenshot` / `scr` / `capture` â€” capture the LVGL screen as a BMP image.
 *
 * Usage:
 *   screenshot              â†’ stream BMP over UART with magic markers
 *   screenshot file.bmp     â†’ save BMP to SD card (current directory)
 *
 * Captures via lv_snapshot_take_to_draw_buf(), converts RGB565 â†’ RGB888 for the BMP,
 * and outputs either to the serial console (with begin/end markers for
 * host-side extraction) or to an SD card file with free-space precheck.
 */
static void shell_command_screenshot(int argc, char **argv)
{
    lv_draw_buf_t *draw_buf = NULL;
    void *pixel_data = NULL;
    lv_obj_t *screen;
    uint32_t width = 0;
    uint32_t height = 0;
    bool to_sd = (argc >= 2);

    if (argc > 2) {
        shell_print_usage("Usage: screenshot [filename.bmp]");
        batch_set_errorlevel(2);
        return;
    }

    shell_print_muted("screenshot: capturing current screen...");

    /* Allocate the lv_draw_buf_t structure from PSRAM */
    draw_buf = (lv_draw_buf_t *)heap_caps_malloc(sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (draw_buf == NULL) {
        draw_buf = (lv_draw_buf_t *)malloc(sizeof(lv_draw_buf_t));
    }
    if (draw_buf == NULL) {
        shell_print_error("screenshot: out of memory for draw buffer structure");
        batch_set_errorlevel(1);
        return;
    }
    memset(draw_buf, 0, sizeof(lv_draw_buf_t));

    /* Take the LVGL lock and do all LVGL operations inside it. */
    if (!lvgl_port_lock(0)) {
        shell_print_error("screenshot: could not acquire LVGL lock");
        free(draw_buf);
        batch_set_errorlevel(1);
        return;
    }

    screen = lv_screen_active();
    if (screen == NULL) {
        lvgl_port_unlock();
        free(draw_buf);
        shell_print_error("screenshot: no active LVGL screen");
        batch_set_errorlevel(1);
        return;
    }

    width = lv_display_get_horizontal_resolution(NULL);
    height = lv_display_get_vertical_resolution(NULL);

    if (width == 0 || height == 0) {
        lvgl_port_unlock();
        free(draw_buf);
        shell_print_error("screenshot: invalid display resolution %lux%lu",
                          (unsigned long)width, (unsigned long)height);
        batch_set_errorlevel(1);
        return;
    }

    /* Calculate required buffer size for RGB565 */
    uint32_t data_size = width * height * 2;  /* RGB565 = 2 bytes per pixel */
    uint32_t stride = width * 2;  /* stride in bytes */

    /* Allocate pixel data from PSRAM */
    pixel_data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_data == NULL) {
        pixel_data = malloc(data_size);
    }
    if (pixel_data == NULL) {
        lvgl_port_unlock();
        free(draw_buf);
        shell_print_error("screenshot: out of memory for pixel data (%lu bytes)",
                          (unsigned long)data_size);
        batch_set_errorlevel(1);
        return;
    }

    /* Initialize the draw buffer with the pre-allocated PSRAM buffer */
    lv_result_t init_result = lv_draw_buf_init(draw_buf, width, height,
                                               LV_COLOR_FORMAT_RGB565,
                                               stride, pixel_data, data_size);
    if (init_result != LV_RESULT_OK) {
        lvgl_port_unlock();
        free(pixel_data);
        free(draw_buf);
        shell_print_error("screenshot: failed to initialize draw buffer");
        batch_set_errorlevel(1);
        return;
    }

    /* Take the snapshot into the pre-created buffer */
    lv_result_t result = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, draw_buf);
    lvgl_port_unlock();

    if (result != LV_RESULT_OK) {
        shell_print_error("screenshot: snapshot capture failed");
        lv_draw_buf_destroy(draw_buf);
        batch_set_errorlevel(1);
        return;
    }

    shell_print_ok("screenshot: captured %lux%lu RGB565 (%lu bytes)",
                   (unsigned long)width, (unsigned long)height,
                   (unsigned long)(draw_buf->data_size));

    if (to_sd) {
        /* Write to SD card file */
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;

        if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("screenshot: invalid path %s", argv[1]);
            lv_draw_buf_destroy(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("screenshot: SD card not present");
            lv_draw_buf_destroy(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Precheck free space: 54-byte header + width*height*3 pixel data */
        {
            uint64_t needed = 54 + (uint64_t)width * (uint64_t)height * 3;
            uint64_t reclaim = storage_get_file_size(resolved);
            if (!storage_check_free_space(needed, reclaim, "screenshot")) {
                shell_sd_end(&session, "screenshot");
                lv_draw_buf_destroy(draw_buf);
                batch_set_errorlevel(1);
                return;
            }
        }

        FILE *f = fopen(resolved, "wb");
        if (f == NULL) {
            shell_print_error("screenshot: cannot create %s", resolved);
            shell_sd_end(&session, "screenshot");
            lv_draw_buf_destroy(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Write BMP headers */
        uint8_t headers[54];
        int hdr_size = screenshot_write_bmp_headers(headers, width, height);
        size_t written = fwrite(headers, 1, hdr_size, f);
        if (written != (size_t)hdr_size) {
            shell_print_error("screenshot: failed to write BMP header");
            fclose(f);
            remove(resolved);
            shell_sd_end(&session, "screenshot");
            lv_draw_buf_destroy(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Convert RGB565 to RGB888 row-by-row, bottom-up (BMP convention).
         * Allocate a row buffer on heap to avoid stack pressure. */
        uint32_t row_bytes = width * 3;
        uint8_t *row_buf = heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (row_buf == NULL) {
            row_buf = malloc(row_bytes);
        }
        if (row_buf == NULL) {
            shell_print_error("screenshot: out of memory for row buffer");
            fclose(f);
            remove(resolved);
            shell_sd_end(&session, "screenshot");
            lv_draw_buf_destroy(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        bool write_ok = true;
        int32_t y;
        for (y = height - 1; y >= 0; y--) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)draw_buf->data +
                                                       (uint32_t)y * draw_buf->header.stride);
            rgb565_to_bmp_row(row_buf, row, width);
            if (fwrite(row_buf, 1, row_bytes, f) != row_bytes) {
                write_ok = false;
                break;
            }
        }

        free(row_buf);
        fclose(f);
        shell_sd_end(&session, "screenshot");

        if (!write_ok) {
            shell_print_error("screenshot: write failed at row, removing partial file");
            remove(resolved);
            lv_draw_buf_destroy(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        shell_print_ok("screenshot: saved %s (%lu bytes)", resolved,
                       (unsigned long)(54 + (uint64_t)width * height * 3));
        lv_draw_buf_destroy(draw_buf);
        batch_set_errorlevel(0);
        return;
    }

    /* Stream raw BMP binary to UART/USB-Serial-JTAG console.
     * Protocol: 4-byte magic "BMPX" + 4-byte little-endian size + raw data.
     * The host reads the magic, then the size, then exactly that many bytes. */
    {
        size_t total_size = 54 + (size_t)width * height * 3;
        shell_print_muted("screenshot: streaming %lu bytes to serial...", (unsigned long)total_size);

        /* Build the full BMP in memory first */
        uint8_t *bmp_data = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (bmp_data == NULL) {
            bmp_data = malloc(total_size);
        }
        if (bmp_data == NULL) {
            shell_print_error("screenshot: out of memory for BMP buffer");
            free(pixel_data);
            free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Write BMP header */
        screenshot_write_bmp_headers(bmp_data, width, height);

        /* Convert RGB565 to RGB888 row-by-row, bottom-up (BMP convention) */
        uint32_t row_bytes = width * 3;
        uint8_t *row_buf = heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (row_buf == NULL) {
            row_buf = malloc(row_bytes);
        }
        if (row_buf == NULL) {
            shell_print_error("screenshot: out of memory for row buffer");
            free(bmp_data);
            free(pixel_data);
            free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        int32_t y;
        for (y = height - 1; y >= 0; y--) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)draw_buf->data +
                                                       (uint32_t)y * draw_buf->header.stride);
            rgb565_to_bmp_row(row_buf, row, width);
            memcpy(bmp_data + 54 + (height - 1 - y) * row_bytes, row_buf, row_bytes);
        }
        free(row_buf);

        /* Send magic header */
        const char magic[4] = {'B', 'M', 'P', 'X'};
        fwrite(magic, 1, 4, stdout);
        /* Send size as 4-byte little-endian */
        uint8_t size_bytes[4];
        size_bytes[0] = (uint8_t)(total_size);
        size_bytes[1] = (uint8_t)(total_size >> 8);
        size_bytes[2] = (uint8_t)(total_size >> 16);
        size_bytes[3] = (uint8_t)(total_size >> 24);
        fwrite(size_bytes, 1, 4, stdout);
        /* Send raw BMP data */
        size_t written = fwrite(bmp_data, 1, total_size, stdout);
        fflush(stdout);

        free(bmp_data);

        shell_print_ok("screenshot: streamed %lu bytes to serial",
                       (unsigned long)written);
    }

    /* Free the pixel data and draw buffer structure */
    if (pixel_data != NULL) {
        free(pixel_data);
    }
    if (draw_buf != NULL) {
        free(draw_buf);
    }
    batch_set_errorlevel(0);
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
     * in place â€” after it runs, `command` would be truncated to the first
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

    /* FreeRTOS task introspection: `ps`, `tasks`, and `top` are the same
     * read-only listing (top adds a summary header). */
    if (shell_text_equals_ignore_case(argv[0], "ps") ||
        shell_text_equals_ignore_case(argv[0], "tasks") ||
        shell_text_equals_ignore_case(argv[0], "top")) {
        shell_command_ps(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "power")) {
        shell_command_power(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sleep")) {
        shell_command_sleep(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "deepsleep")) {
        shell_command_deepsleep(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "pwm")) {
        shell_execute_pwm_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "freq")) {
        shell_execute_freq_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "adc")) {
        shell_execute_adc_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "i2c")) {
        shell_execute_i2c_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "spi")) {
        shell_execute_spi_command(argc, argv);
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

    /* ---- Screenshot command ---- */
    if (shell_text_equals_ignore_case(argv[0], "screenshot") ||
        shell_text_equals_ignore_case(argv[0], "scr") ||
        shell_text_equals_ignore_case(argv[0], "capture")) {
        shell_command_screenshot(argc, argv);
        return true;
    }

    /* ---- Module-routed commands ----
     * These receive the original unsplit line because their own parsers
     * need the full text (for example `wifi connect <ssid> <password>`). */
    if (shell_text_equals_ignore_case(argv[0], "wifi")) {
        esp_err_t wifi_error = networking_handle_wifi_command(family_command);
        free(family_command);
        batch_set_errorlevel(wifi_error == ESP_OK ? 0 : 1);
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
        batch_set_errorlevel(shell_command_disk(family_command));
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
        batch_set_errorlevel(shell_command_del(argc, argv));
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
        batch_set_errorlevel(shell_command_rmdir(argc, argv));
        return true;
    }

    /* ---- Recycle bin ---- */
    if (shell_text_equals_ignore_case(argv[0], "undelete") || shell_text_equals_ignore_case(argv[0], "restore")) {
        batch_set_errorlevel(shell_command_undelete(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "trash") || shell_text_equals_ignore_case(argv[0], "recycle")) {
        batch_set_errorlevel(shell_command_trash(argc, argv));
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
        batch_set_errorlevel(shell_command_xcopy(argc, argv));
        return true;
    }

    /* ---- Volume management ---- */
    if (shell_text_equals_ignore_case(argv[0], "chkdsk") ||
        shell_text_equals_ignore_case(argv[0], "scandisk")) {
        shell_command_chkdsk(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "format")) {
        batch_set_errorlevel(shell_command_format(argc, argv));
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

    if (shell_text_equals_ignore_case(argv[0], "alias")) {
        shell_command_alias(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "unalias")) {
        shell_command_unalias(argc, argv);
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

    /* Time / date / timezone / SNTP commands live in the clock component. */
    if (shell_text_equals_ignore_case(argv[0], "date")) {
        clock_command_date(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "time")) {
        clock_command_time(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sntp") ||
        shell_text_equals_ignore_case(argv[0], "ntpsync")) {
        clock_command_sntp(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "timezone")) {
        clock_command_timezone(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "exit")) {
        shell_command_exit(argc, argv);
        return true;
    }

    /* ---- File utility commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "find")) {
        batch_set_errorlevel(shell_command_find(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "findstr")) {
        batch_set_errorlevel(shell_command_findstr(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "more")) {
        batch_set_errorlevel(shell_command_more(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tree")) {
        shell_command_tree(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "fc")) {
        batch_set_errorlevel(shell_command_fc(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "comp")) {
        batch_set_errorlevel(shell_command_comp(argc, argv));
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sort")) {
        batch_set_errorlevel(shell_command_sort(argc, argv));
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

    /* ---- Network services and diagnostics ----
     * `httpd` drives the SD HTTP file server (start/stop/status) and
     * `netstat` / `ipconfig` report lwIP state. All of it lives in
     * components/networking; the dispatcher only maps results onto
     * ERRORLEVEL so `httpd start && ...` works in batch files. */
    if (shell_text_equals_ignore_case(argv[0], "httpd")) {
        esp_err_t error = ESP_OK;

        if (argc == 2 && shell_text_equals_ignore_case(argv[1], "start")) {
            error = networking_httpd_start();
        } else if (argc == 2 && shell_text_equals_ignore_case(argv[1], "stop")) {
            error = networking_httpd_stop();
        } else if (argc == 2 && shell_text_equals_ignore_case(argv[1], "status")) {
            networking_httpd_status();
        } else {
            shell_print_usage("Usage: httpd start | httpd stop | httpd status");
            batch_set_errorlevel(2);
            return true;
        }
        batch_set_errorlevel(error == ESP_OK ? 0 : 1);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "netstat")) {
        networking_netstat();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ipconfig")) {
        networking_ipconfig();
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
 *   1. Chain splitting on unquoted &, &&, and || â€” one segment at a time
 *   2. Variable expansion (%VAR%, %0..%9, %*) â€” owned by components/batch
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

    /* draw_buf errorlevel rather than clearing it. Clearing would destroy the
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
     * non-zero errorlevel. Comparing against the draw_buf means a stale value
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

    /* DOSKEY-style macro expansion: replace a leading alias with its value
     * before parsing, so `alias ll=dir /s` then typing `ll` runs `dir /s`.
     * Only expands at the interactive prompt, never inside a batch file. */
    if (!batch_alias_expand_command(command, chain_buffer, chain_size)) {
        snprintf(chain_buffer, chain_size, "%s", command);
    }

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
 * CLOCK HOST-RENDER WRAPPERS
 * ========================================================================
 * The clock component's date/time/timezone/sntp commands render through the
 * clock_host_ops_t table. These wrappers map each entry onto the matching
 * shell print/record helper, passing the clock-supplied text as a %s argument
 * so a literal '%' or '@' in it stays data (same injection guard the shell
 * print helpers use internally).
 */

static void command_clock_print_heading(const char *text)
{
    shell_print_heading("%s", text);
}

static void command_clock_print_field(const char *label, const char *value)
{
    shell_print_field(label, "%s", value);
}

static void command_clock_print_ok(const char *text)
{
    shell_print_ok("%s", text);
}

static void command_clock_print_error(const char *text)
{
    shell_print_error("%s", text);
}

static void command_clock_print_warning(const char *text)
{
    shell_print_warning("%s", text);
}

static void command_clock_print_muted(const char *text)
{
    shell_print_muted("%s", text);
}

static void command_clock_print_usage(const char *text)
{
    shell_print_usage("%s", text);
}

static void command_clock_record_error(const char *domain, esp_err_t error, const char *message)
{
    shell_record_errorf(domain, error, "%s", message);
}

static void command_clock_record_warning(const char *domain, const char *message)
{
    shell_record_warningf(domain, "%s", message);
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

    /* Publish the shell render helpers to the clock component. The clock
     * commands (date/time/timezone/sntp) live in components/clock and stay a
     * leaf; they print through this table. Each wrapper passes the text as a
     * %s argument so a literal '%' or '@' in it is treated as data. */
    {
        static const clock_host_ops_t clock_ops = {
            .emit_text          = shell_transcript_append_text,
            .print_heading      = command_clock_print_heading,
            .print_field        = command_clock_print_field,
            .print_ok           = command_clock_print_ok,
            .print_error        = command_clock_print_error,
            .print_warning      = command_clock_print_warning,
            .print_muted        = command_clock_print_muted,
            .print_usage        = command_clock_print_usage,
            .record_error       = command_clock_record_error,
            .record_warning     = command_clock_record_warning,
            .equals_ignore_case = shell_text_equals_ignore_case,
        };
        clock_register_host_ops(&clock_ops);
    }

    s_initialized = true;
    ESP_LOGI(COMMAND_TAG, "Command module initialized");
}

bool command_is_initialized(void)
{
    return s_initialized;
}

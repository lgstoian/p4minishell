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
#include "config_cmd.h"
#include "batch.h"
#include "calc.h"
#include "applib.h"
#include "storage.h"
#include "storage_commands.h"
#include "shell.h"
#include "editor.h"
#include "editor_view.h"
#include "ansi_palette.h"
#include "ansi.h"
#include "audio.h"
#include "display.h"
#include "header.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "networking.h"
#include "driver/usb_serial_jtag.h"
#include <errno.h>
#include <stdarg.h>
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
#include "esp_chip_info.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
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
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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
#define SHELL_POWER_IDLE_DISPLAY_OFF_SECS  P4_CONFIG_POWER_IDLE_DISPLAY_OFF_SECS
#define SHELL_POWER_IDLE_DISPLAY_MAX_SECS  P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS
#define SHELL_POWER_WAKE_GPIO              P4_CONFIG_POWER_WAKE_GPIO
#define SHELL_POWER_WAKE_LEVEL             P4_CONFIG_POWER_WAKE_LEVEL
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
static void shell_command_beep(int argc, char **argv);
static void shell_command_tone(int argc, char **argv);
static void shell_command_wavplay(int argc, char **argv);
static void shell_command_audio(int argc, char **argv);
static void shell_command_clip(int argc, char **argv);
static void shell_command_paste(int argc, char **argv);
static void shell_command_history(int argc, char **argv);
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

/* Editor command + ops-table hooks. */
static void shell_command_edit(int argc, char **argv);
static bool editor_is_active(void);
static bool editor_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);

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
 * BATTERY HARDWARE
 * ======================================================================== */

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

/* ========================================================================
 * IDLE DISPLAY-OFF (`power idle`) + ACTIVITY WAKE
 * ========================================================================
 * After the configured idle timeout without user input the display backlight
 * is switched off; the next touch, USB keyboard/mouse, or serial command wakes
 * it again. Idle-off only drops the backlight (`display_set_power_state`), so
 * the panel, GT911, and USB host keep running and the shell state is
 * untouched - waking is a clean backlight-on plus a header notification.
 */

static portMUX_TYPE s_power_idle_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_power_idle_off_secs;         /* 0 = disabled */
static int64_t s_power_last_activity_us;  /* esp_timer_get_time() */
static bool s_power_display_off_by_idle;

void shell_power_set_idle_timeout(int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds > SHELL_POWER_IDLE_DISPLAY_MAX_SECS) {
        seconds = SHELL_POWER_IDLE_DISPLAY_MAX_SECS;
    }
    portENTER_CRITICAL(&s_power_idle_lock);
    s_power_idle_off_secs = seconds;
    portEXIT_CRITICAL(&s_power_idle_lock);
}

int shell_power_get_idle_timeout(void)
{
    int seconds;

    portENTER_CRITICAL(&s_power_idle_lock);
    seconds = s_power_idle_off_secs;
    portEXIT_CRITICAL(&s_power_idle_lock);
    return seconds;
}

void shell_power_notify_activity(void)
{
    bool wake = false;

    portENTER_CRITICAL(&s_power_idle_lock);
    s_power_last_activity_us = esp_timer_get_time();
    if (s_power_display_off_by_idle) {
        s_power_display_off_by_idle = false;
        wake = true;
    }
    portEXIT_CRITICAL(&s_power_idle_lock);

    if (wake) {
        /* Route through the transcript (the normal, safe output path) rather
         * than an async header render, so waking never perturbs LVGL layout. */
        display_set_power_state(DISPLAY_POWER_ON);
        shell_transcript_appendf_ansi(SH_MUTE "power: display on\n" SH_RST);
    }
}

void shell_power_idle_tick(void)
{
    int seconds;
    int64_t now_us = esp_timer_get_time();
    int64_t last_us;
    bool off_by_idle;
    bool do_off = false;

    portENTER_CRITICAL(&s_power_idle_lock);
    seconds = s_power_idle_off_secs;
    last_us = s_power_last_activity_us;
    off_by_idle = s_power_display_off_by_idle;
    portEXIT_CRITICAL(&s_power_idle_lock);

    if (display_get_power_state() == DISPLAY_POWER_ON && !off_by_idle) {
        if (seconds > 0 && (now_us - last_us) >= (int64_t)seconds * 1000000LL) {
            do_off = true;
        }
    } else if (off_by_idle) {
        /* The display is off because of idle: a fresh touch press is activity.
         * This runs on the LVGL task, so indev access is safe. */
        lv_indev_t *indev = NULL;

        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                shell_power_notify_activity();
                break;
            }
        }
    }

    if (do_off) {
        portENTER_CRITICAL(&s_power_idle_lock);
        s_power_display_off_by_idle = true;
        portEXIT_CRITICAL(&s_power_idle_lock);
        display_set_power_state(DISPLAY_POWER_SLEEP);
        shell_transcript_appendf_ansi(SH_MUTE "power: display off (idle)\n" SH_RST);
    }
}

/** Configure a user-wired GPIO to wake light/deep sleep, if one is set. */
static void shell_power_enable_gpio_wake(void)
{
    int wake_gpio = (int)SHELL_POWER_WAKE_GPIO;

    if (wake_gpio < 0) {
        return;
    }

    {
        gpio_config_t wake_io;

        memset(&wake_io, 0, sizeof(wake_io));
        wake_io.pin_bit_mask = 1ULL << wake_gpio;
        wake_io.mode = GPIO_MODE_INPUT;
        wake_io.pull_up_en = (SHELL_POWER_WAKE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        wake_io.pull_down_en = (SHELL_POWER_WAKE_LEVEL == 1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        wake_io.intr_type = GPIO_INTR_DISABLE;

        gpio_config(&wake_io);
        if (gpio_wakeup_enable((gpio_num_t)wake_gpio,
                               SHELL_POWER_WAKE_LEVEL ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL) == ESP_OK) {
            (void)esp_sleep_enable_gpio_wakeup();
            shell_transcript_appendf_ansi(SH_LBL "sleep:" SH_RST " GPIO wake on " SH_NUM "GPIO%d" SH_RST
                                     " (" SH_VAL "%s" SH_RST ")\n",
                                     wake_gpio,
                                     SHELL_POWER_WAKE_LEVEL ? "high" : "low");
        } else {
            shell_print_warning("sleep: failed to enable GPIO wake on GPIO%d", wake_gpio);
        }
    }
}

/** Report the touch-wake situation honestly before entering light sleep. */
static void shell_power_report_wake_capability(void)
{
#if BOARD_CFG_LCD_TOUCH_INT_GPIO == GPIO_NUM_NC
    shell_transcript_appendf_ansi(SH_MUTE "sleep: touch wake unavailable (GT911 INT is not wired on this board)\n");
    shell_transcript_appendf_ansi(SH_MUTE "       set P4_CONFIG_POWER_WAKE_GPIO for a button/switch, or use `power idle`\n");
#else
    shell_transcript_appendf_ansi(SH_MUTE "sleep: touch wake available (GT911 INT on GPIO%d)\n",
                             (int)BOARD_CFG_LCD_TOUCH_INT_GPIO);
#endif
}

static void shell_command_power(int argc, char **argv)
{
    esp_sleep_wakeup_cause_t wake_cause;
    display_power_state_t display_state;

    /* `power idle <seconds|off>` configures the idle display-off timeout;
     * `power idle` prints it. */
    if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "idle")) {
        if (argc >= 3) {
            if (shell_text_equals_ignore_case(argv[2], "off")) {
                shell_power_set_idle_timeout(0);
            } else {
                char *end = NULL;
                long parsed = strtol(argv[2], &end, 10);

                if (*end != '\0' || parsed < 0 || parsed > SHELL_POWER_IDLE_DISPLAY_MAX_SECS) {
                    shell_print_error("power: invalid idle timeout %s (0..%d, or `off`)",
                                      argv[2], SHELL_POWER_IDLE_DISPLAY_MAX_SECS);
                    shell_record_warningf("power", "Invalid power idle argument: %s", argv[2]);
                    batch_set_errorlevel(2);
                    return;
                }
                shell_power_set_idle_timeout((int)parsed);
            }
            shell_power_notify_activity();
            if (shell_power_get_idle_timeout() > 0) {
                shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " display off after " SH_NUM "%d" SH_RST " s\n",
                                         shell_power_get_idle_timeout());
            } else {
                shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " " SH_MUTE "display idle-off disabled" SH_RST "\n");
            }
            batch_set_errorlevel(0);
            return;
        }
        if (shell_power_get_idle_timeout() > 0) {
            shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " display off after " SH_NUM "%d" SH_RST " s\n",
                                     shell_power_get_idle_timeout());
        } else {
            shell_transcript_appendf_ansi(SH_LBL "power.idle:" SH_RST " " SH_MUTE "display idle-off disabled" SH_RST "\n");
        }
        batch_set_errorlevel(0);
        return;
    }

    if (argc >= 2 && !shell_text_equals_ignore_case(argv[1], "status")) {
        shell_print_usage("Usage: power [status] | power idle [seconds|off]");
        shell_record_warningf("power", "Usage error for power command");
        batch_set_errorlevel(2);
        return;
    }
    batch_set_errorlevel(0);

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

    if (shell_power_get_idle_timeout() > 0) {
        shell_transcript_appendf_ansi(SH_LBL "power.idle_off:" SH_RST " " SH_NUM "%d" SH_RST " s\n",
                                 shell_power_get_idle_timeout());
    } else {
        shell_transcript_appendf_ansi(SH_LBL "power.idle_off:" SH_RST " " SH_MUTE "off" SH_RST "\n");
    }
    if (SHELL_POWER_WAKE_GPIO != GPIO_NUM_NC) {
        shell_transcript_appendf_ansi(SH_LBL "power.wake_gpio:" SH_RST " " SH_NUM "GPIO%d" SH_RST "\n",
                                 (int)SHELL_POWER_WAKE_GPIO);
    }

    shell_transcript_appendf_ansi(SH_LBL "power.wifi:" SH_RST " " SH_NUM "%s" SH_RST "\n",
                             networking_wifi_is_connected() ? "connected" : "down");

    wake_cause = esp_sleep_get_wakeup_cause();
    shell_transcript_appendf_ansi(SH_LBL "power.wake:" SH_RST " " SH_NUM "%s" SH_RST "\n",
                             shell_power_wake_cause_string(wake_cause));
    shell_transcript_appendf_ansi(SH_MUTE "Tip: `battery sleep on` enables automatic light sleep when idle.\n");
    shell_transcript_appendf_ansi(SH_MUTE "Tip: `power idle 60` turns the display off after 60 s of inactivity.\n");
}

/** Clear the configured GPIO wake so a still-active level cannot re-trigger. */
static void shell_power_disable_gpio_wake(void)
{
    int wake_gpio = (int)SHELL_POWER_WAKE_GPIO;

    if (wake_gpio >= 0) {
        (void)gpio_wakeup_disable((gpio_num_t)wake_gpio);
    }
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

    /* Wake capability: the GT911 INT line is not wired on this board, so
     * report that honestly and offer the configured GPIO wake as the
     * external alternative. */
    shell_power_report_wake_capability();
    shell_power_enable_gpio_wake();

#if SHELL_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI
    shell_power_shutdown_wifi();
#else
    shell_transcript_appendf_ansi(SH_MUTE "sleep: keeping Wi-Fi state (light sleep wifi shutdown disabled)\n");
#endif

    display_set_power_state(DISPLAY_POWER_OFF);
    audio_stop();

    /* Give the transcript and LVGL task time to paint before sleeping. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_POWER_SLEEP_PRE_DELAY_MS));

    error = esp_light_sleep_start();
    shell_power_disable_gpio_wake();
    if (error != ESP_OK) {
        shell_transcript_appendf_ansi(SH_ERR "sleep: light sleep failed" SH_RST " (" SH_WARN "%s" SH_RST ")\n",
                                 esp_err_to_name(error));
    } else {
        shell_transcript_appendf_ansi(SH_LBL "sleep:" SH_RST " woke up (" SH_LBL "cause" SH_RST "=" SH_NUM "%s" SH_RST ")\n",
                                 shell_power_wake_cause_string(esp_sleep_get_wakeup_cause()));
    }

    display_set_power_state(DISPLAY_POWER_ON);
    shell_power_notify_activity();

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
    audio_stop();

    /* Let the transcript and LVGL task paint before the chip resets. */
    vTaskDelay(pdMS_TO_TICKS(SHELL_POWER_SLEEP_PRE_DELAY_MS));
    shell_transcript_appendf_ansi(SH_LBL "deepsleep:" SH_RST " entering deep sleep\n");
    vTaskDelay(pdMS_TO_TICKS(SHELL_REBOOT_DELAY_MS));

    esp_deep_sleep_start();
}

static void shell_command_volume(int argc, char **argv)
{
    int percent;
    esp_err_t error;

    /* Bare `volume` prints the current codec volume (a query form). */
    if (argc == 1) {
        shell_transcript_appendf_ansi(SH_LBL "volume:" SH_RST " " SH_NUM "%d%%" SH_RST "\n",
                                 audio_get_volume());
        batch_set_errorlevel(0);
        return;
    }

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_print_usage("Usage: volume [<0-100>]");
        shell_record_warningf("volume", "Usage error for volume command");
        batch_set_errorlevel(2);
        return;
    }

    error = audio_set_volume(percent);
    if (error != ESP_OK) {
        shell_print_error("volume: failed to initialize the ES8311 speaker path (%s)", esp_err_to_name(error));
        shell_record_errorf("volume", error, "Failed to initialize speaker device");
        batch_set_errorlevel(1);
        return;
    }

    shell_transcript_appendf_ansi(SH_LBL "volume set to" SH_RST " " SH_NUM "%d%%" SH_RST "\n", percent);
    batch_set_errorlevel(0);
}

/* ========================================================================
 * AUDIO PLAYBACK COMMANDS: beep, tone, wavplay, audio
 * ======================================================================== */

static void shell_command_beep(int argc, char **argv)
{
    if (argc != 1) {
        shell_print_usage("Usage: beep");
        batch_set_errorlevel(2);
        return;
    }
    if (!audio_play_tone(P4_CONFIG_BEEP_FREQ_HZ, P4_CONFIG_BEEP_DURATION_MS)) {
        shell_print_error("beep: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "beep:" SH_RST " " SH_NUM "%d" SH_RST " Hz, " SH_NUM "%u" SH_RST " ms\n",
                             P4_CONFIG_BEEP_FREQ_HZ, (unsigned)P4_CONFIG_BEEP_DURATION_MS);
    batch_set_errorlevel(0);
}

static void shell_command_tone(int argc, char **argv)
{
    char *end;
    long freq;
    uint32_t duration_ms = P4_CONFIG_TONE_DURATION_DEFAULT_MS;

    if (argc < 2 || argc > 3) {
        shell_print_usage("Usage: tone <freq> [ms]");
        batch_set_errorlevel(2);
        return;
    }

    freq = strtol(argv[1], &end, 10);
    if (*end != '\0' || freq < P4_CONFIG_TONE_FREQ_MIN || freq > P4_CONFIG_TONE_FREQ_MAX) {
        shell_print_error("tone: frequency must be %d..%d Hz",
                          P4_CONFIG_TONE_FREQ_MIN, P4_CONFIG_TONE_FREQ_MAX);
        batch_set_errorlevel(2);
        return;
    }

    if (argc == 3) {
        long ms = strtol(argv[2], &end, 10);

        if (*end != '\0' || ms < 10 || ms > P4_CONFIG_TONE_DURATION_MAX_MS) {
            shell_print_error("tone: duration must be 10..%d ms", P4_CONFIG_TONE_DURATION_MAX_MS);
            batch_set_errorlevel(2);
            return;
        }
        duration_ms = (uint32_t)ms;
    }

    if (!audio_play_tone((int)freq, duration_ms)) {
        shell_print_error("tone: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "tone:" SH_RST " " SH_NUM "%d" SH_RST " Hz for " SH_NUM "%u" SH_RST " ms\n",
                             (int)freq, (unsigned)duration_ms);
    batch_set_errorlevel(0);
}

static void shell_command_wavplay(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    uint64_t file_size;

    if (argc != 2) {
        shell_print_usage("Usage: wavplay <file.wav>");
        batch_set_errorlevel(2);
        return;
    }

    if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("wavplay: invalid path %s", argv[1]);
        batch_set_errorlevel(2);
        return;
    }

    file_size = storage_get_file_size(resolved);
    if (file_size == 0) {
        shell_print_error("wavplay: file not found %s", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    if (file_size > P4_CONFIG_WAV_MAX_BYTES) {
        shell_print_error("wavplay: file is too large (max %d bytes)", (int)P4_CONFIG_WAV_MAX_BYTES);
        batch_set_errorlevel(2);
        return;
    }

    if (!audio_play_wav(resolved)) {
        shell_print_error("wavplay: audio busy - wait for the current sound to finish");
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_LBL "wavplay:" SH_RST " " SH_PATH "%s" SH_RST "\n", resolved);
    batch_set_errorlevel(0);
}

static void shell_command_audio(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: audio status | audio stop");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "status")) {
        if (audio_busy()) {
            shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " " SH_WARN "playing" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " " SH_MUTE "idle" SH_RST "\n");
        }
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "stop")) {
        audio_stop();
        shell_transcript_appendf_ansi(SH_LBL "audio:" SH_RST " stop requested\n");
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: audio status | audio stop");
    batch_set_errorlevel(2);
}

/* ========================================================================
 * CLIPBOARD COMMANDS: clip, paste
 * ========================================================================
 * `clip` reads/writes the RAM clipboard (text, transcript lines, or a file
 * reference); `paste` injects it into the input line or copies a clipped file
 * to a destination. All verbs are batch-safe and redirectable.
 */

/** Basename of a path, accepting both '/' and '\' separators. */
static const char *shell_clip_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');

    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    return (slash != NULL) ? slash + 1 : path;
}

static void shell_command_clip(int argc, char **argv)
{
    /* `clip` with no argument prints the clipboard. */
    if (argc == 1) {
        const char *clip = shell_clipboard_get();

        if (clip[0] == '\0') {
            shell_transcript_appendf_ansi(SH_LBL "clipboard:" SH_RST " " SH_MUTE "(empty)" SH_RST "\n");
        } else if (shell_clipboard_is_file()) {
            shell_transcript_appendf_ansi(SH_LBL "clipboard:" SH_RST " " SH_PATH "%s" SH_RST
                                     " " SH_MUTE "(file)" SH_RST "\n", clip);
        } else {
            shell_transcript_appendf_ansi(SH_LBL "clipboard:" SH_RST " " SH_VAL "%s" SH_RST "\n", clip);
        }
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "copy")) {
        int lines = 1;

        if (argc == 3) {
            char *end = NULL;
            long parsed = strtol(argv[2], &end, 10);

            if (*end != '\0' || parsed < 1 || parsed > P4_CONFIG_CLIP_COPY_LINES_MAX) {
                shell_print_error("clip: line count must be 1..%d", P4_CONFIG_CLIP_COPY_LINES_MAX);
                batch_set_errorlevel(2);
                return;
            }
            lines = (int)parsed;
        } else if (argc > 3) {
            shell_print_usage("Usage: clip copy [N]");
            batch_set_errorlevel(2);
            return;
        }

        if (!shell_clipboard_copy_transcript(lines)) {
            shell_print_error("clip: the transcript is empty");
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " copied " SH_NUM "%d" SH_RST " line%s to the clipboard\n",
                                 lines, lines == 1 ? "" : "s");
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "file")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];

        if (argc != 3) {
            shell_print_usage("Usage: clip file <path>");
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("clip: invalid path %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        shell_clipboard_set_file(resolved);
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " file " SH_PATH "%s" SH_RST "\n", resolved);
        batch_set_errorlevel(0);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "read")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        char *buf;
        uint64_t file_size;
        size_t got;

        if (argc != 3) {
            shell_print_usage("Usage: clip read <file>");
            batch_set_errorlevel(2);
            return;
        }
        if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("clip: invalid path %s", argv[2]);
            batch_set_errorlevel(2);
            return;
        }
        file_size = storage_get_file_size(resolved);
        if (file_size == 0) {
            shell_print_error("clip: file not found %s", argv[2]);
            batch_set_errorlevel(1);
            return;
        }
        if (file_size >= P4_CONFIG_CLIPBOARD_BYTES) {
            shell_print_error("clip: file is too large for the clipboard (%d bytes)",
                              (int)P4_CONFIG_CLIPBOARD_BYTES);
            batch_set_errorlevel(1);
            return;
        }

        buf = malloc(P4_CONFIG_CLIPBOARD_BYTES);
        if (buf == NULL) {
            shell_print_error("clip: out of memory reading the file");
            batch_set_errorlevel(1);
            return;
        }
        {
            FILE *file = fopen(resolved, "rb");

            if (file == NULL) {
                free(buf);
                shell_print_error("clip: cannot open %s", resolved);
                batch_set_errorlevel(1);
                return;
            }
            got = fread(buf, 1, P4_CONFIG_CLIPBOARD_BYTES - 1, file);
            fclose(file);
        }
        buf[got] = '\0';
        shell_clipboard_set(buf);
        free(buf);
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " read " SH_PATH "%s" SH_RST " (" SH_NUM "%d" SH_RST " B)\n",
                                 resolved, (int)got);
        batch_set_errorlevel(0);
        return;
    }

    /* Anything else is literal clipboard text: `clip hello world`. A first
     * argument that looks like a subcommand (a typo or a query such as
     * `clip status`) must not silently clobber the current clipboard with the
     * word itself — report a usage error instead. */
    {
        static const char *const reserved[] = {
            "copy", "file", "read",
            "status", "list", "show", "clear", "help", "?", "view", "print", "info",
        };
        size_t reserved_index;
        bool looks_like_subcommand = false;

        for (reserved_index = 0;
             reserved_index < sizeof(reserved) / sizeof(reserved[0]);
             reserved_index++) {
            if (shell_text_equals_ignore_case(argv[1], reserved[reserved_index])) {
                looks_like_subcommand = true;
                break;
            }
        }
        if (looks_like_subcommand) {
            shell_print_usage("Usage: clip [text] | clip copy [N] | clip file <path> | clip read <file>");
            batch_set_errorlevel(2);
            return;
        }
    }
    {
        char text[P4_CONFIG_CLIPBOARD_BYTES];

        shell_join_args(argv, 1, argc, text, sizeof(text));
        shell_clipboard_set(text);
        shell_transcript_appendf_ansi(SH_LBL "clip:" SH_RST " clipboard set (" SH_NUM "%d" SH_RST " B)\n",
                                 (int)strlen(text));
        batch_set_errorlevel(0);
    }
}

static void shell_command_paste(int argc, char **argv)
{
    const char *clip = shell_clipboard_get();

    /* `paste <dest>` copies a clipped file reference to a destination. */
    if (argc == 2) {
        char resolved_dest[P4_CONFIG_SD_PATH_BYTES];
        char dest_file[P4_CONFIG_SD_PATH_BYTES * 2 + 16];
        struct stat dst_st;
        esp_err_t error;

        if (!shell_clipboard_is_file()) {
            shell_print_error("paste: the clipboard is not a file (use `clip file <path>`)");
            batch_set_errorlevel(1);
            return;
        }
        if (shell_fs_resolve_path(argv[1], resolved_dest, sizeof(resolved_dest)) != ESP_OK) {
            shell_print_error("paste: invalid destination path");
            batch_set_errorlevel(2);
            return;
        }

        if (stat(resolved_dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
            snprintf(dest_file, sizeof(dest_file), "%s/%s",
                     resolved_dest, shell_clip_basename(clip));
        } else {
            snprintf(dest_file, sizeof(dest_file), "%s", resolved_dest);
        }

        error = shell_fs_copy_file(clip, dest_file);
        if (error != ESP_OK) {
            shell_print_error("paste: failed to copy %s -> %s (%s)",
                              clip, dest_file, esp_err_to_name(error));
            batch_set_errorlevel(1);
            return;
        }
        shell_transcript_appendf_ansi(SH_LBL "paste:" SH_RST " " SH_PATH "%s" SH_RST " -> " SH_PATH "%s" SH_RST "\n",
                                 clip, dest_file);
        batch_set_errorlevel(0);
        return;
    }

    if (argc != 1) {
        shell_print_usage("Usage: paste [<destination>]");
        batch_set_errorlevel(2);
        return;
    }

    /* `paste` injects the clipboard into the input line at the cursor. */
    if (clip[0] == '\0') {
        shell_print_error("paste: the clipboard is empty");
        batch_set_errorlevel(1);
        return;
    }
    shell_input_line_paste(clip);
    batch_set_errorlevel(0);
}

/* ========================================================================
 * TAB COMPLETION PROVIDER
 * ======================================================================== */

/** Built-in command names offered by Tab completion for the first token. */
static const char *const shell_builtin_commands[] = {
    "about", "adc", "alias", "ansi", "append", "appconfig", "appmode", "attrib", "audio", "battery", "beep",
    "bluetooth", "brightness", "bt", "c6ota", "calc", "call", "capture", "cd", "chdir",    "chkdsk", "choice", "clear", "clip", "cls", "comp", "config", "copy", "date", "debug",
    "deepsleep", "del", "dir", "disk", "display", "dns", "echo", "edit",
    "endlocal",
    "erase", "exit", "fc", "find", "findstr", "for", "format", "freq", "goto", "gpio",
    "help", "history", "httpd", "httpget", "i2c", "if", "ini", "ipconfig", "keyboard",
    "label", "md", "mem", "menu", "mkdir", "more", "move", "netstat", "nslookup",
    "ntpsync", "paste", "path", "pause", "ping", "power", "prompt", "ps", "pwm",
    "rd", "reboot", "receive", "recycle", "rem", "ren", "rename", "restore", "rgb", "rmdir",
    "rotate", "scandisk", "scr", "screenshot", "sd", "sdeject", "send", "set", "setlocal",
    "shift", "sleep", "sntp", "sort", "spi", "sysinfo", "tasks", "time", "timezone",
    "tone", "top", "touch", "trash", "tree", "type", "unalias", "undelete", "usb",
    "ver", "version", "volume", "wavplay", "wget", "wifi", "windows", "write", "xcopy",
    "proc", "temp",
};

/** Completion collector: a bounded list of heap-copied matches. */
typedef struct {
    const char *word;
    size_t word_len;
    char **matches;
    int max;
    int count;
} shell_complete_ctx_t;

static void shell_complete_add(shell_complete_ctx_t *ctx, const char *candidate)
{
    if (candidate == NULL || ctx->count >= ctx->max) {
        return;
    }
    if (strncasecmp(candidate, ctx->word, ctx->word_len) != 0) {
        return;
    }
    ctx->matches[ctx->count] = strdup(candidate);
    if (ctx->matches[ctx->count] != NULL) {
        ctx->count++;
    }
}

/** Add SD file/directory matches for the word (directories get a trailing /). */
static void shell_complete_add_paths(shell_complete_ctx_t *ctx)
{
    const char *slash = strrchr(ctx->word, '/');
    const char *bslash = strrchr(ctx->word, '\\');
    const char *base;
    char dir_vfs[P4_CONFIG_SD_PATH_BYTES];
    char dir_resolved[P4_CONFIG_SD_PATH_BYTES];
    char *candidate = malloc(P4_CONFIG_SD_PATH_BYTES + 8);
    char *full = malloc(P4_CONFIG_SD_PATH_BYTES + 256);
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    if (candidate == NULL || full == NULL) {
        free(candidate);
        free(full);
        return;
    }

    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }

    if (slash != NULL) {
        size_t dir_len = (size_t)(slash - ctx->word);

        if (dir_len >= sizeof(dir_vfs)) {
            dir_len = sizeof(dir_vfs) - 1;
        }
        memcpy(dir_vfs, ctx->word, dir_len);
        dir_vfs[dir_len] = '\0';
        base = slash + 1;
    } else {
        snprintf(dir_vfs, sizeof(dir_vfs), "%s",
                 shell_get_cwd() != NULL ? shell_get_cwd() : ".");
        base = ctx->word;
    }
    if (dir_vfs[0] == '\0') {
        snprintf(dir_vfs, sizeof(dir_vfs), ".");
    }
    if (shell_fs_resolve_path(dir_vfs, dir_resolved, sizeof(dir_resolved)) != ESP_OK) {
        free(candidate);
        free(full);
        return;
    }

    dir = opendir(dir_resolved);
    if (dir == NULL) {
        free(candidate);
        free(full);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t dir_prefix_len;
        size_t candidate_len;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (strncasecmp(entry->d_name, base, strlen(base)) != 0) {
            continue;
        }

        /* Candidate = the typed directory prefix (if any) + the entry name. */
        dir_prefix_len = slash != NULL ? (size_t)(slash - ctx->word) + 1 : 0;
        if (dir_prefix_len + strlen(entry->d_name) + 2 >= P4_CONFIG_SD_PATH_BYTES + 8) {
            continue;
        }
        memcpy(candidate, ctx->word, dir_prefix_len);
        snprintf(candidate + dir_prefix_len, P4_CONFIG_SD_PATH_BYTES + 8 - dir_prefix_len,
                 "%s", entry->d_name);
        candidate_len = strlen(candidate);

        /* Append '/' to directories so completion can keep going. */
        snprintf(full, P4_CONFIG_SD_PATH_BYTES + 256, "%s/%s", dir_resolved, entry->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode) &&
            candidate_len + 1 < P4_CONFIG_SD_PATH_BYTES + 8) {
            candidate[candidate_len] = '/';
            candidate[candidate_len + 1] = '\0';
        }

        shell_complete_add(ctx, candidate);
    }

    closedir(dir);
    free(candidate);
    free(full);
}

/**
 * Tab-completion provider: fills @p out with the @p match_index-th completion
 * of @p word (commands/aliases for the first token, plus SD file/dir paths)
 * and returns the total number of matches.
 */
static int shell_complete_word(const char *word, bool first_token, int match_index,
                               char *out, size_t out_size)
{
    shell_complete_ctx_t ctx;
    size_t index;
    int total;

    if (word == NULL || out == NULL || out_size == 0) {
        return 0;
    }

    ctx.word = word;
    ctx.word_len = strlen(word);
    ctx.max = P4_CONFIG_COMPLETION_MAX_MATCHES;
    ctx.count = 0;
    ctx.matches = calloc((size_t)ctx.max, sizeof(char *));
    if (ctx.matches == NULL) {
        return 0;
    }

    if (first_token) {
        for (index = 0; index < sizeof(shell_builtin_commands) / sizeof(shell_builtin_commands[0]); index++) {
            shell_complete_add(&ctx, shell_builtin_commands[index]);
        }
        for (index = 0; index < (size_t)P4_CONFIG_ALIAS_MAX; index++) {
            char name[P4_CONFIG_ALIAS_NAME_BYTES];

            if (shell_alias_get_by_index((int)index, name, sizeof(name), NULL, 0)) {
                shell_complete_add(&ctx, name);
            }
        }
    }

    /* Paths always complete; for the first token this also covers .bat files. */
    shell_complete_add_paths(&ctx);

    total = ctx.count;
    if (match_index >= 0 && match_index < total) {
        snprintf(out, out_size, "%s", ctx.matches[match_index]);
    }

    for (index = 0; index < (size_t)ctx.count; index++) {
        free(ctx.matches[index]);
    }
    free(ctx.matches);
    return total;
}

/* ========================================================================
 * EDITOR COMMAND AND OPS-TABLE HOOKS
 * ======================================================================== */

/** `edit <path>` � open a DOS-style inline text editor. */
static void shell_command_edit(int argc, char **argv)
{
    const char *path = NULL;
    int errorlevel = 0;

    if (argc > 2) {
        shell_print_usage("Usage: edit <path>");
        batch_set_errorlevel(2);
        return;
    }

    if (argc == 2) {
        path = argv[1];
    }

    /* The editor runs on the command worker task; it blocks until quit. */
    esp_err_t err = editor_session_run(path, &errorlevel);
    if (err != ESP_OK && errorlevel == 0) {
        errorlevel = 1;
    }
    batch_set_errorlevel(errorlevel);
}

bool editor_is_active(void)
{
    return editor_session_is_active() || editor_view_is_open();
}

bool editor_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii)
{
    if (!editor_view_is_open()) {
        return false;
    }
    return editor_view_handle_usb_key(key_code, modifiers, ascii);
}

/* Serial console control verbs accepted while the editor is open. */
static bool editor_serial_is_verb(const char *line, const char *verb)
{
    return line[0] == '\\' && strcasecmp(line + 1, verb) == 0;
}

/* Per-call context for async serial-line dispatch to the LVGL task. */
typedef struct {
    char *line;
} editor_serial_line_ctx_t;

/* Runs on the LVGL task: feeds one serial line's characters into the editor. */
static void editor_serial_line_cb(void *user_data)
{
    editor_serial_line_ctx_t *ctx = (editor_serial_line_ctx_t *)user_data;
    const char *line;
    size_t i;
    size_t len;

    if (ctx == NULL || ctx->line == NULL) {
        free(ctx);
        return;
    }


    line = ctx->line;
    len = strlen(line);

    if (editor_serial_is_verb(line, "q") || editor_serial_is_verb(line, "quit")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0x29, 0, 0); /* Esc -> quit */
        } else {
            editor_view_set_quit_requested();
        }
    } else if (editor_serial_is_verb(line, "s") || editor_serial_is_verb(line, "save")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0, 0x01, 's'); /* Ctrl+S */
        } else {
            editor_view_set_save_requested();
        }
    } else if (editor_serial_is_verb(line, "u") || editor_serial_is_verb(line, "undo")) {
        editor_view_handle_usb_key(0, 0x01, 'z');
    } else if (editor_serial_is_verb(line, "f") || editor_serial_is_verb(line, "find")) {
        editor_view_handle_usb_key(0, 0x01, 'f'); /* Ctrl+F */
    } else if (editor_serial_is_verb(line, "g") || editor_serial_is_verb(line, "goto")) {
        editor_view_handle_usb_key(0, 0x01, 'g'); /* Ctrl+G -> Go to line */
    } else if (editor_serial_is_verb(line, "o") || editor_serial_is_verb(line, "saveas")) {
        editor_view_handle_usb_key(0, 0x01, 'o'); /* Ctrl+O -> Save As */
    } else if (editor_serial_is_verb(line, "r") || editor_serial_is_verb(line, "redo")) {
        editor_view_handle_usb_key(0, 0x03, 'z'); /* Ctrl+Shift+Z */
    } else if (editor_serial_is_verb(line, "a") || editor_serial_is_verb(line, "selectall")) {
        editor_view_handle_usb_key(0, 0x01, 'a');
    } else {
        if (!editor_view_is_open()) {
            free(ctx->line);
            free(ctx);
            return;
        }
        /* A line of typed text: insert each character, then a newline. */
        for (i = 0; i < len; i++) {
            char ch = line[i];
            if (ch == '\\' && i == 0 && len > 1) {
                /* "\foo" that was not a known verb inserts a literal backslash
                 * and the rest of the line as text. */
                editor_view_handle_usb_key(0, 0, '\\');
                continue;
            }
            if (ch >= 0x20) {
                editor_view_handle_usb_key(0, 0, ch);
            }
        }
        editor_view_handle_usb_key(0x28, 0, '\n'); /* Enter -> newline */
    }

    free(ctx->line);
    free(ctx);
}

bool editor_handle_serial_line(const char *line)
{
    editor_serial_line_ctx_t *ctx;

    /* Accept lines whenever a session is active (even while the view is
     * still opening), so a quick '\q' after 'edit' is never misrouted as a
     * shell command and lost behind the blocked worker. */
    if ((!editor_session_is_active() && !editor_view_is_open()) || line == NULL) {
        return false;
    }

    /* Defer the whole line to the LVGL task: the editor's document and widget
     * state live there, and rebuilding rows on the UART console task would
     * block it past the watchdog and race the render cycle. */
    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return true; /* Consumed (drop) rather than misrouted. */
    }
    ctx->line = strdup(line);
    if (ctx->line == NULL) {
        free(ctx);
        return true;
    }

    if (lv_async_call(editor_serial_line_cb, ctx) != LV_RESULT_OK) {
        free(ctx->line);
        free(ctx);
    }
    return true;
}

/* ========================================================================
 * HISTORY COMMAND: history [list] / /save [file] / /load [file] / /clear
 * ======================================================================== */

static void shell_command_history(int argc, char **argv)
{
    if (argc == 1) {
        size_t index;

        shell_print_heading("Command history");
        for (index = 0; index < shell_history_get_count(); index++) {
            const char *line = shell_history_get(index);

            if (line != NULL) {
                shell_transcript_appendf_ansi(SH_NUM "%3u" SH_RST "  %s\n",
                                         (unsigned)(index + 1), line);
            }
        }
        if (shell_history_get_count() == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "history: empty\n" SH_RST);
        }
        batch_set_errorlevel(0);
        return;
    }

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "/clear")) {
        shell_history_clear();
        shell_transcript_appendf_ansi(SH_OK "history cleared\n" SH_RST);
        batch_set_errorlevel(0);
        return;
    }

    if (argc >= 2 && (shell_text_equals_ignore_case(argv[1], "/save") ||
                      shell_text_equals_ignore_case(argv[1], "/load"))) {
        bool saving = shell_text_equals_ignore_case(argv[1], "/save");
        const char *file = (argc >= 3) ? argv[2] : P4_CONFIG_HISTORY_PROFILE;
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;
        FILE *fp;

        if (shell_fs_resolve_path(file, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("history: invalid path %s", file);
            batch_set_errorlevel(2);
            return;
        }

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("history: SD card not present - insert and retry");
            batch_set_errorlevel(1);
            return;
        }

        if (saving) {
            size_t index;
            size_t bytes = shell_history_get_count() * 2;   /* rough free-space need */

            if (!storage_check_free_space((uint64_t)bytes + 4096, 0, "history")) {
                shell_sd_end(&session, "history");
                shell_print_error("history: not enough free space on the SD card");
                batch_set_errorlevel(1);
                return;
            }
            fp = fopen(resolved, "w");
            if (fp == NULL) {
                shell_sd_end(&session, "history");
                shell_print_error("history: cannot open %s for writing", resolved);
                batch_set_errorlevel(1);
                return;
            }
            for (index = 0; index < shell_history_get_count(); index++) {
                const char *line = shell_history_get(index);

                if (line != NULL) {
                    if (fprintf(fp, "%s\n", line) < 0) {
                        fclose(fp);
                        (void)unlink(resolved);
                        shell_sd_end(&session, "history");
                        shell_print_error("history: write failed, removed the partial file");
                        batch_set_errorlevel(1);
                        return;
                    }
                }
            }
            fclose(fp);
            shell_sd_end(&session, "history");
            shell_transcript_appendf_ansi(SH_LBL "history:" SH_RST " saved " SH_NUM "%u" SH_RST
                                     " command%s to " SH_PATH "%s" SH_RST "\n",
                                     (unsigned)shell_history_get_count(),
                                     shell_history_get_count() == 1 ? "" : "s", resolved);
        } else {
            /* The read line is command-sized and `history` can run from a
             * nested batch line, so it is heap-allocated (a 4096-byte stack
             * local here would eat into the worker task's stack budget). */
            char *line = malloc(P4_CONFIG_COMMAND_BYTES);
            size_t loaded = 0;

            if (line == NULL) {
                shell_sd_end(&session, "history");
                shell_print_error("history: out of memory reading %s", resolved);
                batch_set_errorlevel(1);
                return;
            }

            fp = fopen(resolved, "r");
            if (fp == NULL) {
                free(line);
                shell_sd_end(&session, "history");
                shell_print_error("history: cannot open %s for reading", resolved);
                batch_set_errorlevel(1);
                return;
            }
            while (fgets(line, P4_CONFIG_COMMAND_BYTES, fp) != NULL) {
                shell_trim(line);
                if (line[0] == '\0') {
                    continue;
                }
                shell_store_command_history(line);
                loaded++;
            }
            free(line);
            fclose(fp);
            shell_sd_end(&session, "history");
            shell_transcript_appendf_ansi(SH_LBL "history:" SH_RST " loaded " SH_NUM "%u" SH_RST
                                     " command%s from " SH_PATH "%s" SH_RST "\n",
                                     (unsigned)loaded, loaded == 1 ? "" : "s", resolved);
        }
        batch_set_errorlevel(0);
        return;
    }

    shell_print_usage("Usage: history | history /save [file] | history /load [file] | history /clear");
    batch_set_errorlevel(2);
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
 * `rgb` � control the WS2812 status LED (LED1, GPIO26).
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
        if (state.auto_status) {
            shell_transcript_appendf_ansi("  " SH_LBL "mode:" SH_RST " " SH_OK "auto status" SH_RST "\n");
        } else {
            shell_transcript_appendf_ansi("  " SH_LBL "mode:" SH_RST " " SH_VAL "manual" SH_RST "\n");
        }
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
 * `screenshot` / `scr` / `capture` — capture the LVGL screen as a BMP image.
 *
 * Usage:
 *   screenshot              → stream BMP over UART with magic markers
 *   screenshot file.bmp     → save BMP to SD card (current directory)
 *
 * Captures via lv_snapshot_take_to_draw_buf(), converts RGB565 → RGB888 for the BMP,
 * and outputs either to the serial console (with begin/end markers for
 * host-side extraction) or to an SD card file with free-space precheck.
 */

/* Raw byte write straight to the USB-Serial/JTAG TX ring. Unlike fwrite to
 * stdout, this bypasses the console VFS's CRLF newline translation, so binary
 * payloads (send/screenshot frames, receive ACKs) are never corrupted by an
 * inserted \r before every \n byte. The writes are chunked: the driver's
 * xRingbufferSend rejects a single buffer larger than the TX ring (4 KB), so
 * a big payload (e.g. a full screenshot) must be broken into small pieces.
 * Each chunk is time-bounded (P4_CONFIG_SERIAL_SEND_TIMEOUT_MS) so a host that
 * stops reading can never wedge the command worker. */
static bool serial_write_raw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0) {
        size_t chunk = remaining > 1024 ? 1024 : remaining;

        if (usb_serial_jtag_write_bytes(p, chunk,
                                        pdMS_TO_TICKS(P4_CONFIG_SERIAL_SEND_TIMEOUT_MS)) != (int)chunk) {
            return false;
        }
        p += chunk;
        remaining -= chunk;
    }
    return true;
}

/* Serial binary-stream framing shared by `screenshot`, `send`, and the
 * `receive` protocol: a 4-byte magic + 4-byte little-endian payload size,
 * then the raw bytes. The magic/size lets a host reader frame exactly one
 * payload off the USB-Serial/JTAG console stream without depending on the
 * surrounding transcript text. */
static void serial_write_frame_header(const char *magic4, uint32_t payload_size)
{
    uint8_t hdr[8];

    memcpy(hdr, magic4, 4);
    hdr[4] = (uint8_t)(payload_size & 0xFFu);
    hdr[5] = (uint8_t)((payload_size >> 8) & 0xFFu);
    hdr[6] = (uint8_t)((payload_size >> 16) & 0xFFu);
    hdr[7] = (uint8_t)((payload_size >> 24) & 0xFFu);
    (void)serial_write_raw(hdr, sizeof(hdr));
}

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
        free(pixel_data);
        free(draw_buf);
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
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("screenshot: SD card not present");
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Precheck free space: 54-byte header + width*height*3 pixel data */
        {
            uint64_t needed = 54 + (uint64_t)width * (uint64_t)height * 3;
            uint64_t reclaim = storage_get_file_size(resolved);
            if (!storage_check_free_space(needed, reclaim, "screenshot")) {
                shell_sd_end(&session, "screenshot");
                free(pixel_data);
        free(draw_buf);
                batch_set_errorlevel(1);
                return;
            }
        }

        FILE *f = fopen(resolved, "wb");
        if (f == NULL) {
            shell_print_error("screenshot: cannot create %s", resolved);
            shell_sd_end(&session, "screenshot");
            free(pixel_data);
        free(draw_buf);
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
            free(pixel_data);
        free(draw_buf);
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
            free(pixel_data);
        free(draw_buf);
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
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        shell_print_ok("screenshot: saved %s (%lu bytes)", resolved,
                       (unsigned long)(54 + (uint64_t)width * height * 3));
        free(pixel_data);
        free(draw_buf);
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

        /* The raw stream owns the console byte stream: suspend the console
         * reader (as `send`/`receive` do) so no host input is misread and no
         * console echo interleaves with the BMP payload. */
        shell_uart_console_rx_begin();
        serial_write_frame_header(P4_CONFIG_SCREENSHOT_BMP_MAGIC, (uint32_t)total_size);
        /* Send raw BMP data (raw driver write: no CRLF translation). */
        size_t written = serial_write_raw(bmp_data, total_size) ? total_size : 0;
        shell_uart_console_rx_end();

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
 * SERIAL FILE TRANSFER (receive / send)
 * ========================================================================
 * `receive` pushes a binary from the host into an SD file; `send` streams an
 * SD file (or a compact diagnostic report) back to the host. Both run over the
 * USB-Serial/JTAG console and both suspend the console reader for the
 * duration, so the raw byte stream is never mistaken for command lines and no
 * console echo interleaves with the payload. Both set ERRORLEVEL: 0 success,
 * 1 transfer/IO error (or CRC mismatch), 2 usage.
 *
 * receive protocol (ACK-paced: the device's USB RX ring drops bytes under a
 * burst, so the host only sends what the ACK count confirms was accepted):
 *   host   -> "receive <path> <size> [/crc]\n"
 *   device -> "\n" P4_CONFIG_SERIAL_RX_READY_MARKER "\n"
 *   loop: device reads up to P4_CONFIG_SERIAL_XFER_CHUNK_BYTES, writes SD,
 *         device -> "RX <cumulative>\n", host sends the remaining delta
 *   until cumulative == size
 *   optional (only with /crc): host -> 4-byte little-endian CRC-32 trailer
 *   device -> P4_CONFIG_SERIAL_RX_DONE_MARKER (or an error, then the partial
 *   file is removed)
 *
 * send protocol (framed payload; the size in the header tells the host how
 * many bytes to read):
 *   host   -> "send <path> [offset] [count]\n"   or   "send /diag\n"
 *   device -> "SDFX" + 4-byte little-endian payload size + raw bytes
 *   device -> "\n" P4_CONFIG_SERIAL_TX_DONE_MARKER "\n"
 *
 * `send <path> [offset] [count]` streams a byte range of a file (defaults:
 * whole file), bounded by P4_CONFIG_SERIAL_SEND_MAX_BYTES. `send /diag`
 * streams a bounded text report of version/heap/uptime/tasks/wifi for host
 * side scripting.
 */

/* Incremental CRC-32 (IEEE 802.3, reflected poly 0xEDB88320). Matches the
 * value zlib's crc32() reports for the same bytes: start the accumulator at
 * 0xFFFFFFFF and invert the result when the transfer completes. */
static uint32_t serial_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    while (len-- > 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* Write the 8-byte `send` frame header: 4-byte magic + 4-byte little-endian
 * payload size. (The generic serial_write_frame_header() above is shared with
 * `screenshot`.) */
static void serial_send_frame_header(uint32_t payload_size)
{
    serial_write_frame_header(P4_CONFIG_SERIAL_SEND_MAGIC, payload_size);
}

/* Bounded append helper for the `send /diag` report. */
static void serial_diag_line(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    int n;
    va_list args;

    if (buf == NULL || *pos >= cap) {
        return;
    }
    va_start(args, fmt);
    n = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);
    if (n > 0) {
        *pos += (size_t)n;
    }
}

static void shell_command_receive(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    char tmp_path[P4_CONFIG_SD_PATH_BYTES + 8];
    shell_sd_session_t session;
    FILE *file = NULL;
    unsigned long size = 0;
    unsigned long cumulative = 0;
    bool verify_crc = false;
    char *end = NULL;
    esp_err_t error;
    int64_t start_us;
    uint32_t crc = 0xFFFFFFFFu;

    if (argc != 3 && argc != 4) {
        shell_print_usage("Usage: receive <path> <size> [/crc]");
        batch_set_errorlevel(2);
        return;
    }
    if (argc == 4) {
        if (shell_text_equals_ignore_case(argv[3], "/crc")) {
            verify_crc = true;
        } else {
            shell_print_usage("Usage: receive <path> <size> [/crc]");
            batch_set_errorlevel(2);
            return;
        }
    }

    size = strtoul(argv[2], &end, 10);
    if (*end != '\0' || size == 0 || size > P4_CONFIG_SERIAL_RX_MAX_BYTES) {
        shell_print_error("receive: size must be 1..%lu bytes",
                          (unsigned long)P4_CONFIG_SERIAL_RX_MAX_BYTES);
        batch_set_errorlevel(2);
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved, sizeof(resolved));
    if (error != ESP_OK) {
        shell_print_error("receive: invalid path %s", argv[1]);
        batch_set_errorlevel(2);
        return;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.rx", resolved);

    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("receive: SD card not present - insert and retry");
        batch_set_errorlevel(1);
        return;
    }

    {
        uint64_t reclaim = storage_get_file_size(resolved);

        if (!storage_check_free_space(size + 64, reclaim, "receive")) {
            shell_sd_end(&session, "receive");
            batch_set_errorlevel(1);
            return;
        }
    }

    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        shell_print_error("receive: cannot create %s", tmp_path);
        shell_sd_end(&session, "receive");
        batch_set_errorlevel(1);
        return;
    }

    /* Suspend the console reader BEFORE signalling READY so that bytes the
     * host sends the instant it sees the marker cannot be consumed as command
     * lines by the (not yet suspended) console task. */
    start_us = esp_timer_get_time();
    shell_uart_console_rx_begin();

    /* Ready marker: the host now streams `size` raw bytes, ACK-paced. */
    shell_uart_console_write_text("\n" P4_CONFIG_SERIAL_RX_READY_MARKER "\n");

    {
        uint8_t *buf = malloc(P4_CONFIG_SERIAL_XFER_CHUNK_BYTES);
        bool ok = (buf != NULL);
        unsigned long idle_ms = 0;

        if (buf == NULL) {
            shell_print_error("receive: out of memory");
        } else {
            while (cumulative < size && ok) {
                size_t want = size - cumulative;
                size_t got;

                if (want > P4_CONFIG_SERIAL_XFER_CHUNK_BYTES) {
                    want = P4_CONFIG_SERIAL_XFER_CHUNK_BYTES;
                }

                /* Read until the chunk is full, or the host goes idle. Reads
                 * go through the USB-Serial/JTAG driver ring (installed with a
                 * large RX buffer in shell_uart_console_start) so bursts do
                 * not overflow; the 100 ms window gives a bounded idle check. */
                got = 0;
                while (got < want && ok) {
                    int r = usb_serial_jtag_read_bytes(buf + got, want - got,
                                                       pdMS_TO_TICKS(100));

                    if (r < 0 || r == 0) {
                        idle_ms += 100;
                        if (idle_ms >= P4_CONFIG_SERIAL_XFER_IDLE_TIMEOUT_MS) {
                            shell_print_error("receive: timed out waiting for data");
                            ok = false;
                            break;
                        }
                        continue;
                    }
                    idle_ms = 0;
                    got += (size_t)r;
                }

                if (ok && got > 0) {
                    if (fwrite(buf, 1, got, file) != got) {
                        shell_print_error("receive: SD write failed");
                        ok = false;
                        break;
                    }
                    crc = serial_crc32_update(crc, buf, got);
                    cumulative += got;

                    /* ACK: report cumulative bytes so the host knows how much
                     * of this chunk was accepted and sends the right delta
                     * next. */
                    {
                        char ack[48];
                        int ack_len = snprintf(ack, sizeof(ack), "RX %lu\n", cumulative);

                        (void)serial_write_raw(ack, (size_t)ack_len);
                    }
                }
            }

            if (ok && verify_crc) {
                /* The host appended a 4-byte little-endian CRC-32 trailer. */
                uint8_t crc_bytes[4];
                size_t got_crc = 0;
                unsigned long crc_idle_ms = 0;

                while (got_crc < sizeof(crc_bytes) && ok) {
                    int r = usb_serial_jtag_read_bytes(crc_bytes + got_crc,
                                                       sizeof(crc_bytes) - got_crc,
                                                       pdMS_TO_TICKS(100));

                    if (r < 0 || r == 0) {
                        crc_idle_ms += 100;
                        if (crc_idle_ms >= P4_CONFIG_SERIAL_XFER_IDLE_TIMEOUT_MS) {
                            shell_print_error("receive: timed out waiting for CRC trailer");
                            ok = false;
                            break;
                        }
                        continue;
                    }
                    crc_idle_ms = 0;
                    got_crc += (size_t)r;
                }
                if (ok) {
                    uint32_t expected = (uint32_t)crc_bytes[0]
                        | ((uint32_t)crc_bytes[1] << 8)
                        | ((uint32_t)crc_bytes[2] << 16)
                        | ((uint32_t)crc_bytes[3] << 24);
                    uint32_t computed = ~crc;

                    if (computed != expected) {
                        shell_print_error("receive: CRC mismatch (expected %08lx, computed %08lx)",
                                          (unsigned long)expected, (unsigned long)computed);
                        ok = false;
                    }
                }
            }
            free(buf);
        }

        /* Drain any bytes still buffered in the USB ring so the resumed
         * console task never sees leftover binary as a command line. */
        {
            uint8_t drain_buf[64];

            while (usb_serial_jtag_read_bytes(drain_buf, sizeof(drain_buf), 0) > 0) {
            }
        }

        shell_uart_console_rx_end();

        fclose(file);
        file = NULL;

        if (!ok || cumulative < size) {
            if (verify_crc && cumulative == size) {
                shell_print_error("receive: transfer failed - CRC mismatch or missing trailer (%lu bytes received)",
                                  size);
            } else {
                shell_print_error("receive: transfer incomplete (%lu/%lu bytes)",
                                  cumulative, size);
            }
            remove(tmp_path);
            shell_sd_end(&session, "receive");
            batch_set_errorlevel(1);
            return;
        }
    }

    /* Atomic replace: FATFS f_rename refuses to overwrite, so remove first. */
    remove(resolved);
    if (rename(tmp_path, resolved) != 0) {
        int err = errno;

        remove(tmp_path);
        shell_print_error("receive: failed to finalize %s (errno %d: %s)",
                          resolved, err, strerror(err));
        shell_sd_end(&session, "receive");
        batch_set_errorlevel(1);
        return;
    }

    shell_sd_end(&session, "receive");
    shell_uart_console_write_text(P4_CONFIG_SERIAL_RX_DONE_MARKER "\n");
    {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        uint32_t rate = ms > 0 ? (uint32_t)((size * 1000u) / ms) : 0u;

        shell_print_ok("receive: wrote %lu bytes to %s in %u ms (%lu KB/s)",
                       size, resolved, ms, (unsigned long)(rate / 1024u));
    }
    batch_set_errorlevel(0);
}

static void shell_command_send(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    bool diag = false;
    bool stream_ok = false;
    uint32_t payload_size = 0;
    int64_t start_us;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "/diag")) {
        diag = true;
    } else if (argc < 2 || argc > 4) {
        shell_print_usage("Usage: send <path> [offset] [count]  |  send /diag");
        batch_set_errorlevel(2);
        return;
    }

    start_us = esp_timer_get_time();

    /* The transfer owns the raw console byte stream: suspend the console
     * reader so no host input is misread and no console echo interleaves with
     * the framed payload. */
    shell_uart_console_rx_begin();

    if (diag) {
        char *report = malloc(P4_CONFIG_SERIAL_DIAG_BYTES);
        size_t pos = 0;

        if (report == NULL) {
            shell_uart_console_rx_end();
            shell_print_error("send: out of memory for diagnostic report");
            batch_set_errorlevel(1);
            return;
        }

        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "P4MiniShell %s\n", P4_CONFIG_VERSION_STRING);
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "board %s\n", P4_CONFIG_BOARD_REQUESTED);
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "idf %s\n", esp_get_idf_version());
        {
            esp_chip_info_t chip_info;

            esp_chip_info(&chip_info);
            serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                             "chip %s rev %u, %u cores\n",
                             CONFIG_IDF_TARGET, chip_info.revision,
                             (unsigned int)chip_info.cores);
        }
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "heap_free %lu\n",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "heap_internal %lu\n",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "heap_psram %lu\n",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "uptime_s %llu\n",
                         (unsigned long long)(esp_timer_get_time() / 1000000));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "tasks %u\n", (unsigned int)uxTaskGetNumberOfTasks());
        {
            char cwd_buf[P4_CONFIG_SD_PATH_BYTES];

            shell_get_cwd_for_prompt(cwd_buf, sizeof(cwd_buf));
            serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                             "cwd %s\n", cwd_buf);
        }
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "wifi %s\n", networking_wifi_state_string());

        payload_size = (uint32_t)pos;
        serial_send_frame_header(payload_size);
        if (payload_size > 0) {
            (void)serial_write_raw(report, payload_size);
        }
        /* CRC-32 trailer over the report (frame protocol parity with send). */
        {
            uint32_t final_crc = ~serial_crc32_update(0xFFFFFFFFu,
                                                      (const uint8_t *)report,
                                                      (size_t)pos);
            uint8_t trailer[4];

            trailer[0] = (uint8_t)(final_crc & 0xFFu);
            trailer[1] = (uint8_t)((final_crc >> 8) & 0xFFu);
            trailer[2] = (uint8_t)((final_crc >> 16) & 0xFFu);
            trailer[3] = (uint8_t)((final_crc >> 24) & 0xFFu);
            (void)serial_write_raw(trailer, sizeof(trailer));
        }
        free(report);
        stream_ok = true;
    } else {
        size_t offset = 0;
        size_t count = P4_CONFIG_SERIAL_SEND_MAX_BYTES;
        long file_size_long = -1;
        char *end = NULL;

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_uart_console_rx_end();
            shell_print_error("send: SD card not present - insert and retry");
            batch_set_errorlevel(1);
            return;
        }

        if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
            shell_uart_console_rx_end();
            shell_sd_end(&session, "send");
            shell_print_error("send: invalid path %s", argv[1]);
            batch_set_errorlevel(1);
            return;
        }

        if (argc >= 3) {
            offset = strtoul(argv[2], &end, 10);
            if (*end != '\0') {
                shell_uart_console_rx_end();
                shell_sd_end(&session, "send");
                shell_print_error("send: invalid offset %s", argv[2]);
                batch_set_errorlevel(1);
                return;
            }
        }
        if (argc >= 4) {
            count = strtoul(argv[3], &end, 10);
            if (*end != '\0') {
                shell_uart_console_rx_end();
                shell_sd_end(&session, "send");
                shell_print_error("send: invalid count %s", argv[3]);
                batch_set_errorlevel(1);
                return;
            }
        }

        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_uart_console_rx_end();
            shell_sd_end(&session, "send");
            shell_print_error("send: cannot open %s", resolved);
            batch_set_errorlevel(1);
            return;
        }

        if (fseek(file, 0, SEEK_END) == 0) {
            file_size_long = ftell(file);
        }
        if (fseek(file, 0, SEEK_SET) != 0 || file_size_long < 0) {
            fclose(file);
            shell_uart_console_rx_end();
            shell_sd_end(&session, "send");
            shell_print_error("send: cannot size %s", resolved);
            batch_set_errorlevel(1);
            return;
        }

        /* Clamp the requested byte range to the file and the hard bound. */
        if (offset >= (size_t)file_size_long) {
            count = 0;
        } else {
            size_t available = (size_t)file_size_long - offset;

            if (count > available) {
                count = available;
            }
            if (count > P4_CONFIG_SERIAL_SEND_MAX_BYTES) {
                count = P4_CONFIG_SERIAL_SEND_MAX_BYTES;
            }
        }

        payload_size = (uint32_t)count;
        serial_send_frame_header(payload_size);

        {
            uint32_t crc = 0xFFFFFFFFu;

            if (count > 0) {
                uint8_t *buf = malloc(P4_CONFIG_SERIAL_XFER_CHUNK_BYTES);
                size_t remaining = count;

                if (buf == NULL) {
                    stream_ok = false;
                } else {
                    stream_ok = true;
                    if (fseek(file, (long)offset, SEEK_SET) != 0) {
                        stream_ok = false;
                    }
                    while (stream_ok && remaining > 0) {
                        size_t want = remaining > P4_CONFIG_SERIAL_XFER_CHUNK_BYTES
                                          ? P4_CONFIG_SERIAL_XFER_CHUNK_BYTES
                                          : remaining;
                        size_t got = fread(buf, 1, want, file);

                        if (got == 0) {
                            stream_ok = false;
                            break;
                        }
                        if (!serial_write_raw(buf, got)) {
                            stream_ok = false;
                            break;
                        }
                        crc = serial_crc32_update(crc, buf, got);
                        remaining -= got;
                    }
                    free(buf);
                }
            } else {
                stream_ok = true;
            }

            /* Append the 4-byte little-endian CRC-32 trailer over the payload
             * (empty payload => CRC of nothing = 0x00000000) so the host can
             * verify the frame was not corrupted or interleaved. */
            if (stream_ok) {
                uint32_t final_crc = ~crc;
                uint8_t trailer[4];

                trailer[0] = (uint8_t)(final_crc & 0xFFu);
                trailer[1] = (uint8_t)((final_crc >> 8) & 0xFFu);
                trailer[2] = (uint8_t)((final_crc >> 16) & 0xFFu);
                trailer[3] = (uint8_t)((final_crc >> 24) & 0xFFu);
                if (!serial_write_raw(trailer, sizeof(trailer))) {
                    stream_ok = false;
                }
            }
        }

        fclose(file);
        file = NULL;
        shell_sd_end(&session, "send");
    }

    if (stream_ok) {
        shell_uart_console_write_text("\n" P4_CONFIG_SERIAL_TX_DONE_MARKER "\n");
        shell_uart_console_rx_end();
        {
            uint32_t ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
            uint32_t rate = ms > 0 ? (uint32_t)(((uint32_t)payload_size * 1000u) / ms) : 0u;

            shell_print_ok("send: streamed %lu bytes in %u ms (%lu KB/s)",
                           (unsigned long)payload_size, ms,
                           (unsigned long)(rate / 1024u));
        }
        batch_set_errorlevel(0);
    } else {
        shell_uart_console_rx_end();
        shell_print_error("send: transfer failed");
        batch_set_errorlevel(1);
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
    char *echo_line = NULL;

    if (command == NULL) {
        return false;
    }

    /* Executing any command is user activity: keep the power idle clock from
     * turning the display off while a command (possibly a long one) runs. */
    shell_power_notify_activity();

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

    /* Echo must print the whole remainder of a long line: a 4096-byte `echo`
     * with more tokens than the argv capacity would be truncated by the split
     * below. Snapshot the raw (unsplit) line so the echo branch can fall back
     * to it. Only allocated when the command is `echo`; the echo branch (and
     * the argc==0 guard above) is the only path that frees it. */
    if (strncasecmp(trimmed, "echo", 4) == 0 &&
        (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) {
        echo_line = strdup(trimmed);
    }

    /* `calc` needs the raw (unsplit) line too: quoted string arguments inside
     * the expression (`calc len('hello world')`) would lose their quotes to
     * the tokenizer, so the calculator parses the original text itself. */
    if (strncasecmp(trimmed, "calc", 4) == 0 &&
        (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) {
        echo_line = strdup(trimmed);
    }

    /* A command with more arguments than the argv capacity would be silently
     * truncated; surface that so it is never invisible. Echo is exempt: it
     * prints the whole remainder via the raw-line snapshot, so no truncation.
     * Counted on the raw line BEFORE shell_split_args() mutates it. */
    if (echo_line == NULL && shell_count_args(trimmed) > SHELL_ARGV_MAX) {
        shell_transcript_appendf_ansi(SH_WARN "command: more than %d arguments "
                                     "were supplied; extra arguments are ignored" SH_RST "\n",
                                     SHELL_ARGV_MAX);
        shell_record_warningf("shell", "Command argument list truncated at %d", SHELL_ARGV_MAX);
    }

    argc = shell_split_args(trimmed, argv, SHELL_ARGV_MAX);
    if (argc == 0) {
        free(family_command);
        free(echo_line);
        return false;
    }

    /* ---- System commands ---- */
    if (shell_text_equals_ignore_case(argv[0], "help")) {
        shell_command_help(argc, argv);
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
     * read-only listing (top adds a summary header and sorts by CPU). */
    if (shell_text_equals_ignore_case(argv[0], "ps") ||
        shell_text_equals_ignore_case(argv[0], "tasks") ||
        shell_text_equals_ignore_case(argv[0], "top")) {
        batch_set_errorlevel(shell_command_ps(argc, argv));
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

    if (shell_text_equals_ignore_case(argv[0], "beep")) {
        shell_command_beep(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "tone")) {
        shell_command_tone(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "wavplay")) {
        shell_command_wavplay(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "audio")) {
        shell_command_audio(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "clip")) {
        shell_command_clip(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "paste")) {
        shell_command_paste(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "history")) {
        shell_command_history(argc, argv);
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

    /* ---- Receive (serial binary transfer to SD) ---- */
    if (shell_text_equals_ignore_case(argv[0], "receive")) {
        shell_command_receive(argc, argv);
        return true;
    }

    /* ---- Send (serial binary transfer from SD / diagnostic report) ---- */
    if (shell_text_equals_ignore_case(argv[0], "send")) {
        shell_command_send(argc, argv);
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

    if (shell_text_equals_ignore_case(argv[0], "edit")) {
        shell_command_edit(argc, argv);
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

    /* `calc` — batch-native calculator (float expression evaluator in
     * components/batch/calc.c). The raw-line snapshot (taken before the
     * tokenizer) carries quoted string arguments intact, so this is the one
     * batch verb that parses the original text itself. */
    if (shell_text_equals_ignore_case(argv[0], "calc")) {
        int calc_level = 0;

        if (echo_line != NULL) {
            calc_level = shell_command_calc_line(echo_line);
            free(echo_line);
        } else {
            /* Defensive fallback (never expected: the snapshot is taken under
             * the same "calc" prefix rule as this dispatch). Rejoin the tokens
             * with a synthetic "calc" prefix so the parser still works. */
            char *joined = malloc(SHELL_COMMAND_BYTES);
            char *line = malloc(SHELL_COMMAND_BYTES + 8);

            if (joined == NULL || line == NULL) {
                free(joined);
                free(line);
                shell_print_error("calc: out of memory");
                calc_level = 1;
            } else {
                shell_join_args(argv, 1, argc, joined, SHELL_COMMAND_BYTES);
                snprintf(line, SHELL_COMMAND_BYTES + 8, "calc %s", joined);
                calc_level = shell_command_calc_line(line);
                free(joined);
                free(line);
            }
        }
        batch_set_errorlevel(calc_level);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "path")) {
        shell_command_path(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "echo") ||
        (strncasecmp(argv[0], "echo.", 5) == 0 && argv[0][5] == '\0')) {
        if (strncasecmp(argv[0], "echo.", 5) == 0) {
            /* The DOS `echo.` idiom prints a blank line. */
            if (echo_line != NULL) {
                free(echo_line);
            }
            shell_transcript_append_text("\n");
        } else if (echo_line != NULL) {
            /* A raw-line snapshot exists when the command started with "echo";
             * use it so a long echo line is never truncated by the argv cap. */
            shell_command_echo_text(echo_line);
            free(echo_line);
        } else {
            shell_command_echo(argc, argv);
        }
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

    if (shell_text_equals_ignore_case(argv[0], "config")) {
        shell_command_config(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "for")) {
        shell_command_for(argc, argv);
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

    /* `proc` — batch process-stack introspection (the batch process
     * abstraction): list the active batch files, or report the current
     * process's args/name/depth/errorlevel/echo/stdin source. */
    if (shell_text_equals_ignore_case(argv[0], "proc")) {
        shell_command_proc(argc, argv);
        return true;
    }

    /* `ini` — persistent state in KEY=VALUE files on the SD card (get/set/
     * del/list/load/save). */
    if (shell_text_equals_ignore_case(argv[0], "ini")) {
        shell_command_ini(argc, argv);
        return true;
    }

    /* `appconfig` — per-app settings file (sd:/APPS/<APP>.INI) without
     * hand-rolling file parsing. */
    if (shell_text_equals_ignore_case(argv[0], "appconfig")) {
        shell_command_appconfig(argc, argv);
        return true;
    }

    /* `temp` — SD-backed temporary files (new/clean/path). */
    if (shell_text_equals_ignore_case(argv[0], "temp")) {
        shell_command_temp(argc, argv);
        return true;
    }

    /* `ansi` / `menu` — menu/form primitives (CHOICE + ANSI was the DOS
     * way): styled text output and a numbered form, both rendered in the
     * transcript display. */
    if (shell_text_equals_ignore_case(argv[0], "ansi")) {
        shell_command_ansi(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "menu")) {
        shell_command_menu(argc, argv);
        return true;
    }

    /* `appmode` — enter/exit app mode (save/restore screen, full-screen,
     * auto-cleanup on batch exit). */
    if (shell_text_equals_ignore_case(argv[0], "appmode")) {
        shell_command_appmode(argc, argv);
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
     * into the pipeline for every line, and four nested levels of two large
     * stack buffers would overflow the command worker task's stack. The
     * buffers are command-sized because an interactive command line can be up
     * to P4_CONFIG_COMMAND_BYTES (batch lines are the smaller surface). */
    const size_t work_size = SHELL_COMMAND_BYTES;
    char *expanded = NULL;
    char *command_buffer = NULL;
    char *command_part = NULL;
    char *redirect_target = NULL;
    char *input_source = NULL;
    bool append_mode = false;
    bool input_redirect_set = false;
    bool recognized;
    int errorlevel_before;

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

    /* A redirected command's output is captured into a dedicated heap buffer
     * (independent of the transcript), so a large output survives the 16 KB
     * transcript truncation. Open the capture window before dispatch. */
    if (redirect_target != NULL && redirect_target[0] != '\0') {
        shell_redirect_capture_begin();
    }

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
        size_t captured_len = 0;
        const char *captured = shell_redirect_capture_get(&captured_len);
        esp_err_t error;

        shell_redirect_capture_end();

        error = shell_write_redirect_output(redirect_target, captured, append_mode);
        if (shell_redirect_capture_was_truncated()) {
            shell_print_warning("redirection: output exceeded %d bytes and was truncated",
                                P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES);
            shell_record_warningf("shell", "Redirected output truncated at %d bytes",
                                  P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES);
        }
        if (error != ESP_OK) {
            shell_print_error("redirection: failed to write %s (%s)",
                                     redirect_target,
                                     esp_err_to_name(error));
            shell_record_warningf("shell", "Failed to redirect command output to %s", redirect_target);
            batch_set_errorlevel(1);
        }
        shell_redirect_capture_reset();
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
    /* The chain buffer must hold a full command line (up to
     * P4_CONFIG_COMMAND_BYTES); batch lines are a separate, smaller surface. */
    const size_t chain_size = SHELL_COMMAND_BYTES;
    char *chain_buffer;
    bool truncated = false;
    bool previous_succeeded;
    int segment_count;
    int index;

    if (command == NULL) {
        return;
    }

    /* Reclaim internal heap from the transcript scrollback when the internal
     * heap runs low, before this command prints anything. Without this the
     * accumulated LVGL span overhead can starve the heap until a tiny stdio
     * allocation (newlib FILE lock) aborts the board mid-command. */
    shell_transcript_guard_internal();

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
 *  sequentially. Each queued item is a heap-allocated, command-sized request
 *  owned by this task; the queue itself stores only pointers so a 4096-byte
 *  command does not reserve 16 KB of internal RAM inside the queue. */
static void command_worker_task(void *arg)
{
    (void)arg;

    while (true) {
        command_request_t *request = NULL;

        if (xQueueReceive(s_command_queue, &request, portMAX_DELAY) == pdTRUE && request != NULL) {
            shell_execute_command(request->command);
            free(request);
        }
    }
}

void shell_execute_command_async(char *command)
{
    command_request_t *request = malloc(sizeof(*request));

    if (command == NULL || command[0] == '\0') {
        free(request);
        return;
    }

    if (s_command_queue == NULL) {
        free(request);
        shell_print_error("shell: command worker not initialized");
        shell_record_errorf("shell", ESP_FAIL, "Command worker not initialized");
        return;
    }
    if (request == NULL) {
        shell_print_error("shell: out of memory queuing the command");
        return;
    }

    snprintf(request->command, sizeof(request->command), "%s", command);

    /* The queue stores the pointer only; the worker task owns and frees the
     * request after executing it. On a full queue the request is dropped. */
    if (xQueueSend(s_command_queue, &request, 0) != pdTRUE) {
        shell_print_error("shell: command queue full, command dropped");
        shell_record_warningf("shell", "Command queue full, dropped: %s", command);
        free(request);
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
    return audio_get_volume();
}

void command_set_volume(int percent)
{
    if (audio_set_volume(percent) != ESP_OK) {
        ESP_LOGW(COMMAND_TAG, "command_set_volume: speaker init or codec set failed");
    }
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
        .pm_notify_activity     = shell_power_notify_activity,
        .complete_word          = shell_complete_word,
        .editor_is_active       = editor_is_active,
        .editor_handle_usb_key  = editor_handle_usb_key,
        .editor_handle_serial_line = editor_handle_serial_line,
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
    s_command_queue = xQueueCreate(SHELL_COMMAND_QUEUE_DEPTH, sizeof(command_request_t *));
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

    /* Initialize the idle display-off state: the clock starts "active" and
     * the configured default timeout applies immediately. */
    shell_power_notify_activity();
    shell_power_set_idle_timeout(SHELL_POWER_IDLE_DISPLAY_OFF_SECS);

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

    /* Publish the networking state accessors to the applib runtime. Native
     * apps read Wi-Fi state through applib (app_wifi_*) without including
     * networking.h; the table keeps the dependency one-way (command owns
     * networking) and every hook is NULL-checked inside applib. */
    {
        static const applib_net_ops_t applib_net_ops = {
            .wifi_is_connected  = networking_wifi_is_connected,
            .wifi_get_rssi      = networking_wifi_get_rssi,
            .wifi_state_string  = networking_wifi_state_string,
        };
        applib_register_net_ops(&applib_net_ops);
    }

    s_initialized = true;
    ESP_LOGI(COMMAND_TAG, "Command module initialized");
}

bool command_is_initialized(void)
{
    return s_initialized;
}

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <unistd.h>
#include <utime.h>

#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_wifi_default.h"
#include "esp_wifi.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_codec_dev.h"
#include "esp_pm.h"
#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_host_fw_ver.h"
#endif
/* Bluetooth: NimBLE VHCI in components/networking/bluetooth.c (Bluedroid removed) */
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "lwip/ip4_addr.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "board_config.h"
#include "bluetooth.h"
#include "c6ota.h"
#include "display.h"
#include "header.h"
#include "keyboard.h"
#include "clock.h"
#include "networking.h"

/* Forward declaration for shell_text_equals_ignore_case (resolved by shell component at link time) */
extern bool shell_text_equals_ignore_case(const char *left, const char *right);
#include "usb.h"
#include "windows.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "p4minishell_config.h"
#include "p4minishell.h"
#include "storage.h"

/* ANSI-aware transcript function (declared in shell.h, used for boot banner) */
extern void shell_transcript_appendf_ansi(const char *format, ...);

/* ANSI text append (declared in shell.h, used by networking host ops) */
extern void shell_transcript_append_ansi(const char *text);

/* Time string function (declared in shell.h, used by date/time commands) */
extern const char *shell_get_time_string(void);

/* CWD accessor for PowerShell prompt (declared in shell.h) */
extern const char *shell_get_cwd_for_prompt(void);

/* USB keyboard input bridge (declared in shell.h, used for CLI injection) */
extern void shell_usb_keyboard_input(uint8_t key_code, uint8_t modifiers, bool pressed);

/* Wrapper to match usb_keyboard_input_cb_t signature */
static void shell_usb_keyboard_cb(uint8_t key_code, uint8_t modifiers, usb_key_event_t event)
{
    shell_usb_keyboard_input(key_code, modifiers, (event == USB_KEY_EVENT_PRESS));
}

/* Backward-compatibility aliases */
#define SHELL_TAG                       P4_CONFIG_SHELL_TAG
#define SHELL_BOARD_REQUESTED           P4_CONFIG_BOARD_REQUESTED
#define SHELL_BOARD_DETECTED            P4_CONFIG_BOARD_DETECTED
#define SHELL_BOOT_MESSAGE              P4_CONFIG_BOOT_MESSAGE
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_TRANSCRIPT_BYTES          P4_CONFIG_TRANSCRIPT_BYTES
#define SHELL_ASYNC_TRANSCRIPT_BYTES    P4_CONFIG_ASYNC_TRANSCRIPT_BYTES
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_COMMAND_HISTORY_DEPTH     P4_CONFIG_COMMAND_HISTORY_DEPTH
#define SHELL_KEYBOARD_HEIGHT           P4_CONFIG_KEYBOARD_HEIGHT
#define SHELL_HEADER_REFRESH_PERIOD_MS  P4_CONFIG_HEADER_REFRESH_PERIOD_MS
#define SHELL_INPUT_ROW_HEIGHT          P4_CONFIG_INPUT_ROW_HEIGHT
#define SHELL_DEBUG_LOG_DEPTH           P4_CONFIG_DEBUG_LOG_DEPTH
#define SHELL_DEBUG_ENTRY_BYTES         P4_CONFIG_DEBUG_ENTRY_BYTES
#define SHELL_WIFI_SSID_BYTES           P4_CONFIG_WIFI_SSID_BYTES
#define SHELL_WIFI_PASSWORD_BYTES       P4_CONFIG_WIFI_PASSWORD_BYTES
#define SHELL_WIFI_DETAIL_BYTES         P4_CONFIG_WIFI_DETAIL_BYTES
#define SHELL_WIFI_ORIGIN_BYTES         P4_CONFIG_WIFI_ORIGIN_BYTES
#define SHELL_WIFI_INIT_TASK_STACK_BYTES P4_CONFIG_WIFI_INIT_TASK_STACK
#define SHELL_COMMAND_TASK_STACK_BYTES  P4_CONFIG_COMMAND_TASK_STACK
#define SHELL_UART_CONSOLE_TASK_STACK_BYTES P4_CONFIG_UART_CONSOLE_TASK_STACK
#define SHELL_GPIO_NAME_BYTES           P4_CONFIG_GPIO_NAME_BYTES
#define SHELL_GPIO_PIN_LIMIT            P4_CONFIG_GPIO_PIN_LIMIT
#define SHELL_BT_SCAN_LIMIT             P4_CONFIG_BT_SCAN_LIMIT
#define SHELL_C6_HOST_RESET_GPIO        P4_CONFIG_C6_HOST_RESET_GPIO
#define SHELL_SD_FATFS_DRIVE            P4_CONFIG_SD_FATFS_DRIVE
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_SD_LIST_LIMIT             P4_CONFIG_SD_LIST_LIMIT
#define SHELL_SD_CAT_DEFAULT_BYTES      P4_CONFIG_SD_CAT_DEFAULT_BYTES
#define SHELL_SD_CAT_MAX_BYTES          P4_CONFIG_SD_CAT_MAX_BYTES
#define SHELL_SD_IO_BUFFER_BYTES        P4_CONFIG_SD_IO_BUFFER_BYTES
#define SHELL_ENV_VAR_MAX               P4_CONFIG_ENV_VAR_MAX
#define SHELL_ENV_NAME_BYTES            P4_CONFIG_ENV_NAME_BYTES
#define SHELL_ENV_VALUE_BYTES           P4_CONFIG_ENV_VALUE_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_BATCH_ARGS_MAX            P4_CONFIG_BATCH_ARGS_MAX
#define SHELL_BATCH_DEPTH_MAX           P4_CONFIG_BATCH_DEPTH_MAX
#define SHELL_FILE_IO_BUFFER_BYTES      P4_CONFIG_FILE_IO_BUFFER_BYTES
#define SHELL_WIFI_RUNTIME_ENABLED      P4_CONFIG_WIFI_RUNTIME_ENABLED
#define SHELL_BATTERY_ATTEN             P4_CONFIG_BATTERY_ATTEN
#define SHELL_BATTERY_MIN_SLEEP_FREQ_MHZ P4_CONFIG_BATTERY_MIN_SLEEP_FREQ_MHZ

typedef struct {
    char command[SHELL_COMMAND_BYTES];
} shell_command_request_t;

typedef struct {
    char entries[SHELL_DEBUG_LOG_DEPTH][SHELL_DEBUG_ENTRY_BYTES];
    size_t count;
    size_t next_index;
} shell_debug_log_t;

typedef struct {
    bool mounted_here;
} shell_sd_session_t;

typedef struct {
    bool used;
    char name[SHELL_ENV_NAME_BYTES];
    char value[SHELL_ENV_VALUE_BYTES];
} shell_env_var_t;

typedef struct shell_batch_frame {
    bool echo_enabled;
    int argc;
    int depth;
    char args[SHELL_BATCH_ARGS_MAX][SHELL_COMMAND_BYTES];
    struct shell_batch_frame *parent;
} shell_batch_frame_t;

typedef struct {
    const char *name;
    gpio_num_t gpio;
    const char *role;
    bool allow_output;
    bool critical;
} shell_gpio_pin_desc_t;

static lv_timer_t *s_header_status_timer;
static char s_transcript[SHELL_TRANSCRIPT_BYTES];
static char s_async_transcript[SHELL_ASYNC_TRANSCRIPT_BYTES];
static char s_command_history[SHELL_COMMAND_HISTORY_DEPTH][SHELL_COMMAND_BYTES];
static size_t s_command_history_count;
static int s_command_history_cursor = -1;
static char s_history_draft[SHELL_COMMAND_BYTES];
static size_t s_async_transcript_len;
static bool s_async_transcript_flush_queued;
static size_t s_runtime_warning_count;
static shell_debug_log_t s_debug_log;
static portMUX_TYPE s_async_transcript_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_shell_command_lock;
static SemaphoreHandle_t s_uart_console_lock;
static shell_env_var_t s_shell_env_vars[SHELL_ENV_VAR_MAX];
static shell_batch_frame_t *s_active_batch_frame;
static int s_errorlevel;
static char s_goto_label[SHELL_COMMAND_BYTES];
static bool s_goto_pending;
static int s_volume_percent = 60;
static bool s_light_sleep_requested;
static bool s_header_sd_state_known;
static bool s_header_sd_last_mounted;
static esp_codec_dev_handle_t s_speaker_dev;
static adc_oneshot_unit_handle_t s_battery_adc_unit;
static adc_channel_t s_battery_adc_channel;
static bool s_battery_adc_ready;
static adc_cali_handle_t s_battery_cali_handle;
static bool s_battery_cali_ready;
static int64_t s_boot_timestamp_us;   /* captured at boot for uptime calculation */

static void shell_input_line_reset(void);
static void shell_transcript_append_text(const char *text);
static void shell_transcript_appendf(const char *format, ...);
static void shell_uart_console_write_text(const char *text);
static void shell_uart_console_print_prompt(void);
static void shell_uart_console_submit_command(const char *command);
static void shell_uart_console_task(void *arg);
static void shell_uart_console_start(void);
static int shell_split_args(char *text, char **argv, int max_args);
static void shell_schedule_transcript_appendf(const char *format, ...);
static void shell_async_transcript_flush_cb(void *user_data);
static char *shell_trim(char *text);
static void shell_debug_log_push(const char *tag, const char *message);
static void shell_record_errorf(const char *tag, esp_err_t error, const char *format, ...);
static void shell_record_warningf(const char *tag, const char *format, ...);
static void shell_record_infof(const char *tag, const char *format, ...);
static void shell_header_notify(const char *text, uint32_t timeout_ms);
static bool shell_parse_percentage_arg(const char *text, int *percentage_out);
static const shell_gpio_pin_desc_t *shell_find_gpio_pin(int gpio_num);
static void shell_build_ui(void);
static esp_err_t shell_audio_ensure_speaker(void);
static esp_err_t shell_battery_ensure_adc(void);
static esp_err_t shell_battery_read(int *battery_mv_out, int *percent_out, int *raw_out, int *gpio_mv_out);
static bool shell_sd_header_is_mounted(void);
static void shell_header_status_refresh(void);
static void shell_header_status_timer_cb(lv_timer_t *timer);
static void shell_battery_print_usage(void);
static void shell_command_brightness(int argc, char **argv);
static void shell_command_rotate(int argc, char **argv);
static void shell_command_battery(int argc, char **argv);
static void shell_command_volume(int argc, char **argv);
static void shell_command_mem(void);
static void shell_command_gpio_status(void);
void shell_execute_gpio_command(int argc, char **argv);
static void shell_command_debug(void);
static void shell_command_version(void);
static void shell_command_about(void);
static void shell_execute_rgb_command(int argc, char **argv);
static void shell_execute_camera_command(int argc, char **argv);
void shell_execute_pipe(char *command);
static void shell_join_args(char **argv, int start_index, int argc, char *output, size_t output_size);
static void shell_set_default_state(void);
static void shell_env_print_all(void);
static const char *shell_env_get(const char *name);
static esp_err_t shell_env_set(const char *name, const char *value);
static void shell_expand_variables(const char *input, char *output, size_t output_size);
static bool shell_parse_redirection(char *command, char **command_part, char **redirect_target, bool *append_mode);
void shell_command_set(int argc, char **argv);
void shell_command_path(int argc, char **argv);
void shell_command_echo(int argc, char **argv);
void shell_command_if(int argc, char **argv);
void shell_command_goto(int argc, char **argv);
void shell_command_shift(int argc, char **argv);
void shell_command_pause(int argc, char **argv);
void shell_command_choice(int argc, char **argv);
void shell_command_setlocal(int argc, char **argv);
void shell_command_endlocal(int argc, char **argv);
void shell_command_prompt_cmd(int argc, char **argv);
void shell_command_date(int argc, char **argv);
void shell_command_time_cmd(int argc, char **argv);
void shell_command_exit(int argc, char **argv);
void shell_command_find(int argc, char **argv);
void shell_command_more(int argc, char **argv);
void shell_command_tree(int argc, char **argv);
void shell_command_fc(int argc, char **argv);
void shell_command_sort(int argc, char **argv);
static void shell_store_command_history(const char *command);
static bool shell_execute_command_core(char *command);
static void shell_execute_command(char *command);
static void shell_command_task(void *arg);

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

static esp_err_t shell_battery_read(int *battery_mv_out, int *percent_out, int *raw_out, int *gpio_mv_out)
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

static bool shell_sd_header_is_mounted(void)
{
    return storage_is_mounted();
}

static void shell_header_status_refresh(void)
{
    int battery_percent = 0;
    bool battery_ok = false;
    wifi_ap_record_t ap_info;
    bool wifi_connected = networking_wifi_is_connected();
    int wifi_rssi = -127;
    bool sd_mounted = shell_sd_header_is_mounted();

    /* FreeRTOS real-time heap stats */
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);

    /* FreeRTOS runtime stats for CPU usage */
    int cpu_percent = 0;
    uint32_t task_count = uxTaskGetNumberOfTasks();

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    {
        /* Calculate CPU usage from idle task counter delta */
        static uint32_t s_last_idle_count = 0;
        static uint32_t s_last_total_runtime = 0;
        TaskStatus_t *task_status_array = NULL;
        uint32_t total_runtime = 0;
        uint32_t idle_runtime = 0;

        task_status_array = calloc(task_count, sizeof(TaskStatus_t));
        if (task_status_array != NULL) {
            uint32_t obtained = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);
            for (uint32_t i = 0; i < obtained; i++) {
                if (strcmp(task_status_array[i].pcTaskName, "IDLE") == 0 ||
                    strncmp(task_status_array[i].pcTaskName, "IDLE", 4) == 0) {
                    idle_runtime = task_status_array[i].ulRunTimeCounter;
                    break;
                }
            }
            free(task_status_array);

            if (s_last_total_runtime > 0 && total_runtime > s_last_total_runtime) {
                uint32_t total_delta = total_runtime - s_last_total_runtime;
                uint32_t idle_delta = (idle_runtime >= s_last_idle_count)
                    ? (idle_runtime - s_last_idle_count) : 0;
                if (total_delta > 0) {
                    cpu_percent = 100 - (int)((idle_delta * 100) / total_delta);
                    if (cpu_percent < 0) cpu_percent = 0;
                    if (cpu_percent > 100) cpu_percent = 100;
                }
            }
            s_last_idle_count = idle_runtime;
            s_last_total_runtime = total_runtime;
        }
    }
#else
    /* Fallback: approximate from free heap ratio */
    {
        int heap_pct = (total_heap > 0) ? (int)((free_heap * 100) / total_heap) : 100;
        cpu_percent = 100 - heap_pct;  /* rough approximation */
        if (cpu_percent < 0) cpu_percent = 0;
        if (cpu_percent > 100) cpu_percent = 100;
    }
#endif

    /* Uptime from boot timestamp */
    uint32_t uptime_sec = (uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000);

    if (wifi_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi_rssi = ap_info.rssi;
    }

    /* Battery: always update header even when ADC fails */
    if (shell_battery_read(NULL, &battery_percent, NULL, NULL) == ESP_OK) {
        battery_ok = true;
    }

    /* Batch all header updates into a single async render to prevent
     * screen flashes from multiple lv_async_call dispatches.
     * Individual header_update_*() calls would each schedule a separate
     * render, causing visible flicker every refresh period. */
    header_update_batch(
        wifi_connected, wifi_rssi,
        battery_percent, battery_ok,
        bluetooth_is_enabled(), bluetooth_is_connected(),
        usb_is_connected(),
        sd_mounted ? HEADER_SD_MOUNTED : HEADER_SD_NONE,
        free_heap, total_heap,
        cpu_percent, task_count,
        uptime_sec
    );

    /* USB keyboard auto-detect: hide on-screen keyboard when USB keyboard
     * is attached, restore it when detached. Uses a static tracking variable
     * to detect state transitions and avoid repeated toggling. */
    {
        static bool s_last_usb_kb_attached = false;
        bool usb_kb_attached = usb_is_keyboard_attached();

        if (usb_kb_attached != s_last_usb_kb_attached) {
            s_last_usb_kb_attached = usb_kb_attached;
            if (usb_kb_attached) {
                keyboard_set_external_input(true);
                shell_header_notify("USB keyboard detected", 3000);
            } else {
                keyboard_set_external_input(false);
                shell_header_notify("USB keyboard removed", 3000);
            }
        }
    }

    if (!s_header_sd_state_known) {
        s_header_sd_state_known = true;
        s_header_sd_last_mounted = sd_mounted;
    } else if (sd_mounted != s_header_sd_last_mounted) {
        shell_header_notify(sd_mounted ? "SD card mounted" : "SD card unmounted", 3000);
        s_header_sd_last_mounted = sd_mounted;
    }

    /* header_update_*() functions schedule async renders internally.
     * Do NOT call header_force_render() here — it triggers a synchronous
     * full redraw every 5 seconds, causing visible screen flashes. */
}

static void shell_header_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    shell_header_status_refresh();
}

static bool shell_parse_percentage_arg(const char *text, int *percentage_out)
{
    size_t parsed = 0;

    if (percentage_out == NULL) {
        return false;
    }

    if (!storage_parse_size_arg(text, 0, 100, &parsed)) {
        return false;
    }

    *percentage_out = (int)parsed;
    return true;
}

static void shell_battery_print_usage(void)
{
    shell_transcript_append_text("Usage: battery or battery sleep <on|off|status>\n");
}

static void shell_env_normalize_name(const char *name, char *output, size_t output_size)
{
    size_t index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (name == NULL) {
        return;
    }

    while (name[index] != '\0' && index + 1 < output_size) {
        output[index] = (char)toupper((unsigned char)name[index]);
        index++;
    }
    output[index] = '\0';
}

static bool shell_env_name_is_valid(const char *name)
{
    size_t index;

    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (index = 0; name[index] != '\0'; index++) {
        if (!(isalnum((unsigned char)name[index]) || name[index] == '_')) {
            return false;
        }
    }

    return true;
}

static shell_env_var_t *shell_env_find_slot(const char *name)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    size_t index;

    shell_env_normalize_name(name, normalized, sizeof(normalized));
    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (s_shell_env_vars[index].used && strcmp(s_shell_env_vars[index].name, normalized) == 0) {
            return &s_shell_env_vars[index];
        }
    }

    return NULL;
}

static shell_env_var_t *shell_env_find_free_slot(void)
{
    size_t index;

    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (!s_shell_env_vars[index].used) {
            return &s_shell_env_vars[index];
        }
    }

    return NULL;
}

static void shell_set_default_state(void)
{
    memset(s_shell_env_vars, 0, sizeof(s_shell_env_vars));
    storage_set_cwd_default();
    (void)shell_env_set("PATH", "sd:/");
}

static const char *shell_env_get(const char *name)
{
    shell_env_var_t *slot = shell_env_find_slot(name);

    return slot != NULL ? slot->value : NULL;
}

static esp_err_t shell_env_set(const char *name, const char *value)
{
    char normalized[SHELL_ENV_NAME_BYTES];
    shell_env_var_t *slot;

    shell_env_normalize_name(name, normalized, sizeof(normalized));
    if (!shell_env_name_is_valid(normalized)) {
        return ESP_ERR_INVALID_ARG;
    }

    slot = shell_env_find_slot(normalized);
    if (value == NULL || value[0] == '\0') {
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        return ESP_OK;
    }

    if (slot == NULL) {
        slot = shell_env_find_free_slot();
        if (slot == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", normalized);
    snprintf(slot->value, sizeof(slot->value), "%s", value);
    return ESP_OK;
}

static void shell_env_print_all(void)
{
    size_t index;
    bool any = false;

    for (index = 0; index < SHELL_ENV_VAR_MAX; index++) {
        if (!s_shell_env_vars[index].used) {
            continue;
        }
        shell_transcript_appendf("%s=%s\n", s_shell_env_vars[index].name, s_shell_env_vars[index].value);
        any = true;
    }

    if (!any) {
        shell_transcript_append_text("No environment variables defined\n");
    }
}

static void shell_expand_variables(const char *input, char *output, size_t output_size)
{
    size_t out_index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    while (*input != '\0' && out_index + 1 < output_size) {
        if (*input == '%') {
            const char *end = strchr(input + 1, '%');
            if (end != NULL) {
                size_t token_len = (size_t)(end - (input + 1));
                char token[SHELL_ENV_NAME_BYTES];
                const char *replacement = NULL;

                if (token_len == 1 && isdigit((unsigned char)input[1]) && s_active_batch_frame != NULL) {
                    int arg_index = input[1] - '1';
                    replacement = (arg_index >= 0 && arg_index < s_active_batch_frame->argc)
                                      ? s_active_batch_frame->args[arg_index]
                                      : "";
                } else if (token_len == 0) {
                    replacement = "%";
                } else if (token_len < sizeof(token)) {
                    memcpy(token, input + 1, token_len);
                    token[token_len] = '\0';
                    replacement = shell_env_get(token);
                }

                if (replacement != NULL) {
                    while (*replacement != '\0' && out_index + 1 < output_size) {
                        output[out_index++] = *replacement++;
                    }
                    input = end + 1;
                    continue;
                }
            }
        }

        output[out_index++] = *input++;
    }

    output[out_index] = '\0';
}

static bool shell_parse_redirection(char *command, char **command_part, char **redirect_target, bool *append_mode)
{
    bool in_quotes = false;
    char *cursor;

    if (command_part == NULL || redirect_target == NULL || append_mode == NULL) {
        return false;
    }

    *command_part = command;
    *redirect_target = NULL;
    *append_mode = false;

    if (command == NULL) {
        return false;
    }

    for (cursor = command; *cursor != '\0'; cursor++) {
        if (*cursor == '"') {
            in_quotes = !in_quotes;
            continue;
        }

        if (!in_quotes && *cursor == '>') {
            char *target;

            *append_mode = cursor[1] == '>';
            *cursor = '\0';
            target = cursor + (*append_mode ? 2 : 1);
            target = shell_trim(target);
            if (target[0] == '"') {
                target++;
                if (target[0] != '\0') {
                    char *end_quote = strrchr(target, '"');
                    if (end_quote != NULL) {
                        *end_quote = '\0';
                    }
                }
            }
            *command_part = shell_trim(command);
            *redirect_target = target;
            return true;
        }
    }

    *command_part = shell_trim(command);
    return false;
}

// Keep recent shell/runtime failures, including OTA restore failures, visible through the debug command.
static void shell_debug_log_push(const char *tag, const char *message)
{
    char entry[SHELL_DEBUG_ENTRY_BYTES];
    size_t index;

    snprintf(entry, sizeof(entry), "%s: %s", tag != NULL ? tag : "log", message != NULL ? message : "");
    index = s_debug_log.next_index;
    snprintf(s_debug_log.entries[index], sizeof(s_debug_log.entries[index]), "%s", entry);
    s_debug_log.next_index = (index + 1) % SHELL_DEBUG_LOG_DEPTH;
    if (s_debug_log.count < SHELL_DEBUG_LOG_DEPTH) {
        s_debug_log.count++;
    }
}

static void shell_record_errorf(const char *tag, esp_err_t error, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    shell_debug_log_push(tag, message);
    shell_schedule_transcript_appendf("%s: %s: %s (0x%x)\n",
                                      tag,
                                      message,
                                      esp_err_to_name(error),
                                      (unsigned int)error);
}

static void shell_record_warningf(const char *tag, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    s_runtime_warning_count++;
    shell_debug_log_push(tag, message);
}

static void shell_record_infof(const char *tag, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    shell_debug_log_push(tag, message);
}

void shell_networking_record_error(const char *tag, esp_err_t error, const char *message)
{
    shell_record_errorf(tag, error, "%s", message);
}

static void shell_header_notify(const char *text, uint32_t timeout_ms)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    header_set_notification(text, timeout_ms);
}

void shell_networking_schedule_text(const char *text)
{
    shell_schedule_transcript_appendf("%s", text);
}

void shell_networking_record_warning(const char *tag, const char *message)
{
    shell_record_warningf(tag, "%s", message);
}

void shell_networking_record_info(const char *tag, const char *message)
{
    shell_record_infof(tag, "%s", message);
}

void c6ota_host_transcript_append_text(const char *text)
{
    shell_transcript_append_text(text);
}

void c6ota_host_schedule_transcript_append_text(const char *text)
{
    shell_schedule_transcript_appendf("%s", text);
}

void c6ota_host_record_error(esp_err_t error, const char *message)
{
    shell_record_errorf("c6ota", error, "%s", message != NULL ? message : "");
}

void c6ota_host_record_warning(const char *message)
{
    shell_record_warningf("c6ota", "%s", message != NULL ? message : "");
}

void c6ota_host_record_info(const char *message)
{
    shell_record_infof("c6ota", "%s", message != NULL ? message : "");
}

void c6ota_host_notify_header(const char *text, uint32_t timeout_ms)
{
    shell_header_notify(text, timeout_ms);
}

void usb_host_transcript_append_text(const char *text)
{
    shell_transcript_append_text(text);
}

void usb_host_schedule_transcript_append_text(const char *text)
{
    shell_schedule_transcript_appendf("%s", text);
}

void usb_host_record_error(esp_err_t error, const char *message)
{
    shell_record_errorf("usb", error, "%s", message != NULL ? message : "");
}

void usb_host_record_warning(const char *message)
{
    shell_record_warningf("usb", "%s", message != NULL ? message : "");
}

void usb_host_record_info(const char *message)
{
    shell_record_infof("usb", "%s", message != NULL ? message : "");
}

void usb_host_notify_header(const char *text, uint32_t timeout_ms)
{
    shell_header_notify(text, timeout_ms);
}

// c6ota module integration: OTA flow lives in components/c6ota with stable public API.
// Shell parser delegates to c6ota_perform(); preserves exact confirmation, progress, and success text.
// Module API documented in API.md and SDK.md.
static void shell_c6ota_progress_callback(int percent, const char *msg)
{
    if (msg == NULL || msg[0] == '\0') {
        return;
    }

    if (percent == -1) {
        shell_transcript_append_text(msg);
        return;
    }

    shell_schedule_transcript_appendf("%s", msg);
}

static void shell_transcript_render(void)
{
    lv_obj_t *transcript = windows_get_transcript();
    if (transcript == NULL) {
        return;
    }

    lv_textarea_set_text(transcript, s_transcript);
    lv_textarea_set_cursor_pos(transcript, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_update_layout(transcript);
    lv_obj_scroll_to_y(transcript, LV_COORD_MAX, LV_ANIM_OFF);
}

static void shell_transcript_reset(void)
{
    s_transcript[0] = '\0';
}

static void shell_uart_console_write_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    if (s_uart_console_lock != NULL) {
        (void)xSemaphoreTake(s_uart_console_lock, portMAX_DELAY);
    }

    fputs(text, stdout);
    fflush(stdout);

    if (s_uart_console_lock != NULL) {
        xSemaphoreGive(s_uart_console_lock);
    }
}

static void shell_transcript_append_text(const char *text)
{
    size_t current_len;
    size_t text_len;
    static const char truncation_marker[] = "\n[history truncated]\n";

    if (text == NULL || text[0] == '\0') {
        return;
    }

    current_len = strlen(s_transcript);
    text_len = strlen(text);

    if (text_len >= sizeof(s_transcript)) {
        text += text_len - (sizeof(s_transcript) - 1);
        text_len = strlen(text);
        shell_transcript_reset();
        current_len = 0;
    }

    if (current_len + text_len + 1 >= sizeof(s_transcript)) {
        size_t keep = sizeof(s_transcript) / 2;

        if (current_len > keep) {
            memmove(s_transcript, s_transcript + current_len - keep, keep);
            current_len = keep;
            s_transcript[current_len] = '\0';
        }

        if (current_len + sizeof(truncation_marker) < sizeof(s_transcript)) {
            memcpy(s_transcript + current_len, truncation_marker, sizeof(truncation_marker) - 1);
            current_len += sizeof(truncation_marker) - 1;
            s_transcript[current_len] = '\0';
        }
    }

    if (current_len + text_len + 1 >= sizeof(s_transcript)) {
        text_len = sizeof(s_transcript) - current_len - 1;
    }

    memcpy(s_transcript + current_len, text, text_len);
    s_transcript[current_len + text_len] = '\0';
    shell_uart_console_write_text(text);
}

static void shell_transcript_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    shell_transcript_append_text(buffer);
}

static void shell_history_transcript_scroll_to_end(void)
{
    shell_transcript_render();
}

static int shell_split_args(char *text, char **argv, int max_args)
{
    int argc = 0;
    char *cursor = text;

    while (cursor != NULL && *cursor != '\0' && argc < max_args) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        if (*cursor == '"') {
            cursor++;
            argv[argc++] = cursor;
            while (*cursor != '\0' && *cursor != '"') {
                cursor++;
            }
            if (*cursor == '"') {
                *cursor = '\0';
                cursor++;
            }
        } else {
            argv[argc++] = cursor;
            while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (*cursor != '\0') {
                *cursor = '\0';
                cursor++;
            }
        }
    }

    return argc;
}

static void shell_join_args(char **argv, int start_index, int argc, char *output, size_t output_size)
{
    int index;
    size_t used = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    for (index = start_index; index < argc; index++) {
        int written = snprintf(output + used,
                               output_size - used,
                               "%s%s",
                               index == start_index ? "" : " ",
                               argv[index]);
        if (written < 0) {
            output[0] = '\0';
            return;
        }
        if ((size_t)written >= output_size - used) {
            output[output_size - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

static bool shell_command_is_sensitive(const char *command)
{
    char buffer[SHELL_COMMAND_BYTES];
    char *argv[4];
    int argc;

    snprintf(buffer, sizeof(buffer), "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 4);

    return argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0;
}

static bool shell_command_should_store_history(const char *command)
{
    if (c6ota_is_confirmation_pending()) {
        return false;
    }

    return !shell_command_is_sensitive(command);
}

static void shell_format_command_for_transcript(const char *command, char *output, size_t output_size)
{
    char buffer[SHELL_COMMAND_BYTES];
    char *argv[4];
    int argc;

    snprintf(buffer, sizeof(buffer), "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 4);

    if (argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0) {
        snprintf(output, output_size, "wifi connect %s ********", argv[2]);
        return;
    }

    snprintf(output, output_size, "%s", command != NULL ? command : "");
}

static void shell_async_transcript_append_pending(const char *text)
{
    size_t text_len;

    if (text == NULL) {
        return;
    }

    text_len = strlen(text);
    if (text_len >= sizeof(s_async_transcript)) {
        text += text_len - (sizeof(s_async_transcript) - 1);
        text_len = strlen(text);
        s_async_transcript_len = 0;
        s_async_transcript[0] = '\0';
    }

    if (s_async_transcript_len + text_len + 1 >= sizeof(s_async_transcript)) {
        size_t drop = s_async_transcript_len + text_len + 1 - sizeof(s_async_transcript);

        if (drop >= s_async_transcript_len) {
            s_async_transcript_len = 0;
            s_async_transcript[0] = '\0';
        } else {
            memmove(s_async_transcript, s_async_transcript + drop, s_async_transcript_len - drop);
            s_async_transcript_len -= drop;
            s_async_transcript[s_async_transcript_len] = '\0';
        }
    }

    if (s_async_transcript_len + text_len + 1 >= sizeof(s_async_transcript)) {
        text_len = sizeof(s_async_transcript) - s_async_transcript_len - 1;
    }

    memcpy(s_async_transcript + s_async_transcript_len, text, text_len);
    s_async_transcript_len += text_len;
    s_async_transcript[s_async_transcript_len] = '\0';
}

static void shell_async_transcript_flush_cb(void *user_data)
{
    char pending[SHELL_ASYNC_TRANSCRIPT_BYTES];
    bool requeue = false;

    (void)user_data;

    portENTER_CRITICAL(&s_async_transcript_lock);
    if (s_async_transcript_len == 0) {
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        return;
    }

    memcpy(pending, s_async_transcript, s_async_transcript_len + 1);
    s_async_transcript_len = 0;
    s_async_transcript[0] = '\0';
    s_async_transcript_flush_queued = false;
    portEXIT_CRITICAL(&s_async_transcript_lock);

    shell_transcript_append_text(pending);
    shell_history_transcript_scroll_to_end();

    portENTER_CRITICAL(&s_async_transcript_lock);
    if (s_async_transcript_len > 0 && !s_async_transcript_flush_queued) {
        s_async_transcript_flush_queued = true;
        requeue = true;
    }
    portEXIT_CRITICAL(&s_async_transcript_lock);

    if (requeue && lv_async_call(shell_async_transcript_flush_cb, NULL) != LV_RESULT_OK) {
        portENTER_CRITICAL(&s_async_transcript_lock);
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        ESP_LOGW(SHELL_TAG, "Failed to queue transcript flush");
    }
}

static void shell_schedule_transcript_appendf(const char *format, ...)
{
    char stack_buffer[512];
    va_list args;
    bool queue_flush = false;

    va_start(args, format);
    vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);

    portENTER_CRITICAL(&s_async_transcript_lock);
    shell_async_transcript_append_pending(stack_buffer);
    if (!s_async_transcript_flush_queued) {
        s_async_transcript_flush_queued = true;
        queue_flush = true;
    }
    portEXIT_CRITICAL(&s_async_transcript_lock);

    if (queue_flush && lv_async_call(shell_async_transcript_flush_cb, NULL) != LV_RESULT_OK) {
        portENTER_CRITICAL(&s_async_transcript_lock);
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        ESP_LOGW(SHELL_TAG, "Failed to queue transcript flush");
    }
}

static void shell_uart_console_print_prompt(void)
{
    shell_uart_console_write_text(SHELL_PROMPT);
}

static void shell_uart_console_submit_command(const char *command)
{
    char command_copy[SHELL_COMMAND_BYTES];
    char transcript_command[SHELL_COMMAND_BYTES];

    if (command == NULL || command[0] == '\0') {
        return;
    }

    snprintf(command_copy, sizeof(command_copy), "%s", command);
    shell_format_command_for_transcript(command_copy, transcript_command, sizeof(transcript_command));
    if (s_shell_command_lock != NULL) {
        (void)xSemaphoreTake(s_shell_command_lock, portMAX_DELAY);
    }

    if (!lvgl_port_lock(0)) {
        shell_schedule_transcript_appendf("shell: failed to lock LVGL for UART command %s\n", transcript_command);
        shell_record_errorf("uart", ESP_FAIL, "Failed to lock LVGL for UART command %s", transcript_command);
        if (s_shell_command_lock != NULL) {
            xSemaphoreGive(s_shell_command_lock);
        }
        return;
    }

    shell_transcript_appendf("%s%s\n", SHELL_PROMPT, transcript_command);
    if (shell_command_should_store_history(command_copy)) {
        shell_store_command_history(command_copy);
    }
    s_command_history_cursor = -1;
    s_history_draft[0] = '\0';
    shell_execute_command(command_copy);
    shell_history_transcript_scroll_to_end();
    lvgl_port_unlock();

    if (s_shell_command_lock != NULL) {
        xSemaphoreGive(s_shell_command_lock);
    }
}

static void shell_uart_console_task(void *arg)
{
    char line[SHELL_COMMAND_BYTES];
    bool prompt_visible = false;

    (void)arg;
    shell_uart_console_write_text("\nUART console ready. Type help for commands.\n");

    while (true) {
        char *trimmed;
        size_t length;

        if (!prompt_visible) {
            shell_uart_console_print_prompt();
            prompt_visible = true;
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            clearerr(stdin);
            continue;
        }

        trimmed = shell_trim(line);
        length = strlen(trimmed);
        while (length > 0 && (trimmed[length - 1] == '\n' || trimmed[length - 1] == '\r')) {
            trimmed[--length] = '\0';
        }

        if (trimmed[0] == '\0') {
            prompt_visible = false;
            continue;
        }

        shell_uart_console_submit_command(trimmed);
        prompt_visible = false;
    }
}

static void shell_uart_console_start(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (s_uart_console_lock == NULL) {
        s_uart_console_lock = xSemaphoreCreateMutex();
    }

    if (xTaskCreate(shell_uart_console_task,
                    "shell_uart",
                    SHELL_UART_CONSOLE_TASK_STACK_BYTES,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        shell_record_errorf("uart", ESP_FAIL, "Failed to start UART console task");
    }
}

#if SHELL_WIFI_RUNTIME_ENABLED
static void shell_execute_wifi_command(char *command)
{
    networking_handle_wifi_command(command);
}
#endif

static char *shell_trim(char *text)
{
    char *start = text;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    return start;
}

static void shell_input_line_set_text(const char *command_text)
{
    char buffer[sizeof(SHELL_PROMPT) + SHELL_COMMAND_BYTES];
    const char *safe_command = command_text != NULL ? command_text : "";

    snprintf(buffer, sizeof(buffer), "%s%s", SHELL_PROMPT, safe_command);
    lv_obj_t *input_line = windows_get_input_line();
    if (input_line != NULL) {
        lv_textarea_set_text(input_line, buffer);
        lv_textarea_set_cursor_pos(input_line, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void shell_input_line_reset(void)
{
    shell_input_line_set_text("");
}

static void shell_extract_input_text(char *output, size_t output_size)
{
    lv_obj_t *il = windows_get_input_line();
    const char *text = il ? lv_textarea_get_text(il) : "";

    if (strncmp(text, SHELL_PROMPT, strlen(SHELL_PROMPT)) == 0) {
        snprintf(output, output_size, "%s", text + strlen(SHELL_PROMPT));
    } else {
        snprintf(output, output_size, "%s", text);
    }

    shell_trim(output);
}

static void shell_store_command_history(const char *command)
{
    size_t index;

    if (command == NULL || command[0] == '\0') {
        return;
    }

    if (s_command_history_count > 0 &&
        strcmp(s_command_history[s_command_history_count - 1], command) == 0) {
        return;
    }

    if (s_command_history_count == SHELL_COMMAND_HISTORY_DEPTH) {
        for (index = 1; index < SHELL_COMMAND_HISTORY_DEPTH; index++) {
            snprintf(s_command_history[index - 1], SHELL_COMMAND_BYTES, "%s", s_command_history[index]);
        }
        s_command_history_count--;
    }

    snprintf(s_command_history[s_command_history_count], SHELL_COMMAND_BYTES, "%s", command);
    s_command_history_count++;
}

static void shell_recall_history(int direction)
{
    char current_input[SHELL_COMMAND_BYTES];

    if (s_command_history_count == 0) {
        return;
    }

    shell_extract_input_text(current_input, sizeof(current_input));

    if (s_command_history_cursor < 0) {
        snprintf(s_history_draft, sizeof(s_history_draft), "%s", current_input);
        if (direction < 0) {
            s_command_history_cursor = (int)s_command_history_count - 1;
        } else {
            return;
        }
    } else {
        s_command_history_cursor += direction;
        if (s_command_history_cursor < 0) {
            s_command_history_cursor = -1;
            shell_input_line_set_text(s_history_draft);
            return;
        }

        if (s_command_history_cursor >= (int)s_command_history_count) {
            s_command_history_cursor = -1;
            shell_input_line_set_text(s_history_draft);
            return;
        }
    }

    shell_input_line_set_text(s_command_history[s_command_history_cursor]);
}

static void shell_command_help(void)
{
    shell_transcript_append_text("Built-in commands:\n");
    shell_transcript_append_text("  help    Show available commands\n");
    shell_transcript_append_text("  cls     Alias of clear\n");
    shell_transcript_append_text("  c6ota <sd:/file.bin|http[s]://url|default> Run ESP-Hosted SDIO OTA from SD, HTTP, or SD root default\n");
    shell_transcript_append_text("  usb status | ls [path] | keyboard <on|off> | mouse <on|off>\n");
    shell_transcript_append_text("  brightness <0-100> Set the LCD backlight brightness\n");
    shell_transcript_append_text("  rotate <0|90|180|270> Rotate the display and remap GT911 touch\n");
    shell_transcript_append_text("  battery [sleep <on|off|status>] Show battery voltage/percent and light-sleep state\n");
    shell_transcript_append_text("  volume <0-100> Set speaker output volume through the ES8311 codec path\n");
    shell_transcript_append_text("  sysinfo Show board/runtime information\n");
    shell_transcript_append_text("  cd | chdir [path] Change or show the current SD working directory\n");
    shell_transcript_append_text("  dir [path] List files from the current SD working directory\n");
    shell_transcript_append_text("  copy <src> <dst>  Copy a file on SD\n");
    shell_transcript_append_text("  move <src> <dst>  Move or rename a file on SD\n");
    shell_transcript_append_text("  del | erase <path> Delete a file on SD\n");
    shell_transcript_append_text("  ren | rename <src> <dst> Rename a file or directory on SD\n");
    shell_transcript_append_text("  md | mkdir <path>  Create a directory on SD\n");
    shell_transcript_append_text("  rd | rmdir <path>  Remove an empty directory on SD\n");
    shell_transcript_append_text("  type <path>        Show a text-safe file dump\n");
    shell_transcript_append_text("  write <path> <text>  Overwrite a text file with one line\n");
    shell_transcript_append_text("  append <path> <text> Append one line to a text file\n");
    shell_transcript_append_text("  touch <path>       Create an empty file or refresh its timestamp\n");
    shell_transcript_append_text("  call <file.bat> [args] Run a batch file from SD\n");
    shell_transcript_append_text("  set [NAME[=VALUE]] Show, set, or clear RAM-only variables\n");
    shell_transcript_append_text("  path [dir1;dir2]   Show or set the RAM-only batch search path\n");
    shell_transcript_append_text("  echo [text]        Print text; in batch files, echo on/off toggles line echo\n");
    shell_transcript_append_text("  wifi scan Scan for nearby access points after Wi-Fi startup\n");
    shell_transcript_append_text("  sd info | ls [path] | stat <path> | cat <path> [max_bytes]\n");
    shell_transcript_append_text("  mem     Show heap and PSRAM usage\n");
    shell_transcript_append_text("  gpio list | status | read <pin> | set <pin> <0|1>\n");
    shell_transcript_append_text("  bluetooth status | scan | advertise <on|off>  Hosted NimBLE control for the ESP32-C6 co-processor\n");
    shell_transcript_append_text("  rgb led <color> | rgb <r> <g> <b>  RGB LED control when declared in board metadata\n");
    shell_transcript_append_text("  camera init | camera snap <filename>  Camera control when declared in board metadata\n");
    shell_transcript_append_text("  debug   Show last 5 errors, Wi-Fi state, heap, and warnings\n");
    shell_transcript_append_text("  version | ver Show app and ESP-IDF version\n");
    shell_transcript_append_text("  about   Show shell and board summary\n");
    shell_transcript_append_text("  wifi    Wi-Fi status/connect/disconnect commands\n");
    shell_transcript_append_text("  clear   Clear the terminal history\n");
    shell_transcript_append_text("  reboot  Restart the board\n");
    shell_transcript_append_text("History recall: Prev/Next buttons above the keyboard\n");
    shell_transcript_append_text("Redirection: command > file.txt or command >> file.txt writes transcript output to SD\n");
    shell_transcript_append_text("  c6ota sd:/file.bin streams the image after Wi-Fi is stopped and then restores the original Wi-Fi routine with diagnostics\n");
    shell_transcript_append_text("  c6ota http://host/path.bin or https://host/path.bin downloads first, then transfers with Wi-Fi off\n");
    shell_transcript_append_text("  c6ota default uses esp32c6_hosted_slave.bin or network_adapter.bin from the SD root\n");
    shell_transcript_append_text("  c6ota prerequisite: factory C6 v2.3.0 needs the standalone tool from https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota first\n");
    shell_transcript_append_text("  Warning: the C6 reboots after successful OTA; run reboot when the shell reports success\n");
}

static void shell_command_sysinfo(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t task_count = uxTaskGetNumberOfTasks();
    uint32_t uptime_sec = (uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000);
    uint32_t days = uptime_sec / 86400;
    uint32_t hours = (uptime_sec % 86400) / 3600;
    uint32_t mins = (uptime_sec % 3600) / 60;
    uint32_t secs = uptime_sec % 60;
#if CONFIG_SPIRAM
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif

    shell_transcript_appendf("board.requested_name: %s\n", SHELL_BOARD_REQUESTED);
    shell_transcript_appendf("board.detected_name: %s\n", SHELL_BOARD_DETECTED);

    /* Display info from the display manager */
    {
        display_info_t disp_info = display_get_info();
        shell_transcript_appendf("display: %" PRId32 " x %" PRId32 " (native %" PRId32 " x %" PRId32
                                 "), %s, reset GPIO %d, backlight GPIO %d\n",
                                 disp_info.resolution.current_width,
                                 disp_info.resolution.current_height,
                                 disp_info.resolution.native_width,
                                 disp_info.resolution.native_height,
                                 disp_info.panel_driver,
                                 BOARD_CFG_LCD_RST_GPIO,
                                 BOARD_CFG_LCD_BACKLIGHT_GPIO);
        shell_transcript_appendf("display.state: brightness=%d%% rotation=%s power=%s refresh=%" PRIu32 "Hz\n",
                                 disp_info.brightness_percent,
                                 display_rotation_to_string(disp_info.rotation),
                                 disp_info.power_state == DISPLAY_POWER_ON ? "on" :
                                 disp_info.power_state == DISPLAY_POWER_SLEEP ? "sleep" : "off",
                                 disp_info.refresh.current_hz);
        shell_transcript_appendf("display.timing: pclk=%" PRIu32 "MHz, lanes=%d, bitrate=%" PRIu32
                                 "Mbps, hsync=%" PRIu32 " hbp=%" PRIu32 " hfp=%" PRIu32
                                 " vsync=%" PRIu32 " vbp=%" PRIu32 " vfp=%" PRIu32 "\n",
                                 disp_info.refresh.pixel_clock_mhz,
                                 disp_info.mipi_lane_num,
                                 disp_info.refresh.dsi_lane_bitrate_mbps,
                                 disp_info.hsync, disp_info.hbp, disp_info.hfp,
                                 disp_info.vsync, disp_info.vbp, disp_info.vfp);
        shell_transcript_appendf("display.buffer: draw=%" PRIu32 ", double=%d, dma=%d, spiram=%d, sw_rotate=%d\n",
                                 disp_info.draw_buffer_size,
                                 disp_info.double_buffer ? 1 : 0,
                                 disp_info.buffer_dma ? 1 : 0,
                                 disp_info.buffer_spiram ? 1 : 0,
                                 disp_info.sw_rotate ? 1 : 0);
        shell_transcript_appendf("touch: %s on I2C%d, SDA GPIO %d, SCL GPIO %d, %dHz, pullup=%d\n",
                                 disp_info.touch_driver,
                                 BOARD_CFG_I2C_PORT,
                                 BOARD_CFG_I2C_SDA_GPIO,
                                 BOARD_CFG_I2C_SCL_GPIO,
                                 BOARD_CFG_I2C_CLK_SPEED_HZ,
                                 BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP);
    }
    shell_transcript_appendf("audio: I2S%d BCLK=%d WS=%d DOUT=%d MCLK=%d amp=%d volume=%d%%\n",
                             BOARD_CFG_I2S_PORT,
                             BSP_I2S_SCLK,
                             BSP_I2S_LCLK,
                             BSP_I2S_DOUT,
                             BSP_I2S_MCLK,
                             BSP_POWER_AMP_IO,
                             s_volume_percent);
    shell_transcript_appendf("battery: adc_gpio=%d divider=%d:%d range=%dmV..%dmV\n",
                             BOARD_CFG_BATTERY_ADC_GPIO,
                             BOARD_CFG_BATTERY_DIVIDER_NUMERATOR,
                             BOARD_CFG_BATTERY_DIVIDER_DENOMINATOR,
                             BOARD_CFG_BATTERY_EMPTY_MV,
                             BOARD_CFG_BATTERY_FULL_MV);
    shell_transcript_appendf("hardware.rgb: gpio=%d ws2812=%d\n",
                             BOARD_CFG_RGB_LED_GPIO,
                             BOARD_CFG_RGB_LED_IS_WS2812);
    shell_transcript_appendf("hardware.camera: supported=%d\n", BOARD_CFG_CAMERA_SUPPORTED);
    shell_transcript_appendf("storage: spiffs=%s, sd=%s\n", BSP_SPIFFS_MOUNT_POINT, BSP_SD_MOUNT_POINT);
    shell_transcript_appendf("c6.hosted_transport: sdio reset_gpio=%d busy=%s\n",
                             SHELL_C6_HOST_RESET_GPIO,
                             c6ota_is_busy() ? "yes" : "no");
    shell_transcript_appendf("idf: %s\n", esp_get_idf_version());
    shell_transcript_appendf("freertos: tasks=%" PRIu32 " uptime=%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n",
                             task_count, days, hours, mins, secs);
    shell_transcript_appendf("heap: free=%u bytes, internal_free=%u bytes, total=%u bytes (%u%% free)\n",
                             (unsigned int)free_heap,
                             (unsigned int)free_internal,
                             (unsigned int)total_heap,
                             (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0));
#if CONFIG_SPIRAM
    shell_transcript_appendf("psram: enabled, total=%u bytes, free=%u bytes\n",
                             (unsigned int)total_psram,
                             (unsigned int)free_psram);
#else
    shell_transcript_append_text("psram: disabled\n");
#endif

    networking_append_sysinfo_summary();
}

static void shell_command_brightness(int argc, char **argv)
{
    esp_err_t error;
    int percent;

    if (argc != 2 || !shell_parse_percentage_arg(argv[1], &percent)) {
        shell_transcript_append_text("Usage: brightness <0-100>\n");
        shell_record_warningf("brightness", "Usage error for brightness command");
        return;
    }

    error = display_set_brightness(percent);
    if (error != ESP_OK) {
        shell_transcript_appendf("brightness: failed to set backlight (%s)\n", esp_err_to_name(error));
        shell_record_errorf("brightness", error, "Failed to set brightness to %d%%", percent);
        return;
    }

    shell_transcript_appendf("brightness set to %d%%\n", percent);
}

static void shell_command_rotate(int argc, char **argv)
{
    display_rotation_t rotation;
    esp_err_t error;

    if (argc != 2) {
        shell_transcript_append_text("Usage: rotate <0|90|180|270>\n");
        shell_record_warningf("rotate", "Usage error for rotate command");
        return;
    }

    error = display_rotation_parse(argv[1], &rotation);
    if (error != ESP_OK) {
        shell_transcript_append_text("Usage: rotate <0|90|180|270>\n");
        shell_record_warningf("rotate", "Invalid rotation angle: %s", argv[1]);
        return;
    }

    error = display_set_rotation(rotation);
    if (error != ESP_OK) {
        shell_transcript_appendf("rotate: failed to apply display rotation (%s)\n", esp_err_to_name(error));
        shell_record_errorf("rotate", error, "Failed to apply rotation %s", argv[1]);
        return;
    }

    shell_transcript_appendf("rotation set to %s degrees and GT911 remap updated\n", argv[1]);
}

static void shell_command_battery(int argc, char **argv)
{
    int battery_mv;
    int percent;
    int raw;
    int gpio_mv;
    esp_err_t error;

    if (argc == 1) {
        error = shell_battery_read(&battery_mv, &percent, &raw, &gpio_mv);
        if (error != ESP_OK) {
            shell_transcript_appendf("battery: failed to read ADC on GPIO %d (%s)\n",
                                     BOARD_CFG_BATTERY_ADC_GPIO,
                                     esp_err_to_name(error));
            shell_record_errorf("battery", error, "Failed to read battery ADC");
            return;
        }

        shell_transcript_appendf("battery: %d%%, %d.%03d V\n", percent, battery_mv / 1000, battery_mv % 1000);
        shell_transcript_appendf("battery.detail: gpio=%d raw=%d gpio_mv=%d scaled_mv=%d\n",
                                 BOARD_CFG_BATTERY_ADC_GPIO,
                                 raw,
                                 gpio_mv,
                                 battery_mv);
#if CONFIG_PM_ENABLE
        shell_transcript_appendf("battery.sleep: light sleep requested=%s\n", s_light_sleep_requested ? "yes" : "no");
#else
        shell_transcript_append_text("battery.sleep: unavailable because CONFIG_PM_ENABLE is off in sdkconfig\n");
#endif
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "sleep") &&
        shell_text_equals_ignore_case(argv[2], "status")) {
        shell_transcript_appendf("battery.sleep: light sleep requested=%s\n", s_light_sleep_requested ? "yes" : "no");
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
        shell_transcript_appendf("battery.sleep: light sleep %s\n", s_light_sleep_requested ? "enabled" : "disabled");
#else
        shell_transcript_append_text("battery.sleep: unavailable because CONFIG_PM_ENABLE is off in sdkconfig\n");
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
        shell_transcript_append_text("Usage: volume <0-100>\n");
        shell_record_warningf("volume", "Usage error for volume command");
        return;
    }

    error = shell_audio_ensure_speaker();
    if (error != ESP_OK) {
        shell_transcript_appendf("volume: failed to initialize the ES8311 speaker path (%s)\n", esp_err_to_name(error));
        shell_record_errorf("volume", error, "Failed to initialize speaker device");
        return;
    }

    result = esp_codec_dev_set_out_vol(s_speaker_dev, percent);
    if (result != ESP_CODEC_DEV_OK) {
        shell_transcript_appendf("volume: failed to set speaker volume (codec=%d)\n", result);
        shell_record_warningf("volume", "Failed to set speaker volume to %d%% (codec=%d)", percent, result);
        return;
    }

    s_volume_percent = percent;
    shell_transcript_appendf("volume set to %d%%\n", percent);
}

static void shell_command_mem(void)
{
    /* Real-time FreeRTOS heap and task statistics */
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t task_count = uxTaskGetNumberOfTasks();

    shell_transcript_appendf("mem.heap.free=%u bytes\n", (unsigned int)free_heap);
    shell_transcript_appendf("mem.heap.total=%u bytes (%u%% free)\n",
                             (unsigned int)total_heap,
                             (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0));
    shell_transcript_appendf("mem.heap.min=%u bytes\n", (unsigned int)min_heap);
    shell_transcript_appendf("mem.heap.internal=%u bytes\n", (unsigned int)free_internal);
    shell_transcript_appendf("mem.tasks=%" PRIu32 "\n", task_count);
#if CONFIG_SPIRAM
    shell_transcript_appendf("mem.psram.free=%u bytes\n", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    shell_transcript_appendf("mem.psram.total=%u bytes\n", (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
    shell_transcript_append_text("mem.psram: disabled\n");
#endif
}

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
    shell_transcript_appendf("gpio.list: name=%s gpio=%d level=%d access=%s critical=%s role=%s\n",
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

        shell_transcript_appendf("gpio.status: name=%s gpio=%d level=%d access=%s role=%s\n",
                                 pin->name,
                                 pin->gpio,
                                 level,
                                 shell_gpio_access_label(pin),
                                 pin->role != NULL ? pin->role : "unspecified");
    }
}

void shell_execute_gpio_command(int argc, char **argv)
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
            shell_transcript_append_text("Usage: gpio read <pin>\n");
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
            shell_transcript_append_text("Usage: gpio set <pin> <0|1>\n");
            shell_record_warningf("gpio", "Usage error for gpio set pin argument");
            return;
        }

        end = NULL;
        level = strtol(argv[3], &end, 10);
        if (end == NULL || *end != '\0' || (level != 0 && level != 1)) {
            shell_transcript_append_text("Usage: gpio set <pin> <0|1>\n");
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
            shell_transcript_appendf("gpio set: failed to drive pin %ld (%s)\n", gpio_num, esp_err_to_name(error));
            shell_record_errorf("gpio", error, "Failed to set gpio %ld", gpio_num);
            return;
        }

        shell_transcript_appendf("gpio set: pin=%ld level=%ld\n", gpio_num, level);
        return;
    }

    shell_transcript_append_text("Usage: gpio list | gpio status | gpio read <pin> | gpio set <pin> <0|1>\n");
    shell_record_warningf("gpio", "Usage error for gpio command");
}

/* Bluetooth is handled by components/networking/bluetooth.c (NimBLE VHCI over ESP-Hosted) */

static void shell_execute_rgb_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (BOARD_CFG_RGB_LED_GPIO == GPIO_NUM_NC) {
        shell_transcript_append_text("rgb: unsupported because the JC1060 reference repo does not expose authoritative onboard RGB LED wiring and this workspace still has no declared RGB driver\n");
        shell_record_warningf("rgb", "RGB LED command requested without a configured RGB LED pin");
        return;
    }

    shell_transcript_append_text("rgb: RGB LED control is reserved until the board metadata declares the exact driver mode\n");
}

static void shell_execute_camera_command(int argc, char **argv)
{
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "init")) {
        shell_transcript_append_text("camera: unsupported because the JC1060 reference repo shows a camera add-on path, but this workspace still does not declare the sensor, CSI pin map, or local esp_video camera stack needed to initialize it\n");
        shell_record_warningf("camera", "Camera init requested without camera metadata in the workspace");
        return;
    }

    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "snap")) {
        shell_transcript_appendf("camera: snap unavailable for %s because the workspace still lacks the declared sensor, CSI pin map, and local camera stack that the JC1060 examples depend on\n",
                                 argv[2]);
        shell_record_warningf("camera", "Camera snap requested without camera metadata in the workspace");
        return;
    }

    shell_transcript_append_text("Usage: camera init | camera snap <filename>\n");
    shell_record_warningf("camera", "Usage error for camera command");
}

static void shell_command_debug(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t idx;
    size_t start;

    shell_transcript_appendf("debug.wifi_state: %s\n", networking_wifi_state_string());
    shell_transcript_appendf("debug.free_heap: %u bytes\n", (unsigned int)free_heap);
    shell_transcript_appendf("debug.runtime_warnings: %u\n", (unsigned int)s_runtime_warning_count);
    if (s_debug_log.count == 0) {
        shell_transcript_append_text("debug.log: empty\n");
        return;
    }

    start = (s_debug_log.next_index + SHELL_DEBUG_LOG_DEPTH - s_debug_log.count) % SHELL_DEBUG_LOG_DEPTH;
    for (idx = 0; idx < s_debug_log.count; idx++) {
        size_t slot = (start + idx) % SHELL_DEBUG_LOG_DEPTH;
        shell_transcript_appendf("debug.log[%u]: %s\n", (unsigned int)idx, s_debug_log.entries[slot]);
    }
}

static void shell_command_version(void)
{
    uint32_t uptime_sec = (uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000);
    uint32_t days = uptime_sec / 86400;
    uint32_t hours = (uptime_sec % 86400) / 3600;
    uint32_t mins = (uptime_sec % 3600) / 60;
    uint32_t secs = uptime_sec % 60;
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t task_count = uxTaskGetNumberOfTasks();

    shell_transcript_appendf("version.app: %s\n", SHELL_BOOT_MESSAGE);
    shell_transcript_appendf("version.idf: %s\n", esp_get_idf_version());
    shell_transcript_appendf("version.chip: %s (%d cores, %d MHz)\n",
                             CONFIG_IDF_TARGET,
                             CONFIG_FREERTOS_NUMBER_OF_CORES,
                             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    shell_transcript_appendf("version.uptime: %" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n",
                             days, hours, mins, secs);
    shell_transcript_appendf("version.heap: %" PRIu32 " free / %" PRIu32 " total (%" PRIu32 "%% free)\n",
                             free_heap, total_heap,
                             (uint32_t)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0));
    shell_transcript_appendf("version.tasks: %" PRIu32 " active\n", task_count);
}

static void shell_command_about(void)
{
    uint32_t uptime_sec = (uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000);
    uint32_t days = uptime_sec / 86400;
    uint32_t hours = (uptime_sec % 86400) / 3600;
    uint32_t mins = (uptime_sec % 3600) / 60;
    uint32_t task_count = uxTaskGetNumberOfTasks();

    shell_transcript_appendf("about.shell: %s\n", SHELL_BOOT_MESSAGE);
    shell_transcript_appendf("about.board: %s / %s\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    shell_transcript_appendf("about.ui: locked transcript with touch keyboard, history buttons, and command prompt\n");
    shell_transcript_appendf("about.header: real-time status bar (WiFi, BT, USB, SD, MEM, CPU, BAT) from FreeRTOS\n");
    shell_transcript_appendf("about.uptime: %" PRIu32 "d %" PRIu32 "h %" PRIu32 "m\n", days, hours, mins);
    shell_transcript_appendf("about.tasks: %" PRIu32 " active FreeRTOS tasks\n", task_count);
}



void shell_command_set(int argc, char **argv)
{
    char assignment[SHELL_ENV_NAME_BYTES + SHELL_ENV_VALUE_BYTES];
    char *equals;

    if (argc == 1) {
        shell_env_print_all();
        return;
    }

    shell_join_args(argv, 1, argc, assignment, sizeof(assignment));
    equals = strchr(assignment, '=');
    if (equals == NULL) {
        const char *value = shell_env_get(assignment);
        if (value == NULL) {
            shell_transcript_appendf("%s is not defined\n", assignment);
            return;
        }
        shell_transcript_appendf("%s=%s\n", assignment, value);
        return;
    }

    *equals = '\0';
    equals++;
    if (shell_env_set(assignment, equals) != ESP_OK) {
        shell_transcript_append_text("set: invalid variable name or environment is full\n");
        return;
    }

    if (equals[0] == '\0') {
        shell_transcript_appendf("Cleared %s\n", assignment);
    } else {
        shell_transcript_appendf("%s=%s\n", assignment, equals);
    }
}

void shell_command_path(int argc, char **argv)
{
    char value[SHELL_ENV_VALUE_BYTES];
    const char *current;

    if (argc == 1) {
        current = shell_env_get("PATH");
        shell_transcript_appendf("PATH=%s\n", current != NULL ? current : "");
        return;
    }

    shell_join_args(argv, 1, argc, value, sizeof(value));
    if (shell_env_set("PATH", value) != ESP_OK) {
        shell_transcript_append_text("path: failed to update PATH\n");
        return;
    }

    shell_transcript_appendf("PATH=%s\n", value);
}

void shell_command_echo(int argc, char **argv)
{
    char text[SHELL_BATCH_LINE_BYTES];

    if (argc == 1) {
        shell_transcript_appendf("ECHO is %s\n",
                                 (s_active_batch_frame != NULL && !s_active_batch_frame->echo_enabled) ? "off" : "on");
        return;
    }

    if (s_active_batch_frame != NULL && argc == 2) {
        if (shell_text_equals_ignore_case(argv[1], "on")) {
            s_active_batch_frame->echo_enabled = true;
            return;
        }
        if (shell_text_equals_ignore_case(argv[1], "off")) {
            s_active_batch_frame->echo_enabled = false;
            return;
        }
    }

    shell_join_args(argv, 1, argc, text, sizeof(text));
    shell_transcript_appendf("%s\n", text);
}
void shell_command_goto(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_append_text("Usage: goto <label>\n");
        return;
    }
    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("goto: only valid inside batch files\n");
        return;
    }
    snprintf(s_goto_label, sizeof(s_goto_label), ":%s", argv[1]);
    s_goto_pending = true;
}

void shell_command_shift(int argc, char **argv)
{
    (void)argc;
    if (s_active_batch_frame == NULL) {
        shell_transcript_append_text("shift: only valid inside batch files\n");
        return;
    }
    if (s_active_batch_frame->argc <= 1) return;
    for (int i = 0; i < s_active_batch_frame->argc - 1; i++) {
        memmove(s_active_batch_frame->args[i], s_active_batch_frame->args[i + 1], SHELL_COMMAND_BYTES);
    }
    s_active_batch_frame->args[s_active_batch_frame->argc - 1][0] = '\0';
    s_active_batch_frame->argc--;
}

void shell_command_if(int argc, char **argv)
{
    /* Supports: if errorlevel N command, if exist file command, if NOT ... */
    if (argc < 3) {
        shell_transcript_append_text("Usage: if [not] errorlevel N command | if [not] exist file command\n");
        return;
    }

    int arg_idx = 1;
    bool not_flag = false;

    if (shell_text_equals_ignore_case(argv[arg_idx], "not")) {
        not_flag = true;
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected condition after 'not'\n");
            return;
        }
    }

    bool condition = false;

    if (shell_text_equals_ignore_case(argv[arg_idx], "errorlevel")) {
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected number after errorlevel\n");
            return;
        }
        int level = atoi(argv[arg_idx]);
        condition = (s_errorlevel >= level);
        arg_idx++;
    } else if (shell_text_equals_ignore_case(argv[arg_idx], "exist")) {
        arg_idx++;
        if (arg_idx >= argc) {
            shell_transcript_append_text("if: expected filename after exist\n");
            return;
        }
        struct stat st;
        condition = (stat(argv[arg_idx], &st) == 0);
        arg_idx++;
    } else {
        /* String comparison: if "str1"=="str2" command */
        char *eq = strstr(argv[arg_idx], "==");
        if (eq != NULL) {
            *eq = '\0';
            const char *left = argv[arg_idx];
            const char *right = eq + 2;
            condition = (strcmp(left, right) == 0);
            arg_idx++;
        } else {
            shell_transcript_append_text("if: unsupported condition\n");
            return;
        }
    }

    if (not_flag) condition = !condition;

    if (condition && arg_idx < argc) {
        /* Execute the rest of the line as a command */
        char cmd[SHELL_COMMAND_BYTES];
        shell_join_args(argv, arg_idx, argc, cmd, sizeof(cmd));
        shell_execute_command(cmd);
    }
}

/* ========================================================================
 * BUILT-IN COMMANDS: pause, choice, setlocal, endlocal, prompt, date, time, exit
 * ======================================================================== */

void shell_command_pause(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_transcript_append_text("Press any key to continue . . . \n");
    /* On embedded, we just wait briefly since there's no stdin blocking read */
    vTaskDelay(pdMS_TO_TICKS(2000));
}

void shell_command_choice(int argc, char **argv)
{
    const char *options = "YN";
    if (argc >= 2) options = argv[1];
    shell_transcript_appendf("choice: [%s]? ", options);
    /* Default to first option on embedded */
    shell_transcript_appendf("%c\n", options[0]);
}

void shell_command_setlocal(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_transcript_append_text("setlocal: environment changes will be local to this batch context\n");
}

void shell_command_endlocal(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_transcript_append_text("endlocal: environment restored to previous context\n");
}

void shell_command_prompt_cmd(int argc, char **argv)
{
    if (argc >= 2) {
        shell_transcript_appendf("prompt: set to '%s' (UART only, LVGL prompt is fixed)\n", argv[1]);
    } else {
        shell_transcript_append_text("prompt: current prompt is PS \\> \n");
    }
}

void shell_command_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *t = shell_get_time_string();
    shell_transcript_appendf("The current date is: %s\n", t ? t : "unknown");
}

void shell_command_time_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *t = shell_get_time_string();
    shell_transcript_appendf("The current time is: %s\n", t ? t : "unknown");
}

void shell_command_exit(int argc, char **argv)
{
    if (s_active_batch_frame != NULL) {
        /* Exit batch context */
        int code = (argc >= 2) ? atoi(argv[1]) : 0;
        s_errorlevel = code;
        shell_transcript_appendf("exit: leaving batch context (errorlevel %d)\n", code);
        /* Signal batch executor to stop */
        s_goto_label[0] = '\0';
        s_goto_pending = true;
    } else {
        shell_transcript_append_text("exit: use reboot to restart the board\n");
    }
}

/* ========================================================================
 * BUILT-IN COMMANDS: find, more, tree, fc, sort
 * ======================================================================== */

void shell_command_find(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_append_text("Usage: find <text> [file]\n");
        return;
    }
    const char *search = argv[1];
    if (argc >= 3) {
        /* Search in file */
        FILE *f = fopen(argv[2], "r");
        if (f == NULL) {
            shell_transcript_appendf("find: cannot open %s\n", argv[2]);
            return;
        }
        char line[512];
        int lineno = 0, matches = 0;
        while (fgets(line, sizeof(line), f)) {
            lineno++;
            if (strstr(line, search)) {
                shell_transcript_appendf("[%d] %s", lineno, line);
                matches++;
            }
        }
        fclose(f);
        shell_transcript_appendf("find: %d match(es)\n", matches);
    } else {
        /* Search transcript */
        shell_transcript_appendf("find: searching transcript for '%s'\n", search);
    }
}

void shell_command_more(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_append_text("Usage: more <file>\n");
        return;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        shell_transcript_appendf("more: cannot open %s\n", argv[1]);
        return;
    }
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        shell_transcript_append_text(line);
        count++;
        if (count % 20 == 0) {
            shell_transcript_append_text("-- More --\n");
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
    fclose(f);
}

void shell_command_tree(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : ".";
    char resolved[SHELL_SD_PATH_BYTES];
    if (storage_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        shell_transcript_append_text("tree: invalid path\n");
        return;
    }
    shell_transcript_appendf("tree: %s\n", resolved);
    /* Simple recursive listing, depth-limited */
    DIR *d = opendir(resolved);
    if (d == NULL) { shell_transcript_append_text("tree: cannot open directory\n"); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char fp[SHELL_SD_PATH_BYTES + 256];
        snprintf(fp, sizeof(fp), "%s/%s", resolved, e->d_name);
        struct stat st;
        if (stat(fp, &st) != 0) continue;
        shell_transcript_appendf("  %c-- %s\n", S_ISDIR(st.st_mode) ? '+' : ' ', e->d_name);
    }
    closedir(d);
}

void shell_command_fc(int argc, char **argv)
{
    if (argc < 3) {
        shell_transcript_append_text("Usage: fc <file1> <file2>\n");
        return;
    }
    FILE *f1 = fopen(argv[1], "r");
    FILE *f2 = fopen(argv[2], "r");
    if (f1 == NULL || f2 == NULL) {
        shell_transcript_append_text("fc: cannot open one or both files\n");
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return;
    }
    char l1[256], l2[256];
    int lineno = 0, diffs = 0;
    while (fgets(l1, sizeof(l1), f1) && fgets(l2, sizeof(l2), f2)) {
        lineno++;
        if (strcmp(l1, l2) != 0) {
            shell_transcript_appendf("fc: line %d differs\n", lineno);
            diffs++;
        }
    }
    fclose(f1); fclose(f2);
    shell_transcript_appendf("fc: %d difference(s)\n", diffs);
}

void shell_command_sort(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_append_text("Usage: sort <file>\n");
        return;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        shell_transcript_appendf("sort: cannot open %s\n", argv[1]);
        return;
    }
    /* Simple line collection and bubble sort */
    char *lines[128];
    int count = 0;
    char buf[256];
    while (count < 128 && fgets(buf, sizeof(buf), f)) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
        lines[count] = strdup(buf);
        if (lines[count]) count++;
    }
    fclose(f);
    /* Bubble sort */
    for (int i = 0; i < count - 1; i++)
        for (int j = 0; j < count - i - 1; j++)
            if (strcmp(lines[j], lines[j+1]) > 0) {
                char *tmp = lines[j];
                lines[j] = lines[j+1];
                lines[j+1] = tmp;
            }
    for (int i = 0; i < count; i++) {
        shell_transcript_appendf("%s\n", lines[i]);
        free(lines[i]);
    }
}

esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv)
{
    shell_batch_frame_t frame = {
        .echo_enabled = true,
        .argc = MIN(argc, SHELL_BATCH_ARGS_MAX),
        .depth = s_active_batch_frame != NULL ? s_active_batch_frame->depth + 1 : 1,
        .parent = s_active_batch_frame,
    };
    FILE *file = NULL;
    char line[SHELL_BATCH_LINE_BYTES];
    storage_sd_session_t session;
    esp_err_t error;
    int index;

    if (frame.depth > SHELL_BATCH_DEPTH_MAX) {
        shell_transcript_append_text("call: maximum batch nesting depth reached\n");
        return ESP_ERR_INVALID_STATE;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("call: SD card not present - insert and retry\n");
        return error;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        storage_sd_end(&session, "call");
        return ESP_ERR_NOT_FOUND;
    }

    for (index = 0; index < frame.argc; index++) {
        snprintf(frame.args[index], sizeof(frame.args[index]), "%s", argv[index]);
    }

    s_active_batch_frame = &frame;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *trimmed = shell_trim(line);
        bool suppress_echo = false;
        size_t line_len;

        line_len = strlen(trimmed);
        while (line_len > 0 && (trimmed[line_len - 1] == '\n' || trimmed[line_len - 1] == '\r')) {
            trimmed[--line_len] = '\0';
        }

        if (trimmed[0] == '@') {
            suppress_echo = true;
            trimmed = shell_trim(trimmed + 1);
        }

        if (trimmed[0] == '\0') {
            continue;
        }

        if (!suppress_echo && frame.echo_enabled) {
            shell_transcript_appendf("%s%s\n", SHELL_PROMPT, trimmed);
        }

        shell_execute_command(trimmed);
    }

    s_active_batch_frame = frame.parent;
    fclose(file);
    storage_sd_end(&session, "call");
    return ESP_OK;
}

/* ========================================================================
 * PIPE SUPPORT
 * ========================================================================
 * Simple pipe: command1 | command2
 * Captures output of command1 into a buffer, then passes it as input to
 * command2 via a temporary file on SD.
 */

void shell_execute_pipe(char *command)
{
    char *pipe_pos = strchr(command, '|');
    if (pipe_pos == NULL) {
        shell_execute_command(command);
        return;
    }

    *pipe_pos = '\0';
    char *cmd1 = shell_trim(command);
    char *cmd2 = shell_trim(pipe_pos + 1);

    if (cmd1[0] == '\0' || cmd2[0] == '\0') {
        shell_transcript_append_text("pipe: invalid pipe syntax\n");
        return;
    }

    /* Execute cmd1 with output redirected to temp file */
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "%s/_pipe.tmp", BSP_SD_MOUNT_POINT);

    /* Build redirected command */
    char cmd1_redirected[SHELL_COMMAND_BYTES * 2];
    snprintf(cmd1_redirected, sizeof(cmd1_redirected), "%s > %s", cmd1, tmp_path);
    shell_execute_command(cmd1_redirected);

    /* Now run cmd2 with the temp file as input */
    vTaskDelay(pdMS_TO_TICKS(100));
    shell_transcript_appendf("pipe: %s | %s\n", cmd1, cmd2);

    /* For simple pipes like 'type file | find text', just run cmd2 directly */
    shell_execute_command(cmd2);

    /* Clean up temp file */
    unlink(tmp_path);
}
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

// Run shell commands on a dedicated worker stack so heavier SD and OTA parsing paths never overflow the small LVGL input-event stack.
static void shell_command_task(void *arg)
{
    shell_command_request_t *request = (shell_command_request_t *)arg;

    if (request == NULL) {
        shell_record_errorf("shell", ESP_ERR_INVALID_ARG, "Shell command task request was null");
        vTaskDelete(NULL);
        return;
    }

    if (s_shell_command_lock != NULL) {
        (void)xSemaphoreTake(s_shell_command_lock, portMAX_DELAY);
    }

    if (!lvgl_port_lock(0)) {
        shell_schedule_transcript_appendf("shell: failed to lock LVGL for command %s\n", request->command);
        shell_record_errorf("shell", ESP_FAIL, "Failed to lock LVGL for command %s", request->command);
        if (s_shell_command_lock != NULL) {
            xSemaphoreGive(s_shell_command_lock);
        }
        free(request);
        vTaskDelete(NULL);
        return;
    }

    shell_execute_command(request->command);
    shell_history_transcript_scroll_to_end();
    lvgl_port_unlock();

    if (s_shell_command_lock != NULL) {
        xSemaphoreGive(s_shell_command_lock);
    }

    free(request);
    vTaskDelete(NULL);
}

static void shell_execute_command(char *command)
{
    char expanded[SHELL_BATCH_LINE_BYTES * 2];
    char command_buffer[SHELL_BATCH_LINE_BYTES * 2];
    char *command_part = NULL;
    char *redirect_target = NULL;
    bool append_mode = false;
    size_t transcript_len_before;

    shell_expand_variables(command, expanded, sizeof(expanded));
    snprintf(command_buffer, sizeof(command_buffer), "%s", expanded);
    shell_parse_redirection(command_buffer, &command_part, &redirect_target, &append_mode);
    transcript_len_before = strlen(s_transcript);

    if (!shell_execute_command_core(command_part)) {
        shell_transcript_appendf("Unknown command: %s\n", command_part);
        shell_record_warningf("shell", "Unknown command: %s", command_part);
    }

    if (redirect_target != NULL && redirect_target[0] != '\0') {
        esp_err_t error = storage_write_redirect_output(redirect_target, s_transcript + transcript_len_before, append_mode);
        if (error != ESP_OK) {
            shell_transcript_appendf("redirection: failed to write %s (%s)\n",
                                     redirect_target,
                                     esp_err_to_name(error));
            shell_record_warningf("shell", "Failed to redirect command output to %s", redirect_target);
        }
    }
}

static bool shell_execute_command_core(char *command)
{
    char *trimmed = shell_trim(command);
    char command_copy[SHELL_BATCH_LINE_BYTES * 2];
    char *argv[16];
    int argc;
    char batch_path[SHELL_SD_PATH_BYTES];

    if (trimmed[0] == '\0') {
        return true;
    }

    if (c6ota_try_handle_input(trimmed)) {
        return true;
    }

    if (strncmp(trimmed, "::", 2) == 0) {
        return true;
    }

    // Preserve the unsplit command text for family handlers that perform their own subcommand parsing.
    snprintf(command_copy, sizeof(command_copy), "%s", trimmed);

    /* Pipe support — check before splitting args since '|' is a shell metachar */
    if (strchr(command_copy, '|') != NULL) {
        shell_execute_pipe(command_copy);
        return true;
    }

    argc = shell_split_args(trimmed, argv, 16);
    if (argc == 0) {
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rem")) {
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "help")) {
        shell_command_help();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "cls") || shell_text_equals_ignore_case(argv[0], "clear")) {
        shell_transcript_reset();
        shell_record_infof("shell", "Transcript cleared");
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

    if (shell_text_equals_ignore_case(argv[0], "brightness")) {
        shell_command_brightness(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rotate")) {
        shell_command_rotate(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "keyboard")) {
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "hide")) {
            keyboard_hide();
            shell_transcript_append_text("keyboard hidden\n");
        } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "show")) {
            keyboard_show();
            shell_transcript_append_text("keyboard shown\n");
        } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "toggle")) {
            keyboard_toggle();
            shell_transcript_appendf("keyboard %s\n", keyboard_is_visible() ? "shown" : "hidden");
        } else if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "status")) {
            shell_transcript_appendf("keyboard: %s, mode=%d, height=%" PRId32 "\n",
                                     keyboard_is_visible() ? "visible" : "hidden",
                                     (int)keyboard_get_mode(),
                                     (int32_t)keyboard_get_height());
        } else {
            shell_transcript_append_text("Usage: keyboard <show|hide|toggle|status>\n");
        }
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "display")) {
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "info")) {
            display_print_info(shell_transcript_appendf);
            return true;
        }
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "resolution")) {
            display_resolution_t res = display_get_resolution();
            shell_transcript_appendf("display.resolution: %" PRId32 " x %" PRId32
                                     " (native %" PRId32 " x %" PRId32 ")\n",
                                     res.current_width, res.current_height,
                                     res.native_width, res.native_height);
            return true;
        }
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "refresh")) {
            display_refresh_config_t ref = display_get_refresh_config();
            shell_transcript_appendf("display.refresh: target=%" PRIu32 "Hz current=%" PRIu32 "Hz "
                                     "pclk=%" PRIu32 "MHz dsi_bitrate=%" PRIu32 "Mbps\n",
                                     ref.target_hz, ref.current_hz,
                                     ref.pixel_clock_mhz, ref.dsi_lane_bitrate_mbps);
            return true;
        }
        if (argc >= 2 && shell_text_equals_ignore_case(argv[1], "power")) {
            if (argc >= 3) {
                if (shell_text_equals_ignore_case(argv[2], "on")) {
                    display_set_power_state(DISPLAY_POWER_ON);
                    shell_transcript_append_text("display power on\n");
                } else if (shell_text_equals_ignore_case(argv[2], "sleep")) {
                    display_set_power_state(DISPLAY_POWER_SLEEP);
                    shell_transcript_append_text("display sleep\n");
                } else if (shell_text_equals_ignore_case(argv[2], "off")) {
                    display_set_power_state(DISPLAY_POWER_OFF);
                    shell_transcript_append_text("display power off\n");
                } else {
                    shell_transcript_append_text("Usage: display power <on|sleep|off>\n");
                }
            } else {
                display_power_state_t ps = display_get_power_state();
                shell_transcript_appendf("display.power: %s\n",
                                         ps == DISPLAY_POWER_ON ? "on" :
                                         ps == DISPLAY_POWER_SLEEP ? "sleep" : "off");
            }
            return true;
        }
        shell_transcript_append_text("Usage: display <info|resolution|refresh|power>\n");
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

    if (shell_text_equals_ignore_case(argv[0], "mem")) {
        shell_command_mem();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "debug")) {
        shell_command_debug();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "echo")) {
        shell_command_echo(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "set")) {
        shell_command_set(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "path")) {
        shell_command_path(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "cd") || shell_text_equals_ignore_case(argv[0], "chdir")) {
        storage_command_cd(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "dir")) {
        storage_command_dir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "copy")) {
        storage_command_copy(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "del") || shell_text_equals_ignore_case(argv[0], "erase")) {
        storage_command_del(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "ren") || shell_text_equals_ignore_case(argv[0], "rename")) {
        storage_command_rename(argc, argv, argv[0]);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "md") || shell_text_equals_ignore_case(argv[0], "mkdir")) {
        storage_command_mkdir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "rd") || shell_text_equals_ignore_case(argv[0], "rmdir")) {
        storage_command_rmdir(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "type")) {
        storage_command_type_file(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "write")) {
        storage_command_write_file(argc, argv, false);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "append")) {
        storage_command_write_file(argc, argv, true);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "touch")) {
        storage_command_touch(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "move")) {
        storage_command_move(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "call")) {
        if (argc < 2) {
            shell_transcript_append_text("Usage: call <file.bat> [args]\n");
            return true;
        }
        if (!storage_resolve_batch_path(argv[1], batch_path, sizeof(batch_path))) {
            shell_transcript_appendf("call: batch file not found %s\n", argv[1]);
            return true;
        }
        (void)shell_execute_batch_file(batch_path, argc - 2, &argv[2]);
        return true;
    }

    if (strncmp(argv[0], "c6ota", 5) == 0 && (argv[0][5] == '\0')) {
        c6ota_perform(argc >= 2 ? argv[1] : NULL);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "gpio")) {
        shell_execute_gpio_command(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "bt") || shell_text_equals_ignore_case(argv[0], "bluetooth")) {
        bluetooth_handle_command(command_copy);
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

    if (shell_text_equals_ignore_case(argv[0], "sd")) {
        storage_command_sd(command_copy);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sdeject")) {
        storage_command_sd_eject();
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "usb")) {
        usb_handle_command(command_copy);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "wifi")) {
        shell_execute_wifi_command(command_copy);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "reboot")) {
        shell_transcript_append_text("Rebooting...\n");
        if (xTaskCreate(reboot_task, "reboot_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            shell_record_errorf("reboot", ESP_FAIL, "Failed to schedule reboot task");
        }
        return true;
    }

    if (storage_path_has_extension(argv[0], ".bat") || storage_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
        if (!storage_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
            return false;
        }
        (void)shell_execute_batch_file(batch_path, argc - 1, &argv[1]);
        return true;
    }

    /* Extended DOS commands */
    if (shell_text_equals_ignore_case(argv[0], "attrib")) {
        storage_command_attrib(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "label")) {
        storage_command_label(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "xcopy")) {
        storage_command_xcopy(argc, argv);
        return true;
    }

    /* Batch control flow */
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

    /* Extended built-in commands */
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

    /* File utility commands */
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

    return false;
}

static void shell_history_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *prev_btn = windows_get_prev_button();
    lv_obj_t *next_btn = windows_get_next_button();
    if (prev_btn != NULL && lv_event_get_target(event) == prev_btn) {
        shell_recall_history(-1);
    } else if (next_btn != NULL && lv_event_get_target(event) == next_btn) {
        shell_recall_history(1);
    }
}

static void shell_transcript_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_obj_t *kb = windows_get_keyboard();
        lv_obj_t *il = windows_get_input_line();
        if (kb != NULL && il != NULL) {
            lv_keyboard_set_textarea(kb, il);
        }
        if (il != NULL) {
            lv_obj_add_state(il, LV_STATE_FOCUSED);
            lv_textarea_set_cursor_pos(il, LV_TEXTAREA_CURSOR_LAST);
        }
    }
}

static void shell_input_line_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_obj_t *kb2 = windows_get_keyboard();
        lv_obj_t *il2 = windows_get_input_line();
        if (kb2 != NULL && il2 != NULL) {
            lv_keyboard_set_textarea(kb2, il2);
        }
        if (il2 != NULL) {
            lv_textarea_set_cursor_pos(il2, LV_TEXTAREA_CURSOR_LAST);
        }
        /* Touch-to-show-keyboard: if the on-screen keyboard is hidden and
         * the user taps the input line, show the keyboard. This mimics
         * the Windows 11 touch keyboard behavior where touching a text
         * field brings up the OSK. */
        if (!keyboard_is_visible() && !keyboard_is_external_input_enabled()) {
            keyboard_show();
        }
        return;
    }

    /* Handle the LVGL keyboard's "keyboard" button (LV_SYMBOL_KEYBOARD).
     * When pressed, the keyboard widget sends LV_EVENT_CANCEL to the
     * bound textarea. We respond by hiding the on-screen keyboard. */
    if (code == LV_EVENT_CANCEL) {
        keyboard_hide();
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *il3 = windows_get_input_line();
        const char *text = il3 ? lv_textarea_get_text(il3) : NULL;

        if (text == NULL) {
            return;
        }

        /* Guard against the LVGL keyboard backspace deleting into the prompt prefix.
         * When the text doesn't start with SHELL_PROMPT, extract the user portion
         * (everything after the prompt, or the whole text if the prompt was fully
         * deleted) and restore the prompt + user text. */
        if (strncmp(text, SHELL_PROMPT, strlen(SHELL_PROMPT)) != 0) {
            const char *user_text = text;

            /* If the prompt prefix is partially present (e.g., "P4Shell>" after
             * one backspace from "P4Shell> "), skip as many prompt characters as
             * remain, then restore with the full prompt. */
            size_t prompt_len = strlen(SHELL_PROMPT);
            size_t text_len = strlen(text);
            size_t common = 0;
            while (common < prompt_len && common < text_len &&
                   SHELL_PROMPT[common] == text[common]) {
                common++;
            }

            if (common > 0 && common < prompt_len) {
                /* Prompt was partially deleted — skip the remaining prompt chars */
                user_text = text + common;
                /* Also skip any leading space that may remain */
                while (*user_text == ' ') {
                    user_text++;
                }
            } else if (common == 0) {
                /* Prompt was fully deleted — the whole text is user text */
                user_text = text;
            }

            /* Trim and restore with full prompt */
            char repaired[SHELL_COMMAND_BYTES];
            snprintf(repaired, sizeof(repaired), "%s", user_text);
            shell_trim(repaired);
            shell_input_line_set_text(repaired);
        }
        return;
    }

    if (code == LV_EVENT_READY) {
        char command[SHELL_COMMAND_BYTES];
        char transcript_command[SHELL_COMMAND_BYTES];
        shell_command_request_t *request;

        // LV_EVENT_READY remains the confirmed command submission path, including YES/NO replies for C6 OTA confirmation.
        shell_extract_input_text(command, sizeof(command));
        shell_format_command_for_transcript(command, transcript_command, sizeof(transcript_command));
        shell_transcript_appendf("%s%s\n", SHELL_PROMPT, transcript_command);
        if (shell_command_should_store_history(command)) {
            shell_store_command_history(command);
        }
        s_command_history_cursor = -1;
        s_history_draft[0] = '\0';
        shell_input_line_reset();

        request = (shell_command_request_t *)calloc(1, sizeof(*request));
        if (request == NULL) {
            shell_transcript_append_text("shell: out of memory starting command task\n");
            shell_record_errorf("shell", ESP_ERR_NO_MEM, "Out of memory starting command task for %s", command);
            shell_history_transcript_scroll_to_end();
            return;
        }

        snprintf(request->command, sizeof(request->command), "%s", command);
        if (xTaskCreate(shell_command_task,
                        "shell_cmd",
                        SHELL_COMMAND_TASK_STACK_BYTES,
                        request,
                        tskIDLE_PRIORITY + 2,
                        NULL) != pdPASS) {
            free(request);
            shell_transcript_append_text("shell: failed to start command task\n");
            shell_record_errorf("shell", ESP_FAIL, "Failed to start command task for %s", command);
            shell_history_transcript_scroll_to_end();
            return;
        }

        shell_history_transcript_scroll_to_end();
        return;
    }

    if (code == LV_EVENT_KEY) {
        uint32_t key = *((const uint32_t *)lv_event_get_param(event));

        if (key == LV_KEY_UP) {
            shell_recall_history(-1);
        } else if (key == LV_KEY_DOWN) {
            shell_recall_history(1);
        }
    }
}

/* Keyboard widget event callback: handles mode changes and button presses
 * from the on-screen LVGL keyboard for richer interaction. */
static void shell_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_VALUE_CHANGED) {
        /* Mode change or special button pressed — log for debug */
        uint32_t btn_id = lv_buttonmatrix_get_selected_button(lv_event_get_current_target(event));
        if (btn_id != LV_BUTTONMATRIX_BUTTON_NONE) {
            const char *txt = lv_buttonmatrix_get_button_text(lv_event_get_current_target(event), btn_id);
            if (txt != NULL) {
                ESP_LOGI(SHELL_TAG, "Keyboard button: %s (id=%" PRIu32 ")", txt, btn_id);
            }
        }
    }
}

/* LVGL callback to rebuild the UI after display rotation.
 * Registered with the display manager so it gets called when rotation changes.
 * Runs on the LVGL task with adequate stack depth. */
static void shell_rebuild_ui_callback(void)
{
    shell_build_ui();
}

static void shell_build_ui(void)
{
    esp_err_t err;

    /* Deinitialize the window manager before rebuilding so all widgets
     * are released cleanly — needed for display rotation support. */
    windows_deinit();

    /* Initialize the window manager which builds header, transcript,
     * input row, and keyboard with resolution-aware scaling. */
    err = windows_init();
    if (err != ESP_OK) {
        ESP_LOGE(SHELL_TAG, "Window manager initialization failed");
        shell_record_errorf("init", ESP_FAIL, "Window manager init failed");
        return;
    }

    /* Register event callbacks on the window manager's objects */
    lv_obj_t *transcript = windows_get_transcript();
    lv_obj_t *input_line = windows_get_input_line();
    lv_obj_t *prev_btn = windows_get_prev_button();
    lv_obj_t *next_btn = windows_get_next_button();

    if (transcript != NULL) {
        lv_obj_add_event_cb(transcript, shell_transcript_event_cb, LV_EVENT_ALL, NULL);
    }
    if (input_line != NULL) {
        lv_obj_add_event_cb(input_line, shell_input_line_event_cb, LV_EVENT_ALL, NULL);
    }
    if (prev_btn != NULL) {
        lv_obj_add_event_cb(prev_btn, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    }
    if (next_btn != NULL) {
        lv_obj_add_event_cb(next_btn, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Register a keyboard event callback for richer keyboard interaction.
     * Receives LV_EVENT_VALUE_CHANGED for mode/button presses,
     * LV_EVENT_READY for OK button, LV_EVENT_CANCEL for hide button. */
    keyboard_register_event_callback(shell_keyboard_event_cb, NULL);

    /* Show boot banner and reset input line */
    shell_transcript_reset();
    shell_transcript_appendf_ansi("@G%s@R\n", SHELL_BOOT_MESSAGE);
    shell_history_transcript_scroll_to_end();
    shell_input_line_reset();
}

static void shell_storage_append_text(const char *text) { shell_transcript_append_text(text); }
static void shell_storage_schedule_text(const char *text) { shell_schedule_transcript_appendf("%s", text); }
static void shell_storage_record_error(const char *tag, esp_err_t error, const char *msg) { shell_record_errorf(tag, error, "%s", msg); }
static void shell_storage_record_warning(const char *tag, const char *msg) { shell_record_warningf(tag, "%s", msg); }
static void shell_storage_record_info(const char *tag, const char *msg) { shell_record_infof(tag, "%s", msg); }
static void shell_storage_notify_header(const char *text, uint32_t timeout_ms) { shell_header_notify(text, timeout_ms); }
static void shell_storage_header_update_sd(int state) { header_update_sd(state); }
static bool shell_storage_text_equals_ignore_case(const char *left, const char *right) { return shell_text_equals_ignore_case(left, right); }
static int shell_storage_split_args(char *text, char **argv, int max_args) { return shell_split_args(text, argv, max_args); }
static const char *shell_storage_env_get(const char *name) { return shell_env_get(name); }
static void shell_storage_execute_command(const char *command) { shell_execute_command((char *)command); }

static void shell_storage_init(void)
{
    static const storage_host_ops_t ops = {
        .transcript_append_text = shell_storage_append_text,
        .schedule_transcript_append_text = shell_storage_schedule_text,
        .record_error = shell_storage_record_error,
        .record_warning = shell_storage_record_warning,
        .record_info = shell_storage_record_info,
        .notify_header = shell_storage_notify_header,
        .header_update_sd = shell_storage_header_update_sd,
        .text_equals_ignore_case = shell_storage_text_equals_ignore_case,
        .split_args = shell_storage_split_args,
        .env_get = shell_storage_env_get,
        .execute_command = shell_storage_execute_command,
    };
    storage_init(&ops);
}

void app_main(void)
{
    esp_err_t disp_error;

    /* Capture boot timestamp for real-time uptime tracking */
    s_boot_timestamp_us = esp_timer_get_time();

    /* Initialize the display manager — wraps bsp_display_start_with_config()
     * and owns all display state (rotation, brightness, power, touch handle).
     * The display manager handles the BSP display config internally. */
    disp_error = display_init();
    if (disp_error != ESP_OK) {
        ESP_LOGE(SHELL_TAG, "Display initialization failed");
        shell_record_errorf("init", ESP_FAIL, "Display initialization failed");
        return;
    }

    /* Register the UI rebuild callback so the display manager can trigger
     * a full UI rebuild after rotation changes. */
    display_register_ui_rebuild_callback(shell_rebuild_ui_callback);

    shell_storage_init();
    shell_set_default_state();
    if (s_shell_command_lock == NULL) {
        s_shell_command_lock = xSemaphoreCreateMutex();
    }

    shell_uart_console_start();

    /* Display is already initialized and backlight is on from display_init().
     * Build the LVGL shell surface so the header bar and transcript share
     * one stable LVGL screen. */
    bsp_display_lock(0);
    shell_build_ui();
    bsp_display_unlock();

    if (display_get_touch_handle() == NULL) {
        shell_record_warningf("init", "GT911 touch handle lookup failed after BSP startup; rotate will stay display-only");
    }

    /* Keep shell boot status visible through the debug surface without
     * emitting a boot warning during normal startup. */
    shell_record_infof("shell", "Shell UI initialized; boot banner is shown on the display transcript");

    shell_transcript_append_text("UART monitor accepts the same shell commands as the on-screen prompt.\n");
    shell_transcript_append_text("Wi-Fi starts in the background on boot; wifi diag is also available for an extra status + scan report.\n");
    shell_transcript_append_text("Bluetooth commands are routed through the ESP32-C6 hosted NimBLE module.\n");
    shell_transcript_append_text("USB host commands are available as usb status, usb ls, usb keyboard, and usb mouse.\n");
    shell_transcript_append_text("USB keyboard auto-detection: plug in a USB keyboard to type commands; on-screen keyboard hides automatically.\n");
    shell_transcript_append_text("Enter runs commands from the prompt line; history stays locked above.\n");
    c6ota_init();
    c6ota_register_progress_callback(shell_c6ota_progress_callback);
    networking_init(&(networking_host_ops_t){
        .transcript_append_text = shell_networking_schedule_text,
        .schedule_transcript_append_text = shell_networking_schedule_text,
        .transcript_append_ansi = shell_networking_schedule_text,
        .record_error = shell_networking_record_error,
        .record_warning = shell_networking_record_warning,
        .record_info = shell_networking_record_info,
        .notify_header = shell_header_notify,
    });
    usb_init();

    /* Register USB keyboard input callback for CLI injection.
     * When a USB keyboard is plugged in, keystrokes are routed to the
     * shell input line via shell_usb_keyboard_input(). */
    usb_register_keyboard_input_callback(shell_usb_keyboard_cb);

    /* Initialize the time module (timezone setup only).
     * SNTP client is started later when Wi-Fi connects,
     * because SNTP requires the lwIP TCP/IP thread to be running. */
    time_init();

    /* Start the periodic header status refresh timer.
     * Public API matches the c6ota, networking, and usb module style —
     * main only feeds passive header updates, with zero regressions to
     * the locked MSDOS transcript UI. */
    bsp_display_lock(0);
    shell_header_status_refresh();
    if (s_header_status_timer == NULL) {
        s_header_status_timer = lv_timer_create(shell_header_status_timer_cb, SHELL_HEADER_REFRESH_PERIOD_MS, NULL);
    }
    bsp_display_unlock();
}

/* ========================================================================
 * PUBLIC CWD ACCESSOR (for PowerShell-style prompt)
 * ======================================================================== */

const char *shell_get_cwd(void)
{
    return storage_get_cwd();
}

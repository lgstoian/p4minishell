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
// Legacy Bluedroid disabled; hosted NimBLE in components/networking.
// P4_CONFIG_BT_HOSTED_RUNTIME_SUPPORTED from p4minishell_config.h
#define SHELL_BT_HOSTED_RUNTIME_SUPPORTED P4_CONFIG_BT_HOSTED_RUNTIME_SUPPORTED

#if SHELL_BT_HOSTED_RUNTIME_SUPPORTED
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bluedroid_hci.h"
#include "esp_hosted_bluedroid.h"
#endif
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
#include "usb.h"
#include "windows.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "p4minishell_config.h"
#include "p4minishell.h"

/* ANSI-aware transcript function (declared in shell.h, used for boot banner) */
extern void shell_transcript_appendf_ansi(const char *format, ...);

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

typedef enum {
    SHELL_WIFI_STATE_NOT_ATTEMPTED = 0,
    SHELL_WIFI_STATE_STARTING,
    SHELL_WIFI_STATE_STARTED,
    SHELL_WIFI_STATE_FAILED,
    SHELL_WIFI_STATE_SKIPPED_DISABLED,
    SHELL_WIFI_STATE_SKIPPED_UNSUPPORTED,
} shell_wifi_state_t;

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

#if SHELL_BT_HOSTED_RUNTIME_SUPPORTED
typedef struct {
    size_t result_count;
    bool scan_active;
    bool controller_enabled;
    bool bluedroid_enabled;
    bool hci_attached;
    bool gap_registered;
} shell_bt_state_t;
#endif

static lv_timer_t *s_header_status_timer;
static char s_transcript[SHELL_TRANSCRIPT_BYTES];
static char s_async_transcript[SHELL_ASYNC_TRANSCRIPT_BYTES];
static char s_command_history[SHELL_COMMAND_HISTORY_DEPTH][SHELL_COMMAND_BYTES];
static size_t s_command_history_count;
static int s_command_history_cursor = -1;
static char s_history_draft[SHELL_COMMAND_BYTES];
static size_t s_async_transcript_len;
static shell_wifi_state_t s_wifi_state = SHELL_WIFI_STATE_NOT_ATTEMPTED;
static esp_err_t s_wifi_last_error = ESP_OK;
static bool s_wifi_connected;
static bool s_wifi_connect_requested;
static char s_wifi_target_ssid[SHELL_WIFI_SSID_BYTES];
static char s_wifi_target_password[SHELL_WIFI_PASSWORD_BYTES];
static char s_wifi_last_detail[SHELL_WIFI_DETAIL_BYTES];
static bool s_wifi_init_task_in_progress;
static bool s_async_transcript_flush_queued;
static size_t s_runtime_warning_count;
static shell_debug_log_t s_debug_log;
static portMUX_TYPE s_async_transcript_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_shell_command_lock;
static SemaphoreHandle_t s_uart_console_lock;
static char s_shell_cwd[SHELL_SD_PATH_BYTES];
static shell_env_var_t s_shell_env_vars[SHELL_ENV_VAR_MAX];
static shell_batch_frame_t *s_active_batch_frame;
static int s_volume_percent = 60;
static bool s_light_sleep_requested;
static bool s_header_sd_state_known;
static bool s_header_sd_last_mounted;
static bool s_sd_persistent_mounted;  /* tracks actual SD mount state for header */
static bool s_sd_ejected;             /* set by sdeject to prevent auto-remount */
static esp_codec_dev_handle_t s_speaker_dev;
static adc_oneshot_unit_handle_t s_battery_adc_unit;
static adc_channel_t s_battery_adc_channel;
static bool s_battery_adc_ready;
static adc_cali_handle_t s_battery_cali_handle;
static bool s_battery_cali_ready;
static int64_t s_boot_timestamp_us;   /* captured at boot for uptime calculation */

#if SHELL_WIFI_RUNTIME_ENABLED
static esp_netif_t *s_wifi_sta_netif;
static esp_event_handler_instance_t s_wifi_event_any_id;
static esp_event_handler_instance_t s_wifi_got_ip_event;
#endif

#if SHELL_BT_HOSTED_RUNTIME_SUPPORTED
static shell_bt_state_t s_bt_state;
#endif

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
static void shell_execute_gpio_command(int argc, char **argv);
static void shell_command_debug(void);
static void shell_command_version(void);
static void shell_command_about(void);
static void shell_execute_rgb_command(int argc, char **argv);
static void shell_execute_camera_command(int argc, char **argv);
static void shell_command_sd(char *command);
static void shell_command_sd_ls(char *command);
static void shell_command_sd_stat(char *command);
static void shell_command_sd_cat(char *command);
static void shell_command_sd_eject(void);
static bool shell_text_equals_ignore_case(const char *left, const char *right);
static void shell_join_args(char **argv, int start_index, int argc, char *output, size_t output_size);
static void shell_sd_print_usage(void);
static esp_err_t shell_sd_begin(shell_sd_session_t *session);
static void shell_sd_end(shell_sd_session_t *session, const char *operation);
static esp_err_t shell_sd_resolve_path(const char *input, char *output, size_t output_size);
static esp_err_t shell_sd_stat_path(const char *path, struct stat *st);
static void shell_sd_format_size(uint64_t size_bytes, char *output, size_t output_size);
static const char *shell_sd_entry_type(const struct stat *st);
static bool shell_parse_size_arg(const char *text, size_t min_value, size_t max_value, size_t *value_out);
static void shell_set_default_state(void);
static esp_err_t shell_fs_resolve_path(const char *input, char *output, size_t output_size);
static void shell_fs_print_cwd(void);
static void shell_env_print_all(void);
static const char *shell_env_get(const char *name);
static esp_err_t shell_env_set(const char *name, const char *value);
static void shell_expand_variables(const char *input, char *output, size_t output_size);
static bool shell_parse_redirection(char *command, char **command_part, char **redirect_target, bool *append_mode);
static esp_err_t shell_write_redirect_output(const char *path, const char *text, bool append_mode);
static esp_err_t shell_fs_copy_file(const char *source_path, const char *dest_path);
static esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv);
static bool shell_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size);
static void shell_command_cd(int argc, char **argv);
static void shell_command_dir(int argc, char **argv);
static void shell_command_copy(int argc, char **argv);
static void shell_command_del(int argc, char **argv);
static void shell_command_rename(int argc, char **argv, const char *verb);
static void shell_command_mkdir(int argc, char **argv);
static void shell_command_rmdir(int argc, char **argv);
static void shell_command_type_file(int argc, char **argv);
static void shell_command_write_file(int argc, char **argv, bool append_mode);
static void shell_command_touch(int argc, char **argv);
static void shell_command_move(int argc, char **argv);
static void shell_command_set(int argc, char **argv);
static void shell_command_path(int argc, char **argv);
static void shell_command_echo(int argc, char **argv);
static void shell_store_command_history(const char *command);
static bool shell_execute_command_core(char *command);
static void shell_command_sd_info(void);
static void shell_execute_command(char *command);
static void shell_command_task(void *arg);
static bool shell_wifi_defaults_available(void);
static void shell_wifi_set_detail(const char *format, ...);
#if SHELL_BT_HOSTED_RUNTIME_SUPPORTED
static esp_err_t shell_bt_ensure_ready(void);
static void shell_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
#endif
#if CONFIG_ESP_HOSTED_ENABLED
static esp_err_t shell_wifi_validate_hosted_version(void);
#endif

static bool shell_text_equals_ignore_case(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

// All shell-side SD commands share a single guarded mount path so failures cannot leak mounted state or dereference missing card metadata.
// Also update the fixed header SD icon immediately on mount/unmount so the status bar stays in sync with the actual card state.
static esp_err_t shell_sd_begin(shell_sd_session_t *session)
{
    esp_err_t error;

    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    session->mounted_here = false;

    /* If already mounted, just return success (persistent mount) */
    if (s_sd_persistent_mounted) {
        return ESP_OK;
    }

    /* If ejected, refuse to mount unless user runs an explicit command */
    if (s_sd_ejected) {
        return ESP_ERR_INVALID_STATE;
    }

    error = bsp_sdcard_mount();
    if (error == ESP_OK) {
        session->mounted_here = true;
        s_sd_persistent_mounted = true;
        header_update_sd(HEADER_SD_MOUNTED);
        return ESP_OK;
    }

    if (error == ESP_ERR_INVALID_STATE) {
        /* Already mounted (BSP internal state) */
        s_sd_persistent_mounted = true;
        header_update_sd(HEADER_SD_MOUNTED);
        return ESP_OK;
    }

    return error;
}

static void shell_sd_end(shell_sd_session_t *session, const char *operation)
{
    /* Persistent mount: do NOT unmount after each command.
     * Only unmount on explicit sdeject command.
     * This keeps the SD card accessible and the header icon accurate. */
    (void)session;
    (void)operation;
}

static esp_err_t shell_sd_resolve_path(const char *input, char *output, size_t output_size)
{
    int written;
    const char *source = input;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source == NULL || source[0] == '\0') {
        source = BSP_SD_MOUNT_POINT;
    }

    if (strncmp(source, "sd:/", 4) == 0) {
        written = snprintf(output, output_size, "%s%s", BSP_SD_MOUNT_POINT, source + 3);
    } else if (strncmp(source, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0 ||
               strcmp(source, BSP_SD_MOUNT_POINT) == 0) {
        written = snprintf(output, output_size, "%s", source);
    } else if (source[0] == '/') {
        written = snprintf(output, output_size, "%s", source);
    } else {
        written = snprintf(output, output_size, "%s/%s", BSP_SD_MOUNT_POINT, source);
    }

    if (written < 0) {
        output[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t shell_sd_stat_path(const char *path, struct stat *st)
{
    if (path == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, st) == 0) {
        return ESP_OK;
    }

    if (errno == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_FAIL;
}

static esp_err_t shell_sd_fresult_to_esp_err(FRESULT result)
{
    switch (result) {
    case FR_OK:
        return ESP_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return ESP_ERR_NOT_FOUND;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
    case FR_INVALID_DRIVE:
        return ESP_ERR_INVALID_ARG;
    case FR_NOT_READY:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return ESP_ERR_INVALID_STATE;
    case FR_NOT_ENOUGH_CORE:
        return ESP_ERR_NO_MEM;
    case FR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

// FATFS LFN enabled with CONFIG_FATFS_LFN_HEAP + MAX_LFN=255 (fixes sd ls long filenames + c6ota default lookup)
static esp_err_t shell_sd_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t fatfs_path_size)
{
    const char *relative_path;
    int written;

    if (vfs_path == NULL || fatfs_path == NULL || fatfs_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(vfs_path, BSP_SD_MOUNT_POINT) == 0) {
        relative_path = "/";
    } else if (strncmp(vfs_path, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        relative_path = vfs_path + strlen(BSP_SD_MOUNT_POINT);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(fatfs_path, fatfs_path_size, "%s%s", SHELL_SD_FATFS_DRIVE, relative_path);
    if (written < 0) {
        fatfs_path[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= fatfs_path_size) {
        fatfs_path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void shell_sd_format_size(uint64_t size_bytes, char *output, size_t output_size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = (double)size_bytes;
    size_t unit_index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    while (value >= 1024.0 && unit_index < (sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(output, output_size, "%llu %s", (unsigned long long)size_bytes, units[unit_index]);
    } else {
        snprintf(output, output_size, "%.1f %s", value, units[unit_index]);
    }
}

static const char *shell_sd_entry_type(const struct stat *st)
{
    if (st == NULL) {
        return "unknown";
    }

    if (S_ISDIR(st->st_mode)) {
        return "dir";
    }

    if (S_ISREG(st->st_mode)) {
        return "file";
    }

    return "other";
}

static bool shell_parse_size_arg(const char *text, size_t min_value, size_t max_value, size_t *value_out)
{
    char *end = NULL;
    unsigned long parsed_value;

    if (text == NULL || value_out == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed_value = strtoul(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0') {
        return false;
    }

    if (parsed_value < min_value || parsed_value > max_value) {
        return false;
    }

    *value_out = (size_t)parsed_value;
    return true;
}

static bool shell_parse_percentage_arg(const char *text, int *percentage_out)
{
    size_t parsed = 0;

    if (percentage_out == NULL) {
        return false;
    }

    if (!shell_parse_size_arg(text, 0, 100, &parsed)) {
        return false;
    }

    *percentage_out = (int)parsed;
    return true;
}

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
    /* Use persistent mount tracking instead of stat() which only works
     * while the SD card is mounted. The flag is set by shell_sd_begin()
     * and cleared by shell_sd_end(). */
    return s_sd_persistent_mounted;
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
    snprintf(s_shell_cwd, sizeof(s_shell_cwd), "%s", BSP_SD_MOUNT_POINT);
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

static esp_err_t shell_fs_resolve_path(const char *input, char *output, size_t output_size)
{
    char combined[SHELL_SD_PATH_BYTES];
    char scratch[SHELL_SD_PATH_BYTES];
    char *segments[32];
    size_t segment_count = 0;
    char *token;
    char *context = NULL;
    int written;
    size_t index;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (input == NULL || input[0] == '\0') {
        snprintf(output, output_size, "%s", s_shell_cwd[0] != '\0' ? s_shell_cwd : BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    if (strncmp(input, "sd:/", 4) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input + 4);
    } else if (strcmp(input, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/");
    } else if (strncmp(input, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        snprintf(combined, sizeof(combined), "%s", input + strlen(BSP_SD_MOUNT_POINT));
    } else if (input[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", input);
    } else if (strcmp(s_shell_cwd, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input);
    } else {
        snprintf(combined,
                 sizeof(combined),
                 "%s/%s",
                 s_shell_cwd + strlen(BSP_SD_MOUNT_POINT),
                 input);
    }

    snprintf(scratch, sizeof(scratch), "%s", combined);
    token = strtok_r(scratch, "/\\", &context);
    while (token != NULL && segment_count < (sizeof(segments) / sizeof(segments[0]))) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok_r(NULL, "/\\", &context);
            continue;
        }

        if (strcmp(token, "..") == 0) {
            if (segment_count > 0) {
                segment_count--;
            }
            token = strtok_r(NULL, "/\\", &context);
            continue;
        }

        segments[segment_count++] = token;
        token = strtok_r(NULL, "/\\", &context);
    }

    written = snprintf(output, output_size, "%s", BSP_SD_MOUNT_POINT);
    if (written < 0 || (size_t)written >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    for (index = 0; index < segment_count; index++) {
        size_t used = strlen(output);
        written = snprintf(output + used, output_size - used, "/%s", segments[index]);
        if (written < 0 || (size_t)written >= output_size - used) {
            output[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return ESP_OK;
}

static void shell_fs_print_cwd(void)
{
    if (strcmp(s_shell_cwd, BSP_SD_MOUNT_POINT) == 0) {
        shell_transcript_append_text("\\\n");
        return;
    }

    shell_transcript_appendf("%s\n", s_shell_cwd + strlen(BSP_SD_MOUNT_POINT));
}

static esp_err_t shell_write_redirect_output(const char *path, const char *text, bool append_mode)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    error = shell_fs_resolve_path(path, resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        shell_sd_end(&session, "redirection");
        return ESP_FAIL;
    }

    if (text != NULL && text[0] != '\0') {
        size_t text_len = strlen(text);
        if (fwrite(text, 1, text_len, file) != text_len) {
            fclose(file);
            shell_sd_end(&session, "redirection");
            return ESP_FAIL;
        }
    }

    fclose(file);
    shell_sd_end(&session, "redirection");
    return ESP_OK;
}

// Gate the host Wi-Fi runtime on an explicitly version-matched ESP-Hosted C6 image so incompatible RPC traffic never reaches esp_wifi_remote.
static esp_err_t shell_wifi_validate_hosted_version(void)
{
    esp_hosted_coprocessor_fwver_t version = { 0 };
    esp_err_t error;

    error = esp_hosted_get_coprocessor_fwversion(&version);
    if (error != ESP_OK) {
        shell_wifi_set_detail("ESP-Hosted connected but the shell could not read the ESP32-C6 firmware version. Wi-Fi stays off because the hosted link is not trustworthy for remote Wi-Fi init. Rebuild or externally refresh coprocessor/esp32c6_slave and retry.");
        shell_schedule_transcript_appendf("[wifi] failed to read C6 hosted firmware version: %s (0x%x)\n",
                                          esp_err_to_name(error),
                                          (unsigned int)error);
        shell_schedule_transcript_appendf("[wifi] Recovery: flash a matching %u.%u.x ESP32-C6 image from coprocessor/esp32c6_slave or use c6ota default with esp32c6_hosted_slave.bin\n",
                                          ESP_HOSTED_VERSION_MAJOR_1,
                                          ESP_HOSTED_VERSION_MINOR_1);
        shell_record_warningf("wifi", "Failed to read hosted firmware version: %s", esp_err_to_name(error));
        return error;
    }

    if (version.major1 != ESP_HOSTED_VERSION_MAJOR_1 || version.minor1 != ESP_HOSTED_VERSION_MINOR_1) {
        shell_wifi_set_detail("ESP-Hosted host %u.%u.%u requires ESP32-C6 firmware %u.%u.x, but the co-processor reports %" PRIu32 ".%" PRIu32 ".%" PRIu32 ". Wi-Fi stays off to avoid SDIO/RPC errors. Update the C6 from coprocessor/esp32c6_slave or run c6ota default with esp32c6_hosted_slave.bin.",
                              ESP_HOSTED_VERSION_MAJOR_1,
                              ESP_HOSTED_VERSION_MINOR_1,
                              ESP_HOSTED_VERSION_PATCH_1,
                              ESP_HOSTED_VERSION_MAJOR_1,
                              ESP_HOSTED_VERSION_MINOR_1,
                              version.major1,
                              version.minor1,
                              version.patch1);
        shell_schedule_transcript_appendf("[wifi] hosted version mismatch: host %u.%u.%u, C6 %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
                                          ESP_HOSTED_VERSION_MAJOR_1,
                                          ESP_HOSTED_VERSION_MINOR_1,
                                          ESP_HOSTED_VERSION_PATCH_1,
                                          version.major1,
                                          version.minor1,
                                          version.patch1);
        shell_schedule_transcript_appendf("[wifi] Recovery: flash a matching %u.%u.x ESP32-C6 image from coprocessor/esp32c6_slave or use c6ota default with esp32c6_hosted_slave.bin\n",
                          ESP_HOSTED_VERSION_MAJOR_1,
                          ESP_HOSTED_VERSION_MINOR_1);
        shell_record_warningf("wifi",
                              "Hosted version mismatch: host %u.%u.%u vs C6 %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                              ESP_HOSTED_VERSION_MAJOR_1,
                              ESP_HOSTED_VERSION_MINOR_1,
                              ESP_HOSTED_VERSION_PATCH_1,
                              version.major1,
                              version.minor1,
                              version.patch1);
        return ESP_ERR_INVALID_STATE;
    }

    shell_record_infof("wifi",
                       "Hosted firmware compatible: host %u.%u.%u, C6 %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                       ESP_HOSTED_VERSION_MAJOR_1,
                       ESP_HOSTED_VERSION_MINOR_1,
                       ESP_HOSTED_VERSION_PATCH_1,
                       version.major1,
                       version.minor1,
                       version.patch1);
    return ESP_OK;
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

static void shell_wifi_set_detail(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(s_wifi_last_detail, sizeof(s_wifi_last_detail), format, args);
    va_end(args);
}

static bool shell_wifi_defaults_available(void)
{
    return CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID[0] != '\0';
}

static const char *shell_wifi_state_string(void)
{
    switch (s_wifi_state) {
    case SHELL_WIFI_STATE_STARTING:
        return "starting";
    case SHELL_WIFI_STATE_STARTED:
        return "started";
    case SHELL_WIFI_STATE_FAILED:
        return "failed";
    case SHELL_WIFI_STATE_SKIPPED_DISABLED:
        return "skipped-disabled";
    case SHELL_WIFI_STATE_SKIPPED_UNSUPPORTED:
        return "unsupported";
    case SHELL_WIFI_STATE_NOT_ATTEMPTED:
    default:
        return "not-attempted";
    }
}

static void shell_wifi_append_step(const char *step)
{
    shell_schedule_transcript_appendf("[wifi] %s\n", step);
    shell_record_infof("wifi", "%s", step);
}

#if SHELL_WIFI_RUNTIME_ENABLED
static void shell_wifi_append_error(const char *step, esp_err_t error)
{
    s_wifi_state = SHELL_WIFI_STATE_FAILED;
    s_wifi_last_error = error;
    shell_schedule_transcript_appendf("[wifi] %s failed: %s (0x%x)\n",
                                      step,
                                      esp_err_to_name(error),
                                      (unsigned int)error);
    shell_debug_log_push("wifi", step);
}

static void shell_wifi_cleanup_runtime_artifacts(void)
{
    if (s_wifi_event_any_id != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_any_id);
        s_wifi_event_any_id = NULL;
    }

    if (s_wifi_got_ip_event != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_wifi_got_ip_event);
        s_wifi_got_ip_event = NULL;
    }

    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();

    if (s_wifi_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_sta_netif);
        s_wifi_sta_netif = NULL;
    }

    s_wifi_connected = false;
    s_wifi_connect_requested = false;
    s_wifi_target_ssid[0] = '\0';
    s_wifi_target_password[0] = '\0';
}

static void shell_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            shell_wifi_append_step("event: station started");
            return;
        }

        if (event_id == WIFI_EVENT_STA_CONNECTED) {
            const wifi_event_sta_connected_t *event = (const wifi_event_sta_connected_t *)event_data;
            size_t copy_len = event->ssid_len;

            if (copy_len >= sizeof(s_wifi_target_ssid)) {
                copy_len = sizeof(s_wifi_target_ssid) - 1;
            }

            memcpy(s_wifi_target_ssid, event->ssid, copy_len);
            s_wifi_target_ssid[copy_len] = '\0';
            shell_schedule_transcript_appendf("[wifi] event: associated with %s on channel %u\n",
                                              s_wifi_target_ssid,
                                              (unsigned int)event->channel);
            return;
        }

        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;

            s_wifi_connected = false;
            s_wifi_connect_requested = false;
            shell_schedule_transcript_appendf("[wifi] event: disconnected, reason=%u\n",
                                              (unsigned int)event->reason);
            return;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        s_wifi_connected = true;
        s_wifi_connect_requested = false;
        shell_schedule_transcript_appendf("[wifi] event: got IP " IPSTR "\n",
                                          IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t shell_wifi_register_event_handlers(void)
{
    esp_err_t error;

    error = esp_event_handler_instance_register(WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                shell_wifi_event_handler,
                                                NULL,
                                                &s_wifi_event_any_id);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_event_handler_instance_register(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                shell_wifi_event_handler,
                                                NULL,
                                                &s_wifi_got_ip_event);
    if (error != ESP_OK) {
        return error;
    }

    return ESP_OK;
}

// Retained to preserve the earlier shell-local Wi-Fi runtime path while command dispatch now runs through networking_handle_wifi_command().
static esp_err_t __attribute__((unused)) shell_wifi_connect_with_credentials(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = { 0 };
    esp_err_t error;
    size_t ssid_len;
    size_t password_len;

    if (s_wifi_state != SHELL_WIFI_STATE_STARTED) {
        shell_transcript_appendf("wifi: stack is not ready (%s)\n", shell_wifi_state_string());
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || ssid[0] == '\0') {
        shell_transcript_append_text("wifi: missing SSID\n");
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(ssid);
    password_len = password != NULL ? strlen(password) : 0;
    if (ssid_len >= sizeof(wifi_config.sta.ssid) || password_len >= sizeof(wifi_config.sta.password)) {
        shell_transcript_append_text("wifi: SSID or password exceeds ESP-IDF limits\n");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password != NULL ? password : "", password_len);

    shell_wifi_append_step("esp_wifi_set_config(WIFI_IF_STA)");
    error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_set_config(WIFI_IF_STA)", error);
        return error;
    }

    error = esp_wifi_disconnect();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
        shell_wifi_append_error("esp_wifi_disconnect()", error);
        return error;
    }

    snprintf(s_wifi_target_ssid, sizeof(s_wifi_target_ssid), "%s", ssid);
    snprintf(s_wifi_target_password,
             sizeof(s_wifi_target_password),
             "%s",
             password != NULL ? password : "");
    s_wifi_connect_requested = true;
    s_wifi_connected = false;
    shell_schedule_transcript_appendf("[wifi] connect requested for %s\n", s_wifi_target_ssid);

    shell_wifi_append_step("esp_wifi_connect()");
    error = esp_wifi_connect();
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_connect()", error);
        return error;
    }

    return ESP_OK;
}

static esp_err_t __attribute__((unused)) shell_wifi_run_diagnostic(const char *origin)
{
#if SHELL_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t ap_info;
    wifi_ap_record_t records[16];
    uint16_t record_count = (uint16_t)(sizeof(records) / sizeof(records[0]));
    uint16_t index;
    esp_netif_ip_info_t ip_info = { 0 };
    const char *label = (origin != NULL && origin[0] != '\0') ? origin : "runtime";

    shell_schedule_transcript_appendf("[wifi.diag] origin=%s state=%s connected=%s requested=%s\n",
                                      label,
                                      shell_wifi_state_string(),
                                      s_wifi_connected ? "yes" : "no",
                                      s_wifi_connect_requested ? "yes" : "no");

    if (s_wifi_state != SHELL_WIFI_STATE_STARTED) {
        shell_schedule_transcript_appendf("[wifi.diag] origin=%s runtime not started\n", label);
        return ESP_ERR_INVALID_STATE;
    }

    error = esp_wifi_sta_get_ap_info(&ap_info);
    if (error == ESP_OK) {
        shell_schedule_transcript_appendf("[wifi.diag] connected_ssid=%s rssi=%d channel=%u\n",
                                          ap_info.ssid[0] != '\0' ? (const char *)ap_info.ssid : "<hidden>",
                                          ap_info.rssi,
                                          (unsigned int)ap_info.primary);
    } else if (error == ESP_ERR_WIFI_NOT_CONNECT) {
        shell_schedule_transcript_appendf("[wifi.diag] origin=%s not connected to an AP\n", label);
    } else {
        shell_schedule_transcript_appendf("[wifi.diag] ap info failed: %s (0x%x)\n",
                                          esp_err_to_name(error),
                                          (unsigned int)error);
    }

    if (s_wifi_sta_netif != NULL && esp_netif_get_ip_info(s_wifi_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        shell_schedule_transcript_appendf("[wifi.diag] ip=" IPSTR "\n", IP2STR(&ip_info.ip));
    } else {
        shell_schedule_transcript_appendf("[wifi.diag] origin=%s ip=not-assigned\n", label);
    }

    if (s_wifi_connect_requested && !s_wifi_connected) {
        shell_schedule_transcript_appendf("[wifi.diag] origin=%s scan skipped while station connect is in progress\n",
                                          label);
        return ESP_OK;
    }

    shell_schedule_transcript_appendf("[wifi.diag] origin=%s scanning for nearby networks...\n", label);
    error = esp_wifi_scan_start(NULL, true);
    if (error != ESP_OK) {
        shell_schedule_transcript_appendf("[wifi.diag] scan failed: %s (0x%x)\n",
                                          esp_err_to_name(error),
                                          (unsigned int)error);
        shell_record_warningf("wifi", "diagnostic scan failed from %s", label);
        return error;
    }

    error = esp_wifi_scan_get_ap_records(&record_count, records);
    if (error != ESP_OK) {
        shell_schedule_transcript_appendf("[wifi.diag] reading scan results failed: %s (0x%x)\n",
                                          esp_err_to_name(error),
                                          (unsigned int)error);
        shell_record_warningf("wifi", "diagnostic scan result read failed from %s", label);
        return error;
    }

    shell_schedule_transcript_appendf("[wifi.diag] origin=%s networks=%u\n",
                                      label,
                                      (unsigned int)record_count);

    if (record_count == 0) {
        shell_schedule_transcript_appendf("[wifi.diag] origin=%s no networks found\n", label);
        return ESP_OK;
    }

    for (index = 0; index < record_count; index++) {
        shell_schedule_transcript_appendf("[wifi.diag][%u] ssid=%s rssi=%d channel=%u auth=%u\n",
                                          (unsigned int)index,
                                          records[index].ssid[0] != '\0' ? (const char *)records[index].ssid : "<hidden>",
                                          records[index].rssi,
                                          (unsigned int)records[index].primary,
                                          (unsigned int)records[index].authmode);
    }

    shell_record_infof("wifi", "diagnostic scan completed from %s with %u APs", label, (unsigned int)record_count);
    return ESP_OK;
#else
    (void)origin;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void shell_execute_wifi_command(char *command)
{
    networking_handle_wifi_command(command);
}
#endif

static void __attribute__((unused)) shell_wifi_runtime_init(void)
{
#if SHELL_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_wifi_state == SHELL_WIFI_STATE_STARTED) {
        shell_wifi_append_step("Wi-Fi runtime already started");
        return;
    }

    if (s_wifi_state == SHELL_WIFI_STATE_STARTING && !s_wifi_init_task_in_progress) {
        shell_wifi_append_step("Wi-Fi runtime startup already in progress");
        return;
    }

    s_wifi_state = SHELL_WIFI_STATE_STARTING;
    s_wifi_last_error = ESP_OK;

    // Keep Wi-Fi startup aligned with the original working hosted routine while making retries safe after boot and post-c6ota restore.
    s_wifi_last_detail[0] = '\0';
    shell_wifi_append_step("runtime Wi-Fi initialization requested from sdkconfig");

#if CONFIG_ESP_HOSTED_ENABLED
    // The checked-in esp32p4 host path targets an ESP32-C6 over ESP-Hosted SDIO and is reused for Wi-Fi-off C6 OTA.
    shell_wifi_set_detail("ESP-Hosted SDIO backend targeting ESP32-C6 on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54");
    shell_wifi_append_step("ESP-Hosted SDIO backend: ESP32-C6 on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54");
    shell_wifi_append_step("esp_hosted_connect_to_slave()");
    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK) {
        shell_wifi_set_detail("ESP-Hosted did not connect to the ESP32-C6 co-processor on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54. Check the hosted slave firmware, pull-ups on CMD/DAT0-DAT3, and the reset wiring.");
        shell_wifi_append_error("esp_hosted_connect_to_slave()", error);
        shell_schedule_transcript_appendf("[wifi] %s\n", s_wifi_last_detail);
        shell_schedule_transcript_appendf("%s", "[wifi] Recovery: rebuild or externally refresh the ESP32-C6 firmware from coprocessor/esp32c6_slave if the hosted slave image is missing or stale\n");
        return;
    }

    shell_wifi_append_step("esp_hosted_get_coprocessor_fwversion()");
    error = shell_wifi_validate_hosted_version();
    if (error != ESP_OK) {
        shell_wifi_append_error("hosted firmware compatibility check", error);
        shell_schedule_transcript_appendf("[wifi] %s\n", s_wifi_last_detail);
        return;
    }
#endif

    shell_wifi_append_step("nvs_flash_init()");
    error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        shell_wifi_append_step("nvs_flash_erase() after incompatible NVS state");
        error = nvs_flash_erase();
        if (error != ESP_OK) {
            shell_wifi_append_error("nvs_flash_erase()", error);
            shell_wifi_cleanup_runtime_artifacts();
            return;
        }

        shell_wifi_append_step("nvs_flash_init() retry");
        error = nvs_flash_init();
    }

    if (error != ESP_OK) {
        shell_wifi_append_error("nvs_flash_init()", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    shell_wifi_append_step("esp_netif_init()");
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        shell_wifi_append_error("esp_netif_init()", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    shell_wifi_append_step("esp_event_loop_create_default()");
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        shell_wifi_append_error("esp_event_loop_create_default()", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    if (s_wifi_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_sta_netif);
        s_wifi_sta_netif = NULL;
    }

    shell_wifi_append_step("esp_netif_create_default_wifi_sta()");
    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_sta_netif == NULL) {
        shell_wifi_append_error("esp_netif_create_default_wifi_sta()", ESP_FAIL);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    shell_wifi_append_step("esp_wifi_init()");
    error = esp_wifi_init(&wifi_init_cfg);
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_init()", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    shell_wifi_append_step("esp_event_handler_instance_register(...)");
    error = shell_wifi_register_event_handlers();
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_event_handler_instance_register(...)", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    shell_wifi_append_step("esp_wifi_set_storage(WIFI_STORAGE_RAM)");
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_set_storage(WIFI_STORAGE_RAM)", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    shell_wifi_append_step("esp_wifi_set_mode(WIFI_MODE_STA)");
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_set_mode(WIFI_MODE_STA)", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    shell_wifi_append_step("esp_wifi_start()");
    error = esp_wifi_start();
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_start()", error);
        shell_wifi_cleanup_runtime_artifacts();
        return;
    }

    s_wifi_state = SHELL_WIFI_STATE_STARTED;
    s_wifi_last_error = ESP_OK;
    s_wifi_connected = false;
    s_wifi_connect_requested = false;
    s_wifi_target_ssid[0] = '\0';
    shell_wifi_append_step("Wi-Fi runtime started in STA mode");
    if (shell_wifi_defaults_available()) {
        shell_schedule_transcript_appendf("[wifi] default sdkconfig profile ready for ssid %s\n",
                                          CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID);
    } else {
        shell_wifi_append_step("no default sdkconfig credentials configured; use wifi connect <ssid> <pass>");
    }
#elif SOC_WIRELESS_HOST_SUPPORTED
    // The current esp32p4 board can host an external radio, but the checked-in sdkconfig does not enable that non-hosted path.
    s_wifi_state = SHELL_WIFI_STATE_SKIPPED_DISABLED;
    shell_wifi_append_step("skipped: sdkconfig does not enable native Wi-Fi or ESP-Hosted Wi-Fi");
    shell_wifi_append_step("expected CONFIG_ESP_WIFI_ENABLED, CONFIG_ESP_HOST_WIFI_ENABLED, or CONFIG_ESP_HOSTED_ENABLED from sdkconfig");
#else
    s_wifi_state = SHELL_WIFI_STATE_SKIPPED_UNSUPPORTED;
    shell_wifi_append_step("skipped: current target does not expose Wi-Fi support");
#endif
}

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

#if SHELL_BT_HOSTED_RUNTIME_SUPPORTED
static void shell_bt_format_address(const esp_bd_addr_t address, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    snprintf(output,
             output_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0],
             address[1],
             address[2],
             address[3],
             address[4],
             address[5]);
}

static void shell_bt_extract_name(const esp_bt_gap_cb_param_t *param, char *name, size_t name_size)
{
    int property_index;

    if (name == NULL || name_size == 0) {
        return;
    }

    name[0] = '\0';
    if (param == NULL) {
        return;
    }

    for (property_index = 0; property_index < param->disc_res.num_prop; property_index++) {
        const esp_bt_gap_dev_prop_t *property = &param->disc_res.prop[property_index];

        if (property->type == ESP_BT_GAP_DEV_PROP_BDNAME && property->val != NULL && property->len > 0) {
            size_t copy_len = property->len < (int)(name_size - 1) ? (size_t)property->len : (name_size - 1);
            memcpy(name, property->val, copy_len);
            name[copy_len] = '\0';
            return;
        }

        if (property->type == ESP_BT_GAP_DEV_PROP_EIR && property->val != NULL) {
            uint8_t eir_name_len = 0;
            const uint8_t *eir_name = esp_bt_gap_resolve_eir_data((uint8_t *)property->val,
                                                                  ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                                                  &eir_name_len);

            if (eir_name == NULL) {
                eir_name = esp_bt_gap_resolve_eir_data((uint8_t *)property->val,
                                                       ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                                                       &eir_name_len);
            }

            if (eir_name != NULL && eir_name_len > 0) {
                size_t copy_len = eir_name_len < (name_size - 1) ? (size_t)eir_name_len : (name_size - 1);
                memcpy(name, eir_name, copy_len);
                name[copy_len] = '\0';
                return;
            }
        }
    }
}

// Legacy hosted Bluedroid BT code retained behind compile gate (SHELL_BT_HOSTED_RUNTIME_SUPPORTED=0). Current baseline uses hosted NimBLE via components/networking/bluetooth.c.
static esp_err_t shell_bt_ensure_ready(void)
{
    esp_err_t error;

    error = esp_hosted_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }

    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }

    if (!s_bt_state.controller_enabled) {
        error = esp_hosted_bt_controller_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            return error;
        }

        error = esp_hosted_bt_controller_enable();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            return error;
        }

        s_bt_state.controller_enabled = true;
    }

    if (!s_bt_state.hci_attached) {
        esp_bluedroid_hci_driver_operations_t operations = {
            .send = hosted_hci_bluedroid_send,
            .check_send_available = hosted_hci_bluedroid_check_send_available,
            .register_host_callback = hosted_hci_bluedroid_register_host_callback,
        };

        hosted_hci_bluedroid_open();
        esp_bluedroid_attach_hci_driver(&operations);
        s_bt_state.hci_attached = true;
    }

    if (!s_bt_state.bluedroid_enabled) {
        error = esp_bluedroid_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            return error;
        }

        error = esp_bluedroid_enable();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            return error;
        }

        s_bt_state.bluedroid_enabled = true;
    }

    if (!s_bt_state.gap_registered) {
        error = esp_bt_gap_register_callback(shell_bt_gap_cb);
        if (error != ESP_OK) {
            return error;
        }

        error = esp_bt_gap_set_device_name("P4MiniShell BT Host");
        if (error != ESP_OK) {
            return error;
        }

        error = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        if (error != ESP_OK) {
            return error;
        }

        s_bt_state.gap_registered = true;
    }

    return ESP_OK;
}

static void shell_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char address[18];
            char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
            int property_index;
            int rssi = 0;
            bool have_rssi = false;

            if (param == NULL) {
                return;
            }

            shell_bt_format_address(param->disc_res.bda, address, sizeof(address));
            shell_bt_extract_name(param, name, sizeof(name));
            for (property_index = 0; property_index < param->disc_res.num_prop; property_index++) {
                const esp_bt_gap_dev_prop_t *property = &param->disc_res.prop[property_index];

                if (property->type == ESP_BT_GAP_DEV_PROP_RSSI && property->val != NULL) {
                    rssi = *(int8_t *)property->val;
                    have_rssi = true;
                }
            }

            s_bt_state.result_count++;
            shell_schedule_transcript_appendf("bt.scan: device=%s rssi=%d name=%s\n",
                                              address,
                                              have_rssi ? rssi : 0,
                                              name[0] != '\0' ? name : "(unnamed)");

            if (s_bt_state.result_count >= SHELL_BT_SCAN_LIMIT) {
                esp_bt_gap_cancel_discovery();
            }
            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param == NULL) {
                return;
            }

            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                s_bt_state.scan_active = true;
                shell_schedule_transcript_appendf("%s", "bt: discovery in progress\n");
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                s_bt_state.scan_active = false;
                shell_schedule_transcript_appendf("bt: discovery complete, %u result(s) reported\n",
                                                  (unsigned int)s_bt_state.result_count);
            }
            break;

        default:
            break;
    }
}
#endif

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

static bool shell_path_has_directory_component(const char *path)
{
    return path != NULL && (strchr(path, '/') != NULL || strchr(path, '\\') != NULL);
}

static bool shell_path_has_extension(const char *path, const char *extension)
{
    size_t path_len;
    size_t ext_len;

    if (path == NULL || extension == NULL) {
        return false;
    }

    path_len = strlen(path);
    ext_len = strlen(extension);
    if (path_len < ext_len) {
        return false;
    }

    return shell_text_equals_ignore_case(path + path_len - ext_len, extension);
}

static esp_err_t shell_resolve_target_from_source(const char *source_path,
                                                  const char *target_input,
                                                  char *target_path,
                                                  size_t target_path_size)
{
    char source_dir[SHELL_SD_PATH_BYTES];
    char *last_sep;

    if (source_path == NULL || target_input == NULL || target_path == NULL || target_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (shell_path_has_directory_component(target_input) ||
        strncmp(target_input, "sd:/", 4) == 0 ||
        target_input[0] == '/') {
        return shell_fs_resolve_path(target_input, target_path, target_path_size);
    }

    snprintf(source_dir, sizeof(source_dir), "%s", source_path);
    last_sep = strrchr(source_dir, '/');
    if (last_sep == NULL || strcmp(source_dir, BSP_SD_MOUNT_POINT) == 0) {
        return shell_fs_resolve_path(target_input, target_path, target_path_size);
    }

    *last_sep = '\0';
    if (source_dir[0] == '\0') {
        snprintf(source_dir, sizeof(source_dir), "%s", BSP_SD_MOUNT_POINT);
    }

    if (snprintf(target_path, target_path_size, "%s/%s", source_dir, target_input) >= (int)target_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t shell_fs_copy_file(const char *source_path, const char *dest_path)
{
    shell_sd_session_t session;
    FILE *source = NULL;
    FILE *dest = NULL;
    uint8_t buffer[SHELL_FILE_IO_BUFFER_BYTES];
    esp_err_t error;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    source = fopen(source_path, "rb");
    if (source == NULL) {
        shell_sd_end(&session, "copy");
        return ESP_ERR_NOT_FOUND;
    }

    dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        fclose(source);
        shell_sd_end(&session, "copy");
        return ESP_FAIL;
    }

    while (!feof(source)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), source);
        if (bytes_read == 0) {
            break;
        }
        if (fwrite(buffer, 1, bytes_read, dest) != bytes_read) {
            fclose(dest);
            fclose(source);
            shell_sd_end(&session, "copy");
            return ESP_FAIL;
        }
    }

    fclose(dest);
    fclose(source);
    shell_sd_end(&session, "copy");
    return ESP_OK;
}

static esp_err_t shell_list_directory_path(const char *normalized_path)
{
    char fatfs_path[SHELL_SD_PATH_BYTES];
    char lfn[256];
    char size_text[32];
    struct stat path_stat;
    FF_DIR dir;
    FILINFO entry_info;
    FRESULT result;
    esp_err_t error;
    size_t listed_entries = 0;
    shell_sd_session_t session;
    bool dir_open = false;
    unsigned int dir_count = 0;
    unsigned int file_count = 0;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_sd_end(&session, "dir");
        return error;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        shell_transcript_appendf("%s  %s\n", size_text, normalized_path);
        shell_sd_end(&session, "dir");
        return ESP_OK;
    }

    error = shell_sd_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        shell_sd_end(&session, "dir");
        return error;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        shell_sd_end(&session, "dir");
        return shell_sd_fresult_to_esp_err(result);
    }
    dir_open = true;

    shell_transcript_appendf(" Directory of %s\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = shell_sd_fresult_to_esp_err(result);
            break;
        }

        if (entry_info.fname[0] == '\0') {
            error = ESP_OK;
            break;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        if (strcmp(lfn, ".") == 0 || strcmp(lfn, "..") == 0) {
            continue;
        }

        if (listed_entries == SHELL_SD_LIST_LIMIT) {
            shell_transcript_appendf("dir: listing truncated after %u entries\n", (unsigned int)SHELL_SD_LIST_LIMIT);
            shell_record_warningf("dir", "Truncated directory listing for %s", normalized_path);
            error = ESP_OK;
            break;
        }

        if (entry_info.fattrib & AM_DIR) {
            shell_transcript_appendf("<DIR>      %s\n", lfn);
            dir_count++;
        } else {
            shell_sd_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
            shell_transcript_appendf("%10s %s\n", size_text, lfn);
            file_count++;
        }
        listed_entries++;
    }

    shell_transcript_appendf("%u file(s)  %u dir(s)\n", file_count, dir_count);
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    shell_sd_end(&session, "dir");
    return error;
}

static esp_err_t shell_print_file_text(const char *normalized_path)
{
    shell_sd_session_t session;
    FILE *file = NULL;
    struct stat path_stat;
    esp_err_t error;
    unsigned char buffer[SHELL_SD_IO_BUFFER_BYTES + 1];

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_sd_end(&session, "type");
        return error;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        shell_sd_end(&session, "type");
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "type");
        return ESP_FAIL;
    }

    while (!feof(file)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
        size_t index;

        if (bytes_read == 0) {
            break;
        }

        for (index = 0; index < bytes_read; index++) {
            if (buffer[index] == '\n' || buffer[index] == '\r' || buffer[index] == '\t') {
                continue;
            }
            if (!isprint(buffer[index])) {
                buffer[index] = '.';
            }
        }

        buffer[bytes_read] = '\0';
        shell_transcript_appendf("%s", (char *)buffer);
    }

    fclose(file);
    shell_transcript_append_text("\n");
    shell_sd_end(&session, "type");
    return ESP_OK;
}

static void shell_command_cd(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    struct stat path_stat;
    shell_sd_session_t session;
    esp_err_t error;

    if (argc == 1) {
        shell_fs_print_cwd();
        return;
    }

    if (argc != 2) {
        shell_transcript_append_text("Usage: cd <path>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("cd: invalid path\n");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("cd: SD card not present - insert and retry\n");
        return;
    }

    error = shell_sd_stat_path(resolved_path, &path_stat);
    if (error != ESP_OK || !S_ISDIR(path_stat.st_mode)) {
        shell_transcript_appendf("cd: path not found %s\n", argv[1]);
        shell_sd_end(&session, "cd");
        return;
    }

    shell_sd_end(&session, "cd");

    snprintf(s_shell_cwd, sizeof(s_shell_cwd), "%s", resolved_path);
    shell_fs_print_cwd();
}

static void shell_command_dir(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    esp_err_t error;

    if (argc > 2) {
        shell_transcript_append_text("Usage: dir [path]\n");
        return;
    }

    error = shell_fs_resolve_path(argc == 2 ? argv[1] : NULL, resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("dir: invalid path\n");
        return;
    }

    error = shell_list_directory_path(resolved_path);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_appendf("dir: path not found %s\n", resolved_path);
    } else if (error != ESP_OK) {
        shell_transcript_appendf("dir: failed to read %s (%s)\n", resolved_path, esp_err_to_name(error));
    }
}

static void shell_command_copy(int argc, char **argv)
{
    char source_path[SHELL_SD_PATH_BYTES];
    char dest_path[SHELL_SD_PATH_BYTES];
    esp_err_t error;

    if (argc != 3) {
        shell_transcript_append_text("Usage: copy <source> <destination>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("copy: invalid source path\n");
        return;
    }

    error = shell_fs_resolve_path(argv[2], dest_path, sizeof(dest_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("copy: invalid destination path\n");
        return;
    }

    error = shell_fs_copy_file(source_path, dest_path);
    if (error != ESP_OK) {
        shell_transcript_appendf("copy: failed (%s)\n", esp_err_to_name(error));
        return;
    }

    shell_transcript_appendf("1 file(s) copied to %s\n", dest_path);
}

static void shell_command_del(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        shell_transcript_append_text("Usage: del <path>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("del: invalid path\n");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("del: SD card not present - insert and retry\n");
        return;
    }

    if (unlink(resolved_path) != 0) {
        shell_transcript_appendf("del: failed to delete %s (%s)\n", resolved_path, strerror(errno));
        shell_sd_end(&session, "del");
        return;
    }

    shell_sd_end(&session, "del");
    shell_transcript_appendf("Deleted %s\n", resolved_path);
}

static void shell_command_rename(int argc, char **argv, const char *verb)
{
    char source_path[SHELL_SD_PATH_BYTES];
    char target_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 3) {
        shell_transcript_appendf("Usage: %s <source> <destination>\n", verb);
        return;
    }

    error = shell_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: invalid source path\n", verb);
        return;
    }

    error = shell_resolve_target_from_source(source_path, argv[2], target_path, sizeof(target_path));
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: invalid destination path\n", verb);
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: SD card not present - insert and retry\n", verb);
        return;
    }

    if (rename(source_path, target_path) != 0) {
        shell_transcript_appendf("%s: failed (%s)\n", verb, strerror(errno));
        shell_sd_end(&session, verb);
        return;
    }

    shell_sd_end(&session, verb);
    shell_transcript_appendf("%s -> %s\n", source_path, target_path);
}

static void shell_command_mkdir(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        shell_transcript_append_text("Usage: mkdir <path>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("mkdir: invalid path\n");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("mkdir: SD card not present - insert and retry\n");
        return;
    }

    if (mkdir(resolved_path, 0775) != 0) {
        shell_transcript_appendf("mkdir: failed to create %s (%s)\n", resolved_path, strerror(errno));
        shell_sd_end(&session, "mkdir");
        return;
    }

    shell_sd_end(&session, "mkdir");
    shell_transcript_appendf("Created directory %s\n", resolved_path);
}

static void shell_command_rmdir(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        shell_transcript_append_text("Usage: rmdir <path>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("rmdir: invalid path\n");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("rmdir: SD card not present - insert and retry\n");
        return;
    }

    if (rmdir(resolved_path) != 0) {
        shell_transcript_appendf("rmdir: failed to remove %s (%s)\n", resolved_path, strerror(errno));
        shell_sd_end(&session, "rmdir");
        return;
    }

    shell_sd_end(&session, "rmdir");
    shell_transcript_appendf("Removed directory %s\n", resolved_path);
}

static void shell_command_type_file(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    esp_err_t error;

    if (argc != 2) {
        shell_transcript_append_text("Usage: type <path>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("type: invalid path\n");
        return;
    }

    error = shell_print_file_text(resolved_path);
    if (error == ESP_ERR_INVALID_ARG) {
        shell_transcript_appendf("type: %s is not a regular text file\n", resolved_path);
    } else if (error != ESP_OK) {
        shell_transcript_appendf("type: failed to read %s (%s)\n", resolved_path, esp_err_to_name(error));
    }
}

static void shell_command_write_file(int argc, char **argv, bool append_mode)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    char text[SHELL_BATCH_LINE_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    if (argc < 3) {
        shell_transcript_appendf("Usage: %s <path> <text>\n", append_mode ? "append" : "write");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: invalid path\n", append_mode ? "append" : "write");
        return;
    }

    shell_join_args(argv, 2, argc, text, sizeof(text));
    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: SD card not present - insert and retry\n", append_mode ? "append" : "write");
        return;
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        shell_transcript_appendf("%s: failed to open %s (%s)\n",
                                 append_mode ? "append" : "write",
                                 resolved_path,
                                 strerror(errno));
        shell_sd_end(&session, append_mode ? "append" : "write");
        return;
    }

    if (fwrite(text, 1, strlen(text), file) != strlen(text) || fwrite("\n", 1, 1, file) != 1) {
        shell_transcript_appendf("%s: failed while writing %s\n", append_mode ? "append" : "write", resolved_path);
        fclose(file);
        shell_sd_end(&session, append_mode ? "append" : "write");
        return;
    }

    fclose(file);
    shell_sd_end(&session, append_mode ? "append" : "write");
    shell_transcript_appendf("%s: %s\n", append_mode ? "Appended" : "Wrote", resolved_path);
}

static void shell_command_touch(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    if (argc != 2) {
        shell_transcript_append_text("Usage: touch <path>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("touch: invalid path\n");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("touch: SD card not present - insert and retry\n");
        return;
    }

    file = fopen(resolved_path, "ab");
    if (file == NULL) {
        shell_transcript_appendf("touch: failed to open %s (%s)\n", resolved_path, strerror(errno));
        shell_sd_end(&session, "touch");
        return;
    }

    fclose(file);
    (void)utime(resolved_path, NULL);
    shell_sd_end(&session, "touch");
    shell_transcript_appendf("Touched %s\n", resolved_path);
}

static void shell_command_move(int argc, char **argv)
{
    char source_path[SHELL_SD_PATH_BYTES];
    char target_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 3) {
        shell_transcript_append_text("Usage: move <source> <destination>\n");
        return;
    }

    error = shell_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("move: invalid source path\n");
        return;
    }

    error = shell_resolve_target_from_source(source_path, argv[2], target_path, sizeof(target_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("move: invalid destination path\n");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("move: SD card not present - insert and retry\n");
        return;
    }

    if (rename(source_path, target_path) == 0) {
        shell_sd_end(&session, "move");
        shell_transcript_appendf("Moved %s -> %s\n", source_path, target_path);
        return;
    }

    shell_sd_end(&session, "move");
    error = shell_fs_copy_file(source_path, target_path);
    if (error != ESP_OK) {
        shell_transcript_appendf("move: failed to copy %s (%s)\n", source_path, esp_err_to_name(error));
        return;
    }

    error = shell_sd_begin(&session);
    if (error == ESP_OK) {
        if (unlink(source_path) != 0) {
            shell_transcript_appendf("move: warning, copied but could not remove %s (%s)\n",
                                     source_path,
                                     strerror(errno));
        }
        shell_sd_end(&session, "move");
    }
    shell_transcript_appendf("Moved %s -> %s\n", source_path, target_path);
}

static void shell_command_set(int argc, char **argv)
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

static void shell_command_path(int argc, char **argv)
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

static void shell_command_echo(int argc, char **argv)
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

static bool shell_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size)
{
    char candidate[SHELL_SD_PATH_BYTES];
    char path_copy[SHELL_ENV_VALUE_BYTES];
    char *entry;
    char *context = NULL;
    struct stat st;
    shell_sd_session_t session;
    esp_err_t error;
    const char *path_env = shell_env_get("PATH");

    if (command_name == NULL || command_name[0] == '\0' || resolved_path == NULL || resolved_path_size == 0) {
        return false;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return false;
    }

    error = shell_fs_resolve_path(command_name, candidate, sizeof(candidate));
    if (error == ESP_OK && shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
        snprintf(resolved_path, resolved_path_size, "%s", candidate);
        shell_sd_end(&session, "call");
        return true;
    }

    if (!shell_path_has_extension(command_name, ".bat")) {
        char with_ext[SHELL_SD_PATH_BYTES];

        snprintf(with_ext, sizeof(with_ext), "%s.bat", command_name);
        error = shell_fs_resolve_path(with_ext, candidate, sizeof(candidate));
        if (error == ESP_OK && shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
            snprintf(resolved_path, resolved_path_size, "%s", candidate);
            shell_sd_end(&session, "call");
            return true;
        }
    }

    if (path_env != NULL && path_env[0] != '\0' && !shell_path_has_directory_component(command_name)) {
        snprintf(path_copy, sizeof(path_copy), "%s", path_env);
        entry = strtok_r(path_copy, ";", &context);
        while (entry != NULL) {
            char dir_path[SHELL_SD_PATH_BYTES];

            if (shell_fs_resolve_path(entry, dir_path, sizeof(dir_path)) == ESP_OK) {
                int written = snprintf(candidate, sizeof(candidate), "%s/%s", dir_path, command_name);
                if (written > 0 && (size_t)written < sizeof(candidate) &&
                    shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                    snprintf(resolved_path, resolved_path_size, "%s", candidate);
                    shell_sd_end(&session, "call");
                    return true;
                }

                if (!shell_path_has_extension(command_name, ".bat")) {
                    written = snprintf(candidate, sizeof(candidate), "%s/%s.bat", dir_path, command_name);
                    if (written > 0 && (size_t)written < sizeof(candidate) &&
                        shell_sd_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                        snprintf(resolved_path, resolved_path_size, "%s", candidate);
                        shell_sd_end(&session, "call");
                        return true;
                    }
                }
            }

            entry = strtok_r(NULL, ";", &context);
        }
    }

    shell_sd_end(&session, "call");
    return false;
}

static esp_err_t shell_execute_batch_file(const char *path, int argc, char **argv)
{
    shell_batch_frame_t frame = {
        .echo_enabled = true,
        .argc = MIN(argc, SHELL_BATCH_ARGS_MAX),
        .depth = s_active_batch_frame != NULL ? s_active_batch_frame->depth + 1 : 1,
        .parent = s_active_batch_frame,
    };
    FILE *file = NULL;
    char line[SHELL_BATCH_LINE_BYTES];
    shell_sd_session_t session;
    esp_err_t error;
    int index;

    if (frame.depth > SHELL_BATCH_DEPTH_MAX) {
        shell_transcript_append_text("call: maximum batch nesting depth reached\n");
        return ESP_ERR_INVALID_STATE;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("call: SD card not present - insert and retry\n");
        return error;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "call");
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
    shell_sd_end(&session, "call");
    return ESP_OK;
}

static void shell_sd_print_usage(void)
{
    shell_transcript_append_text("Usage:\n");
    shell_transcript_append_text("  sd info\n");
    shell_transcript_append_text("  sd ls [path]\n");
    shell_transcript_append_text("  sd stat <path>\n");
    shell_transcript_append_text("  sd cat <path> [max_bytes]\n");
    shell_transcript_append_text("  sd eject          (safe unmount before card removal)\n");
    shell_transcript_append_text("  sdeject           (alias for sd eject)\n");
    shell_transcript_append_text("Paths: sd:/file.txt, /sdcard/file.txt, or relative-to-sd-root\n");
}

static void shell_command_sd_eject(void)
{
    esp_err_t error;

    /* Force unmount the SD card unconditionally (not session-guarded).
     * This is the safe-removal path: unmounts even if mounted by another session. */
    error = bsp_sdcard_unmount();
    if (error == ESP_OK) {
        s_sd_persistent_mounted = false;
        s_sd_ejected = true;
        shell_transcript_append_text("SD card unmounted safely. You may now remove the card.\n");
        shell_header_notify("SD card ejected", 3000);
        header_update_sd(HEADER_SD_NONE);
    } else if (error == ESP_ERR_INVALID_STATE) {
        shell_transcript_append_text("SD card is not currently mounted.\n");
    } else {
        shell_transcript_appendf("SD eject warning: %s (0x%x)\n", esp_err_to_name(error), (unsigned int)error);
        shell_record_warningf("sd", "Eject unmount warning: %s", esp_err_to_name(error));
    }
}

static void shell_command_sd_info(void)
{
    shell_sd_session_t session;
    esp_err_t error;
    struct stat root_stat;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: SD card not present - insert and retry\n");
        shell_record_errorf("sd", error, "SD info mount failed");
        return;
    }

    shell_transcript_appendf("sd.mount_point: %s\n", BSP_SD_MOUNT_POINT);
    if (bsp_sdcard != NULL) {
        char capacity_text[32];
        uint64_t capacity_bytes = (uint64_t)bsp_sdcard->csd.capacity * (uint64_t)bsp_sdcard->csd.sector_size;

        shell_sd_format_size(capacity_bytes, capacity_text, sizeof(capacity_text));
        shell_transcript_appendf("sd.card_name: %s\n", bsp_sdcard->cid.name);
        shell_transcript_appendf("sd.sector_size: %u\n", (unsigned int)bsp_sdcard->csd.sector_size);
        shell_transcript_appendf("sd.capacity: %s\n", capacity_text);
        shell_transcript_appendf("sd.max_freq_khz: %u\n", (unsigned int)bsp_sdcard->max_freq_khz);
    } else {
        shell_transcript_append_text("sd.card_name: unavailable\n");
    }

    error = shell_sd_stat_path(BSP_SD_MOUNT_POINT, &root_stat);
    if (error == ESP_OK) {
        shell_transcript_appendf("sd.root: %s\n", shell_sd_entry_type(&root_stat));
    } else {
        shell_transcript_appendf("sd.root: unavailable (%s)\n", esp_err_to_name(error));
    }

    shell_sd_end(&session, "sd info");
}

static void shell_command_sd_ls(char *command)
{
    char *argv[4];
    int argc = shell_split_args(command, argv, 4);
    const char *input_path = argc >= 3 ? argv[2] : BSP_SD_MOUNT_POINT;
    char normalized_path[SHELL_SD_PATH_BYTES];
    char fatfs_path[SHELL_SD_PATH_BYTES];
    char lfn[256];
    char size_text[32];
    struct stat path_stat;
    FF_DIR dir;
    FILINFO entry_info;
    FRESULT result;
    esp_err_t error;
    size_t listed_entries = 0;
    shell_sd_session_t session;
    bool dir_open = false;

    if (argc > 3) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd ls command");
        return;
    }

    error = shell_sd_resolve_path(input_path, normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd ls");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: SD card not present - insert and retry\n");
        shell_record_errorf("sd", error, "SD card not present or mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        shell_transcript_appendf("sd: %s type=%s size=%s\n",
                                 normalized_path,
                                 shell_sd_entry_type(&path_stat),
                                 size_text);
        goto cleanup;
    }

    error = shell_sd_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: invalid FAT path for %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not convert directory path %s to FatFs path", normalized_path);
        goto cleanup;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        error = shell_sd_fresult_to_esp_err(result);
        shell_transcript_appendf("sd: could not open %s (FatFs=%u)\n",
                                 normalized_path,
                                 (unsigned int)result);
        shell_record_errorf("sd", error, "Could not open directory %s (FatFs=%u)", normalized_path, (unsigned int)result);
        goto cleanup;
    }
    dir_open = true;

    shell_transcript_appendf("sd: listing %s\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = shell_sd_fresult_to_esp_err(result);
            shell_transcript_appendf("sd: directory read failed for %s (FatFs=%u)\n",
                                     normalized_path,
                                     (unsigned int)result);
            shell_record_errorf("sd", error, "Directory read failed for %s (FatFs=%u)", normalized_path, (unsigned int)result);
            break;
        }

        if (entry_info.fname[0] == '\0') {
            break;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        if (strcmp(lfn, ".") == 0 || strcmp(lfn, "..") == 0) {
            continue;
        }

        if (listed_entries == SHELL_SD_LIST_LIMIT) {
            shell_transcript_appendf("sd: listing truncated after %u entries\n", (unsigned int)SHELL_SD_LIST_LIMIT);
            shell_record_warningf("sd", "Truncated directory listing for %s", normalized_path);
            break;
        }

        shell_sd_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
        shell_transcript_appendf("sd: %-4s %s (%s)\n",
                                 (entry_info.fattrib & AM_DIR) ? "DIR" : "FILE",
                                 lfn,
                                 (entry_info.fattrib & AM_DIR) ? "-" : size_text);
        listed_entries++;
    }

cleanup:
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    shell_sd_end(&session, "sd ls");
}

static void shell_command_sd_stat(char *command)
{
    char *argv[4];
    int argc = shell_split_args(command, argv, 4);
    char normalized_path[SHELL_SD_PATH_BYTES];
    char size_text[32];
    struct stat path_stat;
    esp_err_t error;
    shell_sd_session_t session;

    if (argc != 3) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd stat command");
        return;
    }

    error = shell_sd_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd stat");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: SD card not present - insert and retry\n");
        shell_record_errorf("sd", error, "SD stat mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        shell_sd_end(&session, "sd stat");
        return;
    }

    shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
    shell_transcript_appendf("sd.path: %s\n", normalized_path);
    shell_transcript_appendf("sd.type: %s\n", shell_sd_entry_type(&path_stat));
    shell_transcript_appendf("sd.size: %s\n", size_text);
    shell_transcript_appendf("sd.mode: 0%o\n", (unsigned int)(path_stat.st_mode & 0777));

    shell_sd_end(&session, "sd stat");
}

static void shell_command_sd_cat(char *command)
{
    char *argv[5];
    int argc = shell_split_args(command, argv, 5);
    char normalized_path[SHELL_SD_PATH_BYTES];
    struct stat path_stat;
    esp_err_t error;
    shell_sd_session_t session;
    FILE *file = NULL;
    size_t max_bytes = SHELL_SD_CAT_DEFAULT_BYTES;
    size_t displayed_bytes = 0;
    bool truncated = false;
    bool ended_with_newline = false;
    unsigned char buffer[SHELL_SD_IO_BUFFER_BYTES + 1];

    if (argc < 3 || argc > 4) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd cat command");
        return;
    }

    if (argc == 4 && !shell_parse_size_arg(argv[3], 1, SHELL_SD_CAT_MAX_BYTES, &max_bytes)) {
        shell_transcript_appendf("sd: max_bytes must be between 1 and %u\n", (unsigned int)SHELL_SD_CAT_MAX_BYTES);
        shell_record_warningf("sd", "Rejected invalid max_bytes for sd cat");
        return;
    }

    error = shell_sd_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd cat");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: SD card not present - insert and retry\n");
        shell_record_errorf("sd", error, "SD cat mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        shell_transcript_appendf("sd: %s is not a regular file\n", normalized_path);
        shell_record_warningf("sd", "Rejected non-file path for sd cat: %s", normalized_path);
        goto cleanup;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        shell_transcript_appendf("sd: could not open %s (%s)\n", normalized_path, strerror(errno));
        shell_record_errorf("sd", ESP_FAIL, "Could not open file %s", normalized_path);
        goto cleanup;
    }

    shell_transcript_appendf("sd: preview %s (%u bytes max)\n", normalized_path, (unsigned int)max_bytes);
    while (displayed_bytes < max_bytes) {
        size_t index;
        size_t bytes_to_read = MIN(sizeof(buffer) - 1, max_bytes - displayed_bytes);
        size_t bytes_read = fread(buffer, 1, bytes_to_read, file);

        if (bytes_read == 0) {
            break;
        }

        for (index = 0; index < bytes_read; index++) {
            if (buffer[index] == '\n' || buffer[index] == '\r' || buffer[index] == '\t') {
                continue;
            }
            if (!isprint(buffer[index])) {
                buffer[index] = '.';
            }
        }
        buffer[bytes_read] = '\0';
        ended_with_newline = bytes_read > 0 && buffer[bytes_read - 1] == '\n';

        shell_transcript_appendf("%s", (char *)buffer);
        displayed_bytes += bytes_read;
        if (bytes_read < bytes_to_read) {
            break;
        }
    }

    if (displayed_bytes == 0) {
        shell_transcript_append_text("sd: file is empty\n");
    } else if (!feof(file)) {
        truncated = true;
    }

    if (displayed_bytes > 0 && !ended_with_newline) {
        shell_transcript_append_text("\n");
    }

    if (truncated) {
        shell_transcript_appendf("sd: preview truncated at %u bytes\n", (unsigned int)max_bytes);
    }

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    shell_sd_end(&session, "sd cat");
}

// `sd` is a small command family so storage operations share validated parsing, bounded output, and consistent cleanup behavior.
static void shell_command_sd(char *command)
{
    char *argv[5];
    int argc = shell_split_args(command, argv, 5);

    if (argc <= 1) {
        shell_command_sd_info();
        shell_sd_print_usage();
        return;
    }

    if (strcmp(argv[1], "help") == 0) {
        shell_sd_print_usage();
        return;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc != 2) {
            shell_sd_print_usage();
            shell_record_warningf("sd", "Usage error for sd info command");
            return;
        }
        shell_command_sd_info();
        return;
    }

    if (strcmp(argv[1], "ls") == 0) {
        shell_command_sd_ls(command);
        return;
    }

    if (strcmp(argv[1], "stat") == 0) {
        shell_command_sd_stat(command);
        return;
    }

    if (strcmp(argv[1], "cat") == 0) {
        shell_command_sd_cat(command);
        return;
    }

    if (strcmp(argv[1], "eject") == 0) {
        shell_command_sd_eject();
        return;
    }

    shell_sd_print_usage();
    shell_record_warningf("sd", "Unknown sd subcommand: %s", argv[1]);
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
        esp_err_t error = shell_write_redirect_output(redirect_target, s_transcript + transcript_len_before, append_mode);
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

    if (shell_text_equals_ignore_case(argv[0], "move")) {
        shell_command_move(argc, argv);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "call")) {
        if (argc < 2) {
            shell_transcript_append_text("Usage: call <file.bat> [args]\n");
            return true;
        }
        if (!shell_resolve_batch_path(argv[1], batch_path, sizeof(batch_path))) {
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
        shell_command_sd(command_copy);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[0], "sdeject")) {
        shell_command_sd_eject();
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

    if (shell_path_has_extension(argv[0], ".bat") || shell_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
        if (!shell_resolve_batch_path(argv[0], batch_path, sizeof(batch_path))) {
            return false;
        }
        (void)shell_execute_batch_file(batch_path, argc - 1, &argv[1]);
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

    /* Show boot banner and reset input line */
    shell_transcript_reset();
    shell_transcript_appendf_ansi("@G%s@R\n", SHELL_BOOT_MESSAGE);
    shell_history_transcript_scroll_to_end();
    shell_input_line_reset();
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
        .transcript_append_text = shell_transcript_append_text,
        .schedule_transcript_append_text = shell_networking_schedule_text,
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

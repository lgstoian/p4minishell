#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_wifi_default.h"
#include "esp_wifi.h"
#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "lwip/ip4_addr.h"
#include "lvgl.h"
#include "esp32_port.h"
#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define SHELL_TAG "p4minishell"
#define SHELL_BOARD_REQUESTED "JC1060P470C"
#define SHELL_BOARD_DETECTED "ESP32-P4-Function-EV-Board"
#define SHELL_BOOT_MESSAGE "P4MiniShell v0.1 ready | JC1060P470C | type help"
#define SHELL_PROMPT "P4Shell> "
#define SHELL_C6UPDATE_DEFAULT_IMAGE "sd:/esp32c6_hosted_slave_merged.bin"
#define SHELL_TRANSCRIPT_BYTES 8192
#define SHELL_ASYNC_TRANSCRIPT_BYTES 2048
#define SHELL_COMMAND_BYTES 256
#define SHELL_COMMAND_HISTORY_DEPTH 10
#define SHELL_KEYBOARD_HEIGHT 240
#define SHELL_INPUT_ROW_HEIGHT 52
#define SHELL_DEBUG_LOG_DEPTH 5
#define SHELL_DEBUG_ENTRY_BYTES 192
#define SHELL_WIFI_SSID_BYTES 33
#define SHELL_WIFI_PASSWORD_BYTES 65
#define SHELL_WIFI_DETAIL_BYTES 256
#define SHELL_WIFI_INIT_TASK_STACK_BYTES 6144
#define SHELL_C6UPDATE_TASK_STACK_BYTES 8192
#define SHELL_C6UPDATE_FLASH_BLOCK_BYTES 1024
#define SHELL_C6UPDATE_PROGRESS_STEP_PERCENT 5
#define SHELL_WIFI_RUNTIME_ENABLED (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_HOST_WIFI_ENABLED || CONFIG_ESP_HOSTED_ENABLED)

#ifndef CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID
#define CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID ""
#endif

#ifndef CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD
#define CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD ""
#endif

typedef enum {
    SHELL_WIFI_STATE_NOT_ATTEMPTED = 0,
    SHELL_WIFI_STATE_STARTING,
    SHELL_WIFI_STATE_STARTED,
    SHELL_WIFI_STATE_FAILED,
    SHELL_WIFI_STATE_SKIPPED_DISABLED,
    SHELL_WIFI_STATE_SKIPPED_UNSUPPORTED,
} shell_wifi_state_t;

typedef struct {
    char path[SHELL_COMMAND_BYTES];
} shell_c6update_request_t;

typedef struct {
    bool use_defaults;
    char ssid[SHELL_WIFI_SSID_BYTES];
    char password[SHELL_WIFI_PASSWORD_BYTES];
} shell_wifi_connect_request_t;

typedef struct {
    char entries[SHELL_DEBUG_LOG_DEPTH][SHELL_DEBUG_ENTRY_BYTES];
    size_t count;
    size_t next_index;
} shell_debug_log_t;

static lv_obj_t *s_history_transcript;
static lv_obj_t *s_input_line;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_history_prev_button;
static lv_obj_t *s_history_next_button;
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
static char s_wifi_last_detail[SHELL_WIFI_DETAIL_BYTES];
static bool s_wifi_init_task_in_progress;
static bool s_c6_update_in_progress;
static bool s_async_transcript_flush_queued;
static size_t s_runtime_warning_count;
static shell_debug_log_t s_debug_log;
static portMUX_TYPE s_async_transcript_lock = portMUX_INITIALIZER_UNLOCKED;

#if SHELL_WIFI_RUNTIME_ENABLED
static esp_netif_t *s_wifi_sta_netif;
static esp_event_handler_instance_t s_wifi_event_any_id;
static esp_event_handler_instance_t s_wifi_got_ip_event;
#endif

static void shell_input_line_reset(void);
static void shell_transcript_append_text(const char *text);
static void shell_transcript_appendf(const char *format, ...);
static int shell_split_args(char *text, char **argv, int max_args);
static void shell_schedule_transcript_appendf(const char *format, ...);
static void shell_async_transcript_flush_cb(void *user_data);
static void shell_debug_log_push(const char *tag, const char *message);
static void shell_record_errorf(const char *tag, esp_err_t error, const char *format, ...);
static void shell_record_warningf(const char *tag, const char *format, ...);
static void shell_record_infof(const char *tag, const char *format, ...);
static void shell_command_mem(void);
static void shell_command_gpio_status(void);
static void shell_command_debug(void);
static void shell_command_version(void);
static void shell_command_about(void);
static void shell_command_sd_ls(char *command);
static void shell_command_wifi_scan(void);
static void shell_wifi_runtime_init(void);
static void shell_wifi_connect_task(void *arg);

static const char *shell_loader_error_string(esp_loader_error_t error)
{
    static const char *mapping[] = {
        "success",
        "unspecified failure",
        "timeout",
        "image too large",
        "invalid md5",
        "invalid parameter",
        "invalid target",
        "unsupported chip",
        "unsupported function",
        "invalid response",
    };

    if (error < 0 || error >= (esp_loader_error_t)(sizeof(mapping) / sizeof(mapping[0]))) {
        return "unknown loader error";
    }

    return mapping[error];
}

static const char *shell_target_chip_string(target_chip_t target)
{
    switch (target) {
    case ESP8266_CHIP:
        return "ESP8266";
    case ESP32_CHIP:
        return "ESP32";
    case ESP32S2_CHIP:
        return "ESP32-S2";
    case ESP32C3_CHIP:
        return "ESP32-C3";
    case ESP32S3_CHIP:
        return "ESP32-S3";
    case ESP32C2_CHIP:
        return "ESP32-C2";
    case ESP32C6_CHIP:
        return "ESP32-C6";
    case ESP32H2_CHIP:
        return "ESP32-H2";
    case ESP32C5_CHIP:
        return "ESP32-C5";
    case ESP32P4_CHIP:
        return "ESP32-P4";
    default:
        return "unknown";
    }
}

static int shell_c6_effective_reset_gpio(void)
{
    if (CONFIG_P4MINISHELL_C6_RESET_GPIO >= 0) {
        return CONFIG_P4MINISHELL_C6_RESET_GPIO;
    }

    return CONFIG_P4MINISHELL_C6_EN_GPIO;
}

static bool shell_c6update_config_ready(char *reason, size_t reason_size)
{
    if (CONFIG_P4MINISHELL_C6_FLASH_UART_TX_GPIO < 0 || CONFIG_P4MINISHELL_C6_FLASH_UART_RX_GPIO < 0) {
        snprintf(reason, reason_size,
                 "No verified P4-to-C6 flash UART is wired on this board baseline. Use PROG_C6 with ESP-Prog first, or ESP-Hosted OTA once the link works.");
        return false;
    }

    if (CONFIG_P4MINISHELL_C6_BOOT_GPIO < 0) {
        snprintf(reason, reason_size,
                 "Set CONFIG_P4MINISHELL_C6_BOOT_GPIO so the host can enter ROM download mode, or use the external PROG_C6 header if this board does not route C6 BOOT to the P4 host.");
        return false;
    }

    if (shell_c6_effective_reset_gpio() < 0) {
        snprintf(reason, reason_size,
                 "Set CONFIG_P4MINISHELL_C6_RESET_GPIO or CONFIG_P4MINISHELL_C6_EN_GPIO for target reset control.");
        return false;
    }

    snprintf(reason, reason_size, "ok");
    return true;
}

static void shell_c6update_normalize_path(const char *input, char *output, size_t output_size)
{
    if (strncmp(input, "sd:/", 4) == 0) {
        snprintf(output, output_size, "%s%s", BSP_SD_MOUNT_POINT, input + 3);
        return;
    }

    if (strncmp(input, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0 ||
        strcmp(input, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(output, output_size, "%s", input);
        return;
    }

    if (input[0] == '/') {
        snprintf(output, output_size, "%s", input);
        return;
    }

    snprintf(output, output_size, "%s/%s", BSP_SD_MOUNT_POINT, input);
}

static void shell_c6update_drive_enable_gpio(bool enabled)
{
    if (CONFIG_P4MINISHELL_C6_EN_GPIO < 0) {
        return;
    }

    gpio_reset_pin((gpio_num_t)CONFIG_P4MINISHELL_C6_EN_GPIO);
    gpio_set_pull_mode((gpio_num_t)CONFIG_P4MINISHELL_C6_EN_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)CONFIG_P4MINISHELL_C6_EN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CONFIG_P4MINISHELL_C6_EN_GPIO, enabled ? 1 : 0);
}

static esp_loader_error_t shell_c6update_connect(uint32_t baudrate)
{
    esp_loader_connect_args_t connect_args = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t loader_error = esp_loader_connect(&connect_args);

    if (loader_error != ESP_LOADER_SUCCESS) {
        shell_schedule_transcript_appendf("c6update: connect failed: %s\n",
                                          shell_loader_error_string(loader_error));
        if (loader_error == ESP_LOADER_ERROR_TIMEOUT) {
            shell_schedule_transcript_appendf("c6update: check UART%d TX=%d RX=%d BOOT=%d RESET=%d EN=%d wiring and C6 power.\n",
                                              CONFIG_P4MINISHELL_C6_FLASH_UART_PORT,
                                              CONFIG_P4MINISHELL_C6_FLASH_UART_TX_GPIO,
                                              CONFIG_P4MINISHELL_C6_FLASH_UART_RX_GPIO,
                                              CONFIG_P4MINISHELL_C6_BOOT_GPIO,
                                              CONFIG_P4MINISHELL_C6_RESET_GPIO,
                                              CONFIG_P4MINISHELL_C6_EN_GPIO);
        }
        return loader_error;
    }

    if (baudrate > 115200 && esp_loader_get_target() != ESP8266_CHIP) {
        loader_error = esp_loader_change_transmission_rate(baudrate);
        if (loader_error == ESP_LOADER_SUCCESS) {
            loader_error = loader_port_change_transmission_rate(baudrate);
        }
        if (loader_error != ESP_LOADER_SUCCESS) {
            shell_schedule_transcript_appendf("c6update: baudrate change to %u failed: %s\n",
                                              (unsigned int)baudrate,
                                              shell_loader_error_string(loader_error));
            return loader_error;
        }
    }

    return ESP_LOADER_SUCCESS;
}

static void shell_c6update_task(void *arg)
{
    shell_c6update_request_t *request = (shell_c6update_request_t *)arg;
    char reason[192];
    char normalized_path[320];
    FILE *firmware = NULL;
    esp_err_t error;
    esp_loader_error_t loader_error = ESP_LOADER_SUCCESS;
    bool mounted_here = false;
    bool flasher_initialized = false;
    bool update_succeeded = false;
    long file_size_long = 0;
    size_t file_size;
    size_t aligned_size;
    size_t padded_remaining;
    size_t actual_remaining;
    size_t last_reported_percent = 0;
    uint8_t payload[SHELL_C6UPDATE_FLASH_BLOCK_BYTES];
    const int effective_reset_gpio = shell_c6_effective_reset_gpio();

    if (request == NULL) {
        s_c6_update_in_progress = false;
        shell_record_errorf("c6update", ESP_ERR_INVALID_ARG, "Background updater request was null");
        vTaskDelete(NULL);
        return;
    }

    shell_schedule_transcript_appendf("c6update: preparing %s\n", request->path);

    if (!shell_c6update_config_ready(reason, sizeof(reason))) {
        shell_schedule_transcript_appendf("c6update: %s\n", reason);
        shell_record_warningf("c6update", "%s", reason);
        goto cleanup;
    }

    shell_c6update_normalize_path(request->path, normalized_path, sizeof(normalized_path));
    shell_schedule_transcript_appendf("c6update: mounting SD card for %s\n", normalized_path);
    error = bsp_sdcard_mount();
    if (error == ESP_OK) {
        mounted_here = true;
    } else if (error != ESP_ERR_INVALID_STATE) {
        shell_schedule_transcript_appendf("c6update: SD mount failed: %s (0x%x)\n",
                                          esp_err_to_name(error),
                                          (unsigned int)error);
        shell_record_errorf("c6update", error, "SD card not present or mount failed");
        goto cleanup;
    }

    firmware = fopen(normalized_path, "rb");
    if (firmware == NULL) {
        shell_schedule_transcript_appendf("c6update: could not open %s\n", normalized_path);
        shell_record_errorf("c6update", ESP_ERR_NOT_FOUND, "Merged image not found at %s", normalized_path);
        goto cleanup;
    }

    if (fseek(firmware, 0, SEEK_END) != 0) {
        shell_schedule_transcript_appendf("c6update: failed to seek %s\n", normalized_path);
        shell_record_errorf("c6update", ESP_FAIL, "Failed to seek image %s", normalized_path);
        goto cleanup;
    }

    file_size_long = ftell(firmware);
    if (file_size_long <= 0) {
        shell_schedule_transcript_appendf("c6update: invalid image size in %s\n", normalized_path);
        shell_record_errorf("c6update", ESP_ERR_INVALID_SIZE, "Invalid image size in %s", normalized_path);
        goto cleanup;
    }

    if (fseek(firmware, 0, SEEK_SET) != 0) {
        shell_schedule_transcript_appendf("c6update: failed to rewind %s\n", normalized_path);
        shell_record_errorf("c6update", ESP_FAIL, "Failed to rewind image %s", normalized_path);
        goto cleanup;
    }

    file_size = (size_t)file_size_long;
    aligned_size = (file_size + 3U) & ~((size_t)3U);
    padded_remaining = aligned_size;
    actual_remaining = file_size;

    shell_schedule_transcript_appendf("c6update: image=%s size=%u bytes flash_addr=0x0\n",
                                      normalized_path,
                                      (unsigned int)file_size);
    shell_schedule_transcript_appendf("c6update: UART%d sync_baud=115200 flash_baud=%u tx=%d rx=%d en=%d reset=%d boot=%d\n",
                                      CONFIG_P4MINISHELL_C6_FLASH_UART_PORT,
                                      (unsigned int)CONFIG_P4MINISHELL_C6_FLASH_UART_BAUDRATE,
                                      CONFIG_P4MINISHELL_C6_FLASH_UART_TX_GPIO,
                                      CONFIG_P4MINISHELL_C6_FLASH_UART_RX_GPIO,
                                      CONFIG_P4MINISHELL_C6_EN_GPIO,
                                      CONFIG_P4MINISHELL_C6_RESET_GPIO,
                                      CONFIG_P4MINISHELL_C6_BOOT_GPIO);
    shell_schedule_transcript_appendf("%s", "c6update: expecting a merged ESP-IDF flash image produced for offset 0x0\n");

    shell_c6update_drive_enable_gpio(true);

    // AI: use the esp-serial-flasher ESP32 host port so BOOT/reset sequencing stays in the supported library path.
    const loader_esp32_config_t loader_config = {
        .baud_rate = 115200,
        .uart_port = CONFIG_P4MINISHELL_C6_FLASH_UART_PORT,
        .uart_rx_pin = CONFIG_P4MINISHELL_C6_FLASH_UART_RX_GPIO,
        .uart_tx_pin = CONFIG_P4MINISHELL_C6_FLASH_UART_TX_GPIO,
        .reset_trigger_pin = effective_reset_gpio,
        .gpio0_trigger_pin = CONFIG_P4MINISHELL_C6_BOOT_GPIO,
    };

    loader_error = loader_port_esp32_init(&loader_config);
    if (loader_error != ESP_LOADER_SUCCESS) {
        shell_schedule_transcript_appendf("c6update: flasher UART init failed: %s\n",
                                          shell_loader_error_string(loader_error));
        shell_record_errorf("c6update", ESP_FAIL, "Flasher UART init failed: %s", shell_loader_error_string(loader_error));
        goto cleanup;
    }
    flasher_initialized = true;

    shell_schedule_transcript_appendf("%s", "c6update: entering bootloader and connecting to target\n");
    loader_error = shell_c6update_connect(CONFIG_P4MINISHELL_C6_FLASH_UART_BAUDRATE);
    if (loader_error != ESP_LOADER_SUCCESS) {
        shell_record_errorf("c6update", ESP_FAIL, "Target connect failed: %s", shell_loader_error_string(loader_error));
        goto cleanup;
    }

    shell_schedule_transcript_appendf("c6update: connected to %s\n",
                                      shell_target_chip_string(esp_loader_get_target()));
    if (esp_loader_get_target() != ESP32C6_CHIP) {
        shell_schedule_transcript_appendf("c6update: refusing to flash %s, expected ESP32-C6\n",
                                          shell_target_chip_string(esp_loader_get_target()));
        shell_record_errorf("c6update", ESP_ERR_INVALID_RESPONSE, "Connected target was %s instead of ESP32-C6", shell_target_chip_string(esp_loader_get_target()));
        goto cleanup;
    }

    loader_error = esp_loader_flash_start(0x0, aligned_size, sizeof(payload));
    if (loader_error != ESP_LOADER_SUCCESS) {
        shell_schedule_transcript_appendf("c6update: flash start failed: %s\n",
                                          shell_loader_error_string(loader_error));
        shell_record_errorf("c6update", ESP_FAIL, "Flash start failed: %s", shell_loader_error_string(loader_error));
        goto cleanup;
    }

    shell_schedule_transcript_appendf("%s", "c6update: erase complete, programming\n");
    while (padded_remaining > 0) {
        const size_t chunk = MIN(sizeof(payload), padded_remaining);
        const size_t read_len = MIN(chunk, actual_remaining);
        size_t bytes_read = 0;
        size_t percent;

        memset(payload, 0xFF, chunk);
        if (read_len > 0) {
            bytes_read = fread(payload, 1, read_len, firmware);
            if (bytes_read != read_len) {
                shell_schedule_transcript_appendf("c6update: read failed at byte %u\n",
                                                  (unsigned int)(file_size - actual_remaining));
                shell_record_errorf("c6update", ESP_FAIL, "Read failed at byte %u", (unsigned int)(file_size - actual_remaining));
                goto cleanup;
            }
        }

        loader_error = esp_loader_flash_write(payload, chunk);
        if (loader_error != ESP_LOADER_SUCCESS) {
            shell_schedule_transcript_appendf("c6update: write failed: %s\n",
                                              shell_loader_error_string(loader_error));
            shell_record_errorf("c6update", ESP_FAIL, "Flash write failed: %s", shell_loader_error_string(loader_error));
            goto cleanup;
        }

        padded_remaining -= chunk;
        actual_remaining -= bytes_read;
        percent = ((file_size - actual_remaining) * 100U) / file_size;
        if (percent >= last_reported_percent + SHELL_C6UPDATE_PROGRESS_STEP_PERCENT || actual_remaining == 0) {
            last_reported_percent = percent;
            shell_schedule_transcript_appendf("c6update: progress %u%%\n", (unsigned int)percent);
        }
    }

    loader_error = esp_loader_flash_finish(true);
    if (loader_error != ESP_LOADER_SUCCESS) {
        shell_schedule_transcript_appendf("c6update: flash finish failed: %s\n",
                                          shell_loader_error_string(loader_error));
        shell_record_errorf("c6update", ESP_FAIL, "Flash finish failed: %s", shell_loader_error_string(loader_error));
        goto cleanup;
    }

    shell_schedule_transcript_appendf("%s", "c6update: flash complete, target rebooted\n");
    shell_record_infof("c6update", "C6 flash completed successfully");
    update_succeeded = true;

cleanup:
    if (firmware != NULL) {
        fclose(firmware);
    }
    if (flasher_initialized && !update_succeeded) {
        esp_loader_reset_target();
    }
    if (flasher_initialized) {
        loader_port_esp32_deinit();
    }
    shell_c6update_drive_enable_gpio(true);
    if (mounted_here) {
        esp_err_t unmount_error = bsp_sdcard_unmount();
        if (unmount_error != ESP_OK) {
            shell_schedule_transcript_appendf("c6update: SD unmount warning: %s (0x%x)\n",
                                              esp_err_to_name(unmount_error),
                                              (unsigned int)unmount_error);
            shell_record_warningf("c6update", "SD unmount warning: %s", esp_err_to_name(unmount_error));
        }
    }
    free(request);
    s_c6_update_in_progress = false;
    vTaskDelete(NULL);
}

static void shell_execute_c6update_command(char *command)
{
    char *argv[3];
    int argc = shell_split_args(command, argv, 3);
    shell_c6update_request_t *request;
    const char *image_path = NULL;

    if (argc > 2) {
        shell_transcript_append_text("Usage: c6update [default|slave|sd:/path/to/merged-image.bin]\n");
        shell_record_warningf("c6update", "Usage error for c6update command");
        return;
    }

    if (s_c6_update_in_progress) {
        shell_transcript_append_text("c6update: another update is already running\n");
        shell_record_warningf("c6update", "Rejected because another update is already running");
        return;
    }

    request = (shell_c6update_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        shell_transcript_append_text("c6update: out of memory\n");
        shell_record_errorf("c6update", ESP_ERR_NO_MEM, "Out of memory allocating update request");
        return;
    }

    if (argc == 1 || strcmp(argv[1], "default") == 0 || strcmp(argv[1], "slave") == 0) {
        image_path = SHELL_C6UPDATE_DEFAULT_IMAGE;
        shell_transcript_appendf("c6update: using default image %s\n", image_path);
        shell_record_infof("c6update", "Using default image %s", image_path);
    } else {
        image_path = argv[1];
    }

    snprintf(request->path, sizeof(request->path), "%s", image_path);
    s_c6_update_in_progress = true;
    if (xTaskCreate(shell_c6update_task,
                    "c6update_task",
                    SHELL_C6UPDATE_TASK_STACK_BYTES,
                    request,
                    tskIDLE_PRIORITY + 2,
                    NULL) != pdPASS) {
        s_c6_update_in_progress = false;
        free(request);
        shell_transcript_append_text("c6update: failed to start background updater task\n");
        shell_record_errorf("c6update", ESP_FAIL, "Failed to start background updater task");
    }
}

// AI: error handling & history buffer keeps the last few failures and warnings visible for the debug command.
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

static void shell_transcript_render(void)
{
    if (s_history_transcript == NULL) {
        return;
    }

    lv_textarea_set_text(s_history_transcript, s_transcript);
    lv_textarea_set_cursor_pos(s_history_transcript, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_update_layout(s_history_transcript);
    lv_obj_scroll_to_y(s_history_transcript, LV_COORD_MAX, LV_ANIM_OFF);
}

static void shell_transcript_reset(void)
{
    s_transcript[0] = '\0';
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
    char *context = NULL;
    char *token = strtok_r(text, " \t", &context);

    while (token != NULL && argc < max_args) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &context);
    }

    return argc;
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

static esp_err_t shell_wifi_connect_with_credentials(const char *ssid, const char *password)
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

static bool shell_wifi_begin_connect_request(const char *ssid, const char *password, bool use_defaults)
{
#if SHELL_WIFI_RUNTIME_ENABLED
    shell_wifi_connect_request_t *request;

    if (s_wifi_init_task_in_progress || s_wifi_state == SHELL_WIFI_STATE_STARTING) {
        shell_transcript_append_text("wifi: initialization is already in progress\n");
        return false;
    }

    request = (shell_wifi_connect_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        shell_transcript_append_text("wifi: failed to allocate connect request\n");
        return false;
    }

    request->use_defaults = use_defaults;
    if (!use_defaults) {
        snprintf(request->ssid, sizeof(request->ssid), "%s", ssid);
        snprintf(request->password, sizeof(request->password), "%s", password != NULL ? password : "");
    }

    s_wifi_init_task_in_progress = true;
    s_wifi_state = SHELL_WIFI_STATE_STARTING;
    s_wifi_last_error = ESP_OK;
    s_wifi_connected = false;
    s_wifi_connect_requested = false;
    shell_schedule_transcript_appendf("[wifi] queued %s connection request\n",
                                      use_defaults ? "default profile" : request->ssid);

    if (xTaskCreate(shell_wifi_connect_task,
                    "wifi_connect",
                    SHELL_WIFI_INIT_TASK_STACK_BYTES,
                    request,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        s_wifi_init_task_in_progress = false;
        s_wifi_state = SHELL_WIFI_STATE_NOT_ATTEMPTED;
        free(request);
        shell_transcript_append_text("wifi: failed to start background init task\n");
        return false;
    }

    return true;
#else
    (void)ssid;
    (void)password;
    (void)use_defaults;
    return false;
#endif
}

static void shell_wifi_connect_task(void *arg)
{
#if SHELL_WIFI_RUNTIME_ENABLED
    shell_wifi_connect_request_t *request = (shell_wifi_connect_request_t *)arg;

    shell_wifi_runtime_init();
    if (s_wifi_state == SHELL_WIFI_STATE_STARTED) {
        if (request->use_defaults) {
            if (shell_wifi_defaults_available()) {
                (void)shell_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                          CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
            } else {
                s_wifi_state = SHELL_WIFI_STATE_FAILED;
                s_wifi_last_error = ESP_ERR_INVALID_STATE;
                shell_transcript_append_text("wifi: sdkconfig default credentials are not configured\n");
            }
        } else {
            (void)shell_wifi_connect_with_credentials(request->ssid, request->password);
        }
    }

    s_wifi_init_task_in_progress = false;
    free(request);
#else
    (void)arg;
#endif
    vTaskDelete(NULL);
}

static void shell_command_wifi_help(void)
{
    shell_transcript_append_text("Wi-Fi commands:\n");
    shell_transcript_append_text("  wifi status                 Show Wi-Fi runtime state and IP info\n");
    shell_transcript_append_text("  wifi scan                   Scan for nearby SSIDs after Wi-Fi starts\n");
    shell_transcript_append_text("  wifi connect                Connect using sdkconfig default credentials\n");
    shell_transcript_append_text("  wifi connect <ssid> <pass>  Connect using runtime credentials\n");
    shell_transcript_append_text("  wifi disconnect             Disconnect the current station session\n");
    shell_transcript_append_text("  wifi connect probes ESP-Hosted in a background task so the shell remains responsive\n");
    shell_transcript_append_text("  wifi connect passwords are masked in transcript history and not stored in command recall\n");
}

static void shell_command_wifi_scan(void)
{
#if SHELL_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t records[16];
    uint16_t record_count = (uint16_t)(sizeof(records) / sizeof(records[0]));
    uint16_t index;

    if (s_wifi_state != SHELL_WIFI_STATE_STARTED) {
        shell_transcript_append_text("wifi: scan requires the Wi-Fi runtime to be started first\n");
        shell_record_warningf("wifi", "Scan rejected because Wi-Fi is not started");
        return;
    }

    shell_transcript_append_text("wifi: scanning for access points...\n");
    error = esp_wifi_scan_start(NULL, true);
    if (error != ESP_OK) {
        shell_record_errorf("wifi", error, "WiFi scan failed - check sdkconfig or hosted link");
        return;
    }

    error = esp_wifi_scan_get_ap_records(&record_count, records);
    if (error != ESP_OK) {
        shell_record_errorf("wifi", error, "Failed to read WiFi scan results");
        return;
    }

    if (record_count == 0) {
        shell_transcript_append_text("wifi.scan: no access points found\n");
        shell_record_infof("wifi", "Scan completed with no visible APs");
        return;
    }

    for (index = 0; index < record_count; index++) {
        shell_transcript_appendf("wifi.scan[%u]: ssid=%s rssi=%d auth=%u channel=%u\n",
                                 (unsigned int)index,
                                 records[index].ssid,
                                 records[index].rssi,
                                 (unsigned int)records[index].authmode,
                                 (unsigned int)records[index].primary);
    }
    shell_record_infof("wifi", "Scan completed with %u APs", (unsigned int)record_count);
#endif
}

static void shell_command_wifi_status(void)
{
    esp_err_t error;
    wifi_ap_record_t ap_info;
    char ap_ssid[SHELL_WIFI_SSID_BYTES];
    esp_netif_ip_info_t ip_info;

    shell_transcript_appendf("wifi.state: %s\n", shell_wifi_state_string());
    shell_transcript_appendf("wifi.default_profile: %s\n", shell_wifi_defaults_available() ? "configured" : "missing");
    if (shell_wifi_defaults_available()) {
        shell_transcript_appendf("wifi.default_ssid: %s\n", CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID);
    }
    if (s_wifi_last_detail[0] != '\0') {
        shell_transcript_appendf("wifi.note: %s\n", s_wifi_last_detail);
    }

    if (s_wifi_state != SHELL_WIFI_STATE_STARTED) {
        if (s_wifi_state == SHELL_WIFI_STATE_FAILED) {
            shell_transcript_appendf("wifi.last_error: %s (0x%x)\n",
                                     esp_err_to_name(s_wifi_last_error),
                                     (unsigned int)s_wifi_last_error);
        }
        if (s_wifi_state == SHELL_WIFI_STATE_STARTING) {
            shell_transcript_append_text("wifi.progress: ESP-Hosted probe is still running\n");
        }
        return;
    }

    shell_transcript_appendf("wifi.connect_requested: %s\n", s_wifi_connect_requested ? "yes" : "no");
    shell_transcript_appendf("wifi.connected: %s\n", s_wifi_connected ? "yes" : "no");
    if (s_wifi_target_ssid[0] != '\0') {
        shell_transcript_appendf("wifi.target_ssid: %s\n", s_wifi_target_ssid);
    }

    error = esp_wifi_sta_get_ap_info(&ap_info);
    if (error == ESP_OK) {
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", (const char *)ap_info.ssid);
        shell_transcript_appendf("wifi.ap: %s, rssi=%d, channel=%u\n",
                                 ap_ssid,
                                 ap_info.rssi,
                                 (unsigned int)ap_info.primary);
    } else if (error != ESP_ERR_WIFI_NOT_CONNECT) {
        shell_transcript_appendf("wifi.ap_info_error: %s (0x%x)\n",
                                 esp_err_to_name(error),
                                 (unsigned int)error);
    }

    if (s_wifi_sta_netif != NULL && esp_netif_get_ip_info(s_wifi_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        shell_transcript_appendf("wifi.ip: " IPSTR "\n", IP2STR(&ip_info.ip));
    }
}

static void shell_command_wifi_disconnect(void)
{
    esp_err_t error;

    if (s_wifi_state == SHELL_WIFI_STATE_STARTING) {
        shell_transcript_append_text("wifi: initialization is in progress\n");
        return;
    }

    if (s_wifi_state != SHELL_WIFI_STATE_STARTED) {
        shell_transcript_appendf("wifi: stack is not ready (%s)\n", shell_wifi_state_string());
        return;
    }

    s_wifi_connect_requested = false;
    s_wifi_connected = false;
    shell_wifi_append_step("esp_wifi_disconnect()");
    error = esp_wifi_disconnect();
    if (error == ESP_OK || error == ESP_ERR_WIFI_NOT_CONNECT) {
        shell_transcript_append_text("wifi: disconnect requested\n");
        return;
    }

    shell_transcript_appendf("wifi: disconnect failed with %s (0x%x)\n",
                             esp_err_to_name(error),
                             (unsigned int)error);
}

static void shell_execute_wifi_command(char *command)
{
    char *argv[5];
    int argc = shell_split_args(command, argv, 5);

    if (argc <= 1 || strcmp(argv[1], "help") == 0) {
        shell_command_wifi_help();
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        shell_command_wifi_status();
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        shell_command_wifi_scan();
        return;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        shell_command_wifi_disconnect();
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        if (argc == 2) {
            if (!shell_wifi_defaults_available()) {
                shell_transcript_append_text("wifi: sdkconfig default credentials are not configured\n");
                return;
            }

            if (s_wifi_state == SHELL_WIFI_STATE_STARTED) {
                (void)shell_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                          CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
            } else {
                shell_transcript_append_text("wifi: starting stack on demand\n");
                (void)shell_wifi_begin_connect_request(NULL, NULL, true);
            }
            return;
        }

        if (argc == 4) {
            if (s_wifi_state == SHELL_WIFI_STATE_STARTED) {
                (void)shell_wifi_connect_with_credentials(argv[2], argv[3]);
            } else {
                shell_transcript_append_text("wifi: starting stack on demand\n");
                (void)shell_wifi_begin_connect_request(argv[2], argv[3], false);
            }
            return;
        }

        shell_transcript_append_text("Usage: wifi connect or wifi connect <ssid> <pass>\n");
        return;
    }

    shell_transcript_appendf("Unknown wifi subcommand: %s\n", argv[1]);
}
#endif

static void shell_wifi_runtime_init(void)
{
#if SHELL_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    // AI: keep Wi-Fi startup strictly runtime-driven from sdkconfig and report every step into the shell transcript.
    s_wifi_last_detail[0] = '\0';
    shell_wifi_append_step("runtime Wi-Fi initialization requested from sdkconfig");

#if CONFIG_ESP_HOSTED_ENABLED
    // AI: the checked-in esp32p4 host path now targets an ESP32-C6 co-processor over ESP-Hosted SDIO.
    shell_wifi_set_detail("ESP-Hosted SDIO backend targeting ESP32-C6 on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54");
    shell_wifi_append_step("ESP-Hosted SDIO backend: ESP32-C6 on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54");
    shell_wifi_append_step("esp_hosted_connect_to_slave()");
    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK) {
        shell_wifi_set_detail("ESP-Hosted did not connect to the ESP32-C6 co-processor on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54. Check the hosted slave firmware, pull-ups on CMD/DAT0-DAT3, and the reset wiring.");
        shell_wifi_append_error("esp_hosted_connect_to_slave()", error);
        shell_schedule_transcript_appendf("[wifi] %s\n", s_wifi_last_detail);
        shell_schedule_transcript_appendf("%s", "[wifi] Recovery: copy coprocessor/esp32c6_slave/build/esp32c6_hosted_slave_merged.bin to the SD card and run c6update <sd:/path/to/esp32c6_hosted_slave_merged.bin> if the C6 firmware is missing or stale\n");
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
            return;
        }

        shell_wifi_append_step("nvs_flash_init() retry");
        error = nvs_flash_init();
    }

    if (error != ESP_OK) {
        shell_wifi_append_error("nvs_flash_init()", error);
        return;
    }

    shell_wifi_append_step("esp_netif_init()");
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        shell_wifi_append_error("esp_netif_init()", error);
        return;
    }

    shell_wifi_append_step("esp_event_loop_create_default()");
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        shell_wifi_append_error("esp_event_loop_create_default()", error);
        return;
    }

    shell_wifi_append_step("esp_netif_create_default_wifi_sta()");
    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_sta_netif == NULL) {
        shell_wifi_append_error("esp_netif_create_default_wifi_sta()", ESP_FAIL);
        return;
    }

    shell_wifi_append_step("esp_wifi_init()");
    error = esp_wifi_init(&wifi_init_cfg);
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_init()", error);
        return;
    }

    shell_wifi_append_step("esp_event_handler_instance_register(...)");
    error = shell_wifi_register_event_handlers();
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_event_handler_instance_register(...)", error);
        return;
    }

    shell_wifi_append_step("esp_wifi_set_storage(WIFI_STORAGE_RAM)");
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_set_storage(WIFI_STORAGE_RAM)", error);
        return;
    }

    shell_wifi_append_step("esp_wifi_set_mode(WIFI_MODE_STA)");
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_set_mode(WIFI_MODE_STA)", error);
        return;
    }

    shell_wifi_append_step("esp_wifi_start()");
    error = esp_wifi_start();
    if (error != ESP_OK) {
        shell_wifi_append_error("esp_wifi_start()", error);
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
    // AI: the current esp32p4 board can host an external radio, but the checked-in sdkconfig does not enable that path.
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
    lv_textarea_set_text(s_input_line, buffer);
    lv_textarea_set_cursor_pos(s_input_line, LV_TEXTAREA_CURSOR_LAST);
}

static void shell_input_line_reset(void)
{
    shell_input_line_set_text("");
}

static void shell_extract_input_text(char *output, size_t output_size)
{
    const char *text = lv_textarea_get_text(s_input_line);

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
    shell_transcript_append_text("  c6update [path|default] Flash a merged ESP32-C6 image from the SD card\n");
    shell_transcript_append_text("  sysinfo Show board/runtime information\n");
    shell_transcript_append_text("  wifi scan Scan for nearby access points after Wi-Fi startup\n");
    shell_transcript_append_text("  sd ls [path] List files on the SD card with DOS-style names\n");
    shell_transcript_append_text("  mem     Show heap and PSRAM usage\n");
    shell_transcript_append_text("  gpio status Show key board and co-processor GPIO levels\n");
    shell_transcript_append_text("  debug   Show last 5 errors, Wi-Fi state, heap, and warnings\n");
    shell_transcript_append_text("  version Show app and ESP-IDF version\n");
    shell_transcript_append_text("  about   Show shell and board summary\n");
    shell_transcript_append_text("  wifi    Wi-Fi status/connect/disconnect commands\n");
    shell_transcript_append_text("  clear   Clear the terminal history\n");
    shell_transcript_append_text("  reboot  Restart the board\n");
    shell_transcript_append_text("History recall: Prev/Next buttons above the keyboard\n");
    shell_transcript_appendf("  c6update default uses %s\n", SHELL_C6UPDATE_DEFAULT_IMAGE);
}

static void shell_command_sysinfo(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif

    shell_transcript_appendf("board.requested_name: %s\n", SHELL_BOARD_REQUESTED);
    shell_transcript_appendf("board.detected_name: %s\n", SHELL_BOARD_DETECTED);
    shell_transcript_appendf("display: %d x %d, JD9165, reset GPIO %d, backlight GPIO %d\n",
                             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_RST, BSP_LCD_BACKLIGHT);
    shell_transcript_appendf("display.timing: pclk=%dMHz, lanes=%d, bitrate=%dMbps, hsync=%d hbp=%d hfp=%d vsync=%d vbp=%d vfp=%d\n",
                             BSP_LCD_PIXEL_CLOCK_MHZ,
                             BSP_LCD_MIPI_DSI_LANE_NUM,
                             BOARD_CFG_LCD_DSI_BUS_LANE_BITRATE_MBPS_RUNTIME,
                             BSP_LCD_MIPI_DSI_LCD_HSYNC,
                             BSP_LCD_MIPI_DSI_LCD_HBP,
                             BSP_LCD_MIPI_DSI_LCD_HFP,
                             BSP_LCD_MIPI_DSI_LCD_VSYNC,
                             BSP_LCD_MIPI_DSI_LCD_VBP,
                             BSP_LCD_MIPI_DSI_LCD_VFP);
    shell_transcript_appendf("display.buffer: draw=%d, double=%d, dma=%d, spiram=%d, sw_rotate=%d\n",
                             BOARD_CFG_LCD_DRAW_BUFFER_SIZE,
                             BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE,
                             BOARD_CFG_APP_BUFFER_DMA,
                             BOARD_CFG_APP_BUFFER_SPIRAM,
                             BOARD_CFG_APP_SW_ROTATE);
    shell_transcript_appendf("touch: GT911 on I2C%d, SDA GPIO %d, SCL GPIO %d, %dHz, pullup=%d, swap_xy=%d, mirror_x=%d, mirror_y=%d\n",
                             BSP_I2C_NUM,
                             BSP_I2C_SDA,
                             BSP_I2C_SCL,
                             BOARD_CFG_I2C_CLK_SPEED_HZ,
                             BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP,
                             BOARD_CFG_TOUCH_SWAP_XY,
                             BOARD_CFG_TOUCH_MIRROR_X,
                             BOARD_CFG_TOUCH_MIRROR_Y);
    shell_transcript_appendf("storage: spiffs=%s, sd=%s\n", BSP_SPIFFS_MOUNT_POINT, BSP_SD_MOUNT_POINT);
    shell_transcript_appendf("c6.flash_uart: port=%d tx=%d rx=%d baud=%d\n",
                             CONFIG_P4MINISHELL_C6_FLASH_UART_PORT,
                             CONFIG_P4MINISHELL_C6_FLASH_UART_TX_GPIO,
                             CONFIG_P4MINISHELL_C6_FLASH_UART_RX_GPIO,
                             CONFIG_P4MINISHELL_C6_FLASH_UART_BAUDRATE);
    shell_transcript_appendf("c6.flash_ctrl: en=%d reset=%d effective_reset=%d boot=%d busy=%s\n",
                             CONFIG_P4MINISHELL_C6_EN_GPIO,
                             CONFIG_P4MINISHELL_C6_RESET_GPIO,
                             shell_c6_effective_reset_gpio(),
                             CONFIG_P4MINISHELL_C6_BOOT_GPIO,
                             s_c6_update_in_progress ? "yes" : "no");
    shell_transcript_appendf("idf: %s\n", esp_get_idf_version());
    shell_transcript_appendf("heap: free=%u bytes, internal_free=%u bytes\n",
                             (unsigned int)free_heap,
                             (unsigned int)free_internal);
#if CONFIG_SPIRAM
    shell_transcript_appendf("psram: enabled, total=%u bytes, free=%u bytes\n",
                             (unsigned int)total_psram,
                             (unsigned int)free_psram);
#else
    shell_transcript_append_text("psram: disabled\n");
#endif

    switch (s_wifi_state) {
    case SHELL_WIFI_STATE_STARTING:
        shell_transcript_append_text("wifi: runtime initialization is in progress\n");
        break;
    case SHELL_WIFI_STATE_STARTED:
        shell_transcript_appendf("wifi: runtime initialized in STA mode from sdkconfig, connected=%s\n",
                                 s_wifi_connected ? "yes" : "no");
        break;
    case SHELL_WIFI_STATE_FAILED:
        shell_transcript_appendf("wifi: runtime initialization failed with %s (0x%x)\n",
                                 esp_err_to_name(s_wifi_last_error),
                                 (unsigned int)s_wifi_last_error);
        break;
    case SHELL_WIFI_STATE_SKIPPED_DISABLED:
        shell_transcript_append_text("wifi: skipped because sdkconfig does not enable native or ESP-Hosted Wi-Fi\n");
        break;
    case SHELL_WIFI_STATE_SKIPPED_UNSUPPORTED:
        shell_transcript_append_text("wifi: unsupported on current target/SoC caps\n");
        break;
    case SHELL_WIFI_STATE_NOT_ATTEMPTED:
    default:
        shell_transcript_append_text("wifi: runtime initialization not attempted yet\n");
        break;
    }
}

static void shell_command_mem(void)
{
    // AI: MSDOS styling keeps command output compact and scan-friendly for the retro terminal presentation.
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    shell_transcript_appendf("mem.heap.free=%u bytes\n", (unsigned int)free_heap);
    shell_transcript_appendf("mem.heap.min=%u bytes\n", (unsigned int)min_heap);
    shell_transcript_appendf("mem.heap.internal=%u bytes\n", (unsigned int)free_internal);
#if CONFIG_SPIRAM
    shell_transcript_appendf("mem.psram.free=%u bytes\n", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    shell_transcript_appendf("mem.psram.total=%u bytes\n", (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
    shell_transcript_append_text("mem.psram: disabled\n");
#endif
}

static void shell_print_gpio_level(const char *name, int gpio_num)
{
    int level;

    if (gpio_num < 0) {
        shell_transcript_appendf("gpio.%s: not-configured\n", name);
        return;
    }

    level = gpio_get_level((gpio_num_t)gpio_num);
    shell_transcript_appendf("gpio.%s: gpio=%d level=%d\n", name, gpio_num, level);
}

static void shell_command_gpio_status(void)
{
    shell_print_gpio_level("display_reset", BSP_LCD_RST);
    shell_print_gpio_level("backlight", BSP_LCD_BACKLIGHT);
    shell_print_gpio_level("c6_en", CONFIG_P4MINISHELL_C6_EN_GPIO);
    shell_print_gpio_level("c6_reset", CONFIG_P4MINISHELL_C6_RESET_GPIO);
    shell_print_gpio_level("c6_boot", CONFIG_P4MINISHELL_C6_BOOT_GPIO);
}

static void shell_command_debug(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t idx;
    size_t start;

    shell_transcript_appendf("debug.wifi_state: %s\n", shell_wifi_state_string());
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
    shell_transcript_appendf("version.app: %s\n", SHELL_BOOT_MESSAGE);
    shell_transcript_appendf("version.idf: %s\n", esp_get_idf_version());
}

static void shell_command_about(void)
{
    shell_transcript_appendf("about.shell: %s\n", SHELL_BOOT_MESSAGE);
    shell_transcript_appendf("about.board: %s / %s\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    shell_transcript_append_text("about.ui: locked transcript with touch keyboard, history buttons, and command prompt\n");
}

static void shell_command_sd_ls(char *command)
{
    char *argv[3];
    int argc = shell_split_args(command, argv, 3);
    const char *input_path = argc >= 3 ? argv[2] : BSP_SD_MOUNT_POINT;
    char normalized_path[320];
    DIR *dir;
    struct dirent *entry;
    esp_err_t error;
    bool mounted_here = false;

    if (argc > 3) {
        shell_transcript_append_text("Usage: sd ls [path]\n");
        shell_record_warningf("sd", "Usage error for sd ls command");
        return;
    }

    if (strncmp(input_path, "sd:/", 4) == 0 || input_path[0] != '/') {
        shell_c6update_normalize_path(input_path, normalized_path, sizeof(normalized_path));
    } else {
        snprintf(normalized_path, sizeof(normalized_path), "%s", input_path);
    }

    error = bsp_sdcard_mount();
    if (error == ESP_OK) {
        mounted_here = true;
    } else if (error != ESP_ERR_INVALID_STATE) {
        shell_transcript_append_text("sd: SD card not present - insert and retry\n");
        shell_record_errorf("sd", error, "SD card not present or mount failed");
        return;
    }

    dir = opendir(normalized_path);
    if (dir == NULL) {
        shell_transcript_appendf("sd: could not open %s\n", normalized_path);
        shell_record_errorf("sd", ESP_ERR_NOT_FOUND, "Could not open directory %s", normalized_path);
        goto cleanup;
    }

    shell_transcript_appendf("sd: listing %s\n", normalized_path);
    while ((entry = readdir(dir)) != NULL) {
        shell_transcript_appendf("sd: %s\n", entry->d_name);
    }
    closedir(dir);
    dir = NULL;

cleanup:
    if (dir != NULL) {
        closedir(dir);
    }
    if (mounted_here) {
        error = bsp_sdcard_unmount();
        if (error != ESP_OK) {
            shell_record_warningf("sd", "Unmount warning after sd ls: %s", esp_err_to_name(error));
        }
    }
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void shell_execute_command(char *command)
{
    char *trimmed = shell_trim(command);

    if (trimmed[0] == '\0') {
        return;
    }

    if (strcmp(trimmed, "help") == 0) {
        shell_command_help();
        return;
    }

    if (strncmp(trimmed, "c6update", 8) == 0 && (trimmed[8] == '\0' || isspace((unsigned char)trimmed[8]))) {
        shell_execute_c6update_command(trimmed);
        return;
    }

    if (strcmp(trimmed, "sysinfo") == 0) {
        shell_command_sysinfo();
        return;
    }

    if (strcmp(trimmed, "mem") == 0) {
        shell_command_mem();
        return;
    }

    if (strcmp(trimmed, "debug") == 0) {
        shell_command_debug();
        return;
    }

    if (strcmp(trimmed, "version") == 0) {
        shell_command_version();
        return;
    }

    if (strcmp(trimmed, "about") == 0) {
        shell_command_about();
        return;
    }

    if (strncmp(trimmed, "gpio", 4) == 0 && (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) {
        if (strcmp(trimmed, "gpio") == 0 || strcmp(trimmed, "gpio status") == 0) {
            shell_command_gpio_status();
            return;
        }
        shell_transcript_append_text("Usage: gpio status\n");
        shell_record_warningf("gpio", "Usage error for gpio command");
        return;
    }

    if (strncmp(trimmed, "sd", 2) == 0 && (trimmed[2] == '\0' || isspace((unsigned char)trimmed[2]))) {
        if (strcmp(trimmed, "sd") == 0 || strncmp(trimmed, "sd ls", 5) == 0) {
            shell_command_sd_ls(trimmed);
            return;
        }
        shell_transcript_append_text("Usage: sd ls [path]\n");
        shell_record_warningf("sd", "Usage error for sd command");
        return;
    }

    if (strncmp(trimmed, "wifi", 4) == 0 && (trimmed[4] == '\0' || isspace((unsigned char)trimmed[4]))) {
    #if SHELL_WIFI_RUNTIME_ENABLED
        shell_execute_wifi_command(trimmed);
#else
        shell_transcript_append_text("wifi commands unavailable because sdkconfig does not enable the Wi-Fi stack\n");
#endif
        return;
    }

    if (strcmp(trimmed, "clear") == 0) {
        shell_transcript_reset();
        shell_record_infof("shell", "Transcript cleared");
        return;
    }

    if (strcmp(trimmed, "reboot") == 0) {
        shell_transcript_append_text("Rebooting...\n");
        if (xTaskCreate(reboot_task, "reboot_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            shell_record_errorf("reboot", ESP_FAIL, "Failed to schedule reboot task");
        }
        return;
    }

    shell_transcript_appendf("Unknown command: %s\n", trimmed);
    shell_record_warningf("shell", "Unknown command: %s", trimmed);
}

static void shell_history_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    if (lv_event_get_target(event) == s_history_prev_button) {
        shell_recall_history(-1);
    } else if (lv_event_get_target(event) == s_history_next_button) {
        shell_recall_history(1);
    }
}

static void shell_transcript_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, s_input_line);
        lv_obj_add_state(s_input_line, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(s_input_line, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void shell_input_line_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(s_keyboard, s_input_line);
        lv_textarea_set_cursor_pos(s_input_line, LV_TEXTAREA_CURSOR_LAST);
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        const char *text = lv_textarea_get_text(s_input_line);

        if (strncmp(text, SHELL_PROMPT, strlen(SHELL_PROMPT)) != 0) {
            char repaired[SHELL_COMMAND_BYTES];

            snprintf(repaired, sizeof(repaired), "%s", text);
            shell_trim(repaired);
            shell_input_line_set_text(repaired);
        }
        return;
    }

    if (code == LV_EVENT_READY) {
        char command[SHELL_COMMAND_BYTES];
        char transcript_command[SHELL_COMMAND_BYTES];

        // AI: LV_EVENT_READY handler is the confirmed working Enter path for command execution on the locked transcript UI.
        shell_extract_input_text(command, sizeof(command));
        shell_format_command_for_transcript(command, transcript_command, sizeof(transcript_command));
        shell_transcript_appendf("%s%s\n", SHELL_PROMPT, transcript_command);
        if (shell_command_should_store_history(command)) {
            shell_store_command_history(command);
        }
        s_command_history_cursor = -1;
        s_history_draft[0] = '\0';
        shell_input_line_reset();
        shell_execute_command(command);
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

static const lv_font_t *shell_select_font(void)
{
#if LV_FONT_UNSCII_16
    return &lv_font_unscii_16;
#else
    return LV_FONT_DEFAULT;
#endif
}

static void shell_build_ui(void)
{
    lv_obj_t *input_row;
    lv_obj_t *screen = lv_screen_active();
    const lv_font_t *terminal_font = shell_select_font();

    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B0F10), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_row(screen, 0, 0);

    // AI: locked transcript UI keeps the scrollback read-only while commands are entered on the dedicated prompt line.
    s_history_transcript = lv_textarea_create(screen);
    lv_obj_set_width(s_history_transcript, LV_PCT(100));
    lv_obj_set_flex_grow(s_history_transcript, 1);
    lv_textarea_set_one_line(s_history_transcript, false);
    lv_textarea_set_cursor_click_pos(s_history_transcript, false);
    lv_obj_set_style_bg_color(s_history_transcript, lv_color_hex(0x050806), 0);
    lv_obj_set_style_bg_opa(s_history_transcript, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_history_transcript, 0, 0);
    lv_obj_set_style_pad_all(s_history_transcript, 16, 0);
    lv_obj_set_style_text_color(s_history_transcript, lv_color_hex(0x8DFF96), 0);
    lv_obj_set_style_text_font(s_history_transcript, terminal_font, 0);
    lv_obj_set_scrollbar_mode(s_history_transcript, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(s_history_transcript, shell_transcript_event_cb, LV_EVENT_ALL, NULL);

    input_row = lv_obj_create(screen);
    lv_obj_set_width(input_row, LV_PCT(100));
    lv_obj_set_height(input_row, SHELL_INPUT_ROW_HEIGHT);
    lv_obj_set_layout(input_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(input_row, 8, 0);
    lv_obj_set_style_pad_ver(input_row, 6, 0);
    lv_obj_set_style_pad_column(input_row, 8, 0);
    lv_obj_set_style_bg_color(input_row, lv_color_hex(0x111816), 0);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);

    s_history_prev_button = lv_button_create(input_row);
    lv_obj_set_size(s_history_prev_button, 82, LV_PCT(100));
    lv_obj_add_event_cb(s_history_prev_button, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_label = lv_label_create(s_history_prev_button);
    lv_label_set_text(prev_label, "Prev");
    lv_obj_center(prev_label);

    s_history_next_button = lv_button_create(input_row);
    lv_obj_set_size(s_history_next_button, 82, LV_PCT(100));
    lv_obj_add_event_cb(s_history_next_button, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_label = lv_label_create(s_history_next_button);
    lv_label_set_text(next_label, "Next");
    lv_obj_center(next_label);

    s_input_line = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(s_input_line, 1);
    lv_obj_set_height(s_input_line, LV_PCT(100));
    lv_textarea_set_one_line(s_input_line, true);
    lv_textarea_set_cursor_click_pos(s_input_line, false);
    lv_obj_set_style_bg_color(s_input_line, lv_color_hex(0x050806), 0);
    lv_obj_set_style_bg_opa(s_input_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_input_line, 0, 0);
    lv_obj_set_style_pad_hor(s_input_line, 12, 0);
    lv_obj_set_style_pad_ver(s_input_line, 10, 0);
    lv_obj_set_style_text_color(s_input_line, lv_color_hex(0x8DFF96), 0);
    lv_obj_set_style_text_font(s_input_line, terminal_font, 0);
    lv_obj_add_event_cb(s_input_line, shell_input_line_event_cb, LV_EVENT_ALL, NULL);

    // AI: MSDOS styling keeps the shell dense, keyboard-driven, and visually close to a classic terminal.
    s_keyboard = lv_keyboard_create(screen);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_height(s_keyboard, SHELL_KEYBOARD_HEIGHT);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_keyboard, s_input_line);
    lv_obj_set_style_text_font(s_keyboard, terminal_font, 0);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x1D2625), 0);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);

    shell_transcript_reset();
    shell_transcript_append_text(SHELL_BOOT_MESSAGE);
    shell_transcript_append_text("\n");
    shell_history_transcript_scroll_to_end();
    shell_input_line_reset();
}

void app_main(void)
{
    lv_display_t *display;

    // AI: preserve the BSP-driven LCD and touch startup path exactly as the original demo.
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BOARD_CFG_LCD_DRAW_BUFFER_SIZE,
        .double_buffer = BOARD_CFG_LCD_DRAW_BUFFER_DOUBLE,
        .flags = {
            .buff_dma = BOARD_CFG_APP_BUFFER_DMA,
            .buff_spiram = BOARD_CFG_APP_BUFFER_SPIRAM,
            .sw_rotate = BOARD_CFG_APP_SW_ROTATE,
        }
    };

    display = bsp_display_start_with_config(&cfg);
    if (display == NULL) {
        ESP_LOGE(SHELL_TAG, "Display initialization failed");
        shell_record_errorf("init", ESP_FAIL, "Display initialization failed");
        return;
    }

    bsp_display_backlight_on();

    // AI: create the LVGL shell surface after the BSP has started LVGL and touch input.
    bsp_display_lock(0);
    shell_build_ui();
    bsp_display_unlock();

    // AI: WiFi init stays on-demand so the shell remains bootable even when the hosted C6 is down.
    shell_transcript_append_text("Wi-Fi initializes on demand when you run a wifi command.\n");
    shell_transcript_append_text("Enter runs commands from the prompt line; history stays locked above.\n");

    // AI: command parsing, prompt management, transcript updates, history recall, and runtime Wi-Fi control are event-driven from the dedicated input line.
}

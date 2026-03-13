#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
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
#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define SHELL_TAG "p4minishell"
#define SHELL_BOARD_REQUESTED "JC1060P470C"
#define SHELL_BOARD_DETECTED "ESP32-P4-Function-EV-Board"
#define SHELL_BOOT_MESSAGE "P4MiniShell v0.1 ready | JC1060P470C | type help"
#define SHELL_PROMPT "P4Shell> "
#define SHELL_TRANSCRIPT_BYTES 8192
#define SHELL_COMMAND_BYTES 256
#define SHELL_COMMAND_HISTORY_DEPTH 10
#define SHELL_KEYBOARD_HEIGHT 240
#define SHELL_INPUT_ROW_HEIGHT 52
#define SHELL_WIFI_SSID_BYTES 33
#define SHELL_WIFI_PASSWORD_BYTES 65
#define SHELL_WIFI_DETAIL_BYTES 256
#define SHELL_WIFI_RUNTIME_ENABLED (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_HOST_WIFI_ENABLED || CONFIG_ESP_HOSTED_ENABLED)

#ifndef CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID
#define CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID ""
#endif

#ifndef CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD
#define CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD ""
#endif

typedef enum {
    SHELL_WIFI_STATE_NOT_ATTEMPTED = 0,
    SHELL_WIFI_STATE_STARTED,
    SHELL_WIFI_STATE_FAILED,
    SHELL_WIFI_STATE_SKIPPED_DISABLED,
    SHELL_WIFI_STATE_SKIPPED_UNSUPPORTED,
} shell_wifi_state_t;

static lv_obj_t *s_history_transcript;
static lv_obj_t *s_input_line;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_history_prev_button;
static lv_obj_t *s_history_next_button;
static char s_transcript[SHELL_TRANSCRIPT_BYTES];
static char s_command_history[SHELL_COMMAND_HISTORY_DEPTH][SHELL_COMMAND_BYTES];
static size_t s_command_history_count;
static int s_command_history_cursor = -1;
static char s_history_draft[SHELL_COMMAND_BYTES];
static shell_wifi_state_t s_wifi_state = SHELL_WIFI_STATE_NOT_ATTEMPTED;
static esp_err_t s_wifi_last_error = ESP_OK;
static bool s_wifi_connected;
static bool s_wifi_connect_requested;
static char s_wifi_target_ssid[SHELL_WIFI_SSID_BYTES];
static char s_wifi_last_detail[SHELL_WIFI_DETAIL_BYTES];

#if SHELL_WIFI_RUNTIME_ENABLED
static esp_netif_t *s_wifi_sta_netif;
static esp_event_handler_instance_t s_wifi_event_any_id;
static esp_event_handler_instance_t s_wifi_got_ip_event;
#endif

static void shell_input_line_reset(void);

static void shell_transcript_render(void)
{
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

static void shell_async_transcript_append_cb(void *user_data)
{
    char *text = (char *)user_data;

    if (text == NULL) {
        return;
    }

    shell_transcript_append_text(text);
    shell_history_transcript_scroll_to_end();
    free(text);
}

static void shell_schedule_transcript_appendf(const char *format, ...)
{
    char stack_buffer[512];
    char *heap_buffer;
    va_list args;
    size_t buffer_len;

    va_start(args, format);
    vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);

    buffer_len = strlen(stack_buffer) + 1;
    heap_buffer = (char *)malloc(buffer_len);
    if (heap_buffer == NULL) {
        ESP_LOGW(SHELL_TAG, "Failed to allocate transcript message buffer");
        return;
    }

    memcpy(heap_buffer, stack_buffer, buffer_len);
    if (lv_async_call(shell_async_transcript_append_cb, heap_buffer) != LV_RESULT_OK) {
        ESP_LOGW(SHELL_TAG, "Failed to queue transcript update");
        free(heap_buffer);
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

static void shell_command_wifi_help(void)
{
    shell_transcript_append_text("Wi-Fi commands:\n");
    shell_transcript_append_text("  wifi status                 Show Wi-Fi runtime state and IP info\n");
    shell_transcript_append_text("  wifi connect                Connect using sdkconfig default credentials\n");
    shell_transcript_append_text("  wifi connect <ssid> <pass>  Connect using runtime credentials\n");
    shell_transcript_append_text("  wifi disconnect             Disconnect the current station session\n");
    shell_transcript_append_text("  wifi connect passwords are masked in transcript history and not stored in command recall\n");
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

            (void)shell_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                      CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
            return;
        }

        if (argc == 4) {
            (void)shell_wifi_connect_with_credentials(argv[2], argv[3]);
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
    shell_transcript_append_text("  sysinfo Show board/runtime information\n");
    shell_transcript_append_text("  wifi    Wi-Fi status/connect/disconnect commands\n");
    shell_transcript_append_text("  clear   Clear the terminal history\n");
    shell_transcript_append_text("  reboot  Restart the board\n");
    shell_transcript_append_text("History recall: Prev/Next buttons above the keyboard\n");
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

    if (strcmp(trimmed, "sysinfo") == 0) {
        shell_command_sysinfo();
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
        return;
    }

    if (strcmp(trimmed, "reboot") == 0) {
        shell_transcript_append_text("Rebooting...\n");
        xTaskCreate(reboot_task, "reboot_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
        return;
    }

    shell_transcript_appendf("Unknown command: %s\n", trimmed);
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

    // AI: the transcript and input line are separated so command submission does not mutate the visible history.
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
        return;
    }

    bsp_display_backlight_on();

    // AI: create the LVGL shell surface after the BSP has started LVGL and touch input.
    bsp_display_lock(0);
    shell_build_ui();
    bsp_display_unlock();

    // AI: initialize the Wi-Fi stack after the UI exists so boot-time status messages can be queued safely into the transcript.
    shell_wifi_runtime_init();

    // AI: command parsing, prompt management, transcript updates, history recall, and runtime Wi-Fi control are event-driven from the dedicated input line.
}

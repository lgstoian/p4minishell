#ifndef P4MINISHELL_NETWORKING_H
#define P4MINISHELL_NETWORKING_H

#include <stdbool.h>

#include "esp_err.h"

#define NETWORKING_WIFI_SSID_BYTES 33
#define NETWORKING_WIFI_PASSWORD_BYTES 65

typedef enum {
    NETWORKING_WIFI_STATE_NOT_ATTEMPTED = 0,
    NETWORKING_WIFI_STATE_STARTING,
    NETWORKING_WIFI_STATE_STARTED,
    NETWORKING_WIFI_STATE_FAILED,
    NETWORKING_WIFI_STATE_SKIPPED_DISABLED,
    NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED,
} networking_wifi_state_t;

typedef struct {
    bool should_restore_runtime;
    bool should_restore_connection;
    char ssid[NETWORKING_WIFI_SSID_BYTES];
    char password[NETWORKING_WIFI_PASSWORD_BYTES];
} networking_wifi_restore_state_t;

typedef struct {
    void (*transcript_append_text)(const char *text);
    void (*schedule_transcript_append_text)(const char *text);
    void (*record_error)(const char *tag, esp_err_t error, const char *message);
    void (*record_warning)(const char *tag, const char *message);
    void (*record_info)(const char *tag, const char *message);
} networking_host_ops_t;

void networking_init(const networking_host_ops_t *ops);

// AI: Networking refactored to separate module (WiFi + Bluetooth via C6 esp-hosted SDIO)
// AI: WiFi behavior preserved 100% - only moved code, no functional change
void networking_handle_wifi_command(char *command);
void networking_wifi_status(void);
void networking_wifi_scan(void);
void networking_wifi_diag(void);
void networking_wifi_disconnect(void);

const char *networking_wifi_state_string(void);
networking_wifi_state_t networking_wifi_state(void);
esp_err_t networking_wifi_last_error(void);
bool networking_wifi_is_connected(void);
void networking_append_sysinfo_summary(void);

esp_err_t networking_wifi_wait_for_ota(void);
bool networking_wifi_is_starting(void);
void networking_wifi_capture_restore_state(networking_wifi_restore_state_t *restore_state);
esp_err_t networking_wifi_shutdown(void);
esp_err_t networking_wifi_restore_after_ota_failure(const networking_wifi_restore_state_t *restore_state);
void networking_wifi_request_post_ota_restore(const networking_wifi_restore_state_t *restore_state);

#endif
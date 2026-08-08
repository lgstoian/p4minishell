#ifndef P4MINISHELL_NETWORKING_H
#define P4MINISHELL_NETWORKING_H

/**
 * @file networking.h
 * @brief Hosted networking module for P4MiniShell.
 *
 * The single owner of every ESP-Hosted and esp_wifi_remote call in the
 * firmware. Wi-Fi and Bluetooth both run on the ESP32-C6 co-processor over
 * SDIO; this module owns the transport bring-up, the Wi-Fi station lifecycle,
 * the hosted NimBLE lifecycle (delegated to bluetooth.c), status reporting,
 * and the OTA wait/restore hooks.
 *
 * Only the official Espressif path is used: `espressif/esp_hosted` for the
 * transport and `espressif/esp_wifi_remote` for the Wi-Fi API. There is no
 * custom RPC, no alternative transport, and no re-implemented control plane.
 *
 * Ownership rule:
 *   No `esp_hosted_*`, `esp_wifi_*`, `esp_netif_*`, or NimBLE call may appear
 *   outside `components/networking/`. The one sanctioned exception is
 *   `components/c6ota/`, which drives `esp_hosted_slave_ota_*` because
 *   co-processor firmware update is its entire purpose. Consumers that need
 *   networking state use the status helpers below.
 *
 * ---------------------------------------------------------------------------
 * CANONICAL INITIALIZATION ORDER
 * ---------------------------------------------------------------------------
 * `networking_wifi_start_runtime()` performs these steps in exactly this
 * order. The order is load-bearing, not stylistic:
 *
 *   1. esp_hosted_init()
 *   2. esp_hosted_connect_to_slave()
 *        The SDIO link must exist before anything else. esp_wifi_init() is
 *        the esp_wifi_remote shim and has no peer to talk to until the slave
 *        is connected.
 *   3. version compatibility gate
 *        Refuse to continue when the C6 firmware is outside the host's
 *        supported range. Proceeding produces failures far harder to diagnose.
 *   4. nvs_flash_init()          (erase-and-retry on a corrupt partition)
 *        esp_wifi_init() persists calibration and config to NVS.
 *   5. esp_netif_init()
 *   6. esp_event_loop_create_default()
 *   7. esp_netif_create_default_wifi_sta()
 *   8. esp_wifi_init()           (via esp_wifi_remote)
 *   9. event handler registration
 *  10. esp_wifi_set_mode(WIFI_MODE_STA)   — station only, always
 *  11. esp_wifi_start()
 *
 * Hosted NimBLE is brought up separately and lazily by bluetooth.c, which
 * receives the same host callback surface during networking_init().
 *
 * Station-only is a hard constraint: no SoftAP, no APSTA, no concurrent modes.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define NETWORKING_WIFI_SSID_BYTES 33
#define NETWORKING_WIFI_PASSWORD_BYTES 65

/** Wi-Fi runtime state enum for status reporting. */
typedef enum {
    NETWORKING_WIFI_STATE_NOT_ATTEMPTED = 0,
    NETWORKING_WIFI_STATE_STARTING,
    NETWORKING_WIFI_STATE_STARTED,
    NETWORKING_WIFI_STATE_FAILED,
    NETWORKING_WIFI_STATE_SKIPPED_DISABLED,
    NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED,
} networking_wifi_state_t;

/** OTA restore state: captures Wi-Fi configuration before OTA for later recovery. */
typedef struct {
    bool should_restore_runtime;
    bool should_restore_connection;
    char ssid[NETWORKING_WIFI_SSID_BYTES];
    char password[NETWORKING_WIFI_PASSWORD_BYTES];
} networking_wifi_restore_state_t;

/**
 * Host callback table shared by Wi-Fi and Bluetooth modules.
 * Filled by main.c and passed to networking_init().
 */
typedef struct {
    void (*transcript_append_text)(const char *text);
    void (*schedule_transcript_append_text)(const char *text);
    void (*transcript_append_ansi)(const char *text);
    void (*record_error)(const char *tag, esp_err_t error, const char *message);
    void (*record_warning)(const char *tag, const char *message);
    void (*record_info)(const char *tag, const char *message);
    void (*notify_header)(const char *text, uint32_t timeout_ms);
} networking_host_ops_t;

/** Initialize networking: stores host callbacks, starts boot-time Wi-Fi restore, bootstraps Bluetooth. */
void networking_init(const networking_host_ops_t *ops);

// ---- Shell-facing Wi-Fi commands ----
void networking_handle_wifi_command(char *command);
void networking_wifi_status(void);
void networking_wifi_scan(void);
void networking_wifi_diag(void);
void networking_wifi_disconnect(void);

// ---- Status helpers ----
const char *networking_wifi_state_string(void);
networking_wifi_state_t networking_wifi_state(void);
esp_err_t networking_wifi_last_error(void);
bool networking_wifi_is_connected(void);
void networking_append_sysinfo_summary(void);

/**
 * Read the RSSI of the currently associated access point.
 *
 * Exists so the header status bar and any other consumer can report signal
 * strength without calling esp_wifi_* directly. Every ESP-Hosted and
 * esp_wifi_remote call in the firmware stays inside this module.
 *
 * @param rssi_out  Receives the RSSI in dBm. Untouched on failure.
 * @return true when a value was read, false when not associated or the
 *         query failed.
 */
bool networking_wifi_get_rssi(int *rssi_out);

// ---- OTA support hooks (consumed by components/c6ota) ----
esp_err_t networking_wifi_wait_for_ota(void);
bool networking_wifi_is_starting(void);
void networking_wifi_capture_restore_state(networking_wifi_restore_state_t *restore_state);
esp_err_t networking_wifi_shutdown(void);
esp_err_t networking_wifi_restore_after_ota_failure(const networking_wifi_restore_state_t *restore_state);
void networking_wifi_request_post_ota_restore(const networking_wifi_restore_state_t *restore_state);

#endif
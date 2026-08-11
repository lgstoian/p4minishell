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

/**
 * Get the host callback table registered by networking_init().
 *
 * Internal helper for the networking component's own translation units
 * (http_server.c, netdiag.c, bluetooth.c) so they share one transcript and
 * debug-history surface. Returns NULL before networking_init() runs.
 */
const networking_host_ops_t *networking_get_host_ops(void);

// ---- Shell-facing Wi-Fi commands ----

/**
 * Dispatch a `wifi ...` command line (family-routed from the command module).
 * Returns ESP_OK on success and a non-OK error for the known-network
 * management subcommands (known/save/forget/clear/preferred) when they fail or
 * the SD known-network list is unavailable, so the command layer can map the
 * result onto ERRORLEVEL. Existing subcommands (status/scan/connect/...) return
 * ESP_OK and leave the previous ERRORLEVEL semantics unchanged.
 */
esp_err_t networking_handle_wifi_command(char *command);
void networking_wifi_status(void);

/**
 * Run a `wifi scan` and print the results sorted by RSSI (strongest first).
 *
 * @param bare  When true, print bare SSID lines without colour or column
 *              alignment so redirected / piped output stays machine-parsable
 *              (the `/b` form). When false, print the aligned report.
 */
void networking_wifi_scan(bool bare);

void networking_wifi_diag(void);
void networking_wifi_disconnect(void);

/**
 * Classic DOS-style `ping <host-or-ip> [count]`.
 *
 * Resolves the target through lwIP DNS, then runs `count` ICMP echo requests
 * (default P4_CONFIG_PING_COUNT_DEFAULT, hard cap P4_CONFIG_PING_COUNT_MAX)
 * on the esp_ping session, printing each reply and a classic statistics
 * summary. The session runs on its own task; this call blocks the caller up
 * to a bounded total (count * (timeout + interval)) and never hangs.
 *
 * Output goes through the transcript appenders, so redirection (`>`) and
 * pipes capture it exactly like any other command. The caller maps the
 * return value onto ERRORLEVEL: ESP_OK = at least one reply (success),
 * anything else = failure.
 *
 * @param host   Hostname or dotted IPv4 address. Must not be NULL/empty.
 * @param count  Number of echo requests. <= 0 selects the configured default;
 *               a value above P4_CONFIG_PING_COUNT_MAX is clamped.
 * @return ESP_OK when at least one reply was received, ESP_ERR_INVALID_STATE
 *         when Wi-Fi is not started/connected, ESP_ERR_INVALID_ARG for a bad
 *         target, ESP_ERR_TIMEOUT when every request timed out or the session
 *         guard fired, otherwise the resolution / session error.
 */
esp_err_t networking_wifi_ping(const char *host, int count);

/**
 * Classic `dns <hostname>` / `nslookup <hostname>` lookup.
 *
 * Resolves A records through lwIP getaddrinfo and prints the resolved IPv4
 * address list (bounded by P4_CONFIG_DNS_RESULT_LIMIT). Redirectable and
 * errorlevel-aware like `ping`.
 *
 * @param hostname  Hostname to resolve. Must not be NULL/empty.
 * @return ESP_OK when at least one A record resolved, ESP_ERR_INVALID_ARG for
 *         a missing argument, ESP_ERR_INVALID_STATE when Wi-Fi is not started,
 *         ESP_ERR_NOT_FOUND when the name did not resolve.
 */
esp_err_t networking_wifi_dns_lookup(const char *hostname);

/**
 * Result of a successful `networking_http_get()` fetch.
 *
 * The body is a heap (PSRAM-preferred) buffer owned by the caller, released
 * with `networking_http_result_free()`. Everything else is a plain copy.
 */
typedef struct {
    int http_status;             /**< HTTP status code (2xx on success). */
    char content_type[64];       /**< Sanitized Content-Type header ("" if none). */
    size_t content_length;       /**< Content-Length header, or 0 when unknown. */
    uint8_t *body;               /**< Heap-allocated body, NULL on failure. */
    size_t body_size;            /**< Actual bytes read into @p body. */
} networking_http_result_t;

/**
 * Perform a simple HTTPS (or HTTP) GET for the `httpget` / `wget` command.
 *
 * The only HTTP / TLS code in the firmware lives here (and in c6ota): this is
 * the sole owner of the esp_http_client surface. The request runs on the
 * calling task with a bounded timeout, so the worker task is never hung.
 * Redirects follow P4_CONFIG_HTTP_FOLLOW_REDIRECTS, the body is buffered in
 * PSRAM up to P4_CONFIG_HTTP_MAX_BODY_BYTES (a larger response is refused),
 * and the User-Agent comes from P4_CONFIG_HTTP_USER_AGENT.
 *
 * The response header (status / content-type / size) is printed to the
 * transcript with semantic colours; the body is returned for the command
 * layer to print or save to SD. Output therefore participates in redirection
 * and pipes like every other command, and the caller maps the return value
 * onto ERRORLEVEL (0 = HTTP 2xx, non-zero otherwise).
 *
 * @param url      http:// or https:// URL. Must not be NULL/empty.
 * @param result   Receives the result. Must not be NULL; zeroed on entry.
 * @return ESP_OK when an HTTP 2xx body was read (caller frees result->body),
 *         ESP_ERR_INVALID_ARG for a NULL argument or unsupported scheme,
 *         ESP_ERR_INVALID_STATE when Wi-Fi is not connected,
 *         ESP_ERR_INVALID_SIZE when the body exceeds the configured cap,
 *         ESP_ERR_TIMEOUT on a network timeout, ESP_ERR_NOT_FOUND on a
 *         non-2xx status, or the esp_http_client error otherwise.
 */
esp_err_t networking_http_get(const char *url, networking_http_result_t *result);

/** Free a body returned by `networking_http_get()`. Safe on a zeroed result. */
void networking_http_result_free(networking_http_result_t *result);

/**
 * Store the target SSID/password for auto-connect and the boot policy.
 *
 * Used by the CONFIG.SYS `WIFI_SSID=` / `WIFI_PASSWORD=` directives. The
 * password is never echoed to the transcript, history, or debug log; it is
 * stored masked the same way the interactive `wifi connect` path does.
 *
 * @param ssid      Target SSID (empty string clears the target).
 * @param password  Target password (may be NULL/empty).
 */
void networking_wifi_set_boot_credentials(const char *ssid, const char *password);

/**
 * Control whether the Wi-Fi watchdog retries a connection to the stored
 * target SSID (the CONFIG.SYS `WIFI_AUTOCONNECT=` policy).
 *
 * @param enabled  true to allow the watchdog to retry, false to suppress it.
 */
void networking_wifi_set_boot_autoconnect(bool enabled);

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
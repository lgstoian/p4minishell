// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file netsvc.h
 * @brief Persistent MQTT service layer (event-driven networking).
 *
 * One background task owns a single MQTT 3.1.1 connection (packet bytes via
 * mqtt_codec.h, sockets via the lwIP code in this component — the sole
 * socket owner). Apps never touch sockets: batch uses the `net` verbs in
 * components/command/netsvc_commands.c, native code uses applib_msg.h, and
 * inbound events reach batch through the registered host ops (existing
 * surfaces only: header/LED notify plus an alarm-style async batch hook).
 *
 * Offline-first: publishes while disconnected land in an SD outbox journal
 * (`P4_CONFIG_NET_OUTBOX_DIR`) and flush oldest-first on reconnect; inbound
 * `$pim/...` topics merge newer-wins through components/pim (never a second
 * merger). Secrets merge only when the registered lock hook allows.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Upward hooks registered once by command_init() (mirrors alarm_host_ops_t). */
typedef struct {
    /** Queue a line on the command worker (the `/onmsg` hook). */
    void (*execute_async)(char *command);
    /** True when secret sync payloads may merge (device unlocked). */
    bool (*can_reveal_private)(void);
    /** Batch environment write for NET_TOPIC/NET_LEN (shell_env_set). */
    esp_err_t (*set_env)(const char *name, const char *value);
    /** True while an OTA owns flash (service pauses; PSRAM off-limits). */
    bool (*ota_active)(void);
} netsvc_host_ops_t;

/** Register (or clear, with NULL) the host ops. Every hook is NULL-checked. */
void netsvc_register_host_ops(const netsvc_host_ops_t *ops);

/** Start the service task on first use (lazy: keeps boot DMA heap intact). */
void netsvc_ensure_init(void);

/** True when the task runs and the broker session is up. */
bool netsvc_is_connected(void);

/** True when background network activity is live (OTA/sleep gate reads this). */
bool netsvc_is_active(void);

/** Persist broker coordinates to the NET profile (host/port/user). */
esp_err_t netsvc_set_broker(const char *host, int port, const char *user);

/** Read the stored broker coordinates (NET profile). Writes host/user as
 *  NUL-terminated strings (may be "") and the port (1883 default). Returns
 *  ESP_OK when a host is configured, ESP_ERR_NOT_FOUND otherwise; the caller's
 *  buffers are always written (empty on not-found). */
esp_err_t netsvc_get_broker(char *host, size_t host_size, int *port_out,
                            char *user, size_t user_size);

/** Persist the broker password (stored like the Wi-Fi known list). */
esp_err_t netsvc_set_password(const char *password);

/** Enable the connection loop (kicks an immediate attempt). */
esp_err_t netsvc_connect(void);

/** Drop the session and stop reconnecting until `net connect`. */
esp_err_t netsvc_disconnect(void);

/** Subscribe / unsubscribe (RAM table, re-sent on every reconnect). */
esp_err_t netsvc_subscribe(const char *filter);
esp_err_t netsvc_unsubscribe(const char *filter);

/** Publish (QoS 1). Journals to the outbox when offline. */
esp_err_t netsvc_publish(const char *topic, const uint8_t *payload, size_t len);

/** Last inbound message slot for `net msg` (false when none yet). */
bool netsvc_last_msg(char *topic_out, size_t topic_size,
                     uint8_t *payload_out, size_t *payload_len_inout,
                     size_t payload_cap);

/** Set the `/onmsg` batch line (RAM-only; NULL/empty clears). */
esp_err_t netsvc_set_onmsg(const char *line);

/** Outbox depth / purge. */
int netsvc_outbox_count(void);
esp_err_t netsvc_outbox_purge(void);

/** Connection state for `net status` (never NULL). */
const char *netsvc_state_string(void);
int netsvc_sub_count(void);
bool netsvc_sub_get(int index, char *out, size_t out_size);

/**
 * Pure reconnect backoff: base doubled per attempt, capped. Jitter (if any)
 * is added by the caller so this stays deterministic and unit-tested.
 */
uint32_t netsvc_backoff_ms(unsigned attempt, uint32_t base_ms, uint32_t cap_ms);

/** Outbox record framing (single file payload, strict decode skips damage). */
int netsvc_outbox_encode(const char *topic, const uint8_t *payload, size_t len,
                         uint8_t qos, char *out, size_t out_size);
int netsvc_outbox_decode(const char *rec, size_t rec_len,
                         char *topic_out, size_t topic_size,
                         const uint8_t **payload_out, size_t *payload_len_out,
                         uint8_t *qos_out);

#ifdef __cplusplus
}
#endif

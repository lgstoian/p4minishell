// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file mqtt_codec.h
 * @brief Pure MQTT 3.1.1 packet codec for the netsvc service layer.
 *
 * Encode/decode for the client subset netsvc speaks (CONNECT, PUBLISH
 * QoS0/1, SUBSCRIBE, UNSUBSCRIBE, PINGREQ, DISCONNECT inbound plus CONNACK,
 * PUBLISH, SUBACK, PUBACK, PINGRESP). No sockets, no heap, no IDF calls:
 * every function works on caller buffers with explicit capacities, so the
 * whole codec is headless unit-testable. The streaming socket driver in
 * netsvc.c owns framing; this file owns bytes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MQTT control packet types spoken here. */
typedef enum {
    MQTT_PKT_CONNECT = 1,
    MQTT_PKT_CONNACK = 2,
    MQTT_PKT_PUBLISH = 3,
    MQTT_PKT_PUBACK = 4,
    MQTT_PKT_SUBSCRIBE = 8,
    MQTT_PKT_SUBACK = 9,
    MQTT_PKT_UNSUBSCRIBE = 10,
    MQTT_PKT_PINGREQ = 12,
    MQTT_PKT_PINGRESP = 13,
    MQTT_PKT_DISCONNECT = 14,
} mqtt_packet_type_t;

/** CONNECT return codes (CONNACK byte 2). */
typedef enum {
    MQTT_RC_ACCEPTED = 0,
    MQTT_RC_BAD_PROTOCOL = 1,
    MQTT_RC_CLIENT_ID_REJECTED = 2,
    MQTT_RC_SERVER_UNAVAILABLE = 3,
    MQTT_RC_BAD_CREDENTIALS = 4,
    MQTT_RC_NOT_AUTHORIZED = 5,
} mqtt_connack_rc_t;

/** CONNECT arguments. Strings are NUL-terminated; NULL username disables it. */
typedef struct {
    const char *client_id;
    uint16_t keepalive_s;
    bool clean_session;
    const char *username; /**< NULL for anonymous. */
    const char *password; /**< NULL for none (needs username). */
} mqtt_connect_args_t;

/** A length-delimited view into a frame (no copy, no NUL). */
typedef struct {
    const uint8_t *data;
    size_t len;
} mqtt_view_t;

/** Encode a CONNECT packet. @return bytes written, or -1 on overflow/bad args. */
int mqtt_encode_connect(uint8_t *out, size_t cap, const mqtt_connect_args_t *args);

/** Encode SUBSCRIBE for one topic filter (QoS 0/1). @return bytes or -1. */
int mqtt_encode_subscribe(uint8_t *out, size_t cap, uint16_t pkt_id,
                          const char *filter, uint8_t qos);

/** Encode UNSUBSCRIBE for one topic filter. @return bytes or -1. */
int mqtt_encode_unsubscribe(uint8_t *out, size_t cap, uint16_t pkt_id,
                            const char *filter);

/** Encode PUBLISH (QoS 0/1). pkt_id is ignored for QoS 0. @return bytes or -1. */
int mqtt_encode_publish(uint8_t *out, size_t cap, const char *topic,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t qos, uint16_t pkt_id, bool retain);

/** Encode PUBACK. @return bytes (2) or -1. */
int mqtt_encode_puback(uint8_t *out, size_t cap, uint16_t pkt_id);

/** Encode PINGREQ (2 bytes) / DISCONNECT (2 bytes). @return 2 or -1. */
int mqtt_encode_pingreq(uint8_t *out, size_t cap);
int mqtt_encode_disconnect(uint8_t *out, size_t cap);

/** Encode a Remaining Length field. @return field bytes (1..4) or -1. */
int mqtt_encode_remaining(uint32_t len, uint8_t *out);

/** Decode a Remaining Length field. @return 1 complete, 0 need more, -1 malformed. */
int mqtt_decode_remaining(const uint8_t *p, size_t avail,
                          uint32_t *len_out, size_t *used_out);

/** Decode a fixed header. @return 1 complete, 0 need more, -1 malformed. */
int mqtt_decode_header(const uint8_t *p, size_t avail,
                       uint8_t *type_out, uint8_t *flags_out,
                       uint32_t *rem_out, size_t *hdrlen_out);

/** Decode a CONNACK body (2 bytes). @return 0 ok, -1 malformed. */
int mqtt_decode_connack(const uint8_t *body, size_t len, uint8_t *rc_out);

/** Decode a PUBLISH body with the flags from its fixed header. */
int mqtt_decode_publish(const uint8_t *body, size_t len, uint8_t flags,
                        mqtt_view_t *topic_out, uint16_t *pkt_id_out,
                        mqtt_view_t *payload_out);

/** Decode a SUBACK body into granted QoS codes. @return count or -1. */
int mqtt_decode_suback(const uint8_t *body, size_t len, uint16_t *pkt_id_out,
                       uint8_t *granted, size_t granted_cap);

/**
 * Topic filter matcher (`+` one level, `#` rest). Pure.
 * @return true when @p topic routes to subscription @p filter.
 */
bool mqtt_topic_matches(const char *filter, const char *topic);

#ifdef __cplusplus
}
#endif

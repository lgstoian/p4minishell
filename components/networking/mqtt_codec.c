// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file mqtt_codec.c
 * @brief Pure MQTT 3.1.1 packet codec (see mqtt_codec.h).
 *
 * Every writer bounds-checks against the caller capacity before emitting a
 * byte; every reader validates lengths before dereferencing. Malformed input
 * fails closed (-1) so the socket driver drops the connection instead of
 * parsing past a frame.
 */
#include "mqtt_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MQTT_MAX_REMAINING 268435455u /* 256 MB: 4-byte varint ceiling. */

static size_t cstr_len(const char *s, size_t max)
{
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

int mqtt_encode_remaining(uint32_t len, uint8_t *out)
{
    int n = 0;
    if (out == NULL || len > MQTT_MAX_REMAINING) {
        return -1;
    }
    do {
        uint8_t b = (uint8_t)(len % 128u);
        len /= 128u;
        if (len > 0) {
            b |= 0x80u;
        }
        out[n++] = b;
    } while (len > 0 && n < 4);
    return (len == 0) ? n : -1;
}

int mqtt_decode_remaining(const uint8_t *p, size_t avail,
                          uint32_t *len_out, size_t *used_out)
{
    uint32_t value = 0;
    uint32_t mult = 1;
    size_t i = 0;
    if (p == NULL) {
        return -1;
    }
    while (i < avail && i < 4) {
        uint8_t b = p[i++];
        value += (uint32_t)(b & 0x7Fu) * mult;
        mult *= 128u;
        if ((b & 0x80u) == 0) {
            if (len_out != NULL) {
                *len_out = value;
            }
            if (used_out != NULL) {
                *used_out = i;
            }
            return 1;
        }
    }
    if (i >= 4) {
        return -1;
    }
    return 0;
}

int mqtt_decode_header(const uint8_t *p, size_t avail,
                       uint8_t *type_out, uint8_t *flags_out,
                       uint32_t *rem_out, size_t *hdrlen_out)
{
    uint32_t rem = 0;
    size_t used = 0;
    int rc;
    if (p == NULL || avail == 0) {
        return 0;
    }
    rc = mqtt_decode_remaining(p + 1, avail - 1, &rem, &used);
    if (rc != 1) {
        return rc;
    }
    if (type_out != NULL) {
        *type_out = (uint8_t)((p[0] >> 4) & 0x0Fu);
    }
    if (flags_out != NULL) {
        *flags_out = (uint8_t)(p[0] & 0x0Fu);
    }
    if (rem_out != NULL) {
        *rem_out = rem;
    }
    if (hdrlen_out != NULL) {
        *hdrlen_out = 1 + used;
    }
    return 1;
}

/* Emit one length-prefixed string. Returns the end offset or (size_t)-1. */
static size_t emit_str(uint8_t *out, size_t cap, size_t pos,
                       const char *s, size_t slen)
{
    if (slen > 65535u || pos + 2 + slen > cap) {
        return (size_t)-1;
    }
    out[pos] = (uint8_t)((slen >> 8) & 0xFFu);
    out[pos + 1] = (uint8_t)(slen & 0xFFu);
    if (slen > 0) {
        memcpy(out + pos + 2, s, slen);
    }
    return pos + 2 + slen;
}

/* Emit the fixed header for a body of @p body_len. Returns body offset or -1. */
static int emit_header(uint8_t *out, size_t cap, uint8_t type_flags,
                       size_t body_len)
{
    uint8_t rl[4];
    int rln;
    if (body_len > MQTT_MAX_REMAINING) {
        return -1;
    }
    rln = mqtt_encode_remaining((uint32_t)body_len, rl);
    if (rln < 0 || (size_t)(1 + rln) > cap) {
        return -1;
    }
    out[0] = type_flags;
    memcpy(out + 1, rl, (size_t)rln);
    return 1 + rln;
}

int mqtt_encode_connect(uint8_t *out, size_t cap, const mqtt_connect_args_t *args)
{
    size_t id_len;
    size_t user_len;
    size_t pass_len;
    size_t body_len;
    size_t pos;
    int hdr;
    uint8_t flags;
    if (out == NULL || cap < 2 || args == NULL ||
        args->client_id == NULL || args->client_id[0] == '\0') {
        return -1;
    }
    id_len = cstr_len(args->client_id, 65535u);
    if (id_len == 0 || id_len > 65535u) {
        return -1;
    }
    user_len = (args->username != NULL) ? cstr_len(args->username, 65535u) : 0;
    pass_len = (args->password != NULL && args->username != NULL)
        ? cstr_len(args->password, 65535u)
        : 0;
    /* Variable header (10) + payload strings. */
    body_len = 10 + 2 + id_len;
    if (args->username != NULL) {
        body_len += 2 + user_len;
    }
    if (args->password != NULL && args->username != NULL) {
        body_len += 2 + pass_len;
    }
    hdr = emit_header(out, cap, (uint8_t)(MQTT_PKT_CONNECT << 4), body_len);
    if (hdr < 0 || (size_t)hdr + body_len > cap) {
        return -1;
    }
    pos = (size_t)hdr;
    /* Protocol name "MQTT" + level 4 + flags + keepalive. */
    pos = emit_str(out, cap, pos, "MQTT", 4);
    if (pos == (size_t)-1) {
        return -1;
    }
    flags = 0;
    if (args->clean_session) {
        flags |= 0x02u;
    }
    if (args->username != NULL) {
        flags |= 0x80u;
    }
    if (args->password != NULL && args->username != NULL) {
        flags |= 0x40u;
    }
    if (pos + 4 > cap) {
        return -1;
    }
    out[pos++] = 4; /* protocol level 3.1.1 */
    out[pos++] = flags;
    out[pos++] = (uint8_t)((args->keepalive_s >> 8) & 0xFFu);
    out[pos++] = (uint8_t)(args->keepalive_s & 0xFFu);
    pos = emit_str(out, cap, pos, args->client_id, id_len);
    if (pos == (size_t)-1) {
        return -1;
    }
    if (args->username != NULL) {
        pos = emit_str(out, cap, pos, args->username, user_len);
        if (pos == (size_t)-1) {
            return -1;
        }
    }
    if (args->password != NULL && args->username != NULL) {
        pos = emit_str(out, cap, pos, args->password, pass_len);
        if (pos == (size_t)-1) {
            return -1;
        }
    }
    return (int)pos;
}

int mqtt_encode_subscribe(uint8_t *out, size_t cap, uint16_t pkt_id,
                          const char *filter, uint8_t qos)
{
    size_t flen;
    size_t body_len;
    size_t pos;
    int hdr;
    if (out == NULL || filter == NULL || filter[0] == '\0' || qos > 1) {
        return -1;
    }
    flen = cstr_len(filter, 65535u);
    if (flen == 0) {
        return -1;
    }
    body_len = 2 + 2 + flen + 1;
    hdr = emit_header(out, cap, (uint8_t)((MQTT_PKT_SUBSCRIBE << 4) | 0x02u), body_len);
    if (hdr < 0) {
        return -1;
    }
    pos = (size_t)hdr;
    if (pos + 2 > cap) {
        return -1;
    }
    out[pos++] = (uint8_t)((pkt_id >> 8) & 0xFFu);
    out[pos++] = (uint8_t)(pkt_id & 0xFFu);
    pos = emit_str(out, cap, pos, filter, flen);
    if (pos == (size_t)-1 || pos + 1 > cap) {
        return -1;
    }
    out[pos++] = qos;
    return (int)pos;
}

int mqtt_encode_unsubscribe(uint8_t *out, size_t cap, uint16_t pkt_id,
                            const char *filter)
{
    size_t flen;
    size_t body_len;
    size_t pos;
    int hdr;
    if (out == NULL || filter == NULL || filter[0] == '\0') {
        return -1;
    }
    flen = cstr_len(filter, 65535u);
    if (flen == 0) {
        return -1;
    }
    body_len = 2 + 2 + flen;
    hdr = emit_header(out, cap, (uint8_t)((MQTT_PKT_UNSUBSCRIBE << 4) | 0x02u), body_len);
    if (hdr < 0) {
        return -1;
    }
    pos = (size_t)hdr;
    if (pos + 2 > cap) {
        return -1;
    }
    out[pos++] = (uint8_t)((pkt_id >> 8) & 0xFFu);
    out[pos++] = (uint8_t)(pkt_id & 0xFFu);
    pos = emit_str(out, cap, pos, filter, flen);
    if (pos == (size_t)-1) {
        return -1;
    }
    return (int)pos;
}

int mqtt_encode_publish(uint8_t *out, size_t cap, const char *topic,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t qos, uint16_t pkt_id, bool retain)
{
    size_t tlen;
    size_t body_len;
    size_t pos;
    int hdr;
    uint8_t flags;
    if (out == NULL || topic == NULL || topic[0] == '\0' || qos > 1 ||
        (payload_len > 0 && payload == NULL)) {
        return -1;
    }
    tlen = cstr_len(topic, 65535u);
    if (tlen == 0) {
        return -1;
    }
    body_len = 2 + tlen + ((qos > 0) ? 2 : 0) + payload_len;
    flags = (uint8_t)((MQTT_PKT_PUBLISH << 4) | ((qos & 0x03u) << 1));
    if (retain) {
        flags |= 0x01u;
    }
    hdr = emit_header(out, cap, flags, body_len);
    if (hdr < 0) {
        return -1;
    }
    pos = (size_t)hdr;
    pos = emit_str(out, cap, pos, topic, tlen);
    if (pos == (size_t)-1) {
        return -1;
    }
    if (qos > 0) {
        if (pos + 2 > cap) {
            return -1;
        }
        out[pos++] = (uint8_t)((pkt_id >> 8) & 0xFFu);
        out[pos++] = (uint8_t)(pkt_id & 0xFFu);
    }
    if (pos + payload_len > cap) {
        return -1;
    }
    if (payload_len > 0) {
        memcpy(out + pos, payload, payload_len);
        pos += payload_len;
    }
    return (int)pos;
}

int mqtt_encode_puback(uint8_t *out, size_t cap, uint16_t pkt_id)
{
    if (out == NULL || cap < 4) {
        return -1;
    }
    out[0] = (uint8_t)(MQTT_PKT_PUBACK << 4);
    out[1] = 2;
    out[2] = (uint8_t)((pkt_id >> 8) & 0xFFu);
    out[3] = (uint8_t)(pkt_id & 0xFFu);
    return 4;
}

int mqtt_encode_pingreq(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2) {
        return -1;
    }
    out[0] = (uint8_t)(MQTT_PKT_PINGREQ << 4);
    out[1] = 0;
    return 2;
}

int mqtt_encode_disconnect(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2) {
        return -1;
    }
    out[0] = (uint8_t)(MQTT_PKT_DISCONNECT << 4);
    out[1] = 0;
    return 2;
}

int mqtt_decode_connack(const uint8_t *body, size_t len, uint8_t *rc_out)
{
    if (body == NULL || len < 2) {
        return -1;
    }
    if (rc_out != NULL) {
        *rc_out = body[1];
    }
    return 0;
}

/* Read one length-prefixed string view. Returns the next offset or -1. */
static int view_str(const uint8_t *body, size_t len, size_t pos,
                    mqtt_view_t *out)
{
    size_t slen;
    if (body == NULL || pos + 2 > len) {
        return -1;
    }
    slen = ((size_t)body[pos] << 8) | body[pos + 1];
    if (pos + 2 + slen > len) {
        return -1;
    }
    if (out != NULL) {
        out->data = body + pos + 2;
        out->len = slen;
    }
    return (int)(pos + 2 + slen);
}

int mqtt_decode_publish(const uint8_t *body, size_t len, uint8_t flags,
                        mqtt_view_t *topic_out, uint16_t *pkt_id_out,
                        mqtt_view_t *payload_out)
{
    int pos;
    uint8_t qos = (uint8_t)((flags >> 1) & 0x03u);
    if (body == NULL) {
        return -1;
    }
    if (qos > 1) {
        return -1;
    }
    pos = view_str(body, len, 0, topic_out);
    if (pos < 0) {
        return -1;
    }
    if (qos > 0) {
        if ((size_t)pos + 2 > len) {
            return -1;
        }
        if (pkt_id_out != NULL) {
            *pkt_id_out = (uint16_t)(((uint16_t)body[pos] << 8) | body[pos + 1]);
        }
        pos += 2;
    } else if (pkt_id_out != NULL) {
        *pkt_id_out = 0;
    }
    if (payload_out != NULL) {
        payload_out->data = body + pos;
        payload_out->len = len - (size_t)pos;
    }
    return 0;
}

int mqtt_decode_suback(const uint8_t *body, size_t len, uint16_t *pkt_id_out,
                       uint8_t *granted, size_t granted_cap)
{
    size_t i;
    size_t count;
    if (body == NULL || len < 3 || granted_cap == 0) {
        return -1;
    }
    if (pkt_id_out != NULL) {
        *pkt_id_out = (uint16_t)(((uint16_t)body[0] << 8) | body[1]);
    }
    count = len - 2;
    if (count > granted_cap) {
        return -1;
    }
    if (granted == NULL) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        granted[i] = body[2 + i];
    }
    return (int)count;
}

/* Compare one level: filter segment (may be "+" or "#") against topic text. */
static bool level_matches(const char *f, size_t flen, const char *t, size_t tlen)
{
    if (flen == 1 && f[0] == '+') {
        return true;
    }
    if (flen == tlen && (flen == 0 || memcmp(f, t, flen) == 0)) {
        return true;
    }
    return false;
}

bool mqtt_topic_matches(const char *filter, const char *topic)
{
    const char *f;
    const char *t;
    if (filter == NULL || topic == NULL) {
        return false;
    }
    f = filter;
    t = topic;
    for (;;) {
        const char *fs = strchr(f, '/');
        const char *ts = strchr(t, '/');
        size_t flen = (fs != NULL) ? (size_t)(fs - f) : strlen(f);
        size_t tlen = (ts != NULL) ? (size_t)(ts - t) : strlen(t);
        if (flen == 1 && f[0] == '#') {
            /* Multi-level wildcard: it must be the final filter level and
             * matches the rest of the topic, including zero levels (so
             * "a/#" matches "a" as well as "a/b/c"). */
            return fs == NULL;
        }
        if (!level_matches(f, flen, t, tlen)) {
            return false;
        }
        if (fs == NULL && ts == NULL) {
            return true;
        }
        if (fs == NULL) {
            return false;
        }
        if (ts == NULL) {
            /* Topic exhausted while the filter continues: only a trailing
             * "/#" can still match the remaining (empty) levels. */
            f = fs + 1;
            return f[0] == '#' && f[1] == '\0';
        }
        f = fs + 1;
        t = ts + 1;
    }
}

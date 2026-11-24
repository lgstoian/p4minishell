/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file native_payload.c
 * @brief Pure envelope parser/verifier for reserved native payload blobs.
 *
 * This translation unit never opens a file, decodes an executable, or calls
 * into payload bytes. It decodes fixed-width little-endian fields, bounds
 * checks the payload tail, runs the shared CRC-32, and delegates SHA-256 and
 * ECDSA to hooks the firmware installs at boot.
 */

#include "native_payload.h"

#include "p4minishell_config.h"
#include "strutil.h"

#include <string.h>

_Static_assert(P4_CONFIG_NATPAY_HEADER_BYTES == NATIVE_PAY_HEADER_LEN,
               "native payload header size in config must match the envelope format");
_Static_assert(P4_CONFIG_NATPAY_MAX_PAYLOAD_BYTES > 0u,
               "native payload size cap must be positive");

/* One ops table and one trust key, installed at boot; no per-call state. */
static native_pay_crypto_ops_t s_ops;
static uint8_t s_key[NATIVE_PAY_PUBKEY_BYTES];
static bool s_key_installed;

/* Little-endian readers; callers have already proved the bytes are in range. */
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int native_pay_parse(const uint8_t *buf, size_t len, native_pay_view_t *out)
{
    uint8_t abi_field[NATIVE_PAY_OFF_SIZE - NATIVE_PAY_OFF_ABI];
    size_t abi_len = 0;
    uint32_t version;
    uint32_t size;
    uint32_t api;

    if (buf == NULL || out == NULL) {
        return NATIVE_PAY_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (len < (size_t)P4_CONFIG_NATPAY_HEADER_BYTES) {
        return NATIVE_PAY_ERR_TOO_SMALL;
    }
    if (memcmp(buf + NATIVE_PAY_OFF_MAGIC, NATIVE_PAY_MAGIC,
               NATIVE_PAY_OFF_VERSION - NATIVE_PAY_OFF_MAGIC) != 0) {
        return NATIVE_PAY_ERR_MAGIC;
    }

    version = read_le32(buf + NATIVE_PAY_OFF_VERSION);
    if (version != NATIVE_PAY_FORMAT_VERSION) {
        return NATIVE_PAY_ERR_VERSION;
    }
    if (memcmp(buf + NATIVE_PAY_OFF_ARCH, NATIVE_PAY_ARCH,
               NATIVE_PAY_OFF_ABI - NATIVE_PAY_OFF_ARCH) != 0) {
        return NATIVE_PAY_ERR_ARCH;
    }

    /* The ABI field is a fixed-width, NUL-padded copy of the firmware tag. */
    memset(abi_field, 0, sizeof(abi_field));
    while (abi_len < sizeof(abi_field) && P4_CONFIG_NATIVE_ABI[abi_len] != '\0') {
        abi_len++;
    }
    if (abi_len == sizeof(abi_field)) {
        return NATIVE_PAY_ERR_ABI;
    }
    memcpy(abi_field, P4_CONFIG_NATIVE_ABI, abi_len);
    if (memcmp(buf + NATIVE_PAY_OFF_ABI, abi_field, sizeof(abi_field)) != 0) {
        return NATIVE_PAY_ERR_ABI;
    }

    size = read_le32(buf + NATIVE_PAY_OFF_SIZE);
    if (size > (uint32_t)P4_CONFIG_NATPAY_MAX_PAYLOAD_BYTES) {
        return NATIVE_PAY_ERR_SIZE;
    }
    if ((size_t)size > len - (size_t)P4_CONFIG_NATPAY_HEADER_BYTES) {
        return NATIVE_PAY_ERR_SIZE;
    }
    if (read_le16(buf + NATIVE_PAY_OFF_RESERVED) != 0u) {
        return NATIVE_PAY_ERR_RESERVED;
    }
    api = read_le32(buf + NATIVE_PAY_OFF_API_SET);
    if ((api & ~NATIVE_PAY_API_KNOWN_MASK) != 0u) {
        return NATIVE_PAY_ERR_API_SET;
    }

    out->header = buf;
    out->payload = buf + NATIVE_PAY_HEADER_LEN;
    out->payload_size = (size_t)size;
    out->total_len = len;
    out->crc32_field = buf + NATIVE_PAY_OFF_CRC32;
    out->sha256_field = buf + NATIVE_PAY_OFF_SHA256;
    out->signature = buf + NATIVE_PAY_OFF_SIGNATURE;
    out->format_version = version;
    out->entry_ordinal = read_le16(buf + NATIVE_PAY_OFF_ENTRY);
    out->required_api_set = api;
    return NATIVE_PAY_OK;
}

int native_pay_verify(const native_pay_view_t *view)
{
    native_pay_view_t parsed;
    native_pay_span_t spans[NATIVE_PAY_SPANS_MAX];
    uint8_t digest[NATIVE_PAY_SHA256_BYTES];
    uint32_t crc;
    uint8_t diff = 0;
    size_t i;
    int rc;

    if (view == NULL) {
        return NATIVE_PAY_ERR_ARG;
    }
    /* Re-derive everything from the buffer: a hand-built view is not trusted. */
    rc = native_pay_parse(view->header, view->total_len, &parsed);
    if (rc != NATIVE_PAY_OK) {
        return rc;
    }

    crc = strutil_crc32_update(0xFFFFFFFFu, parsed.payload, parsed.payload_size) ^
          0xFFFFFFFFu;
    if (crc != read_le32(parsed.crc32_field)) {
        return NATIVE_PAY_ERR_CRC;
    }

    if (s_ops.sha256_spans == NULL || s_ops.ecdsa_verify_p256 == NULL ||
        !s_key_installed) {
        return NATIVE_PAY_ERR_NO_VERIFIER;
    }

    /* Canonical bytes: header without the signature field, then payload. */
    spans[0].data = parsed.header + NATIVE_PAY_OFF_MAGIC;
    spans[0].len = NATIVE_PAY_OFF_SIGNATURE - NATIVE_PAY_OFF_MAGIC;
    spans[1].data = parsed.header + NATIVE_PAY_OFF_ENTRY;
    spans[1].len = NATIVE_PAY_HEADER_LEN - NATIVE_PAY_OFF_ENTRY;
    spans[2].data = parsed.payload;
    spans[2].len = parsed.payload_size;

    memset(digest, 0, sizeof(digest));
    if (s_ops.sha256_spans(spans, NATIVE_PAY_SPANS_MAX, digest) != 0) {
        return NATIVE_PAY_ERR_HASH;
    }
    for (i = 0; i < NATIVE_PAY_SHA256_BYTES; i++) {
        diff |= (uint8_t)(digest[i] ^ parsed.sha256_field[i]);
    }
    if (diff != 0u) {
        return NATIVE_PAY_ERR_SHA;
    }
    if (s_ops.ecdsa_verify_p256(digest, parsed.signature, s_key) != 0) {
        return NATIVE_PAY_ERR_SIGNATURE;
    }
    return NATIVE_PAY_OK;
}

int native_pay_check(const uint8_t *buf, size_t len, native_pay_view_t *out)
{
    int rc = native_pay_parse(buf, len, out);
    if (rc != NATIVE_PAY_OK) {
        return rc;
    }
    return native_pay_verify(out);
}

void native_pay_set_crypto_ops(const native_pay_crypto_ops_t *ops)
{
    if (ops != NULL) {
        s_ops = *ops;
    } else {
        memset(&s_ops, 0, sizeof(s_ops));
    }
}

void native_pay_set_trusted_key(const uint8_t pubkey[NATIVE_PAY_PUBKEY_BYTES])
{
    if (pubkey != NULL) {
        memcpy(s_key, pubkey, sizeof(s_key));
        s_key_installed = true;
    } else {
        memset(s_key, 0, sizeof(s_key));
        s_key_installed = false;
    }
}

const char *native_pay_status_name(native_pay_status_t status)
{
    switch (status) {
        case NATIVE_PAY_OK:            return "ok";
        case NATIVE_PAY_ERR_ARG:       return "bad argument";
        case NATIVE_PAY_ERR_TOO_SMALL: return "header truncated";
        case NATIVE_PAY_ERR_MAGIC:     return "bad magic";
        case NATIVE_PAY_ERR_VERSION:   return "bad format version";
        case NATIVE_PAY_ERR_ARCH:      return "arch mismatch";
        case NATIVE_PAY_ERR_ABI:       return "abi mismatch";
        case NATIVE_PAY_ERR_SIZE:      return "payload size out of range";
        case NATIVE_PAY_ERR_RESERVED:  return "reserved field set";
        case NATIVE_PAY_ERR_API_SET:   return "unknown required api";
        case NATIVE_PAY_ERR_CRC:       return "crc mismatch";
        case NATIVE_PAY_ERR_NO_VERIFIER: return "no verifier installed";
        case NATIVE_PAY_ERR_HASH:      return "hash failed";
        case NATIVE_PAY_ERR_SHA:       return "sha256 mismatch";
        case NATIVE_PAY_ERR_SIGNATURE: return "bad signature";
        default:                       return "unknown";
    }
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_native_payload.c
 * @brief Unit tests for the pure native payload envelope core (components/nativepay).
 *
 * Only the format/verification API is exercised: no file I/O, no loader, and
 * no execution. Envelopes are assembled in a small static buffer so every
 * failure case is a deliberate byte mutation of a known-good blob.
 */

#include "unity.h"
#include "native_payload.h"
#include "p4minishell_config.h"
#include "strutil.h"

#include <string.h>

#define TEST_PAYLOAD_BYTES 16u

static uint8_t s_blob[P4_CONFIG_NATPAY_HEADER_BYTES + 64];

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Build a well-formed envelope with a CRC that matches its payload. SHA-256
 * and signature fields are filled with non-zero placeholders: without crypto
 * hooks (the only mode this suite can assume) they are never consulted. */
static size_t build_blob(size_t payload_bytes)
{
    uint32_t crc;
    size_t i;

    memset(s_blob, 0, sizeof(s_blob));
    memcpy(s_blob + NATIVE_PAY_OFF_MAGIC, NATIVE_PAY_MAGIC,
           NATIVE_PAY_OFF_VERSION - NATIVE_PAY_OFF_MAGIC);
    put_le32(s_blob + NATIVE_PAY_OFF_VERSION, NATIVE_PAY_FORMAT_VERSION);
    memcpy(s_blob + NATIVE_PAY_OFF_ARCH, NATIVE_PAY_ARCH,
           NATIVE_PAY_OFF_ABI - NATIVE_PAY_OFF_ARCH);
    memcpy(s_blob + NATIVE_PAY_OFF_ABI, P4_CONFIG_NATIVE_ABI,
           strlen(P4_CONFIG_NATIVE_ABI));
    put_le32(s_blob + NATIVE_PAY_OFF_SIZE, (uint32_t)payload_bytes);
    put_le16(s_blob + NATIVE_PAY_OFF_ENTRY, 2);
    put_le32(s_blob + NATIVE_PAY_OFF_API_SET,
             NATIVE_PAY_API_CONSOLE | NATIVE_PAY_API_ENV | NATIVE_PAY_API_WEBAPP);

    for (i = 0; i < payload_bytes; i++) {
        s_blob[NATIVE_PAY_HEADER_LEN + i] = (uint8_t)(0xA0u + i);
    }
    crc = strutil_crc32_update(0xFFFFFFFFu,
                               s_blob + NATIVE_PAY_HEADER_LEN, payload_bytes) ^
          0xFFFFFFFFu;
    put_le32(s_blob + NATIVE_PAY_OFF_CRC32, crc);
    memset(s_blob + NATIVE_PAY_OFF_SHA256, 0x5A,
           NATIVE_PAY_OFF_SIGNATURE - NATIVE_PAY_OFF_SHA256);
    memset(s_blob + NATIVE_PAY_OFF_SIGNATURE, 0x11, NATIVE_PAY_SIG_BYTES);
    return (size_t)P4_CONFIG_NATPAY_HEADER_BYTES + payload_bytes;
}

static void clear_crypto(void)
{
    native_pay_set_crypto_ops(NULL);
    native_pay_set_trusted_key(NULL);
}

void test_native_payload_header_parse(void)
{
    size_t len = build_blob(TEST_PAYLOAD_BYTES);
    native_pay_view_t view;

    TEST_ASSERT_EQUAL(NATIVE_PAY_OK, native_pay_parse(s_blob, len, &view));
    TEST_ASSERT_EQUAL_PTR(s_blob, view.header);
    TEST_ASSERT_EQUAL_PTR(s_blob + NATIVE_PAY_HEADER_LEN, view.payload);
    TEST_ASSERT_EQUAL_UINT32(TEST_PAYLOAD_BYTES, view.payload_size);
    TEST_ASSERT_EQUAL_UINT32(len, view.total_len);
    TEST_ASSERT_EQUAL_UINT32(NATIVE_PAY_FORMAT_VERSION, view.format_version);
    TEST_ASSERT_EQUAL_UINT16(2, view.entry_ordinal);
    TEST_ASSERT_EQUAL_UINT32(NATIVE_PAY_API_CONSOLE | NATIVE_PAY_API_ENV |
                             NATIVE_PAY_API_WEBAPP, view.required_api_set);
    TEST_ASSERT_EQUAL_PTR(s_blob + NATIVE_PAY_OFF_SHA256, view.sha256_field);
    TEST_ASSERT_EQUAL_PTR(s_blob + NATIVE_PAY_OFF_SIGNATURE, view.signature);
    TEST_ASSERT_EQUAL_HEX8(0xA0u, view.payload[0]);
}

void test_native_payload_bounds_rejection(void)
{
    size_t len = build_blob(TEST_PAYLOAD_BYTES);
    native_pay_view_t view;

    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_ARG, native_pay_parse(NULL, len, &view));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_ARG, native_pay_parse(s_blob, len, NULL));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_TOO_SMALL,
                      native_pay_parse(s_blob, (size_t)P4_CONFIG_NATPAY_HEADER_BYTES - 1u,
                                       &view));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_MAGIC, native_pay_parse(s_blob + 1, len, &view));

    /* Declared size beyond the configured cap. */
    put_le32(s_blob + NATIVE_PAY_OFF_SIZE,
             (uint32_t)P4_CONFIG_NATPAY_MAX_PAYLOAD_BYTES + 1u);
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_SIZE, native_pay_parse(s_blob, len, &view));

    /* Declared size beyond the bytes actually supplied. */
    build_blob(TEST_PAYLOAD_BYTES);
    put_le32(s_blob + NATIVE_PAY_OFF_SIZE, TEST_PAYLOAD_BYTES + 32u);
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_SIZE, native_pay_parse(s_blob, len, &view));
}

void test_native_payload_arch_abi_mismatch(void)
{
    size_t len = build_blob(TEST_PAYLOAD_BYTES);
    native_pay_view_t view;

    s_blob[NATIVE_PAY_OFF_ARCH] ^= 0x01u;
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_ARCH, native_pay_parse(s_blob, len, &view));

    build_blob(TEST_PAYLOAD_BYTES);
    s_blob[NATIVE_PAY_OFF_ABI + 2] ^= 0x01u;
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_ABI, native_pay_parse(s_blob, len, &view));

    build_blob(TEST_PAYLOAD_BYTES);
    put_le32(s_blob + NATIVE_PAY_OFF_VERSION, NATIVE_PAY_FORMAT_VERSION + 1u);
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_VERSION, native_pay_parse(s_blob, len, &view));
}

void test_native_payload_truncated_payload(void)
{
    native_pay_view_t view;
    size_t len = build_blob(TEST_PAYLOAD_BYTES);

    /* Valid header and CRC, but the caller supplied half the declared payload. */
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_SIZE,
                      native_pay_parse(s_blob, len - (TEST_PAYLOAD_BYTES / 2u), &view));

    /* A byte-for-byte valid blob still verifies at its exact length. */
    build_blob(TEST_PAYLOAD_BYTES);
    clear_crypto();
    TEST_ASSERT_EQUAL(NATIVE_PAY_OK, native_pay_parse(s_blob, len, &view));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_NO_VERIFIER, native_pay_verify(&view));
}

void test_native_payload_no_verifier_degradation(void)
{
    size_t len = build_blob(TEST_PAYLOAD_BYTES);
    native_pay_view_t view;

    clear_crypto();
    TEST_ASSERT_EQUAL(NATIVE_PAY_OK, native_pay_parse(s_blob, len, &view));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_NO_VERIFIER, native_pay_verify(&view));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_NO_VERIFIER, native_pay_check(s_blob, len, &view));
    TEST_ASSERT_EQUAL_STRING("no verifier installed",
                             native_pay_status_name(NATIVE_PAY_ERR_NO_VERIFIER));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_ARG, native_pay_verify(NULL));
}

void test_native_payload_crc_rejection(void)
{
    size_t len = build_blob(TEST_PAYLOAD_BYTES);
    native_pay_view_t view;

    clear_crypto();
    s_blob[NATIVE_PAY_HEADER_LEN] ^= 0x01u;
    TEST_ASSERT_EQUAL(NATIVE_PAY_OK, native_pay_parse(s_blob, len, &view));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_CRC, native_pay_verify(&view));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_CRC, native_pay_check(s_blob, len, &view));
}

void test_native_payload_reserved_and_api_set(void)
{
    size_t len = build_blob(TEST_PAYLOAD_BYTES);
    native_pay_view_t view;

    put_le16(s_blob + NATIVE_PAY_OFF_RESERVED, 1);
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_RESERVED, native_pay_parse(s_blob, len, &view));

    build_blob(TEST_PAYLOAD_BYTES);
    put_le32(s_blob + NATIVE_PAY_OFF_API_SET, NATIVE_PAY_API_KNOWN_MASK | (1u << 31));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_API_SET, native_pay_parse(s_blob, len, &view));
}

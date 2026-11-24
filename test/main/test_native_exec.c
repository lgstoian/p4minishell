/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_native_exec.c
 * @brief Host-safe unit tests for the one fixed-ABI native payload loader.
 *
 * Only the pure half is exercised: envelope header verdict, export-table
 * layout, the "no ELF / no relocation" rule, required-API bits, and the trust
 * verdict with mock crypto hooks. No payload code is mapped or executed and
 * no hardware behaviour is claimed.
 */

#include "unity.h"
#include "native_payload.h"
#include "p4minishell_config.h"
#include "nativeexec.h"
#include "strutil.h"

#include <string.h>

#define EX_PAYLOAD_BYTES 24u
#define EX_EXPORT_COUNT  2u
#define EX_ENTRY_0       16u
#define EX_ENTRY_1       20u

static uint8_t s_blob[P4_CONFIG_NATPAY_HEADER_BYTES + 64u];

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

/** One well-formed envelope whose payload starts with the fixed P4EX table. */
static size_t build_ex_blob(void)
{
    uint32_t crc;
    uint8_t *payload = s_blob + NATIVE_PAY_HEADER_LEN;

    memset(s_blob, 0, sizeof(s_blob));
    memcpy(s_blob + NATIVE_PAY_OFF_MAGIC, NATIVE_PAY_MAGIC, 8u);
    put_le32(s_blob + NATIVE_PAY_OFF_VERSION, NATIVE_PAY_FORMAT_VERSION);
    memcpy(s_blob + NATIVE_PAY_OFF_ARCH, NATIVE_PAY_ARCH, 8u);
    memcpy(s_blob + NATIVE_PAY_OFF_ABI, P4_CONFIG_NATIVE_ABI,
           strlen(P4_CONFIG_NATIVE_ABI));
    put_le32(s_blob + NATIVE_PAY_OFF_SIZE, EX_PAYLOAD_BYTES);
    put_le16(s_blob + NATIVE_PAY_OFF_ENTRY, 1u);
    put_le32(s_blob + NATIVE_PAY_OFF_API_SET,
             NATIVE_PAY_API_CONSOLE | NATIVE_PAY_API_ENV);

    memcpy(payload, NATIVE_EXEC_EXPORT_MAGIC, 4u);
    put_le16(payload + 4, NATIVE_EXEC_EXPORT_VERSION);
    put_le16(payload + 6, EX_EXPORT_COUNT);
    put_le32(payload + 8, EX_ENTRY_0);
    put_le32(payload + 12, EX_ENTRY_1);
    memset(payload + 16, 0x90u, EX_PAYLOAD_BYTES - 16u);

    crc = strutil_crc32_update(0xFFFFFFFFu, payload, EX_PAYLOAD_BYTES) ^
          0xFFFFFFFFu;
    put_le32(s_blob + NATIVE_PAY_OFF_CRC32, crc);
    memset(s_blob + NATIVE_PAY_OFF_SHA256, 0x5Au, 32u);
    memset(s_blob + NATIVE_PAY_OFF_SIGNATURE, 0x11u, 64u);
    return (size_t)P4_CONFIG_NATPAY_HEADER_BYTES + EX_PAYLOAD_BYTES;
}

static int mock_sha256_spans(const native_pay_span_t *spans, size_t span_count,
                             uint8_t digest_out[NATIVE_PAY_SHA256_BYTES])
{
    if (spans == NULL || digest_out == NULL || span_count == 0u ||
        span_count > NATIVE_PAY_SPANS_MAX) {
        return 1;
    }
    memset(digest_out, 0x5A, NATIVE_PAY_SHA256_BYTES);
    return 0;
}

static int mock_ecdsa_verify(const uint8_t digest[NATIVE_PAY_SHA256_BYTES],
                             const uint8_t signature[NATIVE_PAY_SIG_BYTES],
                             const uint8_t pubkey[NATIVE_PAY_PUBKEY_BYTES])
{
    if (digest == NULL || signature == NULL || pubkey == NULL) {
        return 1;
    }
    return (digest[0] == 0x5Au && signature[0] == 0x11u) ? 0 : 1;
}

static void install_mock_crypto(void)
{
    static const native_pay_crypto_ops_t ops = {
        mock_sha256_spans, mock_ecdsa_verify
    };
    static const uint8_t key[NATIVE_PAY_PUBKEY_BYTES] = { 0x42u };

    native_pay_set_crypto_ops(&ops);
    native_pay_set_trusted_key(key);
}

static void clear_crypto(void)
{
    native_pay_set_crypto_ops(NULL);
    native_pay_set_trusted_key(NULL);
}

void test_native_exec_export_table_parse(void)
{
    uint8_t payload[EX_PAYLOAD_BYTES];
    native_exec_layout_t layout;

    memset(payload, 0, sizeof(payload));
    memcpy(payload, NATIVE_EXEC_EXPORT_MAGIC, 4u);
    put_le16(payload + 4, NATIVE_EXEC_EXPORT_VERSION);
    put_le16(payload + 6, EX_EXPORT_COUNT);
    put_le32(payload + 8, EX_ENTRY_0);
    put_le32(payload + 12, EX_ENTRY_1);

    TEST_ASSERT_EQUAL(NATIVE_EXEC_OK,
                      native_exec_abi_validate(payload, sizeof(payload),
                                               0u, &layout));
    TEST_ASSERT_EQUAL_UINT32(EX_EXPORT_COUNT, layout.export_count);
    TEST_ASSERT_EQUAL_UINT32(EX_ENTRY_0, layout.entry_offset);
    TEST_ASSERT_EQUAL_UINT32((size_t)NATIVE_EXEC_EXPORT_HDR_BYTES +
                             (size_t)EX_EXPORT_COUNT * 4u, layout.header_bytes);

    TEST_ASSERT_EQUAL(NATIVE_EXEC_OK,
                      native_exec_abi_validate(payload, sizeof(payload),
                                               1u, &layout));
    TEST_ASSERT_EQUAL_UINT32(EX_ENTRY_1, layout.entry_offset);
}

void test_native_exec_export_table_rejects(void)
{
    uint8_t payload[EX_PAYLOAD_BYTES];
    native_exec_layout_t layout;

    memset(payload, 0, sizeof(payload));
    memcpy(payload, NATIVE_EXEC_EXPORT_MAGIC, 4u);
    put_le16(payload + 4, NATIVE_EXEC_EXPORT_VERSION);
    put_le16(payload + 6, EX_EXPORT_COUNT);
    put_le32(payload + 8, EX_ENTRY_0);
    put_le32(payload + 12, EX_ENTRY_1);

    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ARG,
                      native_exec_abi_validate(NULL, sizeof(payload), 0u, &layout));
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ARG,
                      native_exec_abi_validate(payload, sizeof(payload), 0u, NULL));
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_COUNT,
                      native_exec_abi_validate(payload, 4u, 0u, &layout));

    payload[0] ^= 0x01u;
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_MAGIC,
                      native_exec_abi_validate(payload, sizeof(payload), 0u, &layout));
    memcpy(payload, NATIVE_EXEC_EXPORT_MAGIC, 4u);

    put_le16(payload + 4, NATIVE_EXEC_EXPORT_VERSION + 1u);
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_VERSION,
                      native_exec_abi_validate(payload, sizeof(payload), 0u, &layout));
    put_le16(payload + 4, NATIVE_EXEC_EXPORT_VERSION);

    put_le16(payload + 6, 0u);
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_COUNT,
                      native_exec_abi_validate(payload, sizeof(payload), 0u, &layout));
    put_le16(payload + 6, EX_EXPORT_COUNT);

    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_ORDINAL,
                      native_exec_abi_validate(payload, sizeof(payload),
                                               EX_EXPORT_COUNT, &layout));

    put_le32(payload + 12, EX_PAYLOAD_BYTES);
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_OFFSET,
                      native_exec_abi_validate(payload, sizeof(payload), 1u, &layout));
    put_le32(payload + 12, 8u); /* points into the export table itself */
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_OFFSET,
                      native_exec_abi_validate(payload, sizeof(payload), 1u, &layout));
    put_le32(payload + 12, EX_ENTRY_1 + 1u); /* unaligned */
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_OFFSET,
                      native_exec_abi_validate(payload, sizeof(payload), 1u, &layout));
}

void test_native_exec_api_bits(void)
{
    TEST_ASSERT_TRUE(native_exec_api_supported(NATIVE_PAY_API_CONSOLE |
                                               NATIVE_PAY_API_ENV |
                                               NATIVE_PAY_API_STATE |
                                               NATIVE_PAY_API_TIME));
    TEST_ASSERT_TRUE(native_exec_api_supported(0u));
    TEST_ASSERT_FALSE(native_exec_api_supported(1u << 31));
    TEST_ASSERT_FALSE(native_exec_api_supported(NATIVE_PAY_API_TUI));
    TEST_ASSERT_EQUAL_STRING("ok", native_exec_status_name(NATIVE_EXEC_OK));
}

void test_native_exec_plan_header_and_abi_verdicts(void)
{
    size_t len = build_ex_blob();
    native_exec_plan_t plan;
    native_exec_layout_t layout;
    uint8_t *payload = s_blob + NATIVE_PAY_HEADER_LEN;

    clear_crypto();
    TEST_ASSERT_EQUAL(NATIVE_EXEC_OK, native_exec_plan(s_blob, len, &plan));
    TEST_ASSERT_EQUAL(NATIVE_PAY_OK, plan.envelope_status);
    TEST_ASSERT_EQUAL_UINT16(1u, plan.entry_ordinal);
    TEST_ASSERT_EQUAL_UINT32(EX_ENTRY_1, plan.entry_offset);
    TEST_ASSERT_TRUE(plan.api_ok);

    /* Structural refusal: bad envelope magic never reaches the ABI stage. */
    s_blob[NATIVE_PAY_OFF_MAGIC] ^= 0x01u;
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ENVELOPE,
                      native_exec_plan(s_blob, len, &plan));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_MAGIC, plan.envelope_status);
    build_ex_blob();

    /* No verifier is a distinct verdict, never a pass. */
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ENVELOPE,
                      native_exec_inspect(s_blob, len, &plan));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_NO_VERIFIER, plan.verify_status);

    /* Unsupported API bits are refused by the plan, before any mapping. */
    put_le32(s_blob + NATIVE_PAY_OFF_API_SET, NATIVE_PAY_API_WEBAPP);
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_API, native_exec_plan(s_blob, len, &plan));
    TEST_ASSERT_FALSE(plan.api_ok);
    build_ex_blob();

    /* An ELF header is rejected: the loader has no ELF/relocation path. */
    memcpy(payload, "\x7f" "ELF", 4u);
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ABI_MAGIC,
                      native_exec_abi_validate(payload, EX_PAYLOAD_BYTES,
                                               0u, &layout));
    build_ex_blob();
}

void test_native_exec_inspect_full_verify(void)
{
    size_t len = build_ex_blob();
    native_exec_plan_t plan;

    install_mock_crypto();
    TEST_ASSERT_EQUAL(NATIVE_EXEC_OK, native_exec_inspect(s_blob, len, &plan));
    TEST_ASSERT_EQUAL(NATIVE_PAY_OK, plan.verify_status);
    TEST_ASSERT_EQUAL_UINT32(EX_ENTRY_1, plan.entry_offset);

    s_blob[NATIVE_PAY_OFF_SHA256] ^= 0x01u;
    TEST_ASSERT_EQUAL(NATIVE_EXEC_ERR_ENVELOPE,
                      native_exec_inspect(s_blob, len, &plan));
    TEST_ASSERT_EQUAL(NATIVE_PAY_ERR_SHA, plan.verify_status);
    clear_crypto();
}

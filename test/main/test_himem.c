/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_himem.c
 * @brief Unit tests for the himem extended-memory core (batch/himem.c).
 *
 * Covers the pure size/handle parsers, the shared strutil CRC-32 primitive
 * `himem crc` relies on, handle alloc/free/lock/peek/fill bounds, the
 * per-slot quota, and the job-private environment overlay (allocate on a
 * scratch bg slot bound to the test task, verify the global table is
 * untouched, release). File transfer (load/save) needs the SD card and is
 * hardware-verified through the batch suite.
 */

#include "unity.h"
#include "batch.h"
#include "strutil.h"

#include <string.h>

/* All parse results fit 32 bits; Unity here has no 64-bit asserts, so
 * compare narrowed (overflow inputs expect 0 either way). */

void test_himem_parse_size_plain(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size(""));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)himem_parse_size("1"));
    TEST_ASSERT_EQUAL_UINT32(65536, (uint32_t)himem_parse_size("65536"));
}

void test_himem_parse_size_suffix(void)
{
    /* DOS K/M suffixes, case-insensitive, trailing only. */
    TEST_ASSERT_EQUAL_UINT32(1024, (uint32_t)himem_parse_size("1K"));
    TEST_ASSERT_EQUAL_UINT32(65536, (uint32_t)himem_parse_size("64k"));
    TEST_ASSERT_EQUAL_UINT32(1048576, (uint32_t)himem_parse_size("1M"));
    TEST_ASSERT_EQUAL_UINT32(4194304, (uint32_t)himem_parse_size("4m"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("K"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("1KK"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("K1"));
}

void test_himem_parse_size_hex(void)
{
    TEST_ASSERT_EQUAL_UINT32(255, (uint32_t)himem_parse_size("0xFF"));
    TEST_ASSERT_EQUAL_UINT32(256, (uint32_t)himem_parse_size("0x100"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("0x"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("0xZZ"));
}

void test_himem_parse_size_rejects(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("abc"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("12x"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("-5"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("1.5"));
    /* Overflow saturates to invalid, never wraps. */
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("18446744073709551616"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_parse_size("0x10000000000000000"));
}

void test_himem_parse_handle(void)
{
    TEST_ASSERT_EQUAL_INT(0, himem_parse_handle("0"));
    TEST_ASSERT_EQUAL_INT(7, himem_parse_handle("7"));
    TEST_ASSERT_EQUAL_INT(-1, himem_parse_handle(NULL));
    TEST_ASSERT_EQUAL_INT(-1, himem_parse_handle(""));
    TEST_ASSERT_EQUAL_INT(-1, himem_parse_handle("x"));
    TEST_ASSERT_EQUAL_INT(-1, himem_parse_handle("-1"));
    TEST_ASSERT_EQUAL_INT(-1, himem_parse_handle("1 "));
    /* At/above the table size is out of range. */
    TEST_ASSERT_EQUAL_INT(-1, himem_parse_handle("16"));
    TEST_ASSERT_EQUAL_INT(-1, himem_parse_handle("99999"));
}

void test_strutil_crc32_vectors(void)
{
    /* zlib reference: crc32("123456789") = 0xCBF43926. */
    const char *msg = "123456789";
    uint32_t crc;

    crc = strutil_crc32_update(0xFFFFFFFFu, (const uint8_t *)msg, 9);
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, (uint32_t)~crc);
    /* Empty input folds nothing; NULL data is safe. */
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, strutil_crc32_update(0xFFFFFFFFu,
                                                               (const uint8_t *)"", 0));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, strutil_crc32_update(0xFFFFFFFFu, NULL, 9));
    /* Incremental folding matches one-shot. */
    crc = strutil_crc32_update(0xFFFFFFFFu, (const uint8_t *)"12345", 5);
    crc = strutil_crc32_update(crc, (const uint8_t *)"6789", 4);
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, (uint32_t)~crc);
}

void test_himem_alloc_free_bounds(void)
{
    const uint8_t *ptr = NULL;
    size_t len = 0;
    himem_handle_info_t info;
    int h;

    TEST_ASSERT_EQUAL_INT(-1, himem_alloc(0, 0));
    TEST_ASSERT_EQUAL_INT(-1, himem_alloc((size_t)P4_CONFIG_HIMEM_MAX_BYTES + 1u, 0));
    TEST_ASSERT_EQUAL_INT(-1, himem_alloc(64, -1));
    h = himem_alloc(64, 0);
    TEST_ASSERT_TRUE(h >= 0);
    info = himem_describe(h);
    TEST_ASSERT_TRUE(info.used);
    TEST_ASSERT_EQUAL_UINT32(64, (uint32_t)info.size);
    TEST_ASSERT_EQUAL_INT(0, info.owner_slot);
    TEST_ASSERT_FALSE(info.locked);
    /* Zeroed at alloc (XMS contract). */
    TEST_ASSERT_EQUAL(ESP_OK, himem_peek(h, 0, 0, &ptr, &len));
    TEST_ASSERT_EQUAL_UINT32(64, (uint32_t)len);
    TEST_ASSERT_EACH_EQUAL_UINT8(0, ptr, 64);
    /* Peek bounds: past-end offset fails, short count clips. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, himem_peek(h, 65, 0, &ptr, &len));
    TEST_ASSERT_EQUAL(ESP_OK, himem_peek(h, 60, 99, &ptr, &len));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)len);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, himem_peek(99, 0, 0, &ptr, &len));
    /* Fill + locked-free refusal. */
    TEST_ASSERT_EQUAL(ESP_OK, himem_fill(h, 0, 0xAB, 0, 0));
    TEST_ASSERT_EQUAL(ESP_OK, himem_peek(h, 0, 1, &ptr, &len));
    TEST_ASSERT_EQUAL_UINT8(0xAB, ptr[0]);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, himem_fill(h, 0, 0, 64, 1));
    TEST_ASSERT_TRUE(himem_lock(h, 0) >= 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, himem_free(h, 0));
    TEST_ASSERT_EQUAL_INT(0, himem_unlock(h, 0));
    TEST_ASSERT_EQUAL(ESP_OK, himem_free(h, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, himem_free(h, 0));
    info = himem_describe(h);
    TEST_ASSERT_FALSE(info.used);
}

void test_himem_quota_and_owner(void)
{
    int h;

    /* A bg slot over quota is refused without allocating. */
    TEST_ASSERT_EQUAL_INT(-1, himem_alloc((size_t)P4_CONFIG_HIMEM_JOB_QUOTA_BYTES + 1u, 1));
    h = himem_alloc(128, 1);
    TEST_ASSERT_TRUE(h >= 0);
    TEST_ASSERT_EQUAL_UINT32(128, (uint32_t)himem_slot_bytes(1));
    /* Foreground supervises (may free), unlock of an unlocked block fails. */
    TEST_ASSERT_EQUAL_INT(-1, himem_unlock(h, 1));
    TEST_ASSERT_EQUAL(ESP_OK, himem_free(h, 0));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_slot_bytes(1));
    /* Job-exit cleanup frees owned blocks even when locked (no TSRs). */
    h = himem_alloc(64, 1);
    TEST_ASSERT_TRUE(h >= 0);
    TEST_ASSERT_TRUE(himem_lock(h, 1) >= 0);
    himem_free_owned(1);
    TEST_ASSERT_FALSE(himem_describe(h).used);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)himem_slot_bytes(1));
}

void test_himem_env_overlay_isolation(void)
{
    int slot;

    /* Bind a scratch bg slot to the test task: sets below land in the
     * job-private overlay, never in the global table. */
    TEST_ASSERT_EQUAL_INT(0, batch_bg_current_slot());
    /* Global ground truth, set in the foreground before binding. */
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("P4HIMEM_G", "gval"));
    slot = batch_bg_alloc();
    TEST_ASSERT_TRUE(slot > 0);
    batch_bg_bind(slot);
    TEST_ASSERT_EQUAL_INT(slot, batch_bg_current_slot());
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("P4HIMEM_PROBE", "jobvalue"));
    TEST_ASSERT_EQUAL_STRING("jobvalue", shell_env_get("P4HIMEM_PROBE"));
    /* Shadow-clear hides the global without touching it. */
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("P4HIMEM_G", ""));
    TEST_ASSERT_EQUAL_STRING("", shell_env_get("P4HIMEM_G"));
    batch_bg_release(slot);
    TEST_ASSERT_EQUAL_INT(0, batch_bg_current_slot());
    /* After release the overlay is gone: the probe var never existed
     * globally, and the shadowed global is intact. */
    TEST_ASSERT_NULL(shell_env_get("P4HIMEM_PROBE"));
    TEST_ASSERT_EQUAL_STRING("gval", shell_env_get("P4HIMEM_G"));
    TEST_ASSERT_EQUAL(ESP_OK, shell_env_set("P4HIMEM_G", ""));
}

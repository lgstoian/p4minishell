/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_pim.c
 * @brief Unit tests for the shared PIM render buffer (components/pim).
 *
 * Covers pim_doc_* growth/overflow/escaping and pim_copy_trunc(), plus the
 * sync-identity helpers (uid mint shape, newer-wins decision, identity
 * payload assembly, `mtime=` bump incl. the CSV pass-through). The
 * vCard/iCalendar line parsers are covered by test_import.c; store I/O
 * (db/alarm round-trips) stays hardware-verified.
 *
 * Pure logic with no hardware dependency (PSRAM-backed buffers only).
 */

#include "unity.h"
#include "pim.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void test_pim_doc_init_write(void)
{
    pim_doc_t doc;

    pim_doc_init(&doc);
    TEST_ASSERT_FALSE(doc.overflow);
    TEST_ASSERT_EQUAL_size_t(0, doc.used);
    pim_doc_text(&doc, "hello");
    TEST_ASSERT_FALSE(doc.overflow);
    TEST_ASSERT_EQUAL_size_t(5, doc.used);
    TEST_ASSERT_EQUAL_STRING("hello", doc.data);
    pim_doc_free(&doc);
}

void test_pim_doc_growth(void)
{
    pim_doc_t doc;
    size_t i;

    pim_doc_init(&doc);
    for (i = 0; i < 600; i++) {
        char chunk[9];
        snprintf(chunk, sizeof(chunk), "%08u", (unsigned)i);
        pim_doc_write(&doc, chunk, 8);
    }
    TEST_ASSERT_FALSE(doc.overflow);
    TEST_ASSERT_EQUAL_size_t(600u * 8u, doc.used);
    TEST_ASSERT_TRUE(doc.cap > 4096);
    TEST_ASSERT_EQUAL_STRING_LEN("00000000", doc.data, 8);
    TEST_ASSERT_EQUAL_STRING("00000599", doc.data + doc.used - 8);
    pim_doc_free(&doc);
}

void test_pim_doc_overflow_cap(void)
{
    pim_doc_t doc;
    size_t i;

    pim_doc_init(&doc);
    for (i = 0; i < 20000; i++) {
        pim_doc_text(&doc, "0123456789ABCDEF");
        if (doc.overflow) {
            break;
        }
    }
    TEST_ASSERT_TRUE(doc.overflow);
    TEST_ASSERT_TRUE(doc.used <= (size_t)P4_CONFIG_DB_EXPORT_MAX_BYTES);
    pim_doc_free(&doc);
}

void test_pim_doc_ical_escape(void)
{
    pim_doc_t doc;

    pim_doc_init(&doc);
    pim_doc_ical_text(&doc, "a\\b,c;d\ne\rf");
    TEST_ASSERT_FALSE(doc.overflow);
    TEST_ASSERT_EQUAL_STRING("a\\\\b\\,c\\;d\\nef", doc.data);
    pim_doc_free(&doc);
}

void test_pim_doc_ical_prop(void)
{
    pim_doc_t doc;

    pim_doc_init(&doc);
    pim_doc_ical_prop(&doc, "FN", "Jane Doe");
    TEST_ASSERT_FALSE(doc.overflow);
    TEST_ASSERT_EQUAL_STRING("FN:Jane Doe\r\n", doc.data);
    pim_doc_free(&doc);
}

void test_pim_copy_trunc_basic(void)
{
    char dst[8];

    pim_copy_trunc(dst, sizeof(dst), "hello");
    TEST_ASSERT_EQUAL_STRING("hello", dst);
    pim_copy_trunc(dst, sizeof(dst), "hello world, this is long");
    TEST_ASSERT_EQUAL_STRING("hello w", dst);
    pim_copy_trunc(dst, sizeof(dst), NULL);
    TEST_ASSERT_EQUAL_STRING("", dst);
    pim_copy_trunc(NULL, 8, "x");
    pim_copy_trunc(dst, 0, "x");
    TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_pim_mint_uid_shape(void)
{
    char a[33];
    char b[33];
    size_t i;

    pim_mint_uid(a, sizeof(a));
    TEST_ASSERT_EQUAL(32, strlen(a));
    for (i = 0; i < 32; i++) {
        TEST_ASSERT_TRUE(isxdigit((unsigned char)a[i]) != 0);
        TEST_ASSERT_TRUE(a[i] >= '0' && a[i] <= 'f');
    }
    pim_mint_uid(b, sizeof(b));
    TEST_ASSERT_FALSE(strcmp(a, b) == 0);
    /* NULL/zero destinations are safe no-ops. */
    pim_mint_uid(NULL, 33);
    {
        char tiny[1] = {'X'};
        pim_mint_uid(tiny, 0);
        TEST_ASSERT_EQUAL('X', tiny[0]);
    }
}

void test_pim_merge_should_replace(void)
{
    /* Newer incoming wins; ties apply (idempotent re-push). */
    TEST_ASSERT_TRUE(pim_merge_should_replace(100, 50));
    TEST_ASSERT_TRUE(pim_merge_should_replace(100, 100));
    TEST_ASSERT_FALSE(pim_merge_should_replace(50, 100));
    /* A missing incoming timestamp never overwrites a timestamped record. */
    TEST_ASSERT_FALSE(pim_merge_should_replace(0, 100));
    TEST_ASSERT_FALSE(pim_merge_should_replace(-5, 100));
    /* Timestamped incoming always beats untimestamped stored. */
    TEST_ASSERT_TRUE(pim_merge_should_replace(100, 0));
    /* Two untimestamped records replace (first-sync convergence). */
    TEST_ASSERT_TRUE(pim_merge_should_replace(0, 0));
}

void test_pim_vcf_build_payload_identity(void)
{
    pim_card_t card;
    char plain[1024];
    char synced[1024];

    memset(&card, 0, sizeof(card));
    pim_copy_trunc(card.name, sizeof(card.name), "Ann");
    pim_copy_trunc(card.tel, sizeof(card.tel), "123");
    /* Historical import shape: no identity fields. */
    pim_vcf_build_payload(&card, NULL, 0, plain, sizeof(plain));
    TEST_ASSERT_EQUAL_STRING("name=Ann;tel=123", plain);
    /* Merge shape: uid + mtime appended, content identical otherwise. */
    pim_vcf_build_payload(&card, "abc123", 1700000000, synced, sizeof(synced));
    TEST_ASSERT_EQUAL_STRING("name=Ann;tel=123;uid=abc123;mtime=1700000000",
                             synced);
}

void test_pim_bump_replaces_mtime(void)
{
    char buf[256];

    strcpy(buf, "name=Ann;tel=123;uid=abc123;mtime=1700000000");
    TEST_ASSERT_TRUE(pim_payload_bump_mtime(buf, sizeof(buf), 1800000000));
    TEST_ASSERT_EQUAL_STRING("name=Ann;tel=123;uid=abc123;mtime=1800000000",
                             buf);
}

void test_pim_bump_width_change(void)
{
    char buf[256];

    /* Digit width may grow or shrink; neighbours are preserved. */
    strcpy(buf, "name=Ann;uid=abc;mtime=5");
    TEST_ASSERT_TRUE(pim_payload_bump_mtime(buf, sizeof(buf), 1800000000));
    TEST_ASSERT_EQUAL_STRING("name=Ann;uid=abc;mtime=1800000000", buf);
    strcpy(buf, "name=Ann;uid=abc;mtime=1800000000");
    TEST_ASSERT_TRUE(pim_payload_bump_mtime(buf, sizeof(buf), 7));
    TEST_ASSERT_EQUAL_STRING("name=Ann;uid=abc;mtime=7", buf);
}

void test_pim_bump_appends_mtime(void)
{
    char buf[256];

    /* Identity without a stamp yet (uid present, mtime missing). */
    strcpy(buf, "name=Ann;uid=abc123");
    TEST_ASSERT_TRUE(pim_payload_bump_mtime(buf, sizeof(buf), 1800000000));
    TEST_ASSERT_EQUAL_STRING("name=Ann;uid=abc123;mtime=1800000000", buf);
}

void test_pim_bump_csv_untouched(void)
{
    /* CSV grids are opaque bytes to the sync layer: without a `uid=` field
     * the editor round-trip must leave them byte-identical. Quoted commas,
     * embedded quotes, `=` cells, and even a `uid` column must not trip
     * the identity gate. */
    static const char *csv =
        "id,name,uid,note\n"
        "1,Ann,xyz,\"hi, \"\"yo\"\"\"\n"
        "2,Bob,,x=1\n";
    char buf[512];

    strcpy(buf, csv);
    TEST_ASSERT_FALSE(pim_payload_bump_mtime(buf, sizeof(buf), 1800000000));
    TEST_ASSERT_EQUAL_STRING(csv, buf);
}

void test_pim_bump_plain_and_limits(void)
{
    char buf[64];

    /* Plain k=v without identity: untouched. */
    strcpy(buf, "name=Bob;tel=555");
    TEST_ASSERT_FALSE(pim_payload_bump_mtime(buf, sizeof(buf), 1800000000));
    TEST_ASSERT_EQUAL_STRING("name=Bob;tel=555", buf);
    /* Degenerate inputs are safe no-ops. */
    TEST_ASSERT_FALSE(pim_payload_bump_mtime(NULL, 64, 1800000000));
    TEST_ASSERT_FALSE(pim_payload_bump_mtime(buf, 0, 1800000000));
    TEST_ASSERT_FALSE(pim_payload_bump_mtime(buf, sizeof(buf), 0));
    TEST_ASSERT_EQUAL_STRING("name=Bob;tel=555", buf);
    /* No room to append: refused, buffer intact. */
    strcpy(buf, "name=Ann;uid=abc123");
    TEST_ASSERT_FALSE(pim_payload_bump_mtime(buf, strlen(buf) + 1,
                                             1800000000));
    TEST_ASSERT_EQUAL_STRING("name=Ann;uid=abc123", buf);
    /* No room to grow the stamp: refused, buffer intact. */
    strcpy(buf, "a=1;uid=u;mtime=5");
    TEST_ASSERT_FALSE(pim_payload_bump_mtime(buf, strlen(buf) + 1,
                                             1800000000));
    TEST_ASSERT_EQUAL_STRING("a=1;uid=u;mtime=5", buf);
}

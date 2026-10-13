/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_pim_carddav.c
 * @brief Unit tests for the pure CardDAV helpers.
 *
 * Covers the multistatus scanner, card splitter, UID extractor, URL joiner,
 * and UID safety check from components/pim/pim_carddav.c (behind
 * `pim net pull|push|sync`). Pure (no network/SD) so tested directly with
 * synthetic server responses.
 */

#include "unity.h"
#include "pim.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    char hrefs[4][128];
    char etags[4][64];
    int count;
} scan_ctx_t;

static void scan_collect(const char *href, const char *etag_or_null, void *ctx)
{
    scan_ctx_t *c = (scan_ctx_t *)ctx;

    if (c->count >= 4) {
        return;
    }
    snprintf(c->hrefs[c->count], sizeof(c->hrefs[0]), "%s", href);
    snprintf(c->etags[c->count], sizeof(c->etags[0]), "%s",
             etag_or_null != NULL ? etag_or_null : "");
    c->count++;
}

void test_pim_carddav_scan_prefixed(void)
{
    static const char xml[] =
        "<?xml version=\"1.0\"?>"
        "<d:multistatus xmlns:d=\"DAV:\">"
        "<d:response><d:href>/dav/a/</d:href></d:response>"
        "<d:response><d:href>/dav/a/one.vcf</d:href>"
        "<d:propstat><d:prop><d:getetag>\"abc\"</d:getetag></d:prop></d:propstat>"
        "</d:response>"
        "</d:multistatus>";
    scan_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(2, pim_carddav_scan_multistatus(xml, sizeof(xml) - 1,
                                                          scan_collect, &ctx));
    TEST_ASSERT_EQUAL_INT(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("/dav/a/", ctx.hrefs[0]);
    TEST_ASSERT_EQUAL_STRING("", ctx.etags[0]);
    TEST_ASSERT_EQUAL_STRING("/dav/a/one.vcf", ctx.hrefs[1]);
    TEST_ASSERT_EQUAL_STRING("\"abc\"", ctx.etags[1]);
}

void test_pim_carddav_scan_entities(void)
{
    static const char xml[] =
        "<multistatus><response><href>/dav/a/J%C3%B6rg&amp;x.vcf</href>"
        "</response></multistatus>";
    scan_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(1, pim_carddav_scan_multistatus(xml, sizeof(xml) - 1,
                                                          scan_collect, &ctx));
    TEST_ASSERT_EQUAL_STRING("/dav/a/J%C3%B6rg&x.vcf", ctx.hrefs[0]);
    TEST_ASSERT_EQUAL_INT(-1, pim_carddav_scan_multistatus("x", 1, NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(0, pim_carddav_scan_multistatus(NULL, 0, scan_collect, &ctx));
}

typedef struct {
    int count;
    size_t lens[4];
} split_ctx_t;

static void split_collect(const char *card, size_t len, void *ctx)
{
    split_ctx_t *c = (split_ctx_t *)ctx;

    if (c->count < 4) {
        c->lens[c->count] = len;
        (void)card;
    }
    c->count++;
}

void test_pim_carddav_split_and_uid(void)
{
    static const char coll[] =
        "BEGIN:VCARD\r\nUID:aaa-1\r\nFN:Alice\r\nEND:VCARD\r\n"
        "BEGIN:VCARD\r\nUID;TYPE=home:b-b-2\r\nFN:Bob\r\nEND:VCARD\r\n";
    split_ctx_t ctx;
    char uid[64];

    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(2, pim_carddav_split_cards(coll, sizeof(coll) - 1,
                                                     split_collect, &ctx));
    TEST_ASSERT_EQUAL_INT(2, ctx.count);

    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)pim_carddav_card_uid(coll, sizeof(coll) - 1,
                                                               uid, sizeof(uid)));
    TEST_ASSERT_EQUAL_STRING("aaa-1", uid);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)pim_carddav_card_uid("FN:NoUid\r\n", 10,
                                                               uid, sizeof(uid)));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)pim_carddav_card_uid(NULL, 0, uid, sizeof(uid)));
}

void test_pim_carddav_join_and_safe(void)
{
    char out[128];

    TEST_ASSERT_TRUE(pim_carddav_join_url("https://h/dav/a/", "/dav/a/x.vcf",
                                          out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h/dav/a/x.vcf", out);
    TEST_ASSERT_TRUE(pim_carddav_join_url("https://h/dav/a/", "https://x/y.vcf",
                                          out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://x/y.vcf", out);
    TEST_ASSERT_TRUE(pim_carddav_join_url("https://h/dav/a/", "rel.vcf",
                                          out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h/dav/a/rel.vcf", out);
    TEST_ASSERT_FALSE(pim_carddav_join_url(NULL, "x", out, sizeof(out)));

    TEST_ASSERT_TRUE(pim_carddav_uid_safe("550e8400-e29b-41d4-a716-446655440000"));
    TEST_ASSERT_TRUE(pim_carddav_uid_safe("user@example.com"));
    TEST_ASSERT_FALSE(pim_carddav_uid_safe("../evil"));
    TEST_ASSERT_FALSE(pim_carddav_uid_safe(""));
    TEST_ASSERT_FALSE(pim_carddav_uid_safe(NULL));
}

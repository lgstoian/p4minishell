/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_rss_store.c
 * @brief Unit tests for the RSS store's pure record editors and CONFIG.SYS
 *        directive parser (components/rss/src/rss_store.c).
 *
 * Only I/O-free helpers are covered here; the SD persistence, the fetch path
 * and the poller are hardware-verified (see the `rss` verbs on the bench).
 */

#include "unity.h"
#include "p4minishell_config.h"
#include "rss.h"
#include "rss_store.h"

#include <stdio.h>
#include <string.h>

/** Build one subscription record; keeps the field count obvious. */
static void make_record(char *out, size_t cap, const char *name, const char *url,
                        const char *type, const char *user, const char *token,
                        const char *etag, const char *modified, const char *fetched,
                        const char *enabled, const char *interval, const char *title)
{
    snprintf(out, cap, "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
             name, url, type, user, token, etag, modified, fetched, enabled,
             interval, title);
}

void test_rss_store_name_ok(void)
{
    TEST_ASSERT_TRUE(rss_store_name_ok("news"));
    TEST_ASSERT_TRUE(rss_store_name_ok("a"));
    TEST_ASSERT_TRUE(rss_store_name_ok("A1_-"));
    TEST_ASSERT_TRUE(rss_store_name_ok("0123456789"));

    TEST_ASSERT_FALSE(rss_store_name_ok(NULL));
    TEST_ASSERT_FALSE(rss_store_name_ok(""));
    TEST_ASSERT_FALSE(rss_store_name_ok("has space"));
    TEST_ASSERT_FALSE(rss_store_name_ok("slash/name"));
    TEST_ASSERT_FALSE(rss_store_name_ok("pipe|name"));
    TEST_ASSERT_FALSE(rss_store_name_ok("dot.name"));

    /* One past the capacity is rejected, not truncated. */
    char too_long[P4_CONFIG_RSS_SUB_NAME_BYTES + 1];
    memset(too_long, 'a', P4_CONFIG_RSS_SUB_NAME_BYTES);
    too_long[P4_CONFIG_RSS_SUB_NAME_BYTES] = '\0';
    TEST_ASSERT_FALSE(rss_store_name_ok(too_long));
}

void test_rss_store_record_roundtrip(void)
{
    rss_subscription_t sub;
    rss_subscription_t back;
    char line[P4_CONFIG_RSS_STORE_LINE_BYTES];

    memset(&sub, 0, sizeof(sub));
    strcpy(sub.name, "news");
    strcpy(sub.url, "https://example.com/feed.xml");
    sub.auth_type = RSS_AUTH_BASIC;
    strcpy(sub.auth_user, "alice");
    strcpy(sub.auth_token, "s3cret");
    strcpy(sub.etag, "\"abc123\"");
    strcpy(sub.last_modified, "Mon, 03 Nov 2026 10:00:00 GMT");
    strcpy(sub.last_fetched, "1793683294");
    sub.enabled = true;
    sub.interval_min = 30;
    strcpy(sub.title, "Example News");

    TEST_ASSERT_TRUE(rss_store_render_record(&sub, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "news|https://example.com/feed.xml|1|alice|s3cret|\"abc123\"|"
        "Mon, 03 Nov 2026 10:00:00 GMT|1793683294|1|30|Example News",
        line);

    TEST_ASSERT_EQUAL_INT(ESP_OK, rss_store_parse_record(line, &back));
    TEST_ASSERT_EQUAL_STRING("news", back.name);
    TEST_ASSERT_EQUAL_STRING("https://example.com/feed.xml", back.url);
    TEST_ASSERT_EQUAL_INT((int)RSS_AUTH_BASIC, (int)back.auth_type);
    TEST_ASSERT_EQUAL_STRING("alice", back.auth_user);
    TEST_ASSERT_EQUAL_STRING("s3cret", back.auth_token);
    TEST_ASSERT_EQUAL_STRING("\"abc123\"", back.etag);
    TEST_ASSERT_EQUAL_STRING("Mon, 03 Nov 2026 10:00:00 GMT", back.last_modified);
    TEST_ASSERT_EQUAL_STRING("1793683294", back.last_fetched);
    TEST_ASSERT_TRUE(back.enabled);
    TEST_ASSERT_EQUAL_INT(30, back.interval_min);
    TEST_ASSERT_EQUAL_STRING("Example News", back.title);
}

void test_rss_store_record_no_auth(void)
{
    rss_subscription_t sub;
    rss_subscription_t back;
    char line[P4_CONFIG_RSS_STORE_LINE_BYTES];

    memset(&sub, 0, sizeof(sub));
    strcpy(sub.name, "atomfeed");
    strcpy(sub.url, "https://example.com/atom");
    sub.auth_type = RSS_AUTH_NONE;
    sub.enabled = false;
    sub.interval_min = 0;
    strcpy(sub.last_fetched, "0");

    TEST_ASSERT_TRUE(rss_store_render_record(&sub, line, sizeof(line)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, rss_store_parse_record(line, &back));
    TEST_ASSERT_EQUAL_STRING("atomfeed", back.name);
    TEST_ASSERT_EQUAL_INT((int)RSS_AUTH_NONE, (int)back.auth_type);
    TEST_ASSERT_FALSE(back.enabled);
    TEST_ASSERT_EQUAL_INT(0, back.interval_min);
    TEST_ASSERT_EQUAL_STRING("0", back.last_fetched);
}

void test_rss_store_record_rejects(void)
{
    rss_subscription_t out;
    char line[P4_CONFIG_RSS_STORE_LINE_BYTES];

    /* Comments and blanks are not records. */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record("; note", &out));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record("# note", &out));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record("", &out));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(NULL, &out));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record("n|u", NULL));

    /* Field count. */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record("a|b|c", &out));
    make_record(line, sizeof(line), "n", "u", "0", "", "", "", "", "", "1", "0", "");
    TEST_ASSERT_EQUAL_INT(ESP_OK, rss_store_parse_record(line, &out));

    /* Auth type must be 0..2. */
    make_record(line, sizeof(line), "n", "u", "3", "", "", "", "", "", "1", "0", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));
    make_record(line, sizeof(line), "n", "u", "x", "", "", "", "", "", "1", "0", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));

    /* Enabled must be 0 or 1. */
    make_record(line, sizeof(line), "n", "u", "0", "", "", "", "", "", "2", "0", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));

    /* Interval must be a non-negative integer. */
    make_record(line, sizeof(line), "n", "u", "0", "", "", "", "", "", "1", "-1", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));

    /* FETCHED must be a unix-seconds integer. */
    make_record(line, sizeof(line), "n", "u", "0", "", "", "", "", "soon", "1", "0", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));
    make_record(line, sizeof(line), "n", "u", "0", "", "", "", "", "-1", "1", "0", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));

    /* An over-long URL is refused, never silently truncated (a truncated
     * feed URL would point at a different resource). */
    {
        char long_url[P4_CONFIG_RSS_SUB_URL_BYTES + 8];

        memset(long_url, 'u', sizeof(long_url) - 1);
        long_url[sizeof(long_url) - 1] = '\0';
        make_record(line, sizeof(line), "n", long_url, "0", "", "", "", "", "", "1", "0", "");
        TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));
    }

    /* The name is validated, not just stored. */
    make_record(line, sizeof(line), "bad name", "u", "0", "", "", "", "", "", "1", "0", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rss_store_parse_record(line, &out));

    /* A CRLF-terminated line still parses. */
    make_record(line, sizeof(line), "n", "u", "0", "", "", "", "", "", "1", "0", "");
    strcat(line, "\r\n");
    TEST_ASSERT_EQUAL_INT(ESP_OK, rss_store_parse_record(line, &out));
}

void test_rss_store_render_rejects_separator(void)
{
    rss_subscription_t sub;
    char line[P4_CONFIG_RSS_STORE_LINE_BYTES];

    memset(&sub, 0, sizeof(sub));
    strcpy(sub.name, "news");
    strcpy(sub.url, "https://example.com/feed");

    /* A '|' in any field would corrupt the record, so rendering refuses it. */
    strcpy(sub.title, "a|b");
    TEST_ASSERT_FALSE(rss_store_render_record(&sub, line, sizeof(line)));
    strcpy(sub.title, "fine");

    /* Out-of-range auth type. */
    sub.auth_type = (rss_auth_type_t)9;
    TEST_ASSERT_FALSE(rss_store_render_record(&sub, line, sizeof(line)));
    sub.auth_type = RSS_AUTH_NONE;

    /* A buffer too small for the whole record is refused, never truncated. */
    TEST_ASSERT_TRUE(rss_store_render_record(&sub, line, sizeof(line)));
    TEST_ASSERT_FALSE(rss_store_render_record(&sub, line, 8));

    /* Empty name / url. */
    sub.name[0] = '\0';
    TEST_ASSERT_FALSE(rss_store_render_record(&sub, line, sizeof(line)));
}

void test_rss_store_read_line(void)
{
    char name[P4_CONFIG_RSS_SUB_NAME_BYTES];
    char hash[32];

    TEST_ASSERT_TRUE(rss_store_parse_read_line("news|652f4ad9fd6a1457",
                                               name, sizeof(name), hash, sizeof(hash)));
    TEST_ASSERT_EQUAL_STRING("news", name);
    TEST_ASSERT_EQUAL_STRING("652f4ad9fd6a1457", hash);

    TEST_ASSERT_FALSE(rss_store_parse_read_line("; comment", name, sizeof(name),
                                                hash, sizeof(hash)));
    TEST_ASSERT_FALSE(rss_store_parse_read_line("nohash", name, sizeof(name),
                                                hash, sizeof(hash)));
    TEST_ASSERT_FALSE(rss_store_parse_read_line("bad name|x", name, sizeof(name),
                                                hash, sizeof(hash)));
    TEST_ASSERT_FALSE(rss_store_parse_read_line(NULL, name, sizeof(name),
                                                hash, sizeof(hash)));
}

void test_rss_store_directives(void)
{
    char out[32];
    char buf[32];

    TEST_ASSERT_TRUE(rss_store_apply_directive("RSS_INTERVAL", "45"));
    TEST_ASSERT_TRUE(rss_store_render_directive("RSS_INTERVAL", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("45", out);

    TEST_ASSERT_TRUE(rss_store_apply_directive("interval", "0"));
    TEST_ASSERT_TRUE(rss_store_render_directive("RSS_INTERVAL", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0", out);

    TEST_ASSERT_TRUE(rss_store_apply_directive("autopoll", "ON"));
    TEST_ASSERT_TRUE(rss_store_render_directive("RSS_AUTOPOLL", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ON", out);

    TEST_ASSERT_TRUE(rss_store_apply_directive("RSS_RETENTION", "50"));
    TEST_ASSERT_TRUE(rss_store_render_directive("retention", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("50", out);

    TEST_ASSERT_TRUE(rss_store_apply_directive("RSS_SAVEREAD", "OFF"));
    TEST_ASSERT_TRUE(rss_store_render_directive("saveread", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("OFF", out);

    /* Invalid values are rejected, not clamped silently. */
    TEST_ASSERT_FALSE(rss_store_apply_directive("interval", "-1"));
    TEST_ASSERT_FALSE(rss_store_apply_directive("interval", "abc"));
    TEST_ASSERT_FALSE(rss_store_apply_directive("interval", ""));
    TEST_ASSERT_FALSE(rss_store_apply_directive("autopoll", "maybe"));
    TEST_ASSERT_FALSE(rss_store_apply_directive("retention", "0"));
    TEST_ASSERT_FALSE(rss_store_apply_directive("retention", "99999"));
    TEST_ASSERT_FALSE(rss_store_apply_directive("saveread", "x"));
    TEST_ASSERT_FALSE(rss_store_apply_directive("unknown", "1"));
    TEST_ASSERT_FALSE(rss_store_render_directive("unknown", out, sizeof(out)));

    /* Restore the defaults so later suites see a clean store. */
    rss_store_apply_directive("interval", "60");
    rss_store_apply_directive("autopoll", "OFF");
    snprintf(buf, sizeof(buf), "%d", (int)P4_CONFIG_RSS_ITEM_CAPACITY);
    rss_store_apply_directive("retention", buf);
    rss_store_apply_directive("saveread", "ON");
}

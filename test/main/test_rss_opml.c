/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_rss_opml.c
 * @brief OPML 2.0 import edge cases (components/rss/src/rss_opml.c).
 *
 * The parser is pure (expat + the subscription struct), so the cases build
 * documents in memory: standard attributes, the p4* round-trip extensions,
 * name sanitisation, folders/text-only outlines, the entry cap, malformed
 * XML and over-long values (which must be skipped, never truncated).
 */

#include "unity.h"
#include "p4minishell_config.h"
#include "rss.h"
#include "rss_store.h"

#include <stdio.h>
#include <string.h>

static int parse(const char *xml, rss_subscription_t *out, int max)
{
    int count = -1;

    TEST_ASSERT_EQUAL_INT(ESP_OK, rss_opml_parse(xml, strlen(xml), out, max, &count));
    TEST_ASSERT_TRUE(count >= 0);
    TEST_ASSERT_TRUE(count <= max);
    return count;
}

void test_rss_opml_parse_standard(void)
{
    static const char XML[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<opml version=\"2.0\"><head><title>Feeds</title></head><body>\n"
        "  <outline text=\"Folder\">\n"
        "    <outline text=\"News\" title=\"News feed\" type=\"rss\""
        " xmlUrl=\"https://example.com/news.xml\" />\n"
        "  </outline>\n"
        "  <outline text=\"Tech\" xmlUrl=\"http://example.com/tech\" />\n"
        "  <outline text=\"Text only\" />\n"
        "</body></opml>\n";
    rss_subscription_t out[4];
    int count;

    memset(out, 0, sizeof(out));
    count = parse(XML, out, 4);
    /* The folder and the text-only outline carry no xmlUrl: skipped. */
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_STRING("News", out[0].name);
    TEST_ASSERT_EQUAL_STRING("https://example.com/news.xml", out[0].url);
    TEST_ASSERT_EQUAL_INT((int)RSS_AUTH_NONE, (int)out[0].auth_type);
    TEST_ASSERT_TRUE(out[0].enabled);
    TEST_ASSERT_EQUAL_INT(0, out[0].interval_min);
    TEST_ASSERT_EQUAL_STRING("Tech", out[1].name);
    TEST_ASSERT_EQUAL_STRING("http://example.com/tech", out[1].url);
}

void test_rss_opml_parse_extensions(void)
{
    static const char XML[] =
        "<opml version=\"2.0\"><body>"
        "<outline text=\"Paid\" xmlUrl=\"https://example.com/paid\""
        " p4AuthType=\"2\" p4User=\"alice\" p4Token=\"tok\""
        " p4Etag=\"&quot;v1&quot;\" p4Modified=\"Mon, 03 Nov 2026 10:00:00 GMT\""
        " p4Interval=\"30\" p4Enabled=\"0\" />"
        "</body></opml>";
    rss_subscription_t out[1];
    int count;

    memset(out, 0, sizeof(out));
    count = parse(XML, out, 1);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_INT((int)RSS_AUTH_BEARER, (int)out[0].auth_type);
    TEST_ASSERT_EQUAL_STRING("alice", out[0].auth_user);
    TEST_ASSERT_EQUAL_STRING("tok", out[0].auth_token);
    TEST_ASSERT_EQUAL_STRING("\"v1\"", out[0].etag);
    TEST_ASSERT_EQUAL_STRING("Mon, 03 Nov 2026 10:00:00 GMT", out[0].last_modified);
    TEST_ASSERT_EQUAL_INT(30, out[0].interval_min);
    TEST_ASSERT_FALSE(out[0].enabled);

    /* Invalid extension values leave the documented defaults rather than
     * accepting garbage: auth type falls back to NONE, interval to 0 and the
     * enabled flag to true. */
    {
        static const char BAD[] =
            "<opml><body><outline text=\"Bad\" xmlUrl=\"https://example.com/bad\""
            " p4AuthType=\"9\" p4Interval=\"soon\" p4Enabled=\"2\" />"
            "</body></opml>";

        memset(out, 0, sizeof(out));
        count = parse(BAD, out, 1);
        TEST_ASSERT_EQUAL_INT(1, count);
        TEST_ASSERT_EQUAL_INT((int)RSS_AUTH_NONE, (int)out[0].auth_type);
        TEST_ASSERT_EQUAL_INT(0, out[0].interval_min);
        TEST_ASSERT_TRUE(out[0].enabled);
    }
}

void test_rss_opml_parse_sanitizes_names(void)
{
    static const char XML[] =
        "<opml><body>"
        "<outline text=\"My Feed!\" xmlUrl=\"https://example.com/a\" />"
        "<outline xmlUrl=\"https://feeds.example.com/rss\" />"
        "<outline text=\"\" title=\"\" xmlUrl=\"http://example.org:8080/x\" />"
        "</body></opml>";
    rss_subscription_t out[3];
    int count;

    memset(out, 0, sizeof(out));
    count = parse(XML, out, 3);
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_EQUAL_STRING("My_Feed_", out[0].name);
    /* No text: derive from the URL host (sanitised, no scheme/path). */
    TEST_ASSERT_EQUAL_STRING("feeds_example_com", out[1].name);
    TEST_ASSERT_EQUAL_STRING("example_org_8080", out[2].name);
}

void test_rss_opml_parse_limits_and_duplicates(void)
{
    static const char XML[] =
        "<opml><body>"
        "<outline text=\"One\" xmlUrl=\"https://example.com/1\" />"
        "<outline text=\"Two\" xmlUrl=\"https://example.com/2\" />"
        "<outline text=\"One\" xmlUrl=\"https://example.com/1-duplicate\" />"
        "</body></opml>";
    rss_subscription_t out[8];
    int count;

    memset(out, 0, sizeof(out));
    count = parse(XML, out, 8);
    /* Duplicate names collapse to the first entry: the store is keyed by name
     * and the caller's "added" count must match what it can actually add. */
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_STRING("One", out[0].name);
    TEST_ASSERT_EQUAL_STRING("Two", out[1].name);

    /* The entry cap stops parsing more outlines but still succeeds. */
    memset(out, 0, sizeof(out));
    count = parse(XML, out, 1);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_STRING("One", out[0].name);
}

void test_rss_opml_parse_rejects(void)
{
    static const char NOT_XML[] = "<opml><body><outline text=\"x\" xmlUrl=\"y\">";
    rss_subscription_t out[2];
    int count = -1;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          rss_opml_parse(NULL, 0, out, 2, &count));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          rss_opml_parse("", 0, NULL, 2, &count));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          rss_opml_parse("", 0, out, 0, &count));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          rss_opml_parse("", 0, out, 2, NULL));
    /* Unclosed element: expat fails and the caller gets a hard error. */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          rss_opml_parse(NOT_XML, strlen(NOT_XML), out, 2, &count));
    TEST_ASSERT_EQUAL_INT(-1, count);

    /* A well-formed document with no outline is a successful empty import. */
    {
        static const char EMPTY[] = "<opml version=\"2.0\"><head/><body/></opml>";

        count = -1;
        TEST_ASSERT_EQUAL_INT(ESP_OK,
                              rss_opml_parse(EMPTY, strlen(EMPTY), out, 2, &count));
        TEST_ASSERT_EQUAL_INT(0, count);
    }
}

void test_rss_opml_parse_bounds(void)
{
    char xml[P4_CONFIG_RSS_SUB_URL_BYTES + 512];
    rss_subscription_t out[2];
    int count;

    /* An xmlUrl that cannot fit the store is skipped, never truncated (a
     * truncated URL would fetch a different resource). */
    {
        size_t pos = (size_t)snprintf(xml, sizeof(xml),
                                      "<opml><body><outline text=\"Huge\" xmlUrl=\"https://example.com/");
        size_t i;

        for (i = 0; i < P4_CONFIG_RSS_SUB_URL_BYTES + 8; i++) {
            xml[pos++] = 'a';
        }
        snprintf(xml + pos, sizeof(xml) - pos, "\" /></body></opml>");

        memset(out, 0, sizeof(out));
        count = parse(xml, out, 2);
        TEST_ASSERT_EQUAL_INT(0, count);
    }

    /* An over-long p4User is skipped for the same reason. */
    {
        size_t pos = (size_t)snprintf(xml, sizeof(xml),
                                      "<opml><body><outline text=\"Auth\""
                                      " xmlUrl=\"https://example.com/ok\" p4User=\"");
        size_t i;

        for (i = 0; i < P4_CONFIG_RSS_SUB_AUTH_BYTES + 8; i++) {
            xml[pos++] = 'u';
        }
        snprintf(xml + pos, sizeof(xml) - pos, "\" /></body></opml>");

        memset(out, 0, sizeof(out));
        count = parse(xml, out, 2);
        TEST_ASSERT_EQUAL_INT(0, count);
    }
}

void test_rss_opml_write_null_safe(void)
{
    rss_subscription_t sub;

    memset(&sub, 0, sizeof(sub));
    strcpy(sub.name, "news");
    strcpy(sub.url, "https://example.com/feed");

    /* NULL file / NULL list are no-ops, not crashes; the input is untouched. */
    rss_opml_write(NULL, &sub, 1);
    rss_opml_write(NULL, NULL, 0);
    rss_opml_write(NULL, &sub, 0);
    TEST_ASSERT_EQUAL_STRING("news", sub.name);
    TEST_ASSERT_EQUAL_STRING("https://example.com/feed", sub.url);
}

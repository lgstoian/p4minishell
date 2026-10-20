/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_httpget.c
 * @brief Cookie-jar unit tests for the httpget / browser HTTP surface.
 *
 * The network transport cannot run headless, but the jar itself is pure state:
 * Set-Cookie parsing, domain scoping, replacement, deletion and eviction are
 * exercised here so a firmware change to the header parser is caught without
 * an AP.
 */

#include "unity.h"
#include "networking.h"
#include "p4minishell_config.h"

#include <stdio.h>
#include <string.h>

static void clear_jar(void)
{
    networking_http_cookie_clear();
    (void)networking_http_cookie_take_dirty();
}

void test_cookie_store_and_scope(void)
{
    networking_cookie_t cookie;

    clear_jar();
    TEST_ASSERT_EQUAL_INT(0, networking_http_cookie_count());

    TEST_ASSERT_TRUE(networking_http_cookie_store_header("example.com", "sid=abc123; Path=/"));
    TEST_ASSERT_EQUAL_INT(1, networking_http_cookie_count());
    TEST_ASSERT_TRUE(networking_http_cookie_at(0, &cookie));
    TEST_ASSERT_EQUAL_STRING("example.com", cookie.domain);
    TEST_ASSERT_EQUAL_STRING("sid", cookie.name);
    TEST_ASSERT_EQUAL_STRING("abc123", cookie.value);
    TEST_ASSERT_EQUAL_UINT32(0, cookie.expires_ts);

    /* An explicit Domain attribute (leading dot) is normalized for storage. */
    TEST_ASSERT_TRUE(networking_http_cookie_store_header(
        "www.example.com", "lang=en; Domain=.Example.COM; Path=/"));
    TEST_ASSERT_EQUAL_INT(2, networking_http_cookie_count());

    /* Re-setting the same domain+name replaces rather than duplicates. */
    TEST_ASSERT_TRUE(networking_http_cookie_store_header("example.com", "sid=next; Max-Age=3600"));
    TEST_ASSERT_EQUAL_INT(2, networking_http_cookie_count());
    TEST_ASSERT_TRUE(networking_http_cookie_at(0, &cookie));
    TEST_ASSERT_EQUAL_STRING("next", cookie.value);
    TEST_ASSERT_TRUE(cookie.expires_ts != 0);
}

void test_cookie_delete_and_clear(void)
{
    networking_cookie_t cookie;

    clear_jar();
    TEST_ASSERT_TRUE(networking_http_cookie_store_header("host.test", "a=1"));
    TEST_ASSERT_TRUE(networking_http_cookie_store_header("host.test", "b=2"));
    TEST_ASSERT_EQUAL_INT(2, networking_http_cookie_count());

    /* Max-Age=0 is the conventional delete. */
    TEST_ASSERT_FALSE(networking_http_cookie_store_header("host.test", "a=0; Max-Age=0"));
    TEST_ASSERT_EQUAL_INT(1, networking_http_cookie_count());
    TEST_ASSERT_TRUE(networking_http_cookie_at(0, &cookie));
    TEST_ASSERT_EQUAL_STRING("b", cookie.name);

    /* An epoch Expires date also deletes. */
    TEST_ASSERT_FALSE(networking_http_cookie_store_header(
        "host.test", "b=x; Expires=Thu, 01 Jan 1970 00:00:00 GMT"));
    TEST_ASSERT_EQUAL_INT(0, networking_http_cookie_count());

    TEST_ASSERT_TRUE(networking_http_cookie_add("host.test", "c", "3", 0));
    TEST_ASSERT_TRUE(networking_http_cookie_add("host.test", "d", "4", 0));
    networking_http_cookie_clear();
    TEST_ASSERT_EQUAL_INT(0, networking_http_cookie_count());
}

void test_cookie_rejects_malformed_headers(void)
{
    clear_jar();
    TEST_ASSERT_FALSE(networking_http_cookie_store_header("host.test", "no-equals"));
    TEST_ASSERT_FALSE(networking_http_cookie_store_header("host.test", ""));
    TEST_ASSERT_FALSE(networking_http_cookie_store_header(NULL, "a=b"));
    TEST_ASSERT_FALSE(networking_http_cookie_store_header("host.test", NULL));
    TEST_ASSERT_EQUAL_INT(0, networking_http_cookie_count());

    TEST_ASSERT_FALSE(networking_http_cookie_add(NULL, "a", "b", 0));
    TEST_ASSERT_FALSE(networking_http_cookie_add("host.test", "", "b", 0));
    TEST_ASSERT_FALSE(networking_http_cookie_add("", "a", "b", 0));
    TEST_ASSERT_EQUAL_INT(0, networking_http_cookie_count());
}

void test_cookie_capacity_eviction(void)
{
    char name[16];
    int i;

    clear_jar();
    for (i = 0; i < P4_CONFIG_HTTP_COOKIE_MAX + 4; i++) {
        snprintf(name, sizeof(name), "c%d", i);
        TEST_ASSERT_TRUE(networking_http_cookie_add("host.test", name, "v", 0));
    }
    TEST_ASSERT_EQUAL_INT(P4_CONFIG_HTTP_COOKIE_MAX, networking_http_cookie_count());

    /* The most recent cookies survive; the oldest were evicted. */
    {
        networking_cookie_t cookie;
        TEST_ASSERT_TRUE(networking_http_cookie_at(P4_CONFIG_HTTP_COOKIE_MAX - 1, &cookie));
        TEST_ASSERT_EQUAL_STRING("c19", cookie.name);
    }
    clear_jar();
}

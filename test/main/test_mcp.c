/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_mcp.c
 * @brief Pure tests for the MCP JSON-RPC request builder and client defaults.
 */

#include "unity.h"
#include "mcp.h"

#include <string.h>

void test_mcp_build_request_basic(void)
{
    char out[256];
    size_t n = mcp_build_request(7, "tools/list", "{}", out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\",\"params\":{}}",
        out);
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
}

void test_mcp_build_request_no_params(void)
{
    char out[256];
    size_t n = mcp_build_request(1, "notifications/initialized", NULL,
                                 out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"notifications/initialized\"}",
        out);
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
}

void test_mcp_build_request_truncates(void)
{
    char out[16];

    TEST_ASSERT_EQUAL_size_t(0, mcp_build_request(1, "tools/list", "{}",
                                                  out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_mcp_build_request_rejects_empty(void)
{
    char out[64];

    TEST_ASSERT_EQUAL_size_t(0, mcp_build_request(1, "", "{}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, mcp_build_request(1, NULL, "{}", out, sizeof(out)));
}

void test_mcp_init_defaults(void)
{
    mcp_client_t client;

    memset(&client, 0xFF, sizeof(client));
    mcp_init(&client);
    TEST_ASSERT_FALSE(client.connected);
    TEST_ASSERT_FALSE(client.initialized);
    TEST_ASSERT_EQUAL_INT(1, client.next_id);
    TEST_ASSERT_EQUAL_STRING("", client.url);
    TEST_ASSERT_EQUAL_STRING("", client.server_name);
}

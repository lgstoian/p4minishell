/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mcp.h
 * @module mcp
 * @layer leaf
 * @domain net
 * @provides MCP (Model Context Protocol) JSON-RPC client: initialize, tools/list, tools/call
 * @status experimental
 * @test test/main/test_mcp.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Minimal MCP client over the shared networking HTTP transport.
 *
 * The Model Context Protocol is JSON-RPC 2.0. This leaf speaks the HTTP
 * transport: `initialize`, `notifications/initialized`, `tools/list`, and
 * `tools/call`. It reuses components/networking for the request (TLS policy,
 * timeouts, body caps) and components/json for response parsing - there is no
 * second HTTP client and no second JSON parser.
 *
 * The command surface (`mcp status|connect|tools|call`) lives in
 * components/command/mcp_commands.c; this component never prints or parses a
 * command line.
 */

#ifndef P4MINISHELL_MCP_H
#define P4MINISHELL_MCP_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_URL_MAX   256
#define MCP_TOKEN_MAX 128
#define MCP_NAME_MAX  64

/** One MCP session. Zero-initialize, then mcp_init()/mcp_connect(). */
typedef struct {
    char url[MCP_URL_MAX];
    char token[MCP_TOKEN_MAX];   /**< Reserved for a future bearer header. */
    char server_name[MCP_NAME_MAX];
    char server_version[MCP_NAME_MAX];
    int next_id;
    bool connected;
    bool initialized;
} mcp_client_t;

/** Reset a client to the disconnected state (next_id = 1). */
void mcp_init(mcp_client_t *client);

/** Connect to an MCP endpoint and run `initialize` + the initialized
 *  notification. @p token is stored for a future bearer transport. */
esp_err_t mcp_connect(mcp_client_t *client, const char *url, const char *token);

/** `tools/list`: format `  name` + `      description` lines into @p out. */
esp_err_t mcp_list_tools(mcp_client_t *client, char *out, size_t out_size);

/** `tools/call`: @p args_json is the `arguments` object (NULL/empty -> {}).
 *  Formats the text content items into @p out. */
esp_err_t mcp_call_tool(mcp_client_t *client, const char *tool,
                        const char *args_json, char *out, size_t out_size);

/** Pure JSON-RPC 2.0 request builder (unit-tested).
 *  @return bytes written (excluding NUL), or 0 when the output is too small. */
size_t mcp_build_request(int id, const char *method, const char *params_json,
                         char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_MCP_H */

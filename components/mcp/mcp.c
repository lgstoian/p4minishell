/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mcp.c
 * @brief MCP JSON-RPC client over components/networking.
 *
 * Every request is a JSON-RPC 2.0 object built by mcp_build_request() and sent
 * with networking_http_request(HTTP_METHOD_POST, ...). Responses are walked
 * with the shared json_walk() reader: `result.serverInfo.*` for initialize,
 * `result.tools.*.name/description` for tools/list, and
 * `result.content.*.text` for tools/call. `error.message` is surfaced as a
 * failure.
 */

#include "mcp.h"

#include "json.h"
#include "networking.h"

#include "esp_http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t mcp_build_request(int id, const char *method, const char *params_json,
                         char *out, size_t out_size)
{
    int n;

    if (out == NULL || out_size == 0 || method == NULL || method[0] == '\0') {
        return 0;
    }
    if (params_json != NULL && params_json[0] != '\0') {
        n = snprintf(out, out_size,
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                     id, method, params_json);
    } else {
        n = snprintf(out, out_size,
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}",
                     id, method);
    }
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

void mcp_init(mcp_client_t *client)
{
    if (client == NULL) return;
    memset(client, 0, sizeof(*client));
    client->next_id = 1;
}

typedef struct {
    mcp_client_t *client;
    char *out;
    size_t out_size;
    size_t pos;
    char error[128];
    int mode;   /* 0 initialize, 1 tools/list, 2 tools/call */
} mcp_walk_t;

static void mcp_append(mcp_walk_t *w, const char *fmt, const char *text)
{
    int n;

    if (w->out == NULL || w->pos + 1 >= w->out_size) return;
    n = snprintf(w->out + w->pos, w->out_size - w->pos, fmt, text);
    if (n > 0) {
        w->pos += (size_t)n;
        if (w->pos >= w->out_size) {
            w->pos = w->out_size - 1;
        }
    }
}

static bool mcp_walk_cb(const char *path, const char *value, size_t len, void *ctx)
{
    mcp_walk_t *w = (mcp_walk_t *)ctx;
    char decoded[256];

    if (strstr(path, "error.message") != NULL) {
        (void)json_unescape_span(value, len, decoded, sizeof(decoded));
        snprintf(w->error, sizeof(w->error), "%.*s",
                 (int)(sizeof(w->error) - 1), decoded);
        return true;
    }
    (void)json_unescape_span(value, len, decoded, sizeof(decoded));

    if (w->mode == 0) {
        if (w->client != NULL && strcmp(path, "result.serverInfo.name") == 0) {
            snprintf(w->client->server_name, sizeof(w->client->server_name),
                     "%.*s", (int)(sizeof(w->client->server_name) - 1), decoded);
        } else if (w->client != NULL &&
                   strcmp(path, "result.serverInfo.version") == 0) {
            snprintf(w->client->server_version, sizeof(w->client->server_version),
                     "%.*s", (int)(sizeof(w->client->server_version) - 1), decoded);
        }
        return true;
    }
    if (w->mode == 1) {
        const char *field = strrchr(path, '.');
        if (strncmp(path, "result.tools.", 13) == 0 && field != NULL) {
            if (strcmp(field, ".name") == 0) {
                mcp_append(w, "  %s\n", decoded);
            } else if (strcmp(field, ".description") == 0) {
                mcp_append(w, "      %s\n", decoded);
            }
        }
        return true;
    }
    if (strncmp(path, "result.content.", 15) == 0 &&
        strstr(path, ".text") != NULL) {
        mcp_append(w, "%s\n", decoded);
    } else if (strcmp(path, "result.isError") == 0 && strcmp(decoded, "true") == 0) {
        mcp_append(w, "%s", "[tool error]\n");
    }
    return true;
}

static esp_err_t mcp_post(mcp_client_t *client, const char *body,
                          char **resp_out, size_t *resp_len_out)
{
    networking_http_result_t result;
    esp_err_t err;

    if (client == NULL || body == NULL || resp_out == NULL ||
        resp_len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *resp_out = NULL;
    *resp_len_out = 0;
    memset(&result, 0, sizeof(result));
    err = networking_http_request(HTTP_METHOD_POST, client->url,
                                  "application/json",
                                  (const uint8_t *)body, strlen(body),
                                  NULL, NULL, NULL, &result);
    if (err != ESP_OK) {
        free(result.body);
        return err;
    }
    *resp_out = (char *)result.body;
    *resp_len_out = result.body_size;
    return ESP_OK;
}

static esp_err_t mcp_run(mcp_client_t *client, int mode, const char *method,
                         const char *params, char *out, size_t out_size)
{
    char request[512];
    char *response = NULL;
    size_t response_len = 0;
    mcp_walk_t w;
    esp_err_t err;

    if (client == NULL || !client->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mcp_build_request(client->next_id++, method, params,
                          request, sizeof(request)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = mcp_post(client, request, &response, &response_len);
    if (err != ESP_OK) {
        return err;
    }
    memset(&w, 0, sizeof(w));
    w.client = client;
    w.out = out;
    w.out_size = out_size;
    w.mode = mode;
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    (void)json_walk(response, response_len, mcp_walk_cb, &w, NULL, 0);
    free(response);
    if (w.error[0] != '\0') {
        if (out != NULL && out_size > 0) {
            snprintf(out, out_size, "mcp error: %s\n", w.error);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mcp_connect(mcp_client_t *client, const char *url, const char *token)
{
    esp_err_t err;

    if (client == NULL || url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    mcp_init(client);
    snprintf(client->url, sizeof(client->url), "%s", url);
    snprintf(client->token, sizeof(client->token), "%s", token != NULL ? token : "");
    client->connected = true;
    err = mcp_run(client, 0, "initialize",
                  "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
                  "\"clientInfo\":{\"name\":\"p4minishell\",\"version\":\"1.4.1\"}}",
                  NULL, 0);
    if (err != ESP_OK) {
        client->connected = false;
        return err;
    }
    /* JSON-RPC notification: no id, no response expected. */
    {
        char *ignored = NULL;
        size_t ignored_len = 0;
        (void)mcp_post(client,
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                       &ignored, &ignored_len);
        free(ignored);
    }
    client->initialized = true;
    return ESP_OK;
}

esp_err_t mcp_list_tools(mcp_client_t *client, char *out, size_t out_size)
{
    esp_err_t err = mcp_run(client, 1, "tools/list", "{}", out, out_size);

    if (err == ESP_OK && out != NULL && out_size > 0 && out[0] == '\0') {
        snprintf(out, out_size, "  (no tools)\n");
    }
    return err;
}

esp_err_t mcp_call_tool(mcp_client_t *client, const char *tool,
                        const char *args_json, char *out, size_t out_size)
{
    char params[384];

    if (tool == NULL || tool[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(params, sizeof(params), "{\"name\":\"%s\",\"arguments\":%s}",
             tool,
             (args_json != NULL && args_json[0] != '\0') ? args_json : "{}");
    return mcp_run(client, 2, "tools/call", params, out, out_size);
}

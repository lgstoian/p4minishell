/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mcp_commands.c
 * @brief `mcp` command surface over components/mcp.
 *
 * Thin command-layer wrapper: the JSON-RPC client, HTTP transport, and JSON
 * parsing all live in components/mcp + components/networking + components/json.
 * This file only parses the verb, prints the result, and maps failures onto
 * ERRORLEVEL (0 ok / 1 failure / 2 usage).
 */

#include "command.h"
#include "ansi_palette.h"
#include "mcp.h"
#include "shell.h"

#include <stdio.h>
#include <string.h>

static mcp_client_t s_mcp;

static void mcp_usage(void)
{
    shell_print_usage("Usage: mcp status | mcp connect <url> [token] | "
                      "mcp tools | mcp call <tool> [json]");
}

int shell_command_mcp(int argc, char **argv)
{
    if (argc < 2) {
        mcp_usage();
        return 2;
    }
    if (strcmp(argv[1], "status") == 0) {
        shell_transcript_appendf_ansi(SH_LBL "mcp.connected:" SH_RST " %s\n",
                                      s_mcp.connected ? "yes" : "no");
        shell_transcript_appendf_ansi(SH_LBL "mcp.initialized:" SH_RST " %s\n",
                                      s_mcp.initialized ? "yes" : "no");
        shell_transcript_appendf_ansi(SH_LBL "mcp.url:" SH_RST " %s\n",
                                      s_mcp.url[0] != '\0' ? s_mcp.url : "-");
        shell_transcript_appendf_ansi(SH_LBL "mcp.server:" SH_RST " %s %s\n",
                                      s_mcp.server_name[0] != '\0' ? s_mcp.server_name : "-",
                                      s_mcp.server_version);
        return 0;
    }
    if (strcmp(argv[1], "connect") == 0) {
        const char *url;
        const char *token = NULL;
        esp_err_t err;

        if (argc < 3) {
            mcp_usage();
            return 2;
        }
        url = argv[2];
        if (argc >= 4) token = argv[3];
        err = mcp_connect(&s_mcp, url, token);
        if (err != ESP_OK) {
            shell_print_error("mcp: connect failed (%s)", esp_err_to_name(err));
            return 1;
        }
        shell_print_ok("mcp: connected to %s %s",
                       s_mcp.server_name[0] != '\0' ? s_mcp.server_name : url,
                       s_mcp.server_version);
        return 0;
    }
    if (strcmp(argv[1], "tools") == 0) {
        char out[2048];
        esp_err_t err = mcp_list_tools(&s_mcp, out, sizeof(out));

        if (err != ESP_OK) {
            shell_print_error("mcp: tools/list failed (%s)", esp_err_to_name(err));
            if (out[0] != '\0') shell_transcript_append_text(out);
            return 1;
        }
        shell_transcript_append_text(out);
        return 0;
    }
    if (strcmp(argv[1], "call") == 0) {
        char out[4096];
        const char *args;
        esp_err_t err;

        if (argc < 3) {
            mcp_usage();
            return 2;
        }
        args = (argc >= 4) ? argv[3] : "{}";
        err = mcp_call_tool(&s_mcp, argv[2], args, out, sizeof(out));
        if (err != ESP_OK) {
            shell_print_error("mcp: tools/call failed (%s)", esp_err_to_name(err));
            if (out[0] != '\0') shell_transcript_append_text(out);
            return 1;
        }
        shell_transcript_append_text(out);
        return 0;
    }
    mcp_usage();
    return 2;
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file chat_commands.c
 * @brief `chat` verb family implementation.
 *
 * Verb table (all read-only, no remote execution):
 *   chat host [ssid]          — start SoftAP host (opt-in STA exception)
 *   chat host off             — stop host, restore STA-only
 *   chat peer <host-ip>       — join host AP via `wifi connect`
 *   chat peer off             — disconnect from host
 *   chat send <text...>       — send message to peer (POST /chat)
 *   chat poll                 — fetch new messages since last poll (GET /chat?since=)
 *   chat log                  — show full local log
 *   chat clear                — clear inbox and log
 *   chat status               — show host/peer state
 *   chat onmsg <line>|off     — register /onmsg hook (runs on message receive)
 */

#include "command.h"
#include "networking.h"
#include "chat.h"
#include "shell.h"
#include "strutil.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>

/* Stored peer IP for chat send (set by chat peer <host-ip>). */
static char s_chat_peer_ip[16] = "";

/* Forward declarations */

int shell_command_chat(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_muted("chat host [ssid|off] | peer <host-ip|off> | send <text...> | poll | log | clear | status | onmsg <line>|off");
        return 2;
    }

    const shell_command_entry_t *table = chat_commands_get_table(NULL);
    size_t count = 0;
    chat_commands_get_table(&count);

    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(argv[1], table[i].name) == 0) {
            /* Build new argv for sub-command: shift by 1 */
            int sub_argc = argc - 1;
            char **sub_argv = &argv[1];
            return table[i].handler(sub_argc, sub_argv);
        }
    }

    shell_print_error("chat: unknown sub-command '%s'", argv[1]);
    shell_print_muted("chat host [ssid|off] | peer <host-ip|off> | send <text...> | poll | log | clear | status | onmsg <line>|off");
    return 2;
}

/* Forward declarations */
static esp_err_t chat_cmd_host(int argc, char *argv[]);
static esp_err_t chat_cmd_peer(int argc, char *argv[]);
static esp_err_t chat_cmd_send(int argc, char *argv[]);
static esp_err_t chat_cmd_poll(int argc, char *argv[]);
static esp_err_t chat_cmd_log(int argc, char *argv[]);
static esp_err_t chat_cmd_clear(int argc, char *argv[]);
static esp_err_t chat_cmd_status(int argc, char *argv[]);
static esp_err_t chat_cmd_onmsg(int argc, char *argv[]);

/* ---- chat host ---- */
static esp_err_t chat_cmd_host(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_muted("chat host [ssid|off]");
        return ESP_OK;
    }

if (strutil_text_equals_ignore_case(argv[1], "off")) {
        esp_err_t err = chat_host_off();

        if (err == ESP_OK) {
            shell_print_ok("chat: host stopped");
        }
        return err;
    }

    /* chat host [ssid] — default SSID if omitted */
    const char *ssid = (argc >= 3) ? argv[2] : NULL;
    char active_ssid[64];
    char url[96];
    esp_err_t err = chat_host_on(ssid, active_ssid, sizeof(active_ssid),
                                 url, sizeof(url));

    if (err == ESP_OK) {
        shell_print_ok("chat: host active - SSID %s, %s", active_ssid, url);
    } else {
        shell_print_error("chat: host start failed (%s)", esp_err_to_name(err));
    }
    return err;
}

/* ---- chat peer ---- */
static esp_err_t chat_cmd_peer(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_muted("chat peer <host-ip|off>");
        return ESP_OK;
    }

    if (strutil_text_equals_ignore_case(argv[1], "off")) {
        /* Disconnect from the host AP — just disconnect Wi-Fi.
         * networking_wifi_disconnect() returns void, so call it and report
         * success rather than returning its (non-existent) value. */
        s_chat_peer_ip[0] = '\0';
        networking_wifi_disconnect();
        return ESP_OK;
    }

    /* chat peer <host-ip> — join the host's AP.
     * The host SSID is fixed; we connect to it via wifi connect. */
    const char *host_ip = argv[1];
    /* Validate IP format (basic check). */
    if (host_ip == NULL || host_ip[0] == '\0') {
        shell_print_error("chat: host IP required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Store peer IP for chat send. */
    strlcpy(s_chat_peer_ip, host_ip, sizeof(s_chat_peer_ip));

    /* Use the known host SSID to connect. */
    return networking_wifi_connect_to(P4_CONFIG_CHAT_AP_SSID, NULL);
}

/* ---- chat send ---- */
static esp_err_t chat_cmd_send(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_error("chat: send <text...>");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_chat_peer_ip[0] == '\0') {
        shell_print_error("chat: no peer set (run 'chat peer <host-ip>' first)");
        return ESP_ERR_INVALID_STATE;
    }

    /* Join remaining args as the message text. */
    size_t total_len = 0;
    for (int i = 1; i < argc; i++) {
        total_len += strlen(argv[i]) + 1;
    }

    char *text = malloc(total_len + 1);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *p = text;
    for (int i = 1; i < argc; i++) {
        if (i > 1) *p++ = ' ';
        size_t len = strlen(argv[i]);
        memcpy(p, argv[i], len);
        p += len;
    }
    *p = '\0';

    esp_err_t err = chat_host_send(s_chat_peer_ip, text);
    free(text);

    if (err == ESP_OK) {
        shell_print_muted("chat: message sent");
    } else {
        shell_print_error("chat: send failed (@y%s@R)", esp_err_to_name(err));
    }
    return err;
}

/* ---- chat poll ---- */
static esp_err_t chat_cmd_poll(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    /* We need a persistent since_seq. For simplicity, use a static.
     * In a full implementation, this would be per-session state. */
    static uint32_t since_seq = 0;

    char *messages = NULL;
    esp_err_t err = chat_host_poll(since_seq, &messages);
    if (err != ESP_OK) {
        shell_print_error("chat: poll failed (@y%s@R)", esp_err_to_name(err));
        return err;
    }

    if (messages != NULL && messages[0] != '\0') {
        /* Parse messages to update since_seq and print. */
        char *line = strtok(messages, "\n");
        while (line != NULL) {
            uint32_t seq;
            char *text = strchr(line, '\t');
            if (text != NULL) {
                *text++ = '\0';
                char *from = strchr(text, '\t');
                if (from != NULL) {
                    *from++ = '\0';
                    seq = (uint32_t)strtoul(line, NULL, 10);
                    if (seq > since_seq) since_seq = seq;
                    shell_transcript_appendf_ansi("@M%s@R: %s", from, text);
                }
            }
            line = strtok(NULL, "\n");
        }
    }

    free(messages);
    return ESP_OK;
}

/* ---- chat log ---- */
static esp_err_t chat_cmd_log(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    char *log = NULL;
    esp_err_t err = chat_host_log(&log);
    if (err != ESP_OK) {
        shell_print_error("chat: log failed (@y%s@R)", esp_err_to_name(err));
        return err;
    }

    if (log != NULL && log[0] != '\0') {
        shell_transcript_appendf_ansi("%s", log);
    } else {
        shell_print_muted("chat: log empty");
    }

    free(log);
    return ESP_OK;
}

/* ---- chat clear ---- */
static esp_err_t chat_cmd_clear(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return chat_host_clear();
}

/* ---- chat status ---- */
static esp_err_t chat_cmd_status(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    char *status = NULL;
    esp_err_t err = chat_host_status(&status);
    if (err != ESP_OK) {
        shell_print_error("chat: status failed (@y%s@R)", esp_err_to_name(err));
        return err;
    }

    if (status != NULL) {
        shell_transcript_appendf_ansi("%s", status);
        free(status);
    }
    return ESP_OK;
}

/* ---- chat onmsg ---- */
static esp_err_t chat_cmd_onmsg(int argc, char *argv[])
{
    if (argc < 2) {
        shell_print_muted("chat onmsg <line>|off");
        return ESP_OK;
    }

    if (strutil_text_equals_ignore_case(argv[1], "off")) {
        /* TODO: Clear the onmsg hook. */
        shell_print_muted("chat: onmsg disabled");
        return ESP_OK;
    }

    /* Join remaining args as the batch line to run on message receive. */
    size_t total_len = 0;
    for (int i = 1; i < argc; i++) {
        total_len += strlen(argv[i]) + 1;
    }

    char *line = malloc(total_len + 1);
    if (line == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *p = line;
    for (int i = 1; i < argc; i++) {
        if (i > 1) *p++ = ' ';
        size_t len = strlen(argv[i]);
        memcpy(p, argv[i], len);
        p += len;
    }
    *p = '\0';

    /* TODO: Register the onmsg hook via chat_set_onmsg_cb().
     * For now, just store the line. */
    shell_print_muted("chat: onmsg set to: %s", line);
    free(line);
    return ESP_OK;
}

/* ---- Command table ---- */
static const shell_command_entry_t s_chat_commands[] = {
    { "host",   chat_cmd_host,   "chat host [ssid|off] — start/stop SoftAP host (opt-in APSTA)" },
    { "peer",   chat_cmd_peer,   "chat peer <host-ip|off> — join/leave host AP" },
    { "send",   chat_cmd_send,   "chat send <text...> — send message to peer" },
    { "poll",   chat_cmd_poll,   "chat poll — fetch new messages since last poll" },
    { "log",    chat_cmd_log,    "chat log — show full local message log" },
    { "clear",  chat_cmd_clear,  "chat clear — clear inbox and log" },
    { "status", chat_cmd_status, "chat status — show host/peer state" },
    { "onmsg",  chat_cmd_onmsg,  "chat onmsg <line>|off — register on-message hook" },
};

const shell_command_entry_t *chat_commands_get_table(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(s_chat_commands) / sizeof(s_chat_commands[0]);
    }
    return s_chat_commands;
}
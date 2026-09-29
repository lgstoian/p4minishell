// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file netsvc_commands.c
 * @brief `net` verbs: batch surface for the persistent MQTT service.
 *
 * Thin shells over components/networking/netsvc.c (framing, merge, and
 * journaling all live there): usage, /b machine-readable forms, and
 * ERRORLEVEL only. Mutating verbs refuse while an OTA owns flash (PSRAM
 * payload buffers are off-limits then); `net status`/`net msg` stay
 * readable. Batch-friendly: 0 ok/queued, 1 failed, 2 usage.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "batch.h"
#include "c6ota.h"
#include "command.h"
#include "netsvc.h"
#include "p4minishell_config.h"
#include "shell.h"

static void shell_command_net_usage(void)
{
    shell_print_usage("Usage: net status [/b]");
    shell_print_usage("       net broker <host> [port] [user] | net broker | net password");
    shell_print_usage("       net connect | net disconnect");
    shell_print_usage("       net sub <filter> | net unsub <filter> | net subs [/b]");
    shell_print_usage("       net pub <topic> <text...>");
    shell_print_usage("       net msg [/b] | net onmsg <line...> | net onmsg off");
    shell_print_usage("       net outbox [/b] | net outbox purge");
}

static bool net_ota_busy(void)
{
    return c6ota_is_confirmation_pending() || c6ota_is_busy();
}

static int net_cmd_status(int argc, char **argv)
{
    bool bare = (argc == 3 && shell_text_equals_ignore_case(argv[2], "/b"));
    char host[128];
    char user[64];
    int port = 1883;
    bool have_broker;
    if (argc != 2 && !bare) {
        shell_command_net_usage();
        return 2;
    }
    have_broker = (netsvc_get_broker(host, sizeof(host), &port,
                                     user, sizeof(user)) == ESP_OK);
    if (bare) {
        shell_transcript_appendf("%s\n", netsvc_state_string());
        shell_transcript_appendf("%d\n", netsvc_sub_count());
        shell_transcript_appendf("%d\n", netsvc_outbox_count());
        return 0;
    }
    shell_print_field("net.state:", "%s", netsvc_state_string());
    if (have_broker) {
        shell_print_field("net.broker:", "%s:%d%s%s", host, port,
                          (user[0] != '\0') ? " as " : "", user);
    } else {
        shell_print_muted("net.broker: (not set)");
    }
    shell_print_field_num("net.subs:", (long)netsvc_sub_count());
    shell_print_field_num("net.outbox:", (long)netsvc_outbox_count());
    shell_print_ok("net: %s", netsvc_is_connected() ? "session up" : "no session");
    return 0;
}

static int net_cmd_broker(int argc, char **argv)
{
    int port = 1883;
    const char *user = "";
    char *end = NULL;
    long p;
    if (argc == 2) {
        /* Read-back: show the stored coordinates (read-only). */
        char host[128];
        char stored_user[64];
        int stored_port = 1883;
        if (netsvc_get_broker(host, sizeof(host), &stored_port,
                              stored_user, sizeof(stored_user)) != ESP_OK) {
            shell_print_muted("net.broker: (not set)");
            return 1;
        }
        shell_print_field("net.broker:", "%s:%d%s%s", host, stored_port,
                          (stored_user[0] != '\0') ? " as " : "", stored_user);
        return 0;
    }
    if (argc < 3 || argc > 5) {
        shell_command_net_usage();
        return 2;
    }
    if (net_ota_busy()) {
        shell_print_error("net: paused during OTA");
        return 1;
    }
    if (argc >= 4) {
        p = strtol(argv[3], &end, 10);
        if (end == NULL || *end != '\0' || p <= 0 || p > 65535) {
            shell_print_error("net: port must be 1..65535");
            return 2;
        }
        port = (int)p;
    }
    if (argc >= 5) {
        user = argv[4];
    }
    if (netsvc_set_broker(argv[2], port, user) != ESP_OK) {
        shell_print_error("net: cannot store broker profile");
        return 1;
    }
    shell_print_ok("net: broker %s:%d%s%s", argv[2], port,
                   (user[0] != '\0') ? " as " : "", user);
    return 0;
}

static int net_cmd_password(void)
{
    char pass[P4_CONFIG_NET_PASSWORD_BYTES];
    if (net_ota_busy()) {
        shell_print_error("net: paused during OTA");
        return 1;
    }
    shell_transcript_append_text("Broker password: ");
    if (!shell_read_line_hidden(pass, sizeof(pass), P4_CONFIG_KEY_WAIT_TIMEOUT_MS)) {
        shell_transcript_append_text("\n");
        shell_print_warning("net: password entry cancelled");
        return 1;
    }
    shell_transcript_append_text("\n");
    if (netsvc_set_password(pass) != ESP_OK) {
        memset(pass, 0, sizeof(pass));
        shell_print_error("net: cannot store password");
        return 1;
    }
    memset(pass, 0, sizeof(pass));
    shell_print_ok("net: password stored");
    return 0;
}

static int net_cmd_subs(int argc, char **argv)
{
    bool bare = (argc == 3 && shell_text_equals_ignore_case(argv[2], "/b"));
    int n;
    int i;
    if (argc != 2 && !bare) {
        shell_command_net_usage();
        return 2;
    }
    n = netsvc_sub_count();
    for (i = 0; i < n; i++) {
        char filter[P4_CONFIG_NET_TOPIC_BYTES];
        if (netsvc_sub_get(i, filter, sizeof(filter))) {
            if (bare) {
                shell_transcript_appendf("%s\n", filter);
            } else {
                shell_print_field("net.sub:", "%s", filter);
            }
        }
    }
    if (n == 0 && !bare) {
        shell_print_muted("net: no subscriptions");
    }
    return 0;
}

static int net_cmd_msg(int argc, char **argv)
{
    bool bare = (argc == 3 && shell_text_equals_ignore_case(argv[2], "/b"));
    char topic[P4_CONFIG_NET_TOPIC_BYTES];
    uint8_t *payload = NULL;
    size_t len = P4_CONFIG_NET_LASTMSG_BYTES;
    size_t cap = P4_CONFIG_NET_LASTMSG_BYTES;
    bool have;
    if (argc != 2 && !bare) {
        shell_command_net_usage();
        return 2;
    }
    payload = malloc(cap > 0 ? cap : 1);
    if (payload == NULL) {
        shell_print_error("net: out of memory");
        return 1;
    }
    have = netsvc_last_msg(topic, sizeof(topic), payload, &len, cap);
    if (!have) {
        free(payload);
        shell_print_muted("net: no message yet");
        return 1;
    }
    if (bare) {
        /* Bare payload bytes (may be binary): raw write, no CRLF mangling. */
        shell_uart_console_write_bytes(payload, len, P4_CONFIG_UART_WRITE_TOTAL_MS);
        shell_transcript_append_text("\n");
    } else {
        char *view = malloc(len + 1);
        if (view == NULL) {
            free(payload);
            shell_print_error("net: out of memory");
            return 1;
        }
        memcpy(view, payload, len);
        view[len] = '\0';
        shell_print_field("net.topic:", "%s", topic);
        shell_print_field_num("net.bytes:", (long)len);
        shell_transcript_append_text("---\n");
        shell_transcript_append_text(view);
        shell_transcript_append_text("\n---\n");
        free(view);
    }
    free(payload);
    return 0;
}

static int net_cmd_outbox(int argc, char **argv)
{
    bool bare = false;
    bool purge = false;
    if (argc == 3) {
        if (shell_text_equals_ignore_case(argv[2], "/b")) {
            bare = true;
        } else if (shell_text_equals_ignore_case(argv[2], "purge")) {
            purge = true;
        } else {
            shell_command_net_usage();
            return 2;
        }
    } else if (argc != 2) {
        shell_command_net_usage();
        return 2;
    }
    if (purge) {
        if (net_ota_busy()) {
            shell_print_error("net: paused during OTA");
            return 1;
        }
        if (netsvc_outbox_purge() != ESP_OK) {
            shell_print_error("net: cannot purge outbox");
            return 1;
        }
        shell_print_ok("net: outbox purged");
        return 0;
    }
    if (bare) {
        shell_transcript_appendf("%d\n", netsvc_outbox_count());
        return 0;
    }
    shell_print_field_num("net.outbox:", (long)netsvc_outbox_count());
    return 0;
}

int shell_command_net(int argc, char **argv)
{
    if (argc < 2 || argv == NULL) {
        shell_command_net_usage();
        return 2;
    }
    if (shell_text_equals_ignore_case(argv[1], "status")) {
        return net_cmd_status(argc, argv);
    }
    if (shell_text_equals_ignore_case(argv[1], "broker")) {
        return net_cmd_broker(argc, argv);
    }
    if (shell_text_equals_ignore_case(argv[1], "password")) {
        if (argc != 2) {
            shell_command_net_usage();
            return 2;
        }
        return net_cmd_password();
    }
    if (shell_text_equals_ignore_case(argv[1], "connect")) {
        if (argc != 2) {
            shell_command_net_usage();
            return 2;
        }
        if (net_ota_busy()) {
            shell_print_error("net: paused during OTA");
            return 1;
        }
        if (netsvc_connect() != ESP_OK) {
            shell_print_error("net: cannot start service");
            return 1;
        }
        shell_print_ok("net: connecting (see net status)");
        return 0;
    }
    if (shell_text_equals_ignore_case(argv[1], "disconnect")) {
        if (argc != 2) {
            shell_command_net_usage();
            return 2;
        }
        if (netsvc_disconnect() != ESP_OK) {
            shell_print_error("net: disconnect failed");
            return 1;
        }
        shell_print_ok("net: disconnected");
        return 0;
    }
    if (shell_text_equals_ignore_case(argv[1], "sub")) {
        if (argc != 3) {
            shell_command_net_usage();
            return 2;
        }
        if (net_ota_busy()) {
            shell_print_error("net: paused during OTA");
            return 1;
        }
        if (netsvc_subscribe(argv[2]) != ESP_OK) {
            shell_print_error("net: cannot subscribe %s", argv[2]);
            return 1;
        }
        shell_print_ok("net: subscribed %s", argv[2]);
        return 0;
    }
    if (shell_text_equals_ignore_case(argv[1], "unsub")) {
        if (argc != 3) {
            shell_command_net_usage();
            return 2;
        }
        if (netsvc_unsubscribe(argv[2]) != ESP_OK) {
            shell_print_error("net: not subscribed %s", argv[2]);
            return 1;
        }
        shell_print_ok("net: unsubscribed %s", argv[2]);
        return 0;
    }
    if (shell_text_equals_ignore_case(argv[1], "subs")) {
        return net_cmd_subs(argc, argv);
    }
    if (shell_text_equals_ignore_case(argv[1], "pub")) {
        char *text = NULL;
        esp_err_t err;
        if (argc < 4) {
            shell_command_net_usage();
            return 2;
        }
        if (net_ota_busy()) {
            shell_print_error("net: paused during OTA");
            return 1;
        }
        text = malloc(P4_CONFIG_COMMAND_BYTES);
        if (text == NULL) {
            shell_print_error("net: out of memory");
            return 1;
        }
        shell_join_args(argv, 3, argc, text, P4_CONFIG_COMMAND_BYTES);
        err = netsvc_publish(argv[2], (const uint8_t *)text, strlen(text));
        free(text);
        if (err == ESP_ERR_NO_MEM) {
            shell_print_error("net: outbox full");
            return 1;
        }
        if (err != ESP_OK) {
            shell_print_error("net: publish failed");
            return 1;
        }
        shell_print_ok("net: published %s (%s)", argv[2],
                       netsvc_is_connected() ? "live" : "queued");
        return 0;
    }
    if (shell_text_equals_ignore_case(argv[1], "msg")) {
        return net_cmd_msg(argc, argv);
    }
    if (shell_text_equals_ignore_case(argv[1], "onmsg")) {
        char *line = NULL;
        if (argc == 2) {
            shell_command_net_usage();
            return 2;
        }
        if (net_ota_busy()) {
            shell_print_error("net: paused during OTA");
            return 1;
        }
        if (argc == 3 && shell_text_equals_ignore_case(argv[2], "off")) {
            if (netsvc_set_onmsg(NULL) != ESP_OK) {
                shell_print_error("net: cannot clear hook");
                return 1;
            }
            shell_print_ok("net: hook cleared");
            return 0;
        }
        line = malloc(P4_CONFIG_COMMAND_BYTES);
        if (line == NULL) {
            shell_print_error("net: out of memory");
            return 1;
        }
        shell_join_args(argv, 2, argc, line, P4_CONFIG_COMMAND_BYTES);
        if (netsvc_set_onmsg(line) != ESP_OK) {
            free(line);
            shell_print_error("net: cannot store hook");
            return 1;
        }
        free(line);
        shell_print_ok("net: hook set ($NET_TOPIC/$NET_LEN on arrival)");
        return 0;
    }
    if (shell_text_equals_ignore_case(argv[1], "outbox")) {
        return net_cmd_outbox(argc, argv);
    }
    shell_command_net_usage();
    return 2;
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file rss_commands.c
 * @brief `rss` verb family implementation.
 *
 * RSS 2.0 / Atom 1.0 / JSON Feed reader commands:
 *   rss list                       — subscriptions with their fetch state
 *   rss add <name> <url> [user] [token]
 *   rss remove <name>
 *   rss enable <name> <on|off>
 *   rss interval <name> <minutes>  — 0 = use the CONFIG.SYS default
 *   rss refresh [name]             — fetch now (all feeds when omitted)
 *   rss read <name> [n]            — render an item (n, or the first unread)
 *   rss open <name> [n]            — open an item's link in the browser
 *   rss reset <name>               — mark every item read
 *   rss import <file>              — merge an OPML document
 *   rss export [file]              — write an OPML document
 *   rss set <key> <value>          — runtime tunables (CONFIG.SYS is persistent)
 *
 * The reader itself lives in components/rss; this file only turns argv into
 * store calls and transcript output. Item bodies go through html_render_ansi(),
 * the one HTML-to-ANSI path (the browser uses it too).
 *
 * Sub-command handlers receive the family table's sub-argv: argv[0] is the
 * sub-command name and the first real argument is argv[1].
 */

#include "command.h"
#include "html.h"
#include "markdown.h"
#include "p4minishell_config.h"
#include "rss.h"
#include "rss_store.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RSS_OPML_DEFAULT_PATH       "sd:/RSS/FEEDS.OPML"

/* Forward declarations */
static esp_err_t rss_cmd_list(int argc, char **argv);
static esp_err_t rss_cmd_add(int argc, char **argv);
static esp_err_t rss_cmd_remove(int argc, char **argv);
static esp_err_t rss_cmd_enable(int argc, char **argv);
static esp_err_t rss_cmd_interval(int argc, char **argv);
static esp_err_t rss_cmd_refresh(int argc, char **argv);
static esp_err_t rss_cmd_read(int argc, char **argv);
static esp_err_t rss_cmd_open(int argc, char **argv);
static esp_err_t rss_cmd_reset(int argc, char **argv);
static esp_err_t rss_cmd_import(int argc, char **argv);
static esp_err_t rss_cmd_export(int argc, char **argv);
static esp_err_t rss_cmd_set(int argc, char **argv);

/** The identity an item is remembered by: its guid, falling back to the link
 *  for feeds that publish no guid. */
static const char *rss_item_key(const rss_item_t *item)
{
    return item->guid[0] != '\0' ? item->guid : item->link;
}

/** Fetch @p name into the session cache; reports the outcome. @return the owned
 *  item copy (release with free()) or NULL when nothing could be selected. */
static rss_item_t *rss_load_cached(const char *name, int want, int *index_out)
{
    rss_item_t *item = rss_store_cached_item(name, want, index_out);

    if (item != NULL) {
        return item;
    }
    esp_err_t err = rss_store_fetch(name, NULL);

    if (err == RSS_ERR_NOT_MODIFIED) {
        shell_print_muted("rss: %s is unchanged since its last fetch", name);
        return NULL;
    }
    if (err != ESP_OK) {
        shell_print_error("rss: could not fetch %s (%s)", name, esp_err_to_name(err));
        return NULL;
    }
    item = rss_store_cached_item(name, want, index_out);
    if (item == NULL) {
        shell_print_muted("rss: %s has no items", name);
    }
    return item;
}

/* ---- list ---- */
static esp_err_t rss_cmd_list(int argc, char **argv)
{
    int count;

    (void)argc;
    (void)argv;

    count = rss_store_count();
    shell_print_heading("RSS Subscriptions (%d)", count);
    if (count == 0) {
        shell_print_muted("No feeds yet. Add one with: rss add <name> <url> [user] [token]");
        return 0;
    }

    for (int i = 0; i < count; i++) {
        rss_subscription_t sub;
        char fetched[64];
        int interval;

        if (!rss_store_get(i, &sub)) {
            break;
        }
        fetched[0] = '\0';
        rss_store_format_time(strtoull(sub.last_fetched, NULL, 10), fetched, sizeof(fetched));
        interval = sub.interval_min > 0 ? sub.interval_min : rss_store_get_interval_default();

        shell_print_muted("  %-20s %-4s interval=%-4d %s",
                          sub.name,
                          sub.enabled ? "on" : "off",
                          interval,
                          fetched[0] != '\0' ? fetched : "never");
        shell_print_muted("    %s", sub.url);
        if (sub.title[0] != '\0') {
            shell_print_muted("    %s", sub.title);
        }
    }
    shell_print_muted("Polling: %s, interval %d min, read-state %s",
                      rss_store_get_autopoll() ? "on" : "off",
                      rss_store_get_interval_default(),
                      rss_store_get_save_read() ? "saved" : "session-only");
    return 0;
}

/* ---- add ---- */
static esp_err_t rss_cmd_add(int argc, char **argv)
{
    rss_subscription_t sub;
    const char *name;
    const char *url;
    const char *user;
    const char *token;
    esp_err_t err;

    if (argc < 3) {
        shell_print_usage("Usage: rss add <name> <url> [user] [token]");
        return 2;
    }
    name = argv[1];
    url = argv[2];
    user = (argc > 3) ? argv[3] : "";
    token = (argc > 4) ? argv[4] : "";

    if (!rss_store_name_ok(name)) {
        shell_print_error("rss: name must be 1-%d characters of [A-Za-z0-9_-]",
                          (int)P4_CONFIG_RSS_SUB_NAME_BYTES - 1);
        return 2;
    }
    if (url[0] == '\0') {
        shell_print_error("rss: url must not be empty");
        return 2;
    }

    memset(&sub, 0, sizeof(sub));
    strncpy(sub.name, name, sizeof(sub.name) - 1);
    strncpy(sub.url, url, sizeof(sub.url) - 1);
    sub.enabled = true;
    if (user[0] != '\0' || token[0] != '\0') {
        sub.auth_type = RSS_AUTH_BASIC;
        strncpy(sub.auth_user, user, sizeof(sub.auth_user) - 1);
        strncpy(sub.auth_token, token, sizeof(sub.auth_token) - 1);
    }

    err = rss_store_add(&sub);
    if (err != ESP_OK) {
        shell_print_error("rss: could not add %s (%s)", name, esp_err_to_name(err));
        return 1;
    }
    shell_print_ok("rss: subscribed %s", name);
    return 0;
}

/* ---- remove ---- */
static esp_err_t rss_cmd_remove(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: rss remove <name>");
        return 2;
    }
    if (rss_store_find(argv[1]) < 0) {
        shell_print_error("rss: no subscription named '%s'", argv[1]);
        return 1;
    }
    esp_err_t err = rss_store_remove(argv[1]);

    if (err != ESP_OK) {
        shell_print_error("rss: could not remove %s (%s)", argv[1], esp_err_to_name(err));
        return 1;
    }
    shell_print_ok("rss: removed %s", argv[1]);
    return 0;
}

/* ---- enable ---- */
static esp_err_t rss_cmd_enable(int argc, char **argv)
{
    bool enable;

    if (argc < 3) {
        shell_print_usage("Usage: rss enable <name> <on|off>");
        return 2;
    }
    if (strcasecmp(argv[2], "on") == 0) {
        enable = true;
    } else if (strcasecmp(argv[2], "off") == 0) {
        enable = false;
    } else {
        shell_print_usage("Usage: rss enable <name> <on|off>");
        return 2;
    }
    if (rss_store_find(argv[1]) < 0) {
        shell_print_error("rss: no subscription named '%s'", argv[1]);
        return 1;
    }
    rss_store_set_enabled(argv[1], enable);
    shell_print_ok("rss: %s %s", argv[1], enable ? "enabled" : "disabled");
    return 0;
}

/* ---- interval ---- */
static esp_err_t rss_cmd_interval(int argc, char **argv)
{
    char *end = NULL;
    long minutes;

    if (argc < 3) {
        shell_print_usage("Usage: rss interval <name> <minutes>   (0 = CONFIG.SYS default)");
        return 2;
    }
    minutes = strtol(argv[2], &end, 10);
    if (end == NULL || *end != '\0' || minutes < 0 || minutes > 525600) {
        shell_print_error("rss: interval must be 0..525600 minutes");
        return 2;
    }
    if (rss_store_find(argv[1]) < 0) {
        shell_print_error("rss: no subscription named '%s'", argv[1]);
        return 1;
    }
    rss_store_set_interval(argv[1], (int)minutes);
    shell_print_ok("rss: %s interval %d min", argv[1], (int)minutes);
    return 0;
}

/* ---- refresh ---- */
static esp_err_t rss_cmd_refresh(int argc, char **argv)
{
    if (argc >= 2) {
        int index = rss_store_find(argv[1]);
        rss_subscription_t sub;
        esp_err_t err;

        if (index < 0 || !rss_store_get(index, &sub)) {
            shell_print_error("rss: no subscription named '%s'", argv[1]);
            return 1;
        }
        err = rss_store_fetch(sub.name, NULL);
        if (err == ESP_OK) {
            shell_print_ok("rss: %s refreshed - %d unread", sub.name,
                           rss_store_cached_unread(sub.name));
            return 0;
        }
        if (err == RSS_ERR_NOT_MODIFIED) {
            shell_print_muted("rss: %s unchanged", sub.name);
            return 0;
        }
        shell_print_error("rss: %s refresh failed (%s)", sub.name, esp_err_to_name(err));
        return 1;
    }

    return rss_store_refresh_all();
}

/* ---- read ---- */
static esp_err_t rss_cmd_read(int argc, char **argv)
{
    const char *name;
    rss_item_t *item;
    const char *key;
    int want;
    int index;

    if (argc < 2) {
        shell_print_usage("Usage: rss read <name> [n]   (n omits to the first unread)");
        return 2;
    }
    name = argv[1];
    if (rss_store_find(name) < 0) {
        shell_print_error("rss: no subscription named '%s'", name);
        return 1;
    }
    want = (argc >= 3) ? atoi(argv[2]) : 0;

    item = rss_load_cached(name, want, &index);
    if (item == NULL) {
        return 1;
    }

    key = rss_item_key(item);

    shell_print_heading("%s", item->title[0] != '\0' ? item->title : "(untitled)");
    shell_print_field_num("item", index + 1);
    shell_print_field("date", "%s", item->published[0] != '\0' ? item->published : "-");
    shell_print_field("author", "%s", item->author[0] != '\0' ? item->author : "-");
    shell_print_field("unread", "%d", rss_store_cached_unread(name));
    if (item->categories[0] != '\0') {
        shell_print_field("categories", "%s", item->categories);
    }
    shell_print_muted("");

    {
        const char *body = item->content[0] != '\0' ? item->content : item->summary;

        if (body[0] != '\0') {
            size_t need = html_render_ansi(body, NULL, 0);
            char *rendered = malloc(need + 1);

            if (rendered != NULL) {
                html_render_ansi(body, rendered, need + 1);
                shell_transcript_append_ansi(rendered);
                free(rendered);
            }
        }
    }

    if (key[0] != '\0') {
        rss_store_mark_read(name, key);
    }
    free(item);
    return 0;
}

/* ---- open ---- */
static esp_err_t rss_cmd_open(int argc, char **argv)
{
    const char *name;
    rss_item_t *item;
    int want;
    int index;
    char *command;

    if (argc < 2) {
        shell_print_usage("Usage: rss open <name> [n]");
        return 2;
    }
    name = argv[1];
    if (rss_store_find(name) < 0) {
        shell_print_error("rss: no subscription named '%s'", name);
        return 1;
    }
    want = (argc >= 3) ? atoi(argv[2]) : 0;

    item = rss_load_cached(name, want, &index);
    if (item == NULL) {
        return 1;
    }
    if (item->link[0] == '\0') {
        shell_print_error("rss: item %d has no link", index + 1);
        free(item);
        return 1;
    }

    command = malloc(strlen(item->link) + 16);
    if (command == NULL) {
        shell_print_error("rss: out of memory");
        free(item);
        return 1;
    }
    sprintf(command, "browser %s", item->link);
    shell_execute_command(command);
    free(command);
    free(item);
    return 0;
}

/* ---- reset ---- */
static esp_err_t rss_cmd_reset(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: rss reset <name>");
        return 2;
    }
    if (rss_store_find(argv[1]) < 0) {
        shell_print_error("rss: no subscription named '%s'", argv[1]);
        return 1;
    }
    rss_store_reset_read(argv[1]);
    shell_print_ok("rss: %s marked as fully read", argv[1]);
    return 0;
}

/* ---- import / export ---- */
static esp_err_t rss_cmd_import(int argc, char **argv)
{
    int added = 0;
    esp_err_t err;

    if (argc < 2) {
        shell_print_usage("Usage: rss import <file.opml>");
        return 2;
    }
    err = rss_store_import(argv[1], &added);
    if (err != ESP_OK) {
        shell_print_error("rss: import failed (%s)", esp_err_to_name(err));
        return 1;
    }
    shell_print_ok("rss: imported %d feed(s); existing names were left untouched", added);
    return 0;
}

static esp_err_t rss_cmd_export(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : RSS_OPML_DEFAULT_PATH;
    esp_err_t err;

    err = rss_store_export(path);
    if (err != ESP_OK) {
        shell_print_error("rss: export failed (%s)", esp_err_to_name(err));
        return 1;
    }
    shell_print_ok("rss: exported %d feed(s) to %s", rss_store_count(), path);
    shell_print_warning("rss: the OPML file contains feed tokens in plain text");
    return 0;
}

/* ---- set ---- */
static esp_err_t rss_cmd_set(int argc, char **argv)
{
    const char *key;
    const char *value;

    if (argc < 3) {
        shell_print_usage("Usage: rss set <autopoll|interval|retention|saveread> <value>");
        return 2;
    }
    key = argv[1];
    value = argv[2];

    if (!rss_store_apply_directive(key, value)) {
        shell_print_error("rss: unknown key or bad value for '%s'", key);
        shell_print_muted("keys: autopoll ON|OFF, interval <minutes>, "
                          "retention <items>, saveread ON|OFF");
        return 2;
    }

    /* The poller follows the flag immediately so the command is useful at
     * runtime, not just at the next boot. */
    if (strcasecmp(key, "autopoll") == 0 || strcasecmp(key, "RSS_AUTOPOLL") == 0) {
        if (rss_store_get_autopoll()) {
            rss_store_start_poller();
        } else {
            rss_store_stop_poller();
        }
    }

    shell_print_ok("rss: %s set to %s", key, value);
    return 0;
}

/* ---- Command table ---- */
static const shell_command_entry_t s_rss_commands[] = {
    { "list",    rss_cmd_list,    "rss list — subscriptions and their fetch state" },
    { "add",     rss_cmd_add,     "rss add <name> <url> [user] [token] — subscribe" },
    { "remove",  rss_cmd_remove,  "rss remove <name> — unsubscribe" },
    { "enable",  rss_cmd_enable,  "rss enable <name> <on|off> — poll this feed" },
    { "interval", rss_cmd_interval, "rss interval <name> <minutes> — 0 = default" },
    { "refresh", rss_cmd_refresh, "rss refresh [name] — fetch now" },
    { "read",    rss_cmd_read,    "rss read <name> [n] — render an item" },
    { "open",    rss_cmd_open,    "rss open <name> [n] — open an item's link" },
    { "reset",   rss_cmd_reset,   "rss reset <name> — mark every item read" },
    { "import",  rss_cmd_import,  "rss import <file.opml> — merge an OPML list" },
    { "export",  rss_cmd_export,  "rss export [file.opml] — write an OPML list" },
    { "set",     rss_cmd_set,     "rss set <key> <value> — runtime tunables" },
};

const shell_command_entry_t *rss_commands_get_table(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_rss_commands) / sizeof(s_rss_commands[0]);
    }
    return s_rss_commands;
}

int shell_command_rss(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_muted("rss list | add <name> <url> [user] [token] | remove <name> | "
                          "enable <name> <on|off> | interval <name> <minutes> | "
                          "refresh [name] | read <name> [n] | open <name> [n] | "
                          "reset <name> | import <file> | export [file] | set <key> <value>");
        return 2;
    }

    const shell_command_entry_t *table = rss_commands_get_table(NULL);
    size_t count = 0;

    rss_commands_get_table(&count);
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(argv[1], table[i].name) == 0) {
            int sub_argc = argc - 1;
            char **sub_argv = &argv[1];

            return table[i].handler(sub_argc, sub_argv);
        }
    }

    shell_print_error("rss: unknown sub-command '%s'", argv[1]);
    shell_print_muted("rss list | add | remove | enable | interval | refresh | read | "
                      "open | reset | import | export | set");
    return 2;
}

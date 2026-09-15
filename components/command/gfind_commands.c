/**
 * @file gfind_commands.c
 * @brief `gfind` - Global Find across the P4MiniShell "apps" (Palm-style).
 *
 * Palm's Find searched across the built-in apps (Address, Date Book, To Do,
 * Memo, etc.). The P4MiniShell equivalent is the structured stores: the
 * `db` record store (per-database records with keys, fields, and text
 * payloads) and the `alarm`/calendar store. `gfind` searches them all through
 * the EXISTING store APIs (db_list + db_find, alarm_list) - it contains no
 * parallel search logic and no filesystem code. It is a thin orchestrator only.
 *
 * Private-record policy: secret records are only surfaced/revealed when the
 * device is unlocked (`security_can_reveal_private()`); the conceal mode can
 * hide them entirely.
 *
 * Batch compatibility: ERRORLEVEL 0 (one or more matches), 1 (no matches),
 * 2 (usage / I/O). `/b` selects bare, machine-readable lines for `for /f`;
 * the default is coloured + labelled for humans. Options may appear anywhere.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "batch.h"
#include "db.h"
#include "security_commands.h"
#include "shell.h"

#define GFIND_POS_MAX       8
#define GFIND_DB_MAX        P4_CONFIG_DB_MAX_DATABASES
#define GFIND_ALARM_MAX     P4_CONFIG_ALARM_MAX_EVENTS

/* ------------------------------------------------------------------------
 * Options
 * ---------------------------------------------------------------------- */

typedef struct {
    bool bare;
    bool ignore_case;
    bool include_alarms;
    bool include_db;
    bool count_only;
    int  cat;                /* category filter; -1 = any */
    const char *db_filter;   /* restrict to a named database (NULL = all) */
    const char *field;       /* optional k=v field filter */
} gfind_opts_t;

static void gfind_opts_init(gfind_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->include_alarms = true;
    o->include_db = true;
    o->cat = -1;
}

/** Apply an option token; returns true when it was an option. */
static bool gfind_apply_option(const char *a, gfind_opts_t *o)
{
    const char *opt = a + 1;
    const char *colon = strchr(opt, ':');
    size_t nlen;
    const char *val;

    if (a[0] != '/' || a[1] == '\0') {
        return false;
    }
    if (colon != NULL) {
        nlen = (size_t)(colon - opt);
        val = colon + 1;
        if (nlen == 2 && strncasecmp(opt, "db", 2) == 0) {
            o->db_filter = val;
            return true;
        }
        if (nlen == 3 && strncasecmp(opt, "cat", 3) == 0) {
            o->cat = atoi(val);
            return true;
        }
        if (nlen == 5 && strncasecmp(opt, "field", 5) == 0) {
            o->field = val;
            return true;
        }
        return true;
    }
    if (strcasecmp(opt, "b") == 0) { o->bare = true; return true; }
    if (strcasecmp(opt, "i") == 0) { o->ignore_case = true; return true; }
    if (strcasecmp(opt, "count") == 0) { o->count_only = true; return true; }
    if (strcasecmp(opt, "noalarms") == 0) { o->include_alarms = false; return true; }
    if (strcasecmp(opt, "nodb") == 0) { o->include_db = false; return true; }
    return true;
}

static int gfind_collect(int argc, char **argv, int start, gfind_opts_t *o,
                         char **pos, int pos_cap)
{
    int pcount = 0;
    int i;

    for (i = start; i < argc; i++) {
        if (!gfind_apply_option(argv[i], o) && pcount < pos_cap) {
            pos[pcount++] = argv[i];
        }
    }
    return pcount;
}

/* ------------------------------------------------------------------------
 * Case-insensitive substring helper (no strcasestr dependency issues)
 * ---------------------------------------------------------------------- */

static bool gfind_contains(const char *hay, const char *needle, bool ignore_case)
{
    size_t hlen = strlen(hay);
    size_t nlen = strlen(needle);

    if (nlen == 0 || nlen > hlen) {
        return nlen == 0;
    }
    if (!ignore_case) {
        return strstr(hay, needle) != NULL;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

/** Case-insensitive k=v field match against a `name=value;...` payload. */
static bool gfind_field_match(const char *payload, const char *field, bool ignore_case)
{
    const char *eq = strchr(field, '=');
    char name[64];
    const char *want;
    char value[P4_CONFIG_DB_FIELD_VALUE_BYTES + 1];
    size_t nlen;

    if (eq == NULL) {
        return true;   /* bare field name: not a k=v filter, ignore */
    }
    nlen = (size_t)(eq - field);
    if (nlen >= sizeof(name)) {
        return false;
    }
    memcpy(name, field, nlen);
    name[nlen] = '\0';
    want = eq + 1;

    if (!db_field_get(payload, name, value, sizeof(value))) {
        return false;
    }
    if (ignore_case) {
        return strcasecmp(value, want) == 0;
    }
    return strcmp(value, want) == 0;
}

/* ------------------------------------------------------------------------
 * db-record search (reuses db_list + db_find)
 * ---------------------------------------------------------------------- */

static int gfind_db_matches;

/** Context threading the needle + options through db_list/db_find callbacks. */
typedef struct {
    gfind_opts_t *o;
    const char *needle;
    const char *dbname;
} gfind_db_ctx_t;

static bool gfind_db_dump_cb(const db_record_info_t *info, void *ctx)
{
    gfind_db_ctx_t *c = (gfind_db_ctx_t *)ctx;
    const char *dbname = c->dbname != NULL ? c->dbname
                                           : (c->o->db_filter != NULL ? c->o->db_filter : "");
    char *payload = NULL;
    bool reveal = security_can_reveal_private();

    /* Optional k=v field filter: fetch the payload and match a field. */
    if (c->o->field != NULL && strchr(c->o->field, '=') != NULL) {
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;

        payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        if (payload != NULL) {
            if (db_get(dbname, info->id, payload, &len, reveal, NULL) != ESP_OK) {
                free(payload);
                payload = NULL;
            }
        }
        if (payload == NULL) {
            return true;
        }
        if (!gfind_field_match(payload, c->o->field, c->o->ignore_case)) {
            free(payload);
            return true;
        }
        free(payload);
    }

    gfind_db_matches++;
    if (c->o->count_only) {
        return true;
    }
    if (c->o->bare) {
        shell_transcript_appendf("DB|%s|%lu|%s\n", dbname,
                                 (unsigned long)info->id, info->key);
    } else {
        shell_transcript_appendf_ansi("  " SH_EXE "DB" SH_RST " " SH_PATH "%s" SH_RST
                                      " #" SH_NUM "%lu" SH_RST " " SH_VAL "%s" SH_RST "\n",
                                      dbname, (unsigned long)info->id, info->key);
    }
    return true;
}

static bool gfind_db_cb(const char *dbname, void *ctx)
{
    gfind_db_ctx_t *c = (gfind_db_ctx_t *)ctx;
    int mcount = 0;
    gfind_db_ctx_t dump = { c->o, c->needle, dbname };

    /* db_find matches the payload via its text filter; passing the needle as
     * BOTH key_filter and text_filter makes a hit on either a hit. Secret
     * records are skipped unless the device is unlocked. */
    (void)db_find(dbname, (c->o->cat >= 0) ? (uint8_t)c->o->cat : 0xFF,
                  c->needle, c->needle, c->o->ignore_case,
                  security_can_reveal_private(), gfind_db_dump_cb, &dump, &mcount);
    return true;
}

static void gfind_search_db(const char *needle, gfind_opts_t *o)
{
    gfind_db_ctx_t ctx = { o, needle, NULL };

    gfind_db_matches = 0;
    if (o->db_filter != NULL && o->db_filter[0] != '\0') {
        int mcount = 0;
        gfind_db_ctx_t dump = { o, needle, o->db_filter };
        (void)db_find(o->db_filter, (o->cat >= 0) ? (uint8_t)o->cat : 0xFF,
                      needle, needle, o->ignore_case,
                      security_can_reveal_private(), gfind_db_dump_cb, &dump, &mcount);
    } else {
        db_list(gfind_db_cb, &ctx);
    }
}

/* ------------------------------------------------------------------------
 * alarm/calendar search (reuses alarm_list)
 * ---------------------------------------------------------------------- */

static int gfind_alarm_matches;

static bool gfind_alarm_dump_cb(const alarm_event_t *e, void *ctx)
{
    gfind_db_ctx_t *c = (gfind_db_ctx_t *)ctx;   /* needle + opts */
    char when_s[24];
    struct tm tmv;

    if (!gfind_contains(e->title, c->needle, c->o->ignore_case) &&
        !gfind_contains(e->msg, c->needle, c->o->ignore_case)) {
        return true;
    }
    gfind_alarm_matches++;
    if (c->o->count_only) {
        return true;
    }
    if (localtime_r(&e->when, &tmv) == NULL ||
        strftime(when_s, sizeof(when_s), "%Y-%m-%d %H:%M", &tmv) == 0) {
        snprintf(when_s, sizeof(when_s), "?");
    }
    if (c->o->bare) {
        shell_transcript_appendf("ALARM|%lu|%s|%s\n",
                                 (unsigned long)e->id, when_s, e->title);
    } else {
        shell_transcript_appendf_ansi("  " SH_EXE "ALARM" SH_RST " #" SH_NUM "%lu" SH_RST
                                      " " SH_VAL "%s" SH_RST " " SH_TEXT "%s" SH_RST "\n",
                                      (unsigned long)e->id, when_s, e->title);
    }
    return true;
}

static void gfind_search_alarms(const char *needle, gfind_opts_t *o)
{
    gfind_db_ctx_t ctx = { o, needle, NULL };

    gfind_alarm_matches = 0;
    alarm_list(gfind_alarm_dump_cb, &ctx);
}

/* ------------------------------------------------------------------------
 * Dispatcher
 * ---------------------------------------------------------------------- */

void shell_command_gfind(int argc, char **argv)
{
    gfind_opts_t opts;
    char *pos[GFIND_POS_MAX];
    int pcount;
    const char *needle = NULL;
    int total;

    if (argc < 2) {
        shell_print_usage("Usage: gfind <text> [/b] [/i] [/count] [/cat:N] [/field:k=v] [/db:name] [/noalarms] [/nodb]");
        batch_set_errorlevel(2);
        return;
    }
    gfind_opts_init(&opts);
    pcount = gfind_collect(argc, argv, 1, &opts, pos, GFIND_POS_MAX);
    if (pcount < 1) {
        shell_print_usage("Usage: gfind <text> [/b] [/i] [/count] [/cat:N] [/field:k=v] [/db:name] [/noalarms] [/nodb]");
        batch_set_errorlevel(2);
        return;
    }
    needle = pos[0];
    if (pcount > 1) {
        shell_print_error("gfind: too many arguments (quote the search text)");
        batch_set_errorlevel(2);
        return;
    }

    if (opts.include_db) {
        gfind_search_db(needle, &opts);
    }
    if (opts.include_alarms) {
        gfind_search_alarms(needle, &opts);
    }
    total = gfind_db_matches + gfind_alarm_matches;

    if (opts.count_only) {
        shell_transcript_appendf("gfind.db=%d\n", gfind_db_matches);
        shell_transcript_appendf("gfind.alarms=%d\n", gfind_alarm_matches);
        shell_transcript_appendf("gfind.total=%d\n", total);
    } else if (!opts.bare) {
        shell_print_field_num("gfind.matches", total);
    }
    batch_set_errorlevel(total > 0 ? 0 : 1);
}

/**
 * @file db_commands.c
 * @brief The `db` command: a Palm-OS-style SD-backed record store surface.
 *
 * Everything here is a thin argv-verb dispatcher over the `db` core
 * (components/db/db.c). It owns printing and ERRORLEVEL:
 *   0 = ok / found
 *   1 = not-found / empty result
 *   2 = usage / I/O error
 * `/b` selects bare (uncoloured, pipe/for-friendly) output. Options can
 * appear anywhere on the line; the remaining non-option tokens are positional.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "storage.h"
#include "shell.h"
#include "batch.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "command.h"
#include "security_commands.h"

#define SHELL_SD_PATH_BYTES     P4_CONFIG_SD_PATH_BYTES
#define SHELL_COMMAND_BYTES     P4_CONFIG_COMMAND_BYTES
#define DB_POS_MAX              8

/* ------------------------------------------------------------------------
 * Option scanning
 * ---------------------------------------------------------------------- */

typedef struct {
    bool bare;
    bool secret;
    bool reveal;
    bool permanent;
    bool ignore_case;
    int cat;
    const char *key;
    const char *text;
    const char *creator;
    const char *type;
    uint32_t version;
    const char *field;
    const char *sort;
} db_opts_t;

static void db_opts_init(db_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->cat = -1;
    o->version = 1;
    o->creator = "P4SH";
    o->type = "DATA";
}

/** Apply an option token to @p o; returns true when it was an option. */
static bool db_apply_option(const char *a, db_opts_t *o)
{
    const char *opt;
    const char *colon;
    size_t nlen;
    const char *val;

    if (a[0] != '/' || a[1] == '\0') {
        return false;
    }
    opt = a + 1;
    colon = strchr(opt, ':');
    if (colon != NULL) {
        nlen = (size_t)(colon - opt);
        val = colon + 1;
        if (nlen == 3 && strncasecmp(opt, "cat", 3) == 0) { o->cat = atoi(val); return true; }
        if (nlen == 3 && strncasecmp(opt, "key", 3) == 0) { o->key = val; return true; }
        if (nlen == 4 && strncasecmp(opt, "text", 4) == 0) { o->text = val; return true; }
        if (nlen == 5 && strncasecmp(opt, "field", 5) == 0) { o->field = val; return true; }
        if (nlen == 4 && strncasecmp(opt, "sort", 4) == 0) { o->sort = val; return true; }
        if (nlen == 2 && strncasecmp(opt, "cr", 2) == 0) { o->creator = val; return true; }
        if (nlen == 2 && strncasecmp(opt, "tp", 2) == 0) { o->type = val; return true; }
        if (nlen == 2 && strncasecmp(opt, "vr", 2) == 0) { o->version = (uint32_t)strtoul(val, NULL, 10); return true; }
        return true;
    }
    if (strcasecmp(opt, "b") == 0) { o->bare = true; return true; }
    if (strcasecmp(opt, "i") == 0) { o->ignore_case = true; return true; }
    if (strcasecmp(opt, "secret") == 0) { o->secret = true; return true; }
    if (strcasecmp(opt, "reveal") == 0) { o->reveal = true; return true; }
    if (strcasecmp(opt, "p") == 0) { o->permanent = true; return true; }
    return true;   /* unknown /flag ignored */
}

/** Scan argv[2..] into options + a positional-token array. */
static int db_collect(int argc, char **argv, db_opts_t *o, char **pos, int pos_cap)
{
    int pcount = 0;
    int i;

    for (i = 2; i < argc; i++) {
        if (!db_apply_option(argv[i], o) && pcount < pos_cap) {
            pos[pcount++] = argv[i];
        }
    }
    return pcount;
}

/** Join positional tokens [from..pcount) into a heap payload string. */
static char *db_join_payload(char **pos, int from, int pcount)
{
    size_t cap = SHELL_COMMAND_BYTES;
    size_t used = 0;
    char *buf;
    int i;

    buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    buf[0] = '\0';
    for (i = from; i < pcount; i++) {
        size_t len = strlen(pos[i]);

        if (used > 0) {
            if (used + 1 >= cap) break;
            buf[used++] = ' ';
        }
        if (used + len >= cap) len = cap - used - 1;
        memcpy(buf + used, pos[i], len);
        used += len;
        buf[used] = '\0';
    }
    return buf;
}

/* ------------------------------------------------------------------------
 * Printers
 * ---------------------------------------------------------------------- */

static void db_print_line_bare(const char *fmt, ...)
{
    char buf[P4_CONFIG_DB_INDEX_LINE_BYTES * 2 + 64];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    shell_transcript_appendf("%s\n", buf);
}

static bool db_name_ok(const char *name)
{
    if (!db_name_valid(name)) {
        shell_print_usage("db: invalid database name %s", name);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Verbatim handlers (each returns ERRORLEVEL)
 * ---------------------------------------------------------------------- */

static bool db_list_dump_cb(const char *name, void *ctx);
static bool db_cat_dump_cb(uint8_t cat, const char *label, void *ctx);
static bool db_find_dump_cb(const db_record_info_t *info, void *ctx);
static bool db_resolve_db(char **pos, int pcount, int *idx, char *out, size_t out_size);

static int db_cmd_create(char **pos, int pcount, db_opts_t *o)
{
    esp_err_t error;

    if (pcount < 1) {
        shell_print_usage("Usage: db create <name> [/cr:XXXX] [/tp:XXXX] [/vr:N]");
        return 2;
    }
    if (!db_name_ok(pos[0])) {
        return 2;
    }
    error = db_create(pos[0], o->creator, o->type, o->version);
    if (error == ESP_ERR_INVALID_STATE) {
        shell_print_error("db: %s already exists", pos[0]);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: could not create %s (%s)", pos[0], esp_err_to_name(error));
        return 2;
    }
    shell_print_ok("db: created %s (creator=%s type=%s ver=%lu)", pos[0], o->creator,
                   o->type, (unsigned long)o->version);
    return 0;
}

static int db_cmd_list(char **pos, int pcount, db_opts_t *o)
{
    db_info_t info;
    esp_err_t error;
    int n = 0;

    if (pcount >= 1) {
        if (!db_name_ok(pos[0])) {
            return 2;
        }
        error = db_info(pos[0], &info);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("db: %s not found", pos[0]);
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("db: could not read %s (%s)", pos[0], esp_err_to_name(error));
            return 2;
        }
        if (o->bare) {
            db_print_line_bare("%s|%s|%s|%lu|%lu|%lu", info.name, info.creator, info.type,
                               (unsigned long)info.version, (unsigned long)info.record_count,
                               (unsigned long)info.deleted_count);
        } else {
            shell_print_field("db.name", "%s", info.name);
            shell_print_field("db.creator", "%s", info.creator);
            shell_print_field("db.type", "%s", info.type);
            shell_print_field_num("db.version", (long)info.version);
            shell_print_field_num("db.records", (long)info.record_count);
            shell_print_field_num("db.deleted", (long)info.deleted_count);
            shell_print_field_num("db.next_id", (long)info.next_id);
            shell_print_field("db.modified", "%lus", (unsigned long)info.modified_sec);
        }
        return 0;
    }

    error = db_list(db_list_dump_cb, &n);
    if (error != ESP_OK) {
        shell_print_error("db: could not list databases (%s)", esp_err_to_name(error));
        return 2;
    }
    shell_print_field_num("db.databases", n);
    return 0;
}

static bool db_list_dump_cb(const char *name, void *ctx)
{
    int *n = (int *)ctx;

    shell_transcript_appendf_ansi("  " SH_PATH "%s" SH_RST "\n", name);
    if (n != NULL) {
        (*n)++;
    }
    return true;
}

static int db_cmd_drop(char **pos, int pcount, db_opts_t *o)
{
    esp_err_t error;

    (void)o;
    if (pcount < 1) {
        shell_print_usage("Usage: db drop <name>");
        return 2;
    }
    if (!db_name_ok(pos[0])) {
        return 2;
    }
    error = db_drop(pos[0]);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("db: %s not found", pos[0]);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: could not drop %s (%s)", pos[0], esp_err_to_name(error));
        return 2;
    }
    shell_print_ok("db: dropped %s", pos[0]);
    return 0;
}

static int db_cmd_open(char **pos, int pcount, db_opts_t *o)
{
    char cur[P4_CONFIG_DB_NAME_BYTES];
    esp_err_t error;

    (void)o;
    if (pcount >= 1) {
        if (!db_name_ok(pos[0])) {
            return 2;
        }
        error = db_current_set(pos[0]);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("db: %s not found", pos[0]);
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("db: could not open %s (%s)", pos[0], esp_err_to_name(error));
            return 2;
        }
        shell_print_ok("db: current = %s", pos[0]);
        return 0;
    }
    if (db_current_get(cur, sizeof(cur))) {
        shell_print_ok("db: current = %s", cur);
    } else {
        shell_print_muted("db: no database open");
    }
    return 0;
}

static int db_cmd_close(char **pos, int pcount, db_opts_t *o)
{
    (void)pos;
    (void)pcount;
    (void)o;
    db_current_set(NULL);
    shell_print_ok("db: closed");
    return 0;
}

static int db_cmd_categories(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    esp_err_t error;
    int idx = 0;

    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    if (idx >= pcount) {
        shell_print_usage("Usage: db categories <name> list|set <n> <label>|clear <n>");
        return 2;
    }
    if (shell_text_equals_ignore_case(pos[idx], "list")) {
        struct { int n; } ctx = {0};
        error = db_categories_foreach(name, (bool (*)(uint8_t, const char *, void *))db_cat_dump_cb, &ctx);
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
            shell_print_error("db: could not list categories (%s)", esp_err_to_name(error));
            return 2;
        }
        return 0;
    }
    if (shell_text_equals_ignore_case(pos[idx], "set")) {
        uint8_t cat;
        if (idx + 2 >= pcount) {
            shell_print_usage("Usage: db categories <name> set <n> <label>");
            return 2;
        }
        cat = (uint8_t)atoi(pos[idx + 1]);
        if (cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
            shell_print_error("db: category must be 0..%d", P4_CONFIG_DB_CATEGORY_COUNT - 1);
            return 2;
        }
        error = db_category_set(name, cat, pos[idx + 2]);
        if (error != ESP_OK) {
            shell_print_error("db: could not set category (%s)", esp_err_to_name(error));
            return 2;
        }
        shell_print_ok("db: category %u = %s", (unsigned)cat, pos[idx + 2]);
        return 0;
    }
    if (shell_text_equals_ignore_case(pos[idx], "clear")) {
        uint8_t cat;
        if (idx + 1 >= pcount) {
            shell_print_usage("Usage: db categories <name> clear <n>");
            return 2;
        }
        cat = (uint8_t)atoi(pos[idx + 1]);
        if (cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
            shell_print_error("db: category must be 0..%d", P4_CONFIG_DB_CATEGORY_COUNT - 1);
            return 2;
        }
        error = db_category_set(name, cat, "");
        if (error != ESP_OK) {
            shell_print_error("db: could not clear category (%s)", esp_err_to_name(error));
            return 2;
        }
        shell_print_ok("db: cleared category %u", (unsigned)cat);
        return 0;
    }
    shell_print_usage("Usage: db categories <name> list|set <n> <label>|clear <n>");
    return 2;
}

static bool db_cat_dump_cb(uint8_t cat, const char *label, void *ctx)
{
    int *n = (int *)ctx;

    shell_transcript_appendf_ansi("  " SH_NUM "%u" SH_RST ". " SH_VAL "%s" SH_RST "\n",
                                  (unsigned)cat, label);
    if (n != NULL) {
        (*n)++;
    }
    return true;
}

static int db_cmd_add(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    char resolved[SHELL_SD_PATH_BYTES];
    char *payload_buf = NULL;
    size_t payload_len = 0;
    uint32_t new_id = 0;
    esp_err_t error;
    int idx = 0;

    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    /* Remaining positional tokens after the name form the payload. If the
     * first payload token resolves to an existing file, use its contents. */
    if (idx < pcount && strchr(pos[idx], '*') == NULL && strchr(pos[idx], '?') == NULL) {
        if (shell_fs_resolve_path(pos[idx], resolved, sizeof(resolved)) == ESP_OK) {
            shell_sd_session_t session;
            FILE *f = NULL;

            if (shell_sd_begin(&session) == ESP_OK) {
                f = fopen(resolved, "rb");
                if (f != NULL) {
                    long size;
                    fseek(f, 0, SEEK_END);
                    size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    if (size >= 0 && (size_t)size <= P4_CONFIG_DB_RECORD_MAX_BYTES) {
                        payload_buf = malloc((size_t)size + 1);
                        if (payload_buf != NULL) {
                            if (fread(payload_buf, 1, (size_t)size, f) == (size_t)size) {
                                payload_len = (size_t)size;
                                payload_buf[size] = '\0';
                            } else {
                                free(payload_buf);
                                payload_buf = NULL;
                            }
                        }
                    }
                    fclose(f);
                }
                shell_sd_end(&session, "db add");
            }
        }
    }

    if (payload_buf == NULL) {
        payload_buf = db_join_payload(pos, idx, pcount);
        if (payload_buf == NULL) {
            shell_print_error("db: out of memory");
            return 2;
        }
        payload_len = strlen(payload_buf);
    }

    error = db_add(name, (uint8_t)(o->cat >= 0 ? o->cat : 0),
                   o->key, o->secret, payload_buf, payload_len, &new_id);
    free(payload_buf);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("db: %s not found", name);
        return 1;
    }
    if (error == ESP_ERR_NO_MEM) {
        shell_print_error("db: %s is full", name);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: could not add record (%s)", esp_err_to_name(error));
        return 2;
    }
    if (o->bare) {
        db_print_line_bare("%lu", (unsigned long)new_id);
    } else {
        shell_print_field_num("db.added", (long)new_id);
    }
    return 0;
}

static int db_cmd_get(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    uint32_t id;
    db_record_info_t info;
    char *payload;
    size_t len;
    esp_err_t error;
    int idx = 0;

    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    if (idx >= pcount) {
        shell_print_usage("Usage: db get [<name>] <id> [/b] [/reveal] [/field:name]");
        return 2;
    }
    id = (uint32_t)strtoul(pos[idx], NULL, 10);

    /* Record-sized buffer: heap-allocate (this runs on the batch path). */
    payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (payload == NULL) {
        shell_print_error("db: out of memory");
        return 2;
    }
    len = P4_CONFIG_DB_RECORD_MAX_BYTES;

    error = db_get(name, id, payload, &len, o->reveal, &info);
    if (error == ESP_ERR_NOT_FOUND) {
        free(payload);
        shell_print_error("db: record %lu not found in %s", (unsigned long)id, name);
        return 1;
    }
    if (error != ESP_OK) {
        free(payload);
        shell_print_error("db: could not read record (%s)", esp_err_to_name(error));
        return 2;
    }

    /* Single-field extraction for batch pipelines (`for /f`, `set /p`). */
    if (o->field != NULL && o->field[0] != '\0' && strchr(o->field, '=') == NULL) {
        char value[P4_CONFIG_DB_FIELD_VALUE_BYTES + 1];
        if ((info.flags & DB_FLAG_SECRET) && !o->reveal) {
            free(payload);
            shell_print_error("db: record %lu is secret (use /reveal)", (unsigned long)id);
            return 1;
        }
        if (!db_field_get(payload, o->field, value, sizeof(value))) {
            free(payload);
            if (!o->bare) {
                shell_print_error("db: record %lu has no field '%s'", (unsigned long)id, o->field);
            }
            return 1;
        }
        if (o->bare) {
            db_print_line_bare("%s", value);
        } else {
            shell_print_field(o->field, "%s", value);
        }
        free(payload);
        return 0;
    }

    if (o->bare) {
        db_print_line_bare("%lu|%u|%u|%s|%s", (unsigned long)info.id, (unsigned)info.category,
                           (unsigned)info.flags, info.key, payload);
        free(payload);
        return 0;
    }
    shell_print_field_num("db.id", (long)info.id);
    shell_print_field("db.key", "%s", info.key);
    if (info.flags & DB_FLAG_SECRET) {
        shell_print_field("db.payload", "%zu bytes (secret)", (size_t)info.size);
        if (!o->reveal) {
            free(payload);
            shell_print_muted("db: use /reveal to show it");
            return 0;
        }
    }
    if (len > 0) {
        shell_transcript_appendf_ansi(SH_LBL "db.payload:" SH_RST " %s\n", payload);
    } else {
        shell_print_field("db.payload", "(empty)");
    }
    free(payload);
    return 0;
}

static int db_cmd_set(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    uint32_t id;
    char *payload_buf;
    size_t payload_len;
    esp_err_t error;
    int idx = 0;

    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    if (idx >= pcount) {
        shell_print_usage("Usage: db set [<name>] <id> [/cat:N] [/key:K] [text...]");
        return 2;
    }
    id = (uint32_t)strtoul(pos[idx], NULL, 10);
    idx++;
    payload_buf = db_join_payload(pos, idx, pcount);
    if (payload_buf == NULL) {
        shell_print_error("db: out of memory");
        return 2;
    }
    payload_len = strlen(payload_buf);

    error = db_set(name, id, (uint8_t)(o->cat >= 0 ? o->cat : 0),
                   o->key, o->secret, payload_buf, payload_len);
    free(payload_buf);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("db: record %lu not found in %s", (unsigned long)id, name);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: could not update record (%s)", esp_err_to_name(error));
        return 2;
    }
    shell_print_ok("db: updated %lu", (unsigned long)id);
    return 0;
}

static int db_cmd_del(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    uint32_t id;
    esp_err_t error;
    int idx = 0;

    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    if (idx >= pcount) {
        shell_print_usage("Usage: db del [<name>] <id> [/p]");
        return 2;
    }
    id = (uint32_t)strtoul(pos[idx], NULL, 10);
    error = db_del(name, id, o->permanent);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("db: record %lu not found in %s", (unsigned long)id, name);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: could not delete record (%s)", esp_err_to_name(error));
        return 2;
    }
    shell_print_ok("db: %s %lu in %s", o->permanent ? "permanently removed" : "soft-deleted",
                   (unsigned long)id, name);
    return 0;
}

static int db_cmd_purge(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    esp_err_t error;
    int idx = 0;

    (void)o;
    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    error = db_purge(name);
    if (error != ESP_OK) {
        shell_print_error("db: could not purge %s (%s)", name, esp_err_to_name(error));
        return 2;
    }
    shell_print_ok("db: purged %s", name);
    return 0;
}

static int db_cmd_count(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    int count = 0;
    esp_err_t error;
    int idx = 0;

    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    error = db_count(name, (uint8_t)(o->cat >= 0 ? o->cat : 0xFF), &count);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("db: %s not found", name);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: could not count (%s)", esp_err_to_name(error));
        return 2;
    }
    if (o->bare) {
        db_print_line_bare("%d", count);
    } else {
        shell_print_field_num("db.count", count);
    }
    return 0;
}

typedef struct { bool bare; } db_find_ctx_t;

/** Collected candidate for field filtering / sorting (heap array). */
typedef struct {
    db_record_info_t info;
    char sortkey[P4_CONFIG_DB_FIELD_VALUE_BYTES + 1];
} db_candidate_t;

static bool db_collect_cb(const db_record_info_t *info, void *ctx)
{
    db_candidate_t *list = ((db_candidate_t **)ctx)[0];
    int *count = (int *)(((void **)ctx)[1]);

    if (*count >= P4_CONFIG_DB_FIND_MAX) {
        return false;
    }
    list[*count].info = *info;
    list[*count].sortkey[0] = '\0';
    (*count)++;
    return true;
}

/** Split a `/field:k=v` filter into heap name/value (value may be empty).
 *  @return true on a well-formed filter (both out-params set). */
static bool db_split_field_filter(const char *filter, char *name_out, size_t name_size,
                                  char *value_out, size_t value_size)
{
    const char *eq;
    size_t name_len;

    if (filter == NULL || name_out == NULL || value_out == NULL ||
        name_size == 0 || value_size == 0) {
        return false;
    }
    eq = strchr(filter, '=');
    if (eq == NULL || eq == filter) {
        return false;
    }
    name_len = (size_t)(eq - filter);
    if (name_len >= name_size) {
        name_len = name_size - 1;
    }
    memcpy(name_out, filter, name_len);
    name_out[name_len] = '\0';
    snprintf(value_out, value_size, "%s", eq + 1);
    return true;
}

/** True when @p actual equals @p wanted (case per @p ignore_case). */
static bool db_field_value_match(const char *actual, const char *wanted, bool ignore_case)
{
    if (wanted == NULL || wanted[0] == '\0') {
        return actual[0] == '\0';
    }
    if (ignore_case) {
        return strcasecmp(actual, wanted) == 0;
    }
    return strcmp(actual, wanted) == 0;
}

static int db_candidate_compare(const db_candidate_t *a, const db_candidate_t *b,
                                bool ignore_case)
{
    if (ignore_case) {
        return strcasecmp(a->sortkey, b->sortkey);
    }
    return strcmp(a->sortkey, b->sortkey);
}

/** Insertion sort over the candidate list (n is tiny; keeps the comparator
 *  context explicit instead of sharing qsort-global state across workers). */
static void db_candidate_sort(db_candidate_t *list, int n, bool ignore_case)
{
    int i;
    for (i = 1; i < n; i++) {
        db_candidate_t tmp = list[i];
        int j = i - 1;
        while (j >= 0 && db_candidate_compare(&tmp, &list[j], ignore_case) < 0) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = tmp;
    }
}

static int db_cmd_find(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    int count = 0;
    db_find_ctx_t ctx;
    esp_err_t error;
    int idx = 0;

    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    /* Fast path (unchanged streaming behaviour) when no field filter or
     * explicit sort is requested. */
    if ((o->field == NULL || o->field[0] == '\0') &&
        (o->sort == NULL || o->sort[0] == '\0')) {
        ctx.bare = o->bare;
        error = db_find(name, (uint8_t)(o->cat >= 0 ? o->cat : 0xFF),
                        o->key, o->text, o->ignore_case, o->reveal,
                        (db_find_cb_t)db_find_dump_cb, &ctx, &count);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("db: %s not found", name);
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("db: find failed (%s)", esp_err_to_name(error));
            return 2;
        }
        if (!o->bare) {
            shell_print_field_num("db.matches", count);
        }
        return 0;
    }
    /* Collect path: gather candidates, filter on a payload field, sort. */
    {
        db_candidate_t *list;
        void *cb_ctx[2];
        int collected = 0;
        int kept = 0;
        int i;
        char field_name[P4_CONFIG_DB_KEY_BYTES];
        char field_want[P4_CONFIG_DB_FIELD_VALUE_BYTES + 1];
        bool use_field = (o->field != NULL && o->field[0] != '\0');
        bool sort_by_id = false;
        bool sort_by_key = false;
        const char *sort_field = NULL;

        if (o->sort != NULL && o->sort[0] != '\0') {
            if (strcasecmp(o->sort, "id") == 0) {
                sort_by_id = true;
            } else if (strcasecmp(o->sort, "key") == 0) {
                sort_by_key = true;
            } else {
                sort_field = o->sort;
            }
        }
        if (use_field && !db_split_field_filter(o->field, field_name, sizeof(field_name),
                                                field_want, sizeof(field_want))) {
            shell_print_usage("Usage: db find [<name>] [/field:k=v] [/sort:key|id|field] [...]");
            return 2;
        }
        list = malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_candidate_t));
        if (list == NULL) {
            shell_print_error("db: out of memory");
            return 2;
        }
        cb_ctx[0] = list;
        cb_ctx[1] = &collected;
        ctx.bare = o->bare;
        error = db_find(name, (uint8_t)(o->cat >= 0 ? o->cat : 0xFF),
                        o->key, o->text, o->ignore_case, o->reveal,
                        (db_find_cb_t)db_collect_cb, cb_ctx, &count);
        if (error == ESP_ERR_NOT_FOUND) {
            free(list);
            shell_print_error("db: %s not found", name);
            return 1;
        }
        if (error != ESP_OK) {
            free(list);
            shell_print_error("db: find failed (%s)", esp_err_to_name(error));
            return 2;
        }
        for (i = 0; i < collected; i++) {
            char *payload = NULL;
            size_t len = 0;
            db_record_info_t info;

            if (use_field || sort_field != NULL) {
                payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
                if (payload == NULL) {
                    free(list);
                    shell_print_error("db: out of memory");
                    return 2;
                }
                len = P4_CONFIG_DB_RECORD_MAX_BYTES;
                error = db_get(name, list[i].info.id, payload, &len, o->reveal, &info);
                if (error != ESP_OK) {
                    free(payload);
                    continue;
                }
                payload[len] = '\0';
            }
            if (use_field) {
                char actual[P4_CONFIG_DB_FIELD_VALUE_BYTES + 1];
                bool present = db_field_get(payload != NULL ? payload : "",
                                            field_name, actual, sizeof(actual));
                if (!present || !db_field_value_match(actual, field_want, o->ignore_case)) {
                    free(payload);
                    continue;
                }
            }
            if (sort_by_id) {
                snprintf(list[kept].sortkey, sizeof(list[kept].sortkey), "%010lu",
                         (unsigned long)list[i].info.id);
            } else if (sort_by_key) {
                snprintf(list[kept].sortkey, sizeof(list[kept].sortkey), "%s",
                         list[i].info.key);
            } else if (sort_field != NULL && payload != NULL) {
                if (!db_field_get(payload, sort_field, list[kept].sortkey,
                                  sizeof(list[kept].sortkey))) {
                    list[kept].sortkey[0] = '\0';
                }
            }
            free(payload);
            if (kept != i) {
                list[kept].info = list[i].info;
            }
            kept++;
        }
        if (o->sort != NULL && o->sort[0] != '\0') {
            db_candidate_sort(list, kept, o->ignore_case);
        }
        for (i = 0; i < kept; i++) {
            db_find_dump_cb(&list[i].info, &ctx);
        }
        if (!o->bare) {
            shell_print_field_num("db.matches", kept);
        }
        free(list);
        return 0;
    }
}

static bool db_find_dump_cb(const db_record_info_t *info, void *ctx)
{
    db_find_ctx_t *c = (db_find_ctx_t *)ctx;

    if (c->bare) {
        db_print_line_bare("%lu|%u|%s", (unsigned long)info->id, (unsigned)info->category,
                           info->key);
    } else {
        shell_transcript_appendf_ansi("  " SH_NUM "%lu" SH_RST " cat=" SH_NUM "%u" SH_RST
                                      " key=" SH_VAL "%s" SH_RST " size=" SH_NUM "%lu" SH_RST "\n",
                                      (unsigned long)info->id, (unsigned)info->category,
                                      info->key, (unsigned long)info->size);
    }
    return true;
}

static int db_cmd_export(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    const char *file = NULL;
    esp_err_t error;
    int idx = 0;

    (void)o;
    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    if (idx < pcount) {
        file = pos[idx];
    }
    error = db_export(name, file);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("db: %s not found", name);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: export failed (%s)", esp_err_to_name(error));
        return 2;
    }
    shell_print_ok("db: exported %s", name);
    return 0;
}

static int db_cmd_import(char **pos, int pcount, db_opts_t *o)
{
    char name[P4_CONFIG_DB_NAME_BYTES];
    const char *file = NULL;
    esp_err_t error;
    int idx = 0;

    (void)o;
    if (!db_resolve_db(pos, pcount, &idx, name, sizeof(name))) {
        return 2;
    }
    if (idx < pcount) {
        file = pos[idx];
    }
    error = db_import(name, file);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("db: %s not found (or no import file)", name);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("db: import failed (%s)", esp_err_to_name(error));
        return 2;
    }
    shell_print_ok("db: imported into %s", name);
    return 0;
}

/* ------------------------------------------------------------------------
 * Dispatcher
 * ---------------------------------------------------------------------- */

static int db_cmd_current(char **pos, int pcount, db_opts_t *o);

/** Resolve the database name: explicit positional, or the current database.
 *  When a current database is set and the next positional token is all-numeric,
 *  that token is a record id, so the current database is used instead. */
static bool db_resolve_db(char **pos, int pcount, int *idx, char *out, size_t out_size)
{
    char cur[P4_CONFIG_DB_NAME_BYTES];

    if (*idx < pcount) {
        const char *name = pos[*idx];

        /* A pure number as the first positional is a record id, not a name;
         * use the current database (so `db get 5` works after `db open x`). */
        if (db_current_get(cur, sizeof(cur))) {
            const char *p = name;
            bool numeric = (p[0] != '\0');
            for (; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') {
                    numeric = false;
                    break;
                }
            }
            if (numeric) {
                snprintf(out, out_size, "%s", cur);
                return true;
            }
        }

        if (!db_name_valid(name)) {
            shell_print_usage("db: invalid database name %s", name);
            return false;
        }
        snprintf(out, out_size, "%s", name);
        (*idx)++;
        return true;
    }
    if (db_current_get(out, out_size)) {
        return true;
    }
    shell_print_error("db: no database given and none is open (use db open <name>)");
    return false;
}

void shell_command_db(int argc, char **argv)
{
    const char *verb;
    db_opts_t opts;
    char *pos[DB_POS_MAX];
    int pcount;
    int result;

    if (argc < 2) {
        shell_print_usage("Usage: db <create|list|info|drop|open|close|categories|add|get|set|del|purge|count|find|export|import> [...]");
        batch_set_errorlevel(2);
        return;
    }
    verb = argv[1];

    db_opts_init(&opts);
    pcount = db_collect(argc, argv, &opts, pos, DB_POS_MAX);

    /* Private-record policy: when the device is locked, /reveal is refused so
     * secret payloads stay hidden. Default (no passcode) is unchanged. */
    if (opts.reveal && !security_can_reveal_private()) {
        opts.reveal = false;
        shell_print_warning("db: device locked - secret payloads stay hidden");
    }

    if (shell_text_equals_ignore_case(verb, "create")) {
        result = db_cmd_create(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "list") ||
               shell_text_equals_ignore_case(verb, "info")) {
        result = db_cmd_list(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "drop") ||
               shell_text_equals_ignore_case(verb, "delete")) {
        result = db_cmd_drop(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "open")) {
        result = db_cmd_open(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "close")) {
        result = db_cmd_close(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "current")) {
        result = db_cmd_current(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "categories") ||
               shell_text_equals_ignore_case(verb, "cat")) {
        result = db_cmd_categories(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "add")) {
        result = db_cmd_add(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "get")) {
        result = db_cmd_get(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "set")) {
        result = db_cmd_set(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "del") ||
               shell_text_equals_ignore_case(verb, "remove")) {
        result = db_cmd_del(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "purge")) {
        result = db_cmd_purge(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "count")) {
        result = db_cmd_count(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "find")) {
        result = db_cmd_find(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "export")) {
        result = db_cmd_export(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "import")) {
        result = db_cmd_import(pos, pcount, &opts);
    } else {
        shell_print_error("db: unknown verb %s", verb);
        shell_print_usage("Usage: db <create|list|info|drop|open|close|categories|add|get|set|del|purge|count|find|export|import> [...]");
        result = 2;
    }

    batch_set_errorlevel(result);
}

static int db_cmd_current(char **pos, int pcount, db_opts_t *o)
{
    char cur[P4_CONFIG_DB_NAME_BYTES];

    (void)pos;
    (void)pcount;
    (void)o;
    if (db_current_get(cur, sizeof(cur))) {
        shell_print_ok("db: current = %s", cur);
    } else {
        shell_print_muted("db: no database open");
    }
    return 0;
}

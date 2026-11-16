/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file rss_store.c
 * @brief Persistent subscription store, read/unread state and feed poller.
 *
 * See rss_store.h for the format and the safety contract. Everything here is
 * reached through the guarded SD session; a missing card or file degrades to an
 * empty store instead of failing the caller.
 */

#include "rss_store.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "clock.h"
#include "storage.h"

#define RSS_STORE_TAG                 P4_CONFIG_RSS_LOG_TAG

/** Record field count of a subscription line. */
#define RSS_RECORD_FIELDS             11
/** Separator between record fields. */
#define RSS_RECORD_SEP                '|'

/** FNV-1a 64-bit, used to fold an item guid into a bounded read-state key. */
#define RSS_HASH_OFFSET_BASIS         0xCBF29CE484222325ULL
#define RSS_HASH_PRIME                0x100000001B3ULL

/** Poller cadence: how often the task wakes to look for due feeds. */
#define RSS_POLLER_TICK_MS            5000
/** Delay between feeds, so one slow refresh cannot starve the UI. */
#define RSS_POLLER_FEED_DELAY_MS      P4_CONFIG_RSS_POLL_OVERLAP_MS

static const char RSS_RECORD_HEADER[] =
    "; P4MiniShell RSS subscriptions\n"
    "; format: NAME|URL|TYPE|USER|TOKEN|ETAG|MODIFIED|FETCHED|ENABLED|INTERVAL|TITLE\n"
    "; TYPE: 0=none 1=basic 2=bearer. FETCHED is unix seconds (0 = never).\n"
    "; Fields must not contain '|'. Tokens are persisted but never printed.\n";

static const char RSS_READ_HEADER[] =
    "; P4MiniShell RSS read/unread state (NAME|HASH of item guid)\n";

/* ========================================================================
 * STATE
 * ======================================================================== */

/* Copy of the host callback table, not a pointer: main() passes the address of
 * a local, so keeping the pointer would leave a dangling frame behind. */
static networking_host_ops_t s_ops;
static SemaphoreHandle_t s_lock;

/* The subscription mirror lives in PSRAM: at P4_CONFIG_RSS_SUB_CAPACITY x
 * ~1.4 KB per record it is ~45 KB, which would exhaust the P4's internal DRAM
 * (the linker fails on .sbss placement long before runtime). */
static rss_subscription_t *s_subs;
static int s_count;

/* Read state, PSRAM: whole guids (up to 256 bytes each) would cost ~50 KB of
 * scarce internal DRAM, so only a 64-bit hash is kept. */
static uint64_t *s_read_hash;
static char (*s_read_name)[P4_CONFIG_RSS_SUB_NAME_BYTES];
static int s_read_count;

/* Session cache of the last feed fetched by a reader command. Only the command
 * worker writes it (the poller passes its own feed), so handing out item copies
 * under the lock is race-free. */
static rss_feed_t s_cache;
static char s_cache_name[P4_CONFIG_RSS_SUB_NAME_BYTES];
static bool s_cache_valid;

static int s_interval_min = 60;
static bool s_autopoll = false;
static int s_retention = P4_CONFIG_RSS_ITEM_CAPACITY;
static bool s_save_read = true;

static TaskHandle_t s_poller_task;
static volatile bool s_poller_stop;

/* Line scratch, PSRAM for the same reason as the mirror. Static in the sense of
 * a single owner (the store lock), never on a task stack. */
static char *s_line;

/* ========================================================================
 * SMALL HELPERS
 * ======================================================================== */

static void store_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void store_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void store_append(const char *format, ...)
{
    char buffer[160];
    va_list args;

    if (s_ops.transcript_append_text == NULL) {
        return;
    }
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_ops.transcript_append_text(buffer);
}

static void store_record_warning(const char *message)
{
    if (s_ops.record_warning != NULL) {
        s_ops.record_warning(RSS_STORE_TAG, message);
    }
}

static void store_record_error(esp_err_t error, const char *message)
{
    if (s_ops.record_error != NULL) {
        s_ops.record_error(RSS_STORE_TAG, error, message);
    }
}

static uint64_t store_now_seconds(void)
{
    return (uint64_t)time(NULL);
}

static uint64_t store_hash_guid(const char *guid)
{
    uint64_t hash = RSS_HASH_OFFSET_BASIS;

    if (guid == NULL) {
        return 0;
    }
    for (const char *p = guid; *p != '\0'; p++) {
        hash ^= (uint64_t)(unsigned char)*p;
        hash *= RSS_HASH_PRIME;
    }
    return hash;
}

static int store_find_read_unlocked(const char *name, uint64_t hash)
{
    for (int i = 0; i < s_read_count; i++) {
        if (s_read_hash[i] == hash && strcasecmp(s_read_name[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ========================================================================
 * PURE RECORD EDITORS
 * ======================================================================== */

bool rss_store_name_ok(const char *name)
{
    size_t i;
    size_t len;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    len = strlen(name);
    if (len >= P4_CONFIG_RSS_SUB_NAME_BYTES) {
        return false;
    }
    for (i = 0; i < len; i++) {
        char c = name[i];

        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/** True when a field can be written into a pipe-separated record. */
static bool store_field_ok(const char *field)
{
    if (field == NULL) {
        return false;
    }
    return strchr(field, RSS_RECORD_SEP) == NULL &&
           strchr(field, '\n') == NULL &&
           strchr(field, '\r') == NULL;
}

/** Split one record on '|'. @return the field count, or -1 when the count is
 *  wrong or a field is embedded with a newline. */
static int record_split(char *line, char *fields[], size_t max_fields)
{
    size_t count = 0;
    char *cursor = line;
    char *sep;

    for (;;) {
        if (count >= max_fields) {
            return -1;
        }
        sep = strchr(cursor, RSS_RECORD_SEP);
        fields[count++] = cursor;
        if (sep == NULL) {
            break;
        }
        *sep = '\0';
        cursor = sep + 1;
    }

    for (size_t i = 0; i < count; i++) {
        if (strchr(fields[i], '\n') != NULL || strchr(fields[i], '\r') != NULL) {
            return -1;
        }
    }
    return (int)count;
}

esp_err_t rss_store_parse_record(const char *line, rss_subscription_t *out)
{
    char buffer[P4_CONFIG_RSS_STORE_LINE_BYTES];
    char *fields[RSS_RECORD_FIELDS];
    int count;
    int type;
    int enabled;
    int interval;
    char *end = NULL;

    if (line == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(buffer, line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        buffer[--len] = '\0';
    }
    {
        const char *q = buffer;

        while (*q != '\0' && isspace((unsigned char)*q)) {
            q++;
        }
        if (*q == '\0' || *q == ';' || *q == '#') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    count = record_split(buffer, fields, RSS_RECORD_FIELDS);
    if (count != RSS_RECORD_FIELDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rss_store_name_ok(fields[0]) || fields[1][0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < RSS_RECORD_FIELDS; i++) {
        if (!store_field_ok(fields[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    type = (int)strtol(fields[2], &end, 10);
    if (end == NULL || *end != '\0' || type < 0 || type > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    enabled = (int)strtol(fields[8], &end, 10);
    if (end == NULL || *end != '\0' || (enabled != 0 && enabled != 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    interval = (int)strtol(fields[9], &end, 10);
    if (end == NULL || *end != '\0' || interval < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strtoull(fields[7], &end, 10), end == NULL || *end != '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->name, fields[0], sizeof(out->name) - 1);
    strncpy(out->url, fields[1], sizeof(out->url) - 1);
    out->auth_type = (rss_auth_type_t)type;
    strncpy(out->auth_user, fields[3], sizeof(out->auth_user) - 1);
    strncpy(out->auth_token, fields[4], sizeof(out->auth_token) - 1);
    strncpy(out->etag, fields[5], sizeof(out->etag) - 1);
    strncpy(out->last_modified, fields[6], sizeof(out->last_modified) - 1);
    strncpy(out->last_fetched, fields[7], sizeof(out->last_fetched) - 1);
    out->enabled = (enabled != 0);
    out->interval_min = interval;
    strncpy(out->title, fields[10], sizeof(out->title) - 1);
    return ESP_OK;
}

bool rss_store_render_record(const rss_subscription_t *sub, char *out, size_t cap)
{
    int written;

    if (sub == NULL || out == NULL || cap == 0) {
        return false;
    }
    if (!rss_store_name_ok(sub->name) || sub->url[0] == '\0') {
        return false;
    }
    if (sub->auth_type < RSS_AUTH_NONE || sub->auth_type > RSS_AUTH_BEARER) {
        return false;
    }
    if (!store_field_ok(sub->name) || !store_field_ok(sub->url) ||
        !store_field_ok(sub->auth_user) || !store_field_ok(sub->auth_token) ||
        !store_field_ok(sub->etag) || !store_field_ok(sub->last_modified) ||
        !store_field_ok(sub->last_fetched) || !store_field_ok(sub->title)) {
        return false;
    }

    written = snprintf(out, cap, "%s%c%s%c%d%c%s%c%s%c%s%c%s%c%s%c%d%c%d%c%s",
                       sub->name, RSS_RECORD_SEP,
                       sub->url, RSS_RECORD_SEP,
                       (int)sub->auth_type, RSS_RECORD_SEP,
                       sub->auth_user, RSS_RECORD_SEP,
                       sub->auth_token, RSS_RECORD_SEP,
                       sub->etag, RSS_RECORD_SEP,
                       sub->last_modified, RSS_RECORD_SEP,
                       sub->last_fetched, RSS_RECORD_SEP,
                       sub->enabled ? 1 : 0, RSS_RECORD_SEP,
                       sub->interval_min, RSS_RECORD_SEP,
                       sub->title);
    return written >= 0 && (size_t)written < cap;
}

bool rss_store_parse_read_line(const char *line, char *name, size_t name_size,
                               char *hash, size_t hash_size)
{
    const char *sep;

    if (line == NULL || name == NULL || hash == NULL) {
        return false;
    }
    while (*line != '\0' && isspace((unsigned char)*line)) {
        line++;
    }
    if (*line == '\0' || *line == ';' || *line == '#') {
        return false;
    }
    sep = strchr(line, RSS_RECORD_SEP);
    if (sep == NULL) {
        return false;
    }
    {
        size_t name_len = (size_t)(sep - line);

        if (name_len == 0 || name_len >= name_size || strlen(sep + 1) >= hash_size) {
            return false;
        }
        /* Validate the name on its own: the raw line still contains the '|'. */
        memcpy(name, line, name_len);
        name[name_len] = '\0';
        if (!rss_store_name_ok(name)) {
            return false;
        }
    }
    strncpy(hash, sep + 1, hash_size - 1);
    hash[hash_size - 1] = '\0';
    return true;
}

bool rss_store_format_time(uint64_t unix_seconds, char *out, size_t out_size)
{
    time_t seconds = (time_t)unix_seconds;
    struct tm local;

    if (unix_seconds == 0 || out == NULL || out_size == 0) {
        return false;
    }
    if (!time_is_set()) {
        return false;
    }
    if (localtime_r(&seconds, &local) == NULL) {
        return false;
    }
    out[0] = '\0';
    strftime(out, out_size, "%Y-%m-%d %H:%M", &local);
    return out[0] != '\0';
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void rss_store_init(const networking_host_ops_t *ops)
{
    if (ops != NULL) {
        s_ops = *ops;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_subs == NULL) {
        s_subs = heap_caps_malloc(sizeof(rss_subscription_t) * P4_CONFIG_RSS_SUB_CAPACITY,
                                  MALLOC_CAP_SPIRAM);
        if (s_subs == NULL) {
            store_record_warning("subscription mirror unavailable (PSRAM)");
        }
    }
    if (s_line == NULL) {
        s_line = heap_caps_malloc(P4_CONFIG_RSS_STORE_LINE_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (s_read_hash == NULL || s_read_name == NULL) {
        s_read_hash = heap_caps_malloc(sizeof(uint64_t) * P4_CONFIG_RSS_READ_MAX,
                                       MALLOC_CAP_SPIRAM);
        s_read_name = heap_caps_malloc(sizeof(char) * P4_CONFIG_RSS_SUB_NAME_BYTES *
                                       P4_CONFIG_RSS_READ_MAX, MALLOC_CAP_SPIRAM);
        if (s_read_hash == NULL || s_read_name == NULL) {
            free(s_read_hash);
            free(s_read_name);
            s_read_hash = NULL;
            s_read_name = NULL;
            s_read_count = 0;
            store_record_warning("read-state table unavailable (PSRAM)");
        }
    }
}

/** True when the RAM mirrors are usable; a failed allocation disables the
 *  store instead of letting every caller dereference NULL. */
static bool store_ready(void)
{
    return s_subs != NULL && s_line != NULL;
}

/** Open a guarded session and resolve a store path. On success the caller must
 *  shell_sd_end() the session. */
static esp_err_t store_open(const char *path, shell_sd_session_t *session, char *resolved,
                            size_t resolved_size)
{
    if (shell_sd_begin(session) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (shell_fs_resolve_path(path, resolved, resolved_size) != ESP_OK) {
        shell_sd_end(session, "rss store");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t rss_store_load(void)
{
    shell_sd_session_t session;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    FILE *file;
    bool loaded_subs = false;

    if (!store_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    store_lock();

    s_count = 0;
    s_read_count = 0;
    if (s_cache_valid) {
        rss_feed_free(&s_cache);
        memset(&s_cache, 0, sizeof(s_cache));
        s_cache_name[0] = '\0';
        s_cache_valid = false;
    }

    if (store_open(P4_CONFIG_RSS_SUBS_FILE, &session, resolved, sizeof(resolved)) == ESP_OK) {
        file = fopen(resolved, "r");
        if (file != NULL) {
            while (s_count < P4_CONFIG_RSS_SUB_CAPACITY &&
                   fgets(s_line, P4_CONFIG_RSS_STORE_LINE_BYTES, file) != NULL) {
                rss_subscription_t parsed;

                if (rss_store_parse_record(s_line, &parsed) == ESP_OK) {
                    s_subs[s_count++] = parsed;
                }
                /* Malformed lines are skipped, not fatal: hand-editable file. */
            }
            fclose(file);
            loaded_subs = true;
        }
        shell_sd_end(&session, "rss store");
    }

    if (store_open(P4_CONFIG_RSS_READ_FILE, &session, resolved, sizeof(resolved)) == ESP_OK) {
        file = fopen(resolved, "r");
        if (file != NULL) {
            while (s_read_count < P4_CONFIG_RSS_READ_MAX &&
                   fgets(s_line, P4_CONFIG_RSS_STORE_LINE_BYTES, file) != NULL) {
                char name[P4_CONFIG_RSS_SUB_NAME_BYTES];
                char hash[32];

                if (!rss_store_parse_read_line(s_line, name, sizeof(name), hash, sizeof(hash))) {
                    continue;
                }
                s_read_hash[s_read_count] = strtoull(hash, NULL, 16);
                strncpy(s_read_name[s_read_count], name, P4_CONFIG_RSS_SUB_NAME_BYTES - 1);
                s_read_name[s_read_count][P4_CONFIG_RSS_SUB_NAME_BYTES - 1] = '\0';
                s_read_count++;
            }
            fclose(file);
        }
        shell_sd_end(&session, "rss store");
    }

    store_unlock();
    return loaded_subs ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---- writers (lock held) ---- */

static esp_err_t store_write_subs_unlocked(void)
{
    shell_sd_session_t session;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    char resolved_tmp[P4_CONFIG_SD_PATH_BYTES];
    char dir_resolved[P4_CONFIG_SD_PATH_BYTES];
    char temp[P4_CONFIG_SD_PATH_BYTES];
    FILE *file;
    uint64_t needed;

    /* storage_mkdir_p works on a resolved VFS path, not the sd:/ alias. */
    if (shell_fs_resolve_path(P4_CONFIG_RSS_STORE_DIR, dir_resolved,
                              sizeof(dir_resolved)) != ESP_OK ||
        storage_mkdir_p(dir_resolved) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (store_open(P4_CONFIG_RSS_SUBS_FILE, &session, resolved, sizeof(resolved)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    needed = (uint64_t)s_count * (P4_CONFIG_RSS_STORE_LINE_BYTES + 1) + 512;
    if (!storage_check_free_space(needed, storage_get_file_size(resolved), "rss save")) {
        shell_sd_end(&session, "rss save");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(temp, sizeof(temp), "%s.tmp", P4_CONFIG_RSS_SUBS_FILE);
    if (shell_fs_resolve_path(temp, resolved_tmp, sizeof(resolved_tmp)) != ESP_OK) {
        shell_sd_end(&session, "rss save");
        return ESP_ERR_NOT_FOUND;
    }

    file = fopen(resolved_tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, "rss save");
        return ESP_ERR_NOT_FOUND;
    }

    fputs(RSS_RECORD_HEADER, file);
    for (int i = 0; i < s_count; i++) {
        if (!rss_store_render_record(&s_subs[i], s_line, P4_CONFIG_RSS_STORE_LINE_BYTES)) {
            continue;
        }
        fprintf(file, "%s\n", s_line);
    }
    fclose(file);

    if (storage_replace_file(resolved_tmp, resolved) != ESP_OK) {
        shell_sd_end(&session, "rss save");
        return ESP_ERR_NOT_FOUND;
    }
    shell_sd_end(&session, "rss save");
    return ESP_OK;
}

static void store_write_read_unlocked(void)
{
    shell_sd_session_t session;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    char resolved_tmp[P4_CONFIG_SD_PATH_BYTES];
    char dir_resolved[P4_CONFIG_SD_PATH_BYTES];
    char temp[P4_CONFIG_SD_PATH_BYTES];
    FILE *file;

    if (shell_fs_resolve_path(P4_CONFIG_RSS_STORE_DIR, dir_resolved,
                              sizeof(dir_resolved)) != ESP_OK ||
        storage_mkdir_p(dir_resolved) != ESP_OK) {
        return;
    }
    if (store_open(P4_CONFIG_RSS_READ_FILE, &session, resolved, sizeof(resolved)) != ESP_OK) {
        return;
    }
    snprintf(temp, sizeof(temp), "%s.tmp", P4_CONFIG_RSS_READ_FILE);
    if (shell_fs_resolve_path(temp, resolved_tmp, sizeof(resolved_tmp)) != ESP_OK) {
        shell_sd_end(&session, "rss save");
        return;
    }
    file = fopen(resolved_tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, "rss save");
        return;
    }
    fputs(RSS_READ_HEADER, file);
    for (int i = 0; i < s_read_count; i++) {
        fprintf(file, "%s%c%016llx\n", s_read_name[i], RSS_RECORD_SEP,
                (unsigned long long)s_read_hash[i]);
    }
    fclose(file);
    storage_replace_file(resolved_tmp, resolved);
    shell_sd_end(&session, "rss save");
}

static esp_err_t store_save_unlocked(void)
{
    esp_err_t err = store_write_subs_unlocked();

    /* Read state rides along; a failure there must not fail the whole save. */
    store_write_read_unlocked();
    return err;
}

esp_err_t rss_store_save(void)
{
    esp_err_t err;

    if (!store_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    store_lock();
    err = store_save_unlocked();
    store_unlock();
    return err;
}

/* ========================================================================
 * SUBSCRIPTIONS
 * ======================================================================== */

int rss_store_count(void)
{
    int count;

    if (!store_ready()) {
        return 0;
    }
    store_lock();
    count = s_count;
    store_unlock();
    return count;
}

bool rss_store_get(int index, rss_subscription_t *out)
{
    bool ok = false;

    if (!store_ready()) {
        return false;
    }
    store_lock();
    if (index >= 0 && index < s_count && out != NULL) {
        *out = s_subs[index];
        ok = true;
    }
    store_unlock();
    return ok;
}

int rss_store_find(const char *name)
{
    int found = -1;

    if (name == NULL || !store_ready()) {
        return -1;
    }
    store_lock();
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_subs[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    store_unlock();
    return found;
}

static int store_find_unlocked(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_subs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t rss_store_add(const rss_subscription_t *sub)
{
    esp_err_t err;
    int index;

    if (sub == NULL || !rss_store_name_ok(sub->name) || sub->url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready()) {
        store_record_warning("subscription mirror unavailable");
        return ESP_ERR_NO_MEM;
    }
    if (sub->auth_type < RSS_AUTH_NONE || sub->auth_type > RSS_AUTH_BEARER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_field_ok(sub->url) || !store_field_ok(sub->auth_user) ||
        !store_field_ok(sub->auth_token) || !store_field_ok(sub->etag) ||
        !store_field_ok(sub->last_modified) || !store_field_ok(sub->title)) {
        return ESP_ERR_INVALID_ARG;
    }

    store_lock();
    index = store_find_unlocked(sub->name);
    if (index < 0 && s_count >= P4_CONFIG_RSS_SUB_CAPACITY) {
        store_unlock();
        store_record_warning("subscription list is full");
        return ESP_ERR_NO_MEM;
    }
    if (index >= 0) {
        s_subs[index] = *sub;
    } else {
        s_subs[s_count++] = *sub;
    }
    err = store_save_unlocked();
    store_unlock();

    if (err != ESP_OK) {
        store_record_error(err, "subscription kept in memory but not saved");
    }
    return err;
}

esp_err_t rss_store_remove(const char *name)
{
    esp_err_t err;

    if (!store_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    store_lock();
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_subs[i].name, name) == 0) {
            memmove(&s_subs[i], &s_subs[i + 1],
                    sizeof(rss_subscription_t) * (size_t)(s_count - i - 1));
            s_count--;
            break;
        }
    }
    err = store_save_unlocked();
    store_unlock();
    return err;
}

esp_err_t rss_store_update(const rss_subscription_t *sub)
{
    if (sub == NULL || store_find_unlocked(sub->name) < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return rss_store_add(sub);
}

esp_err_t rss_store_set_enabled(const char *name, bool enabled)
{
    esp_err_t err;

    if (!store_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    store_lock();
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_subs[i].name, name) == 0) {
            s_subs[i].enabled = enabled;
            break;
        }
    }
    err = store_save_unlocked();
    store_unlock();
    return err;
}

esp_err_t rss_store_set_interval(const char *name, int minutes)
{
    esp_err_t err;

    if (minutes < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    store_lock();
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_subs[i].name, name) == 0) {
            s_subs[i].interval_min = minutes;
            break;
        }
    }
    err = store_save_unlocked();
    store_unlock();
    return err;
}

/* ========================================================================
 * FETCH + SESSION CACHE
 * ======================================================================== */

static void cache_replace_unlocked(const char *name, const rss_feed_t *feed)
{
    if (s_cache_valid) {
        rss_feed_free(&s_cache);
        memset(&s_cache, 0, sizeof(s_cache));
    }
    s_cache = *feed;
    strncpy(s_cache_name, name, sizeof(s_cache_name) - 1);
    s_cache_name[sizeof(s_cache_name) - 1] = '\0';
    s_cache_valid = true;
}

void rss_store_clear_cache(void)
{
    store_lock();
    if (s_cache_valid) {
        rss_feed_free(&s_cache);
        memset(&s_cache, 0, sizeof(s_cache));
        s_cache_name[0] = '\0';
        s_cache_valid = false;
    }
    store_unlock();
}

const rss_feed_t *rss_store_cached(const char *name)
{
    const rss_feed_t *feed = NULL;

    store_lock();
    if (s_cache_valid && (name == NULL || strcasecmp(s_cache_name, name) == 0)) {
        feed = &s_cache;
    }
    store_unlock();
    return feed;
}

/** Deep-copy one item out of the session cache while holding the store lock.
 *
 *  A borrowed feed pointer would race the poller (which replaces the cache), so
 *  readers get their own copy instead: rss_item_t is entirely inline arrays, so
 *  a struct copy is already a deep copy and needs no release call.
 *
 *  @param want  1-based item number, or 0 for the first unread (falling back to
 *               the newest item when everything is read).
 *  @return true when @p item and @p index_out were filled. */
rss_item_t *rss_store_cached_item(const char *name, int want, int *index_out)
{
    rss_item_t *copy = NULL;

    store_lock();
    if (s_cache_valid && name != NULL && strcasecmp(s_cache_name, name) == 0 &&
        s_cache.item_count > 0 && s_cache.items != NULL) {
        int index = -1;

        if (want > 0) {
            if (want <= s_cache.item_count) {
                index = want - 1;
            }
        } else {
            for (int i = 0; i < s_cache.item_count; i++) {
                const rss_item_t *it = &s_cache.items[i];
                const char *key = it->guid[0] != '\0' ? it->guid : it->link;

                if (store_find_read_unlocked(name, store_hash_guid(key)) == -1) {
                    index = i;
                    break;
                }
            }
            if (index < 0) {
                index = 0;                   /* everything read: show the newest */
            }
        }
        if (index >= 0) {
            copy = heap_caps_malloc(sizeof(rss_item_t), MALLOC_CAP_SPIRAM);
            if (copy != NULL) {
                *copy = s_cache.items[index];
                *index_out = index;
            }
        }
    }
    store_unlock();
    return copy;
}

/** Unread count for the cached feed (0 when nothing is cached for @p name). */
int rss_store_cached_unread(const char *name)
{
    int unread = 0;

    store_lock();
    if (s_cache_valid && name != NULL && strcasecmp(s_cache_name, name) == 0 &&
        s_cache.items != NULL) {
        for (int i = 0; i < s_cache.item_count; i++) {
            const rss_item_t *it = &s_cache.items[i];
            const char *key = it->guid[0] != '\0' ? it->guid : it->link;

            if (store_find_read_unlocked(name, store_hash_guid(key)) == -1) {
                unread++;
            }
        }
    }
    store_unlock();
    return unread;
}

esp_err_t rss_store_fetch(const char *name, rss_feed_t *feed)
{
    char sub_name[P4_CONFIG_RSS_SUB_NAME_BYTES];
    rss_subscription_t *sub;
    rss_feed_t *parsed;
    char etag[P4_CONFIG_RSS_SUB_ETAG_BYTES];
    char last_modified[P4_CONFIG_RSS_ITEM_DATE_BYTES];
    int index;
    esp_err_t err;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (feed != NULL) {
        memset(feed, 0, sizeof(*feed));
    }

    /* The subscription record (~1.4 KB) and the feed struct (~2 KB) are far too
     * big for a task stack: the fetch path already needs >11 KB for the HTTP
     * client and the parser. Both live in PSRAM. */
    sub = heap_caps_malloc(sizeof(rss_subscription_t), MALLOC_CAP_SPIRAM);
    parsed = heap_caps_malloc(sizeof(rss_feed_t), MALLOC_CAP_SPIRAM);
    if (sub == NULL || parsed == NULL) {
        free(sub);
        free(parsed);
        return ESP_ERR_NO_MEM;
    }

    store_lock();
    index = store_find_unlocked(name);
    if (index < 0) {
        store_unlock();
        free(sub);
        free(parsed);
        return ESP_ERR_NOT_FOUND;
    }
    *sub = s_subs[index];
    strncpy(sub_name, sub->name, sizeof(sub_name) - 1);
    sub_name[sizeof(sub_name) - 1] = '\0';
    store_unlock();

    memset(parsed, 0, sizeof(*parsed));
    err = rss_fetch(sub->url, sub, parsed, etag, sizeof(etag),
                    last_modified, sizeof(last_modified));
    free(sub);
    if (err != ESP_OK) {
        rss_feed_free(parsed);
        free(parsed);
        if (err != RSS_ERR_NOT_MODIFIED) {
            ESP_LOGD(RSS_STORE_TAG, "fetch %s: %s", sub_name, esp_err_to_name(err));
        }
        return err;
    }

    store_lock();
    if (index < s_count && strcasecmp(s_subs[index].name, sub_name) == 0) {
        strncpy(s_subs[index].etag, etag, sizeof(s_subs[index].etag) - 1);
        strncpy(s_subs[index].last_modified, last_modified,
                sizeof(s_subs[index].last_modified) - 1);
        snprintf(s_subs[index].last_fetched, sizeof(s_subs[index].last_fetched),
                 "%llu", (unsigned long long)store_now_seconds());
        if (parsed->title[0] != '\0') {
            strncpy(s_subs[index].title, parsed->title, sizeof(s_subs[index].title) - 1);
        }
        store_save_unlocked();
    }
    if (feed != NULL) {
        *feed = *parsed;                /* ownership of the items moves */
    } else {
        cache_replace_unlocked(sub_name, parsed);
    }
    free(parsed);
    store_unlock();
    return ESP_OK;
}

esp_err_t rss_store_refresh_all(void)
{
    int count;
    esp_err_t first_error = ESP_OK;

    store_lock();
    count = s_count;
    store_unlock();

    for (int i = 0; i < count; i++) {
        rss_subscription_t sub;

        if (!rss_store_get(i, &sub)) {
            break;
        }
        if (!sub.enabled) {
            store_append("rss: %s skipped (disabled)\n", sub.name);
            continue;
        }
        esp_err_t err = rss_store_fetch(sub.name, NULL);
        if (err == ESP_OK) {
            store_append("rss: %s refreshed - %d unread\n",
                         sub.name, rss_store_cached_unread(sub.name));
        } else if (err == RSS_ERR_NOT_MODIFIED) {
            store_append("rss: %s unchanged\n", sub.name);
        } else if (err == ESP_ERR_INVALID_STATE) {
            store_append("rss: %s needs Wi-Fi\n", sub.name);
            if (first_error == ESP_OK) {
                first_error = err;
            }
        } else {
            store_append("rss: %s refresh failed (%s)\n", sub.name, esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RSS_POLLER_FEED_DELAY_MS));
    }
    return first_error;
}

/* ========================================================================
 * READ / UNREAD STATE
 * ======================================================================== */

bool rss_store_is_read(const char *name, const char *guid)
{
    bool read = false;

    if (name == NULL || guid == NULL) {
        return false;
    }
    store_lock();
    read = store_find_read_unlocked(name, store_hash_guid(guid)) >= 0;
    store_unlock();
    return read;
}

void rss_store_mark_read(const char *name, const char *guid)
{
    uint64_t hash;

    if (name == NULL || guid == NULL || !s_save_read) {
        return;
    }

    store_lock();
    hash = store_hash_guid(guid);
    if (store_find_read_unlocked(name, hash) >= 0) {
        store_unlock();
        return;
    }
    if (s_read_count >= P4_CONFIG_RSS_READ_MAX) {
        /* FIFO: drop the oldest entry so the table stays bounded. */
        memmove(&s_read_hash[0], &s_read_hash[1],
                sizeof(uint64_t) * (size_t)(P4_CONFIG_RSS_READ_MAX - 1));
        memmove(&s_read_name[0], &s_read_name[1],
                sizeof(s_read_name[0]) * (size_t)(P4_CONFIG_RSS_READ_MAX - 1));
        s_read_count = P4_CONFIG_RSS_READ_MAX - 1;
    }
    s_read_hash[s_read_count] = hash;
    strncpy(s_read_name[s_read_count], name, P4_CONFIG_RSS_SUB_NAME_BYTES - 1);
    s_read_name[s_read_count][P4_CONFIG_RSS_SUB_NAME_BYTES - 1] = '\0';
    s_read_count++;

    store_write_read_unlocked();
    store_unlock();
}

int rss_store_unread_count(const char *name, const rss_feed_t *feed)
{
    int unread = 0;

    if (feed == NULL) {
        return 0;
    }
    store_lock();
    for (int i = 0; i < feed->item_count; i++) {
        rss_item_t *item = &feed->items[i];

        if (item->guid[0] != '\0' &&
            store_find_read_unlocked(name, store_hash_guid(item->guid)) >= 0) {
            item->read = true;
            continue;
        }
        /* Feeds without a guid fall back to the item link. */
        if (store_find_read_unlocked(name, store_hash_guid(item->link)) >= 0) {
            item->read = true;
            continue;
        }
        item->read = false;
        unread++;
    }
    store_unlock();
    return unread;
}

esp_err_t rss_store_reset_read(const char *name)
{
    store_lock();
    for (int i = 0; i < s_read_count;) {
        if (strcasecmp(s_read_name[i], name) == 0) {
            memmove(&s_read_hash[i], &s_read_hash[i + 1],
                    sizeof(uint64_t) * (size_t)(s_read_count - i - 1));
            memmove(&s_read_name[i], &s_read_name[i + 1],
                    sizeof(s_read_name[0]) * (size_t)(s_read_count - i - 1));
            s_read_count--;
            continue;
        }
        i++;
    }
    store_write_read_unlocked();
    store_unlock();
    return ESP_OK;
}

void rss_store_set_save_read(bool enable)
{
    s_save_read = enable;
}

bool rss_store_get_save_read(void)
{
    return s_save_read;
}

/* ========================================================================
 * TUNABLES
 * ======================================================================== */

void rss_store_set_interval_default(int minutes)
{
    s_interval_min = minutes > 0 ? minutes : 0;
}

int rss_store_get_interval_default(void)
{
    return s_interval_min;
}

void rss_store_set_autopoll(bool enable)
{
    s_autopoll = enable;
}

bool rss_store_get_autopoll(void)
{
    return s_autopoll;
}

void rss_store_set_retention(int items)
{
    s_retention = items > 0 ? items : P4_CONFIG_RSS_ITEM_CAPACITY;
}

int rss_store_get_retention(void)
{
    return s_retention;
}

/* ========================================================================
 * CONFIG.SYS DIRECTIVES
 * ======================================================================== */

/** Accept either the CONFIG.SYS spelling ("RSS_INTERVAL") or the short one
 *  ("interval") so the same parser serves both callers. */
static bool directive_is(const char *key, const char *upper, const char *lower)
{
    return strcasecmp(key, upper) == 0 || strcasecmp(key, lower) == 0;
}

bool rss_store_apply_directive(const char *key, const char *value)
{
    char *end = NULL;
    long parsed;

    if (key == NULL || value == NULL || value[0] == '\0') {
        return false;
    }

    if (directive_is(key, "RSS_INTERVAL", "interval")) {
        parsed = strtol(value, &end, 10);
        if (end == NULL || *end != '\0' || parsed < 0 || parsed > 525600) {
            return false;
        }
        rss_store_set_interval_default((int)parsed);
        return true;
    }
    if (directive_is(key, "RSS_AUTOPOLL", "autopoll")) {
        if (strcasecmp(value, "ON") == 0) {
            rss_store_set_autopoll(true);
            return true;
        }
        if (strcasecmp(value, "OFF") == 0) {
            rss_store_set_autopoll(false);
            return true;
        }
        return false;
    }
    if (directive_is(key, "RSS_RETENTION", "retention")) {
        parsed = strtol(value, &end, 10);
        if (end == NULL || *end != '\0' || parsed < 1 ||
            parsed > P4_CONFIG_RSS_ITEM_CAPACITY) {
            return false;
        }
        rss_store_set_retention((int)parsed);
        return true;
    }
    if (directive_is(key, "RSS_SAVEREAD", "saveread")) {
        if (strcasecmp(value, "ON") == 0) {
            rss_store_set_save_read(true);
            return true;
        }
        if (strcasecmp(value, "OFF") == 0) {
            rss_store_set_save_read(false);
            return true;
        }
        return false;
    }
    return false;
}

bool rss_store_render_directive(const char *key, char *out, size_t out_size)
{
    if (key == NULL || out == NULL || out_size == 0) {
        return false;
    }
    if (directive_is(key, "RSS_INTERVAL", "interval")) {
        snprintf(out, out_size, "%d", rss_store_get_interval_default());
        return true;
    }
    if (directive_is(key, "RSS_AUTOPOLL", "autopoll")) {
        snprintf(out, out_size, "%s", rss_store_get_autopoll() ? "ON" : "OFF");
        return true;
    }
    if (directive_is(key, "RSS_RETENTION", "retention")) {
        snprintf(out, out_size, "%d", rss_store_get_retention());
        return true;
    }
    if (directive_is(key, "RSS_SAVEREAD", "saveread")) {
        snprintf(out, out_size, "%s", rss_store_get_save_read() ? "ON" : "OFF");
        return true;
    }
    return false;
}

/* ========================================================================
 * BACKGROUND POLLER
 * ======================================================================== */

/** Seconds until a subscription is due, or -1 when it is not due (disabled or
 *  interval 0 means "manual only"). */
static int store_seconds_to_due(const rss_subscription_t *sub)
{
    int interval = sub->interval_min > 0 ? sub->interval_min : s_interval_min;
    uint64_t last;

    if (!sub->enabled || interval <= 0) {
        return -1;
    }
    if (sub->last_fetched[0] == '\0') {
        return 0;
    }
    last = strtoull(sub->last_fetched, NULL, 10);
    if (last == 0) {
        return 0;
    }
    int64_t elapsed = (int64_t)(store_now_seconds() - last);
    int64_t span = (int64_t)interval * 60;

    if (elapsed < 0) {
        return 0;
    }
    return elapsed >= span ? 0 : (int)(span - elapsed);
}

static void poller_task(void *arg)
{
    (void)arg;
    ESP_LOGI(RSS_STORE_TAG, "poller started");

    while (!s_poller_stop) {
        int count;

        store_lock();
        count = s_count;
        store_unlock();

        for (int i = 0; i < count && !s_poller_stop; i++) {
            rss_subscription_t sub;

            if (!rss_store_get(i, &sub)) {
                break;
            }
            if (store_seconds_to_due(&sub) != 0) {
                continue;
            }
            /* The poller passes its own feed so it never touches the session
             * cache (which only reader commands write). */
            rss_feed_t parsed;
            esp_err_t err = rss_store_fetch(sub.name, &parsed);

            rss_feed_free(&parsed);
            if (err != ESP_OK && err != RSS_ERR_NOT_MODIFIED) {
                ESP_LOGD(RSS_STORE_TAG, "poll %s: %s", sub.name, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(RSS_POLLER_FEED_DELAY_MS));
        }
        for (int tick = 0; tick < RSS_POLLER_TICK_MS / 100 && !s_poller_stop; tick++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(RSS_STORE_TAG, "poller stopped");
    s_poller_task = NULL;
    vTaskDelete(NULL);
}

void rss_store_start_poller(void)
{
    if (s_poller_task != NULL || !s_autopoll || s_interval_min <= 0) {
        return;
    }
    s_poller_stop = false;
    /* PSRAM stack: internal DRAM is the scarce resource on both boards, and the
     * poller is opt-in, so it must not silently cost 8 KB of it. */
    if (xTaskCreateWithCaps(poller_task, "rss_poll", P4_CONFIG_RSS_POLLER_TASK_STACK,
                            NULL, 5, &s_poller_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_poller_task = NULL;
        store_record_warning("feed poller could not be started");
    }
}

void rss_store_stop_poller(void)
{
    if (s_poller_task == NULL) {
        return;
    }
    s_poller_stop = true;
}

bool rss_store_poller_running(void)
{
    return s_poller_task != NULL;
}

/* ========================================================================
 * OPML IMPORT / EXPORT
 * ======================================================================== */

esp_err_t rss_store_export(const char *path)
{
    shell_sd_session_t session;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    char resolved_tmp[P4_CONFIG_SD_PATH_BYTES];
    char temp[P4_CONFIG_SD_PATH_BYTES];
    FILE *file;

    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        shell_sd_end(&session, "rss export");
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (shell_fs_resolve_path(temp, resolved_tmp, sizeof(resolved_tmp)) != ESP_OK) {
        shell_sd_end(&session, "rss export");
        return ESP_ERR_NOT_FOUND;
    }
    file = fopen(resolved_tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, "rss export");
        return ESP_ERR_NOT_FOUND;
    }
    store_lock();
    rss_opml_write(file, s_subs, s_count);
    store_unlock();
    fclose(file);

    if (storage_replace_file(resolved_tmp, resolved) != ESP_OK) {
        shell_sd_end(&session, "rss export");
        return ESP_ERR_NOT_FOUND;
    }
    shell_sd_end(&session, "rss export");
    return ESP_OK;
}

esp_err_t rss_store_import(const char *path, int *added)
{
    shell_sd_session_t session;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    rss_subscription_t *parsed;
    char *text;
    FILE *file;
    size_t got;
    int count = 0;
    esp_err_t result = ESP_OK;

    if (added != NULL) {
        *added = 0;
    }
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        shell_sd_end(&session, "rss import");
        return ESP_ERR_NOT_FOUND;
    }
    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_sd_end(&session, "rss import");
        return ESP_ERR_NOT_FOUND;
    }

    /* The subscription array is ~1.3 KB per entry, so the parse buffer lives in
     * PSRAM rather than on a task stack. */
    parsed = heap_caps_malloc(sizeof(rss_subscription_t) * P4_CONFIG_RSS_SUB_CAPACITY,
                              MALLOC_CAP_SPIRAM);
    text = heap_caps_malloc((size_t)P4_CONFIG_RSS_OPML_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (parsed == NULL || text == NULL) {
        free(parsed);
        free(text);
        fclose(file);
        shell_sd_end(&session, "rss import");
        return ESP_ERR_NO_MEM;
    }

    got = fread(text, 1, P4_CONFIG_RSS_OPML_MAX_BYTES, file);
    fclose(file);
    shell_sd_end(&session, "rss import");
    text[got] = '\0';

    if (rss_opml_parse(text, got, parsed, P4_CONFIG_RSS_SUB_CAPACITY, &count) != ESP_OK) {
        free(parsed);
        free(text);
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < count; i++) {
        if (rss_store_add(&parsed[i]) == ESP_OK) {
            if (added != NULL) {
                (*added)++;
            }
        } else {
            result = ESP_ERR_INVALID_STATE;
        }
    }

    free(parsed);
    free(text);
    return result;
}

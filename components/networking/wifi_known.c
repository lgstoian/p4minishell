/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file wifi_known.c
 * @brief Persistent known Wi-Fi network list (sd:/WIFI.KNOWN).
 *
 * In-memory cache + SD persistence for previously-used networks. All SD I/O
 * uses the guarded storage session API and is safe at every step; any failure
 * degrades to an empty list / non-OK return without crashing, freezing, or
 * blocking boot.
 *
 * File format (plain text, one network per line, hand-editable):
 *
 *   ; P4MiniShell known Wi-Fi networks
 *   ; format: SSID|PASSWORD|AUTH|PRIORITY|PREFERRED|LAST_CONNECTED|CONNECT_COUNT
 *   ; SSID and PASSWORD must not contain '|'. Lines starting with ';' or '#'
 *   ; and blank lines are ignored. AUTH is the wifi_auth_mode_t numeric value.
 *   MyHome|secret123|3|10|1|1786450000|5
 *
 * The file is written atomically (temp file + rename) with a free-space
 * pre-check and partial-file cleanup, per the storage guardrail rules.
 */

#include "wifi_known.h"

#include "storage.h"
#include "ansi.h"
#include "ansi_palette.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_KNOWN_TAG P4_CONFIG_NETWORKING_TAG

/* ---- Host render/log callbacks (bound by networking_init) ---- */
static networking_host_ops_t s_ops;

/* ---- In-memory cache ---- */
static networking_wifi_known_entry_t s_known[P4_CONFIG_WIFI_KNOWN_MAX];
static int s_known_count;

static SemaphoreHandle_t s_known_mutex;

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

static void known_lock(void)
{
    if (s_known_mutex != NULL) {
        xSemaphoreTake(s_known_mutex, portMAX_DELAY);
    }
}

static void known_unlock(void)
{
    if (s_known_mutex != NULL) {
        xSemaphoreGive(s_known_mutex);
    }
}

static void known_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    if (s_ops.transcript_append_ansi == NULL) {
        return;
    }
    va_start(args, format);
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_ops.transcript_append_ansi(buffer);
}

static void known_record_warningf(const char *format, ...)
{
    char buffer[256];
    va_list args;

    if (s_ops.record_warning == NULL) {
        return;
    }
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_ops.record_warning(WIFI_KNOWN_TAG, buffer);
}

static bool known_ssid_equals(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    /* 802.11 SSIDs are case-sensitive byte strings: two networks whose names
     * differ only in case are distinct. */
    return strcmp(left, right) == 0;
}

static int known_index_of(const char *ssid)
{
    int index;

    for (index = 0; index < s_known_count; index++) {
        if (known_ssid_equals(s_known[index].ssid, ssid)) {
            return index;
        }
    }
    return -1;
}

/**
 * Evict the least desirable entry to make room: preferred entries last, then
 * higher priority first, then more recently connected first. Returns the index
 * to evict, or -1 when the list is empty.
 */
static int known_eviction_index(void)
{
    int index;
    int victim = -1;

    for (index = 0; index < s_known_count; index++) {
        if (victim < 0) {
            victim = index;
            continue;
        }
        /* A non-preferred entry beats a preferred one for eviction. */
        if (!s_known[index].preferred && s_known[victim].preferred) {
            victim = index;
            continue;
        }
        if (s_known[index].preferred != s_known[victim].preferred) {
            continue;
        }
        /* Lower priority first. */
        if (s_known[index].priority < s_known[victim].priority) {
            victim = index;
            continue;
        }
        if (s_known[index].priority != s_known[victim].priority) {
            continue;
        }
        /* Older last_connected first. */
        if (s_known[index].last_connected < s_known[victim].last_connected) {
            victim = index;
        }
    }
    return victim;
}

/** Clear the in-memory cache (does not touch SD). */
static void known_reset_cache(void)
{
    memset(s_known, 0, sizeof(s_known));
    s_known_count = 0;

}

/**
 * Refresh the in-memory cache from the SD file before a read-modify-write
 * (upsert / remove / set-preferred). Without this, a save would silently
 * overwrite the file with only the newly-added entry, and a card inserted after
 * boot would be missed. Fully safe when the card is absent: load() leaves an
 * empty cache. A tiny single-file read per mutation is acceptable.
 */
static void known_ensure_loaded(void)
{
    (void)networking_wifi_known_load();
}

/* ========================================================================
 * SD I/O (guarded storage session, fully failure-tolerant)
 * ======================================================================== */

static esp_err_t known_resolve_path(char *path, size_t path_size)
{
    return shell_sd_resolve_path(P4_CONFIG_WIFI_KNOWN_FILE, path, path_size);
}

static esp_err_t known_parse_entry(char *line, networking_wifi_known_entry_t *entry)
{
    char *fields[7];
    int field_count = 0;
    char *cursor = line;
    char *token;
    char *end = NULL;
    size_t len;

    /* Strip trailing newline/CR. */
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

    /* Skip blank lines and comments. */
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '\0' || *cursor == ';' || *cursor == '#') {
        return ESP_ERR_NOT_FOUND;   /* not an entry, but not a parse error */
    }

    /* Split on '|' into exactly 7 fields. */
    while (field_count < 7 && (token = strsep(&cursor, "|")) != NULL) {
        fields[field_count++] = token;
    }
    if (field_count != 7) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ssid, sizeof(entry->ssid), "%s", fields[0]);
    if (entry->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(entry->password, sizeof(entry->password), "%s", fields[1]);

    end = NULL;
    entry->authmode = (int)strtol(fields[2], &end, 10);
    if (end == fields[2] || *end != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    end = NULL;
    entry->priority = (int)strtol(fields[3], &end, 10);
    if (end == fields[3] || *end != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    end = NULL;
    entry->preferred = (strtol(fields[4], &end, 10) != 0);
    if (end == fields[4] || *end != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    end = NULL;
    entry->last_connected = (int64_t)strtoll(fields[5], &end, 10);
    if (end == fields[5] || *end != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    end = NULL;
    entry->connect_count = (int)strtol(fields[6], &end, 10);
    if (end == fields[6] || *end != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t networking_wifi_known_load(void)
{
    shell_sd_session_t session;
    char path[128];
    char line[P4_CONFIG_WIFI_KNOWN_LINE_BYTES];
    FILE *file = NULL;
    int count = 0;

    if (shell_sd_begin(&session) != ESP_OK) {
        known_lock();
        known_reset_cache();
        known_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (known_resolve_path(path, sizeof(path)) != ESP_OK) {
        shell_sd_end(&session, "wifi known");
        known_lock();
        known_reset_cache();
        known_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        shell_sd_end(&session, "wifi known");
        known_lock();
        known_reset_cache();
        known_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    /* Parse directly into the static cache under the lock: no large stack
     * array (the cache is P4_CONFIG_WIFI_KNOWN_MAX entries, ~2 KB, which
     * would overflow the command-worker or event-task stack). The SD read is
     * fast and the lock is only held for this module's own state. */
    known_lock();
    known_reset_cache();
    while (count < P4_CONFIG_WIFI_KNOWN_MAX &&
           fgets(line, sizeof(line), file) != NULL) {
        esp_err_t parsed_error = known_parse_entry(line, &s_known[count]);
        if (parsed_error == ESP_OK) {
            count++;
        }
        /* Other errors (comment / blank / malformed) are skipped. */
    }
    s_known_count = count;
    known_unlock();

    fclose(file);
    shell_sd_end(&session, "wifi known");


    return ESP_OK;
}

esp_err_t networking_wifi_known_save(void)
{
    shell_sd_session_t session;
    char path[128];
    char tmp[160];
    FILE *file = NULL;
    int index;

    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    if (known_resolve_path(path, sizeof(path)) != ESP_OK) {
        shell_sd_end(&session, "wifi known");
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* Free-space pre-check: estimate worst-case file size, reclaim the size of
     * the file we are about to overwrite. storage_check_free_space prints a
     * refusal message when there is not enough room. */
    {
        uint64_t needed = (uint64_t)P4_CONFIG_WIFI_KNOWN_MAX *
                          (NETWORKING_WIFI_SSID_BYTES + NETWORKING_WIFI_PASSWORD_BYTES + 64) + 512;
        uint64_t reclaim = storage_get_file_size(path);
        if (!storage_check_free_space(needed, reclaim, "wifi save")) {
            shell_sd_end(&session, "wifi known");
            return ESP_ERR_INVALID_STATE;
        }
    }

    file = fopen(tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, "wifi known");
        return ESP_ERR_NOT_FOUND;
    }

    fprintf(file, "; P4MiniShell known Wi-Fi networks\n");
    fprintf(file, "; format: SSID|PASSWORD|AUTH|PRIORITY|PREFERRED|LAST_CONNECTED|CONNECT_COUNT\n");
    fprintf(file, "; SSID/PASSWORD must not contain '|'. AUTH = wifi_auth_mode_t numeric.\n");

    known_lock();
    for (index = 0; index < s_known_count; index++) {
        const networking_wifi_known_entry_t *entry = &s_known[index];
        /* Sanitise: never write a '|' into a field (breaks the format). */
        if (strchr(entry->ssid, '|') != NULL || strchr(entry->password, '|') != NULL) {
            continue;
        }
        fprintf(file, "%s|%s|%d|%d|%d|%" PRId64 "|%d\n",
                entry->ssid,
                entry->password,
                entry->authmode,
                entry->priority,
                entry->preferred ? 1 : 0,
                entry->last_connected,
                entry->connect_count);
    }
    known_unlock();

    {
        int flush_rc = fflush(file);
        int close_rc = fclose(file);
        if (flush_rc != 0 || close_rc != 0) {
            remove(tmp);
            shell_sd_end(&session, "wifi known");
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    /* Atomic replace. FATFS f_rename refuses to overwrite an existing target,
     * so when the file already exists, remove it and retry the rename. */
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            shell_sd_end(&session, "wifi known");

            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    shell_sd_end(&session, "wifi known");

    return ESP_OK;
}

/* ========================================================================
 * PUBLIC CRUD
 * ======================================================================== */

esp_err_t networking_wifi_known_upsert(const char *ssid, const char *password,
                                       int authmode, bool mark_connected)
{
    int index;

    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (password == NULL) {
        password = "";
    }

    known_ensure_loaded();
    known_lock();
    index = known_index_of(ssid);
    if (index < 0) {
        if (s_known_count >= P4_CONFIG_WIFI_KNOWN_MAX) {
            int victim = known_eviction_index();
            if (victim < 0) {
                known_unlock();
                return ESP_ERR_NO_MEM;
            }
            memmove(&s_known[victim], &s_known[victim + 1],
                    (size_t)(s_known_count - victim - 1) * sizeof(s_known[0]));
            s_known_count--;
        }
        index = s_known_count;
        memset(&s_known[index], 0, sizeof(s_known[index]));
        snprintf(s_known[index].ssid, sizeof(s_known[index].ssid), "%s", ssid);
        s_known_count++;
    }

    if (password[0] != '\0') {
        snprintf(s_known[index].password, sizeof(s_known[index].password), "%s", password);
    }
    if (authmode >= 0) {
        s_known[index].authmode = authmode;
    }
    if (mark_connected) {
        s_known[index].connect_count++;
        s_known[index].last_connected = (int64_t)(esp_timer_get_time() / 1000000ULL);
    }
    known_unlock();

    return networking_wifi_known_save();
}

esp_err_t networking_wifi_known_remove(const char *ssid)
{
    int index;

    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    known_ensure_loaded();
    known_lock();
    index = known_index_of(ssid);
    if (index < 0) {
        known_unlock();
        return ESP_OK;
    }
    memmove(&s_known[index], &s_known[index + 1],
            (size_t)(s_known_count - index - 1) * sizeof(s_known[0]));
    s_known_count--;
    known_unlock();

    return networking_wifi_known_save();
}

esp_err_t networking_wifi_known_clear(void)
{
    known_lock();
    known_reset_cache();
    known_unlock();

    return networking_wifi_known_save();
}

esp_err_t networking_wifi_known_set_preferred(const char *ssid, bool preferred)
{
    int index;

    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    known_ensure_loaded();
    known_lock();
    index = known_index_of(ssid);
    if (index < 0) {
        known_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    s_known[index].preferred = preferred;
    known_unlock();

    return networking_wifi_known_save();
}

int networking_wifi_known_count(void)
{
    int count;
    known_lock();
    count = s_known_count;
    known_unlock();
    return count;
}

bool networking_wifi_known_get(int index, networking_wifi_known_entry_t *out)
{
    bool ok = false;
    if (out == NULL) {
        return false;
    }
    known_lock();
    if (index >= 0 && index < s_known_count) {
        *out = s_known[index];
        ok = true;
    }
    known_unlock();
    return ok;
}

int networking_wifi_known_pick_visible(const wifi_ap_record_t *records, uint16_t count)
{
    int best = -1;
    bool best_preferred = false;
    int best_priority = 0;
    int best_rssi = 0;
    int index;

    if (records == NULL) {
        return -1;
    }

    known_lock();
    for (index = 0; index < s_known_count; index++) {
        const networking_wifi_known_entry_t *entry = &s_known[index];
        int rssi = 0;
        int r = 0;
        bool visible = false;

        for (r = 0; r < count; r++) {
            if (records[r].ssid[0] != '\0' &&
                known_ssid_equals((const char *)records[r].ssid, entry->ssid)) {
                if (!visible || records[r].rssi > rssi) {
                    rssi = records[r].rssi;
                }
                visible = true;
            }
        }
        if (!visible) {
            continue;
        }

        if (best < 0 ||
            (entry->preferred && !best_preferred) ||
            (entry->preferred == best_preferred &&
             (entry->priority > best_priority ||
              (entry->priority == best_priority && rssi > best_rssi)))) {
            best = index;
            best_preferred = entry->preferred;
            best_priority = entry->priority;
            best_rssi = rssi;
        }
    }
    known_unlock();
    return best;
}

esp_err_t networking_wifi_known_list(void)
{
    shell_sd_session_t session;
    char path[128];
    bool file_ok = false;

    /* Report SD availability without printing on absence. */
    if (shell_sd_begin(&session) == ESP_OK) {
        if (known_resolve_path(path, sizeof(path)) == ESP_OK) {
            FILE *f = fopen(path, "r");
            if (f != NULL) {
                fclose(f);
                file_ok = true;
            }
        }
        shell_sd_end(&session, "wifi known");
    }

    if (!file_ok) {
        known_appendf(SH_WARN "wifi.known:" SH_RST " the SD known-network list is unavailable (no card or file)\n");
        known_record_warningf("Known-network list unavailable (no SD card or missing file)");
        return ESP_ERR_NOT_FOUND;
    }

    /* Ensure the cache reflects the file (safe no-op if already loaded). */
    (void)networking_wifi_known_load();

    known_lock();
    if (s_known_count == 0) {
        known_appendf(SH_MUTE "wifi.known:" SH_RST " no known networks stored\n");
    } else {
        int index;
        known_appendf(SH_HEAD "Known Wi-Fi Networks (%d)" SH_RST "\n", s_known_count);
        for (index = 0; index < s_known_count; index++) {
            const networking_wifi_known_entry_t *entry = &s_known[index];
            known_appendf("  " SH_LBL "%d." SH_RST " " SH_VAL "%-32s" SH_RST " ",
                          index + 1,
                          entry->ssid);
            if (entry->preferred) {
                known_appendf(SH_OK "preferred" SH_RST " ");
            }
            if (entry->priority > 0) {
                known_appendf(SH_NUM "prio=%d" SH_RST " ", entry->priority);
            }
            if (entry->last_connected > 0) {
                known_appendf(SH_MUTE "last=%" PRId64 " count=%d" SH_RST,
                              entry->last_connected, entry->connect_count);
            }
            known_appendf("\n");
        }
    }
    known_unlock();

    return ESP_OK;
}

void networking_wifi_known_record_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    if (password == NULL) {
        password = "";
    }

    /* Read the authmode of the live association for metadata. */
    int authmode = -1;
    {
        wifi_ap_record_t ap_info;
        memset(&ap_info, 0, sizeof(ap_info));
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            authmode = (int)ap_info.authmode;
        }
    }

    (void)networking_wifi_known_upsert(ssid, password, authmode, true);
}

void networking_wifi_known_init(const networking_host_ops_t *ops)
{
    if (ops != NULL) {
        s_ops = *ops;
    }
    if (s_known_mutex == NULL) {
        s_known_mutex = xSemaphoreCreateMutex();
    }
    known_lock();
    known_reset_cache();
    known_unlock();
}

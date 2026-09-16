/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file alarm.c
 * @brief SD-persisted alarm / event store + background checker (see alarm.h).
 *
 * All event data lives on the SD card under `sd:/ALARMS/`:
 *   INDEX.INI         next_id, count, version
 *   E<id>.INI         id, when, title, msg, flags, recur, action, run
 *
 * A single checker task polls the store every P4_CONFIG_ALARM_POLL_MS and
 * fires due events through the existing surfaces only: the header notification
 * (`shell_header_notify`), the LED (`led_notify`), the speaker
 * (`audio_play_tone`), and — for the `/run:` action — a command queued to the
 * command worker through `alarm_host_ops_t.execute_async` (never run on the
 * checker's stack). There is no second notification loop and no RAM-only
 * store.
 *
 * Thread safety: a FreeRTOS mutex guards the store; every public store call
 * and the fire pass take it. The checker task has a deliberately small stack
 * (P4_CONFIG_ALARM_TASK_STACK) and heap-allocates anything larger than one
 * event.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "alarm.h"
#include "audio.h"
#include "led.h"
#include "shell.h"
#include "storage.h"
#include "bsp/esp-bsp.h"

#define ALARM_TAG               "alarm"
#define SHELL_SD_PATH_BYTES     P4_CONFIG_SD_PATH_BYTES

#define ALARM_INDEX_FILE        "INDEX.INI"
#define ALARM_INDEX_VERSION     1

/* ------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static bool s_initialized;
static bool s_catchup_done;
static TaskHandle_t s_checker_task;
static SemaphoreHandle_t s_mutex;
static alarm_host_ops_t s_host_ops;

/* ------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

static void alarm_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void alarm_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

/** Resolve the alarm store base directory (absolute VFS path). */
static esp_err_t alarm_base_dir(char *out, size_t out_size)
{
    if (shell_fs_resolve_path(P4_CONFIG_ALARM_PATH, out, out_size) != ESP_OK) {
        /* Fall back to the literal absolute path under the mount point. */
        snprintf(out, out_size, "%s/ALARMS", BSP_SD_MOUNT_POINT);
    }
    return ESP_OK;
}

/** Build the resolved path of an event file into @p out. */
static void alarm_event_path(const char *base, uint32_t id, char *out, size_t out_size)
{
    /* Bound the base so the "/E<id>.INI" suffix always fits. */
    int base_room = (int)(out_size - 16);
    if (base_room < 0) {
        base_room = 0;
    }
    snprintf(out, out_size, "%.*s/E%06lu.INI", base_room, base, (unsigned long)id);
}

/** Build the resolved path of INDEX.INI. */
static void alarm_index_path(const char *base, char *out, size_t out_size)
{
    int base_room = (int)(out_size - 16);
    if (base_room < 0) {
        base_room = 0;
    }
    snprintf(out, out_size, "%.*s/%s", base_room, base, ALARM_INDEX_FILE);
}

/** True when the event is armed and not yet fired. */
static bool alarm_is_pending(const alarm_event_t *e, time_t now)
{
    if (e->flags & ALARM_FLAG_FIRED) {
        return false;
    }
    if (!(e->flags & ALARM_FLAG_ENABLED)) {
        return false;
    }
    return e->when <= now;
}

/* ------------------------------------------------------------------------
 * Pure helpers (unit-testable, no SD / no commands)
 * ---------------------------------------------------------------------- */

/** True when a WEEKLY recurrence's weekday mask includes @p wday (0=Sun..6=Sat). */
bool alarm_recur_weekday_matches(uint8_t recur, int wday)
{
    if (!(recur & ALARM_RECUR_WEEKLY)) {
        return false;
    }
    if (wday < 0 || wday > 6) {
        return false;
    }
    return (recur & (uint8_t)(1u << wday)) != 0;
}

/** Advance a fired recurrence to its next occurrence strictly after @p when. */
time_t alarm_advance_recur(time_t when, uint8_t recur)
{
    struct tm tmv;

    if (recur == ALARM_RECUR_DAILY) {
        return when + 86400;
    }
    if (recur & ALARM_RECUR_WEEKLY) {
        time_t next = when + 86400;
        int i;
        for (i = 0; i < 7; i++) {
            if (localtime_r(&next, &tmv) == NULL) {
                break;
            }
            if (alarm_recur_weekday_matches(recur, tmv.tm_wday)) {
                return next;
            }
            next += 86400;
        }
        return when + 7 * 86400;   /* fallback: same weekday next week */
    }
    return when;   /* non-recurring: no advance */
}

/** Days in a month (proleptic Gregorian; pure). */
static int alarm_month_days(int year, int month)
{
    static const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days;

    if (month < 1 || month > 12) {
        return 0;
    }
    days = table[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        days = 29;
    }
    return days;
}

/** Date of the nth weekday (1..5, -1 = last) in a month, 0 when absent. */
static int alarm_nth_weekday(int year, int month, int wday, int nth)
{
    struct tm probe;
    time_t stamp;
    int first_wday;
    int date;

    if (wday < 0 || wday > 6 || nth == 0 || nth < -1 || nth > 5) {
        return 0;
    }
    memset(&probe, 0, sizeof(probe));
    probe.tm_year = year - 1900;
    probe.tm_mon = month - 1;
    probe.tm_mday = 1;
    probe.tm_isdst = -1;
    stamp = mktime(&probe);
    if (stamp == (time_t)-1 || localtime_r(&stamp, &probe) == NULL) {
        return 0;
    }
    first_wday = probe.tm_wday;
    if (nth > 0) {
        date = 1 + ((wday - first_wday + 7) % 7) + (nth - 1) * 7;
        return (date <= alarm_month_days(year, month)) ? date : 0;
    }
    /* Last occurrence: walk back from month end. */
    date = alarm_month_days(year, month);
    memset(&probe, 0, sizeof(probe));
    probe.tm_year = year - 1900;
    probe.tm_mon = month - 1;
    probe.tm_mday = date;
    probe.tm_isdst = -1;
    stamp = mktime(&probe);
    if (stamp == (time_t)-1 || localtime_r(&stamp, &probe) == NULL) {
        return 0;
    }
    date -= (probe.tm_wday - wday + 7) % 7;
    return (date >= 1) ? date : 0;
}

time_t alarm_advance_monthly(time_t when, int day, int nth, int wday)
{
    struct tm base;
    int k;

    if (localtime_r(&when, &base) == NULL) {
        return when + 30 * 86400;
    }
    if (nth == 0 && (day < 1 || day > 31)) {
        day = base.tm_mday;
    }
    if (nth != 0 && (wday < 0 || wday > 6)) {
        wday = base.tm_wday;
    }
    for (k = 1; k <= 24; k++) {
        int total = base.tm_mon + k; /* tm_mon is 0-based */
        int y = base.tm_year + 1900 + total / 12;
        int m = total % 12 + 1;
        int target = (nth != 0) ? alarm_nth_weekday(y, m, wday, nth)
                                : ((day <= alarm_month_days(y, m)) ? day : 0);
        if (target == 0) {
            continue; /* short month: skipped, like wall calendars */
        }
        {
            struct tm cand = base;
            time_t t;
            cand.tm_year = y - 1900;
            cand.tm_mon = m - 1;
            cand.tm_mday = target;
            cand.tm_isdst = -1;
            t = mktime(&cand);
            if (t != (time_t)-1 && t > when) {
                return t;
            }
        }
    }
    return when + 30 * 86400;
}

time_t alarm_advance_yearly(time_t when, int month, int day)
{
    struct tm base;
    int k;

    if (localtime_r(&when, &base) == NULL) {
        return when + 365 * 86400;
    }
    if (month < 1 || month > 12) {
        month = base.tm_mon + 1;
    }
    if (day < 1 || day > 31) {
        day = base.tm_mday;
    }
    for (k = 1; k <= 10; k++) {
        int y = base.tm_year + 1900 + k;
        if (day > alarm_month_days(y, month)) {
            continue; /* Feb 29 in a common year: skipped */
        }
        {
            struct tm cand = base;
            time_t t;
            cand.tm_year = y - 1900;
            cand.tm_mon = month - 1;
            cand.tm_mday = day;
            cand.tm_isdst = -1;
            t = mktime(&cand);
            if (t != (time_t)-1 && t > when) {
                return t;
            }
        }
    }
    return when + 365 * 86400;
}

time_t alarm_advance_event(time_t when, const alarm_event_t *e)
{
    struct tm tmv;

    if (e == NULL) {
        return when;
    }
    if (e->recur == ALARM_RECUR_MONTHLY) {
        int day = (e->recur_day >= 1 && e->recur_day <= 31) ? e->recur_day : 0;
        int wday = 0;
        if (e->recur_nth != 0) {
            if (localtime_r(&when, &tmv) == NULL) {
                return when + 30 * 86400;
            }
            wday = tmv.tm_wday;
        }
        return alarm_advance_monthly(when, day, e->recur_nth, wday);
    }
    if (e->recur == ALARM_RECUR_YEARLY) {
        return alarm_advance_yearly(when, e->recur_month, e->recur_day);
    }
    return alarm_advance_recur(when, e->recur);
}

/**
 * Parse a local "YYYY-MM-DD" date and "HH:MM[:SS]" time into a Unix timestamp
 * (timezone-aware via mktime, matching the `date`/`time` commands). Returns
 * false on malformed input or an out-of-range date.
 */
bool alarm_parse_datetime(const char *date, const char *time_str, time_t *out)
{
    struct tm tmv;
    int year = 0, mon = 0, day = 0;
    int hour = 0, min = 0, sec = 0;
    int matched;
    time_t stamp;

    if (date == NULL || time_str == NULL || out == NULL) {
        return false;
    }
    matched = sscanf(date, "%d-%d-%d", &year, &mon, &day);
    if (matched != 3 || year < 1970 || year > 2099 || mon < 1 || mon > 12 ||
        day < 1 || day > 31) {
        return false;
    }
    matched = sscanf(time_str, "%d:%d:%d", &hour, &min, &sec);
    if (matched < 2 || hour < 0 || hour > 23 || min < 0 || min > 59 ||
        sec < 0 || sec > 59) {
        return false;
    }

    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;
    stamp = mktime(&tmv);
    if (stamp == (time_t)-1) {
        return false;
    }
    *out = stamp;
    return true;
}

/* ------------------------------------------------------------------------
 * Event file read/write
 * ---------------------------------------------------------------------- */

/** Write an event atomically (builds the INI text, one atomic write). */
static esp_err_t alarm_event_save(const char *base, const alarm_event_t *e)
{
    char path[SHELL_SD_PATH_BYTES];
    size_t cap = P4_CONFIG_ALARM_TITLE_BYTES + P4_CONFIG_ALARM_MSG_BYTES +
                 P4_CONFIG_SD_PATH_BYTES + 256;
    char *text;
    esp_err_t error;

    alarm_event_path(base, e->id, path, sizeof(path));
    text = malloc(cap);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if P4_CONFIG_ALARM_ENABLE_RUN_ACTION
    snprintf(text, cap,
             "id=%lu\n"
             "when=%lld\n"
             "title=%s\n"
             "msg=%s\n"
             "flags=%u\n"
             "recur=%u\n"
             "recur_day=%u\n"
             "recur_month=%u\n"
             "recur_nth=%d\n"
             "action=%u\n"
             "run=%s\n",
             (unsigned long)e->id, (long long)e->when, e->title, e->msg,
             (unsigned)e->flags, (unsigned)e->recur,
             (unsigned)e->recur_day, (unsigned)e->recur_month, (int)e->recur_nth,
             (unsigned)e->action, e->run);
#else
    snprintf(text, cap,
             "id=%lu\n"
             "when=%lld\n"
             "title=%s\n"
             "msg=%s\n"
             "flags=%u\n"
             "recur=%u\n"
             "recur_day=%u\n"
             "recur_month=%u\n"
             "recur_nth=%d\n"
             "action=%u\n",
             (unsigned long)e->id, (long long)e->when, e->title, e->msg,
             (unsigned)e->flags, (unsigned)e->recur,
             (unsigned)e->recur_day, (unsigned)e->recur_month, (int)e->recur_nth,
             (unsigned)e->action);
#endif
    error = storage_write_text_file(path, text);
    free(text);
    return error;
}

/** Read one event file. Returns ESP_ERR_NOT_FOUND when absent/unreadable. */
static esp_err_t alarm_event_load(const char *base, uint32_t id, alarm_event_t *e)
{
    char path[SHELL_SD_PATH_BYTES];
    char value[24];
    esp_err_t error;
    long long when = 0;

    alarm_event_path(base, id, path, sizeof(path));
    memset(e, 0, sizeof(*e));
    e->id = id;

    error = storage_ini_file_get(path, "when", value, sizeof(value));
    if (error != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    when = atoll(value);
    e->when = (time_t)when;

    if (storage_ini_file_get(path, "title", e->title, sizeof(e->title)) != ESP_OK) {
        e->title[0] = '\0';
    }
    if (storage_ini_file_get(path, "msg", e->msg, sizeof(e->msg)) != ESP_OK) {
        e->msg[0] = '\0';
    }
    if (storage_ini_file_get(path, "flags", value, sizeof(value)) == ESP_OK) {
        e->flags = (uint8_t)atoi(value);
    }
    if (storage_ini_file_get(path, "recur", value, sizeof(value)) == ESP_OK) {
        e->recur = (uint8_t)atoi(value);
    }
    /* Monthly/yearly params are optional: older files predate them. */
    if (storage_ini_file_get(path, "recur_day", value, sizeof(value)) == ESP_OK) {
        int day = atoi(value);
        e->recur_day = (day >= 1 && day <= 31) ? (uint8_t)day : 0;
    }
    if (storage_ini_file_get(path, "recur_month", value, sizeof(value)) == ESP_OK) {
        int month = atoi(value);
        e->recur_month = (month >= 1 && month <= 12) ? (uint8_t)month : 0;
    }
    if (storage_ini_file_get(path, "recur_nth", value, sizeof(value)) == ESP_OK) {
        int nth = atoi(value);
        e->recur_nth = (nth >= -1 && nth <= 5 && nth != 0) ? (int8_t)nth : 0;
    }
    if (storage_ini_file_get(path, "action", value, sizeof(value)) == ESP_OK) {
        e->action = (uint8_t)atoi(value);
    }
#if P4_CONFIG_ALARM_ENABLE_RUN_ACTION
    if (storage_ini_file_get(path, "run", e->run, sizeof(e->run)) != ESP_OK) {
        e->run[0] = '\0';
    }
#endif
    return ESP_OK;
}

/** True when a path names an event file (E<6 digits>.INI). */
static bool alarm_name_is_event(const char *name)
{
    size_t len = strlen(name);

    /* "E" + 6 digits + ".INI" = 11 characters. */
    if (len != 11 || strncmp(name, "E", 1) != 0 ||
        strcmp(name + len - 4, ".INI") != 0) {
        return false;
    }
    for (size_t i = 1; i < len - 4; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

/** Extract the id from an event file name. */
static uint32_t alarm_name_id(const char *name)
{
    return (uint32_t)strtoul(name + 1, NULL, 10);
}

/* ------------------------------------------------------------------------
 * INDEX.INI helpers
 * ---------------------------------------------------------------------- */

static uint32_t alarm_index_get(const char *base, const char *key)
{
    char path[SHELL_SD_PATH_BYTES];
    char value[24];

    alarm_index_path(base, path, sizeof(path));
    if (storage_ini_file_get(path, key, value, sizeof(value)) != ESP_OK) {
        return 0;
    }
    return (uint32_t)strtoul(value, NULL, 10);
}

static void alarm_index_set(const char *base, const char *key, uint32_t v)
{
    char path[SHELL_SD_PATH_BYTES];
    char value[24];

    snprintf(value, sizeof(value), "%lu", (unsigned long)v);
    alarm_index_path(base, path, sizeof(path));
    (void)storage_ini_file_set(path, key, value);
}

/** Ensure the ALARMS directory exists (ignore "already exists"). */
static esp_err_t alarm_ensure_dir(const char *base)
{
    char *copy;
    char *p;
    shell_sd_session_t session;
    esp_err_t error = ESP_OK;

    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    copy = strdup(base);
    if (copy == NULL) {
        shell_sd_end(&session, ALARM_TAG);
        return ESP_ERR_NO_MEM;
    }
    for (p = copy; *p != '\0'; p++) {
        if (*p == '/') {
            char save = *p;
            *p = '\0';
            if (copy[0] != '\0' && mkdir(copy, 0755) != 0 && errno != EEXIST) {
                error = ESP_FAIL;
                *p = save;
                break;
            }
            *p = save;
        }
    }
    if (error == ESP_OK && copy[0] != '\0' && mkdir(copy, 0755) != 0 && errno != EEXIST) {
        error = ESP_FAIL;
    }
    free(copy);
    shell_sd_end(&session, ALARM_TAG);
    return error;
}

/* ------------------------------------------------------------------------
 * Store API
 * ---------------------------------------------------------------------- */

esp_err_t alarm_add(const char *title, const char *msg, time_t when,
                    uint8_t recur, uint8_t action, const char *run_path,
                    uint32_t *out_id)
{
    return alarm_add_ex(title, msg, when, recur, action, run_path, NULL, out_id);
}

esp_err_t alarm_add_ex(const char *title, const char *msg, time_t when,
                       uint8_t recur, uint8_t action, const char *run_path,
                       const alarm_recur_params_t *params, uint32_t *out_id)
{
    char base[SHELL_SD_PATH_BYTES];
    alarm_event_t e;
    uint32_t next_id;
    uint32_t count;
    esp_err_t error;

    if (title == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (recur != ALARM_RECUR_NONE && recur != ALARM_RECUR_DAILY &&
        recur != ALARM_RECUR_MONTHLY && recur != ALARM_RECUR_YEARLY &&
        !(recur & ALARM_RECUR_WEEKLY)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (params != NULL) {
        if (params->day < 0 || params->day > 31 ||
            params->month < 0 || params->month > 12 ||
            params->nth < -1 || params->nth > 5) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (action == 0) {
        action = ALARM_ACTION_NOTIFY;
    }

    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    error = alarm_ensure_dir(base);
    if (error != ESP_OK) {
        alarm_unlock();
        return error;
    }

    next_id = alarm_index_get(base, "next_id");
    if (next_id == 0) {
        next_id = 1;
        alarm_index_set(base, "version", ALARM_INDEX_VERSION);
    }
    count = alarm_index_get(base, "count");
    if (count >= P4_CONFIG_ALARM_MAX_EVENTS) {
        alarm_unlock();
        return ESP_ERR_NO_MEM;
    }

    memset(&e, 0, sizeof(e));
    e.id = next_id;
    e.when = when;
    snprintf(e.title, sizeof(e.title), "%s", title);
    if (msg != NULL) {
        snprintf(e.msg, sizeof(e.msg), "%s", msg);
    }
    e.flags = ALARM_FLAG_ENABLED;
    e.recur = recur;
    e.action = action;
    /* Monthly/yearly params default from `when` (nth stays monthday mode). */
    if (params != NULL) {
        e.recur_day = (uint8_t)params->day;
        e.recur_month = (uint8_t)params->month;
        e.recur_nth = (int8_t)params->nth;
    }
    if (recur == ALARM_RECUR_MONTHLY || recur == ALARM_RECUR_YEARLY) {
        struct tm tmv;
        if (localtime_r(&when, &tmv) != NULL) {
            if (e.recur_day == 0) {
                e.recur_day = (uint8_t)tmv.tm_mday;
            }
            if (recur == ALARM_RECUR_YEARLY && e.recur_month == 0) {
                e.recur_month = (uint8_t)(tmv.tm_mon + 1);
            }
        }
    }
#if P4_CONFIG_ALARM_ENABLE_RUN_ACTION
    if (run_path != NULL) {
        snprintf(e.run, sizeof(e.run), "%s", run_path);
    }
#endif

    error = alarm_event_save(base, &e);
    if (error != ESP_OK) {
        alarm_unlock();
        return error;
    }
    alarm_index_set(base, "next_id", next_id + 1);
    alarm_index_set(base, "count", count + 1);
    if (out_id != NULL) {
        *out_id = next_id;
    }
    alarm_unlock();
    return ESP_OK;
}

esp_err_t alarm_get(uint32_t id, alarm_event_t *out)
{
    char base[SHELL_SD_PATH_BYTES];
    esp_err_t error;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    error = alarm_event_load(base, id, out);
    alarm_unlock();
    return error;
}

esp_err_t alarm_list(bool (*cb)(const alarm_event_t *event, void *ctx), void *ctx)
{
    char base[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir = NULL;
    struct dirent *entry;
    uint32_t ids[P4_CONFIG_ALARM_MAX_EVENTS];
    int count = 0;
    int i;
    esp_err_t error = ESP_OK;

    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    alarm_lock();
    alarm_base_dir(base, sizeof(base));

    if (shell_sd_begin(&session) != ESP_OK) {
        alarm_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    dir = opendir(base);
    if (dir == NULL) {
        shell_sd_end(&session, ALARM_TAG);
        alarm_unlock();
        return ESP_OK;   /* no store yet = empty */
    }
    while ((entry = readdir(dir)) != NULL && count < P4_CONFIG_ALARM_MAX_EVENTS) {
        if (alarm_name_is_event(entry->d_name)) {
            ids[count++] = alarm_name_id(entry->d_name);
        }
    }
    closedir(dir);
    shell_sd_end(&session, ALARM_TAG);

    /* Insertion-sort the ids so output is in id order. */
    for (i = 1; i < count; i++) {
        uint32_t key = ids[i];
        int j = i - 1;
        while (j >= 0 && ids[j] > key) {
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = key;
    }

    for (i = 0; i < count; i++) {
        alarm_event_t e;
        if (alarm_event_load(base, ids[i], &e) != ESP_OK) {
            continue;
        }
        if (!cb(&e, ctx)) {
            break;
        }
    }
    alarm_unlock();
    return error;
}

esp_err_t alarm_del(uint32_t id, bool permanent)
{
    char base[SHELL_SD_PATH_BYTES];
    char path[SHELL_SD_PATH_BYTES];
    alarm_event_t e;
    uint32_t count;
    esp_err_t error;

    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    error = alarm_event_load(base, id, &e);
    if (error != ESP_OK) {
        alarm_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (permanent) {
        alarm_event_path(base, id, path, sizeof(path));
        {
            shell_sd_session_t session;
            if (shell_sd_begin(&session) == ESP_OK) {
                remove(path);
                shell_sd_end(&session, ALARM_TAG);
            }
        }
        count = alarm_index_get(base, "count");
        if (count > 0) {
            alarm_index_set(base, "count", count - 1);
        }
    } else {
        /* Soft delete: mark fired + disabled so it stays until purge. */
        e.flags |= ALARM_FLAG_FIRED;
        e.flags &= (uint8_t)~ALARM_FLAG_ENABLED;
        error = alarm_event_save(base, &e);
        if (error != ESP_OK) {
            alarm_unlock();
            return error;
        }
    }
    alarm_unlock();
    return ESP_OK;
}

esp_err_t alarm_del_all(void)
{
    char base[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir = NULL;
    struct dirent *entry;
    uint32_t removed = 0;

    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    if (shell_sd_begin(&session) != ESP_OK) {
        alarm_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    dir = opendir(base);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (alarm_name_is_event(entry->d_name)) {
                char path[SHELL_SD_PATH_BYTES];
                alarm_event_path(base, alarm_name_id(entry->d_name), path, sizeof(path));
                if (remove(path) == 0) {
                    removed++;
                }
            }
        }
        closedir(dir);
    }
    shell_sd_end(&session, ALARM_TAG);

    /* Fresh id space: count=0, next_id=1. */
    alarm_index_set(base, "count", 0);
    alarm_index_set(base, "next_id", 1);
    alarm_index_set(base, "version", ALARM_INDEX_VERSION);
    alarm_unlock();
    return ESP_OK;
}

esp_err_t alarm_enable(uint32_t id, bool enable)
{
    char base[SHELL_SD_PATH_BYTES];
    alarm_event_t e;
    esp_err_t error;

    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    error = alarm_event_load(base, id, &e);
    if (error != ESP_OK) {
        alarm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (enable) {
        e.flags |= ALARM_FLAG_ENABLED;
        e.flags &= (uint8_t)~ALARM_FLAG_FIRED;
    } else {
        e.flags &= (uint8_t)~ALARM_FLAG_ENABLED;
    }
    error = alarm_event_save(base, &e);
    alarm_unlock();
    return error;
}

esp_err_t alarm_snooze(uint32_t id, int minutes)
{
    char base[SHELL_SD_PATH_BYTES];
    alarm_event_t e;
    esp_err_t error;

    if (minutes <= 0) {
        minutes = 10;
    } else if (minutes > 24 * 60) {
        minutes = 24 * 60;
    }
    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    error = alarm_event_load(base, id, &e);
    if (error != ESP_OK) {
        alarm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    e.when = time(NULL) + (time_t)minutes * 60;
    e.flags |= ALARM_FLAG_ENABLED;
    e.flags &= (uint8_t)~ALARM_FLAG_FIRED;
    error = alarm_event_save(base, &e);
    alarm_unlock();
    return error;
}

esp_err_t alarm_purge(void)
{
    char base[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir = NULL;
    struct dirent *entry;
    uint32_t removed = 0;

    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    if (shell_sd_begin(&session) != ESP_OK) {
        alarm_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    dir = opendir(base);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (alarm_name_is_event(entry->d_name)) {
                uint32_t id = alarm_name_id(entry->d_name);
                alarm_event_t e;
                char path[SHELL_SD_PATH_BYTES];

                if (alarm_event_load(base, id, &e) == ESP_OK &&
                    (e.flags & ALARM_FLAG_FIRED) &&
                    !(e.flags & ALARM_FLAG_ENABLED)) {
                    alarm_event_path(base, id, path, sizeof(path));
                    remove(path);
                    removed++;
                }
            }
        }
        closedir(dir);
    }
    shell_sd_end(&session, ALARM_TAG);
    if (removed > 0) {
        uint32_t count = alarm_index_get(base, "count");
        if (count >= removed) {
            alarm_index_set(base, "count", count - removed);
        } else {
            alarm_index_set(base, "count", 0);
        }
    }
    alarm_unlock();
    return ESP_OK;
}

esp_err_t alarm_status(alarm_status_t *out)
{
    char base[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir = NULL;
    struct dirent *entry;
    time_t next = 0;
    uint32_t count = 0;
    uint32_t enabled = 0;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    if (shell_sd_begin(&session) == ESP_OK) {
        dir = opendir(base);
        if (dir != NULL) {
            while ((entry = readdir(dir)) != NULL && count < P4_CONFIG_ALARM_MAX_EVENTS) {
                if (alarm_name_is_event(entry->d_name)) {
                    alarm_event_t e;
                    count++;
                    if (alarm_event_load(base, alarm_name_id(entry->d_name), &e) == ESP_OK) {
                        if (e.flags & ALARM_FLAG_ENABLED) {
                            enabled++;
                            if (!(e.flags & ALARM_FLAG_FIRED) &&
                                (next == 0 || e.when < next)) {
                                next = e.when;
                            }
                        }
                    }
                }
            }
            closedir(dir);
        }
        shell_sd_end(&session, ALARM_TAG);
    }
    alarm_unlock();

    out->count = count;
    out->enabled = enabled;
    out->next_due = next;
    out->checker_running = alarm_checker_running();
    out->catchup_done = s_catchup_done;
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Firing
 * ---------------------------------------------------------------------- */

/** Perform one event's actions (called outside the store mutex). */
static void alarm_fire_event(const alarm_event_t *e)
{
    char notify[P4_CONFIG_HEADER_NOTIFICATION_BYTES];

    if (e->action & ALARM_ACTION_NOTIFY) {
        size_t used = 0;

        used += (size_t)snprintf(notify + used, sizeof(notify) - used, "%s", e->title);
        if (e->msg[0] != '\0' && used + 3 < sizeof(notify)) {
            used += (size_t)snprintf(notify + used, sizeof(notify) - used, " - ");
            if (used < sizeof(notify)) {
                (void)snprintf(notify + used, sizeof(notify) - used, "%.*s",
                               (int)(sizeof(notify) - used - 1), e->msg);
            }
        }
        shell_header_notify_level(notify,
                                  (uint32_t)P4_CONFIG_ALARM_DEFAULT_NOTIFY_SECS * 1000u,
                                  HEADER_NOTIFY_WARN);
    }

    if (!(e->flags & ALARM_FLAG_SILENT)) {
        if (e->action & ALARM_ACTION_BEEP) {
            (void)audio_play_tone(880, 300);
        }
        if (e->action & ALARM_ACTION_LED) {
            led_notify(LED_EVENT_ALARM);
        }
    }

#if P4_CONFIG_ALARM_ENABLE_RUN_ACTION
    if ((e->action & ALARM_ACTION_RUN) && e->run[0] != '\0' &&
        s_host_ops.execute_async != NULL) {
        /* Queue the batch file onto the command worker — never run it on the
         * checker's stack. "call <path>" gives the batch engine full
         * expansion / redirection / ERRORLEVEL semantics. */
        char command[P4_CONFIG_SD_PATH_BYTES + 8];
        snprintf(command, sizeof(command), "call %s", e->run);
        s_host_ops.execute_async(command);
    }
#endif
}

/**
 * One fire pass: mark + persist every due event under the mutex (advancing
 * recurring events to their next occurrence), then fire their actions outside
 * the mutex. Heap-allocates the due-event list (never the checker stack).
 */
void alarm_tick(void)
{
    char base[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir = NULL;
    struct dirent *entry;
    time_t now = time(NULL);
    alarm_event_t *due;
    int due_count = 0;
    int cap = P4_CONFIG_ALARM_MAX_EVENTS;
    int i;

    if (!s_initialized) {
        return;
    }

    due = malloc((size_t)cap * sizeof(alarm_event_t));
    if (due == NULL) {
        ESP_LOGW(ALARM_TAG, "tick: out of memory collecting due events");
        return;
    }

    alarm_lock();
    alarm_base_dir(base, sizeof(base));
    if (shell_sd_begin(&session) != ESP_OK) {
        alarm_unlock();
        free(due);
        return;
    }
    dir = opendir(base);
    if (dir == NULL) {
        shell_sd_end(&session, ALARM_TAG);
        alarm_unlock();
        free(due);
        return;
    }

    while ((entry = readdir(dir)) != NULL && due_count < cap) {
        alarm_event_t e;

        if (!alarm_name_is_event(entry->d_name)) {
            continue;
        }
        if (alarm_event_load(base, alarm_name_id(entry->d_name), &e) != ESP_OK) {
            continue;
        }
        if (!alarm_is_pending(&e, now)) {
            continue;
        }

        /* Persist the post-fire state: recurring advances, one-shot fires. */
        if (e.recur == ALARM_RECUR_NONE) {
            e.flags |= ALARM_FLAG_FIRED;
            e.flags &= (uint8_t)~ALARM_FLAG_ENABLED;
        } else {
            time_t next = alarm_advance_event(e.when, &e);
            int guard = 0;
            while (next <= now && guard < 40) {
                next = alarm_advance_event(next, &e);
                guard++;
            }
            e.when = next;
        }
        (void)alarm_event_save(base, &e);
        due[due_count++] = e;
    }
    closedir(dir);
    shell_sd_end(&session, ALARM_TAG);
    alarm_unlock();

    /* Fire actions outside the mutex so a queued /run: never blocks the store. */
    for (i = 0; i < due_count; i++) {
        alarm_fire_event(&due[i]);
    }
    free(due);
}

/* ------------------------------------------------------------------------
 * Checker task + lifecycle
 * ---------------------------------------------------------------------- */

static void alarm_checker_task(void *arg)
{
    (void)arg;
    /* Let boot settle before the first pass (SD mount, clock, catch-up). */
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        alarm_tick();
        vTaskDelay(pdMS_TO_TICKS(P4_CONFIG_ALARM_POLL_MS));
    }
}

void alarm_register_host_ops(const alarm_host_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_host_ops, 0, sizeof(s_host_ops));
        return;
    }
    memcpy(&s_host_ops, ops, sizeof(s_host_ops));
}

bool alarm_checker_running(void)
{
    return s_checker_task != NULL;
}

bool alarm_is_initialized(void)
{
    return s_initialized;
}

esp_err_t alarm_init(void)
{
    char base[SHELL_SD_PATH_BYTES];

    if (s_initialized) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(ALARM_TAG, "out of memory creating the store mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Make sure the store directory exists (best-effort; the checker retries). */
    alarm_base_dir(base, sizeof(base));
    (void)alarm_ensure_dir(base);

    /* Boot catch-up: fire anything that became due while powered off. */
#if P4_CONFIG_ALARM_CATCHUP_ON_BOOT
    alarm_tick();
    s_catchup_done = true;
#endif

    if (xTaskCreateWithCaps(alarm_checker_task, "alarm",
                    P4_CONFIG_ALARM_TASK_STACK, NULL,
                    P4_CONFIG_ALARM_TASK_PRIORITY, &s_checker_task,
                    MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(ALARM_TAG, "failed to create the checker task");
        s_checker_task = NULL;
        s_initialized = true;   /* store still works; no background firing */
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(ALARM_TAG, "alarm store ready (%s)", base);
    return ESP_OK;
}

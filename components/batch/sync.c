/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file sync.c
 * @brief Batch synchronization: named mutexes + named events for jobs.
 *
 * Real-concurrency coordination without a concurrency-model change: one
 * mutex table and one event table in the batch engine (never a second
 * implementation per caller). Entries are owner-tracked and released
 * automatically when the owning job exits (batch_bg_release), so a dead
 * job can never wedge a mutex. Signals are sticky until taken; `wait`
 * auto-clears on take, so one signal wakes exactly one waiter.
 *
 * Blocking calls wait in 100 ms chunks and poll the same cooperative
 * points as `delay` (kill flag, auto-kill deadline, foreground break), so
 * `taskkill`, `start /t:`, and Ctrl+C stay responsive mid-wait.
 */

#include "batch.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "modal_surf.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "shell.h"

#ifndef SYNC_TAG
#define SYNC_TAG "sync"
#endif

/** Chunk for every blocking wait (kill/deadline/abort stay responsive). */
#define SYNC_POLL_MS 100

/* Own mutex for the sync tables (separate from the batch shared lock:
 * no shared data, so no lock ordering to get wrong). */
static SemaphoreHandle_t s_sync_lock = NULL;

static void sync_lock_init(void)
{
    if (s_sync_lock == NULL) {
        s_sync_lock = xSemaphoreCreateMutex();
    }
}

static void sync_take(void)
{
    sync_lock_init();
    if (s_sync_lock != NULL) {
        xSemaphoreTake(s_sync_lock, portMAX_DELAY);
    }
}

static void sync_give(void)
{
    if (s_sync_lock != NULL) {
        xSemaphoreGive(s_sync_lock);
    }
}

/** One named mutex. Entries persist (reusable names); owner -1 = free. */
typedef struct {
    bool used;
    char name[P4_CONFIG_SYNC_NAME_BYTES];
    SemaphoreHandle_t mutex;
    int owner_slot;
} sync_mutex_t;

/** One named event (sticky until taken). */
typedef struct {
    bool used;
    char name[P4_CONFIG_SYNC_NAME_BYTES];
    EventBits_t bit;
} sync_event_t;

/* Bulk tables live in PSRAM (allocated on first use), never BSS: every
 * internal-RAM byte counts in the boot window that the command worker
 * (32 KB contiguous) and the Wi-Fi task (12 KB) need. A failed allocation
 * degrades to clean ESP_ERR_NO_MEM refusals. */
static sync_mutex_t *s_mutexes = NULL;
static sync_event_t *s_events = NULL;
static EventGroupHandle_t s_event_group = NULL;
static EventBits_t s_event_bits = 0;

/** Ensure the tables exist (PSRAM). False when out of memory. */
static bool sync_ensure_tables(void)
{
    if (s_mutexes == NULL) {
        s_mutexes = p4heap_calloc_psram(P4_CONFIG_SYNC_MUTEX_MAX, sizeof(*s_mutexes));
        if (s_mutexes == NULL) {
            return false;
        }
    }
    if (s_events == NULL) {
        s_events = p4heap_calloc_psram(P4_CONFIG_SYNC_EVENT_MAX, sizeof(*s_events));
        if (s_events == NULL) {
            return false;
        }
    }
    return true;
}

/** Normalize a sync name in place terms: non-empty, fits, identifier-ish.
 *  Pure, unit-tested. */
bool sync_name_ok(const char *name)
{
    size_t len;

    if (name == NULL || *name == '\0') {
        return false;
    }
    len = strlen(name);
    if (len >= (size_t)P4_CONFIG_SYNC_NAME_BYTES) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];

        if (!(c == '_' || c == '-' ||
              (c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            return false;
        }
    }
    return true;
}

/** Find a mutex by exact name (case-sensitive: sync names are not env
 *  vars), or NULL. NULL-safe before the tables exist. */
static sync_mutex_t *sync_mutex_find(const char *name)
{
    if (s_mutexes == NULL) {
        return NULL;
    }
    for (int i = 0; i < (int)P4_CONFIG_SYNC_MUTEX_MAX; i++) {
        if (s_mutexes[i].used && strcmp(s_mutexes[i].name, name) == 0) {
            return &s_mutexes[i];
        }
    }
    return NULL;
}

/** Find an event by exact name, or NULL. NULL-safe before tables exist. */
static sync_event_t *sync_event_find(const char *name)
{
    if (s_events == NULL) {
        return NULL;
    }
    for (int i = 0; i < (int)P4_CONFIG_SYNC_EVENT_MAX; i++) {
        if (s_events[i].used && strcmp(s_events[i].name, name) == 0) {
            return &s_events[i];
        }
    }
    return NULL;
}

/** True while the calling task must keep waiting (no kill, no deadline,
 *  no foreground break). One shared predicate for every blocking wait. */
static bool sync_wait_continue(void)
{
    if (batch_bg_kill_requested()) {
        return false;
    }
    batch_bg_poll_timeout();
    if (batch_bg_kill_requested()) {
        return false;
    }
    if (shell_abort_requested()) {
        shell_mark_foreground_break();
        return false;
    }
    return true;
}

esp_err_t sync_mutex_lock(const char *name, uint32_t timeout_ms, int slot)
{
    sync_mutex_t *m;
    uint32_t waited = 0;

    if (!sync_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot < 0 || slot > (int)P4_CONFIG_BG_TASKS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sync_ensure_tables()) {
        return ESP_ERR_NO_MEM;
    }
    sync_take();
    m = sync_mutex_find(name);
    if (m == NULL) {
        for (int i = 0; i < (int)P4_CONFIG_SYNC_MUTEX_MAX; i++) {
            if (!s_mutexes[i].used) {
                m = &s_mutexes[i];
                break;
            }
        }
        if (m == NULL) {
            sync_give();
            return ESP_ERR_NO_MEM;
        }
        m->mutex = xSemaphoreCreateMutex();
        if (m->mutex == NULL) {
            sync_give();
            return ESP_ERR_NO_MEM;
        }
        snprintf(m->name, sizeof(m->name), "%s", name);
        m->used = true;
        m->owner_slot = -1;
    }
    sync_give();
    /* Recursive take by the owner is a no-op success (batch has no
     * re-entrancy tracking need; DOS SHARE-style simplicity). */
    if (m->owner_slot == slot) {
        return ESP_OK;
    }
    for (;;) {
        if (xSemaphoreTake(m->mutex, pdMS_TO_TICKS(SYNC_POLL_MS)) == pdTRUE) {
            m->owner_slot = slot;
            return ESP_OK;
        }
        if (timeout_ms > 0) {
            waited += SYNC_POLL_MS;
            if (waited >= timeout_ms) {
                return ESP_ERR_TIMEOUT;
            }
        }
        if (!sync_wait_continue()) {
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t sync_mutex_unlock(const char *name, int slot)
{
    sync_mutex_t *m;

    if (!sync_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    sync_take();
    m = sync_mutex_find(name);
    if (m == NULL) {
        sync_give();
        return ESP_ERR_NOT_FOUND;
    }
    /* Owner, or the foreground supervisor acting over a job's mutex. */
    if (m->owner_slot != slot && slot != 0) {
        sync_give();
        return ESP_ERR_INVALID_STATE;
    }
    if (m->owner_slot < 0) {
        sync_give();
        return ESP_ERR_INVALID_STATE;
    }
    m->owner_slot = -1;
    xSemaphoreGive(m->mutex);
    sync_give();
    return ESP_OK;
}

void sync_release_owned(int slot)
{
    if (slot < 0 || slot > (int)P4_CONFIG_BG_TASKS) {
        return;
    }
    if (s_mutexes == NULL) {
        return;
    }
    /* A dead job can never wedge a mutex: force-release whatever it held.
     * Entries persist (names are reusable); only ownership clears. */
    sync_take();
    for (int i = 0; i < (int)P4_CONFIG_SYNC_MUTEX_MAX; i++) {
        if (s_mutexes[i].used && s_mutexes[i].owner_slot == slot) {
            s_mutexes[i].owner_slot = -1;
            xSemaphoreGive(s_mutexes[i].mutex);
        }
    }
    sync_give();
}

esp_err_t sync_event_signal(const char *name)
{
    sync_event_t *e;

    if (!sync_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sync_ensure_tables()) {
        return ESP_ERR_NO_MEM;
    }
    sync_take();
    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
        if (s_event_group == NULL) {
            sync_give();
            return ESP_ERR_NO_MEM;
        }
    }
    e = sync_event_find(name);
    if (e == NULL) {
        EventBits_t bit = 0;

        for (int i = 0; i < (int)P4_CONFIG_SYNC_EVENT_MAX; i++) {
            if (!s_events[i].used) {
                EventBits_t candidate = (EventBits_t)1u << (unsigned)i;

                if ((s_event_bits & candidate) == 0) {
                    bit = candidate;
                    e = &s_events[i];
                    break;
                }
            }
        }
        if (e == NULL) {
            sync_give();
            return ESP_ERR_NO_MEM;
        }
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->bit = bit;
        e->used = true;
        s_event_bits |= bit;
    }
    xEventGroupSetBits(s_event_group, e->bit);
    sync_give();
    return ESP_OK;
}

esp_err_t sync_event_wait(const char *name, uint32_t timeout_ms, int slot)
{
    sync_event_t *e;
    uint32_t waited = 0;
    EventBits_t bit;

    (void)slot;
    if (!sync_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    sync_take();
    e = sync_event_find(name);
    bit = (e != NULL) ? e->bit : 0;
    if (s_event_group == NULL || e == NULL) {
        /* No group / unknown event: create the entry so a later signal
         * finds it, then wait on its (clear) bit. */
        esp_err_t rc;

        sync_give();
        rc = sync_event_signal(name);
        if (rc != ESP_OK) {
            return rc;
        }
        xEventGroupClearBits(s_event_group, sync_event_find(name)->bit);
        return sync_event_wait(name, timeout_ms, slot);
    }
    sync_give();
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(s_event_group, bit,
                                              pdTRUE /* clear on take: one
                                                        signal wakes exactly
                                                        one waiter */,
                                              pdFALSE, pdMS_TO_TICKS(SYNC_POLL_MS));

        if ((got & bit) != 0) {
            return ESP_OK;
        }
        if (timeout_ms > 0) {
            waited += SYNC_POLL_MS;
            if (waited >= timeout_ms) {
                return ESP_ERR_TIMEOUT;
            }
        }
        if (!sync_wait_continue()) {
            return ESP_ERR_TIMEOUT;
        }
    }
}

/* -------------------------------------------------------------------------
 * VERBS (thin shells over the tables above)
 * ---------------------------------------------------------------------- */

void shell_command_lock(int argc, char **argv)
{
    const char *name = NULL;
    uint32_t timeout_ms = 0;
    esp_err_t rc;

    for (int i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (name == NULL) {
            name = argv[i];
        }
    }
    if (name == NULL) {
        shell_print_usage("Usage: lock <name> [/t:secs]");
        batch_set_errorlevel(2);
        return;
    }
    rc = sync_mutex_lock(name, timeout_ms, batch_bg_current_slot());
    if (rc == ESP_ERR_TIMEOUT) {
        shell_print_error("lock: timeout waiting for %s", name);
        batch_set_errorlevel(1);
        return;
    }
    if (rc != ESP_OK) {
        shell_print_error("lock: cannot lock %s", name);
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

void shell_command_unlock(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] == '\0') {
        shell_print_usage("Usage: unlock <name>");
        batch_set_errorlevel(2);
        return;
    }
    if (sync_mutex_unlock(argv[1], batch_bg_current_slot()) != ESP_OK) {
        shell_print_error("unlock: cannot unlock %s (bad name, foreign, or free)", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

void shell_command_signal(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] == '\0') {
        shell_print_usage("Usage: signal <name>");
        batch_set_errorlevel(2);
        return;
    }
    if (sync_event_signal(argv[1]) != ESP_OK) {
        shell_print_error("signal: cannot signal %s", argv[1]);
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

void shell_command_wait(int argc, char **argv)
{
    const char *name = NULL;
    uint32_t timeout_ms = 0;
    esp_err_t rc;

    for (int i = 1; i < argc; i++) {
        if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            continue;
        } else if (name == NULL) {
            name = argv[i];
        }
    }
    if (name == NULL) {
        shell_print_usage("Usage: wait <name> [/t:secs]");
        batch_set_errorlevel(2);
        return;
    }
    rc = sync_event_wait(name, timeout_ms, batch_bg_current_slot());
    if (rc == ESP_ERR_TIMEOUT) {
        shell_print_error("wait: timeout waiting for %s", name);
        batch_set_errorlevel(1);
        return;
    }
    if (rc != ESP_OK) {
        shell_print_error("wait: cannot wait for %s", name);
        batch_set_errorlevel(1);
        return;
    }
    batch_set_errorlevel(0);
}

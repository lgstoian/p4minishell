/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file svc.c
 * @brief Resident services: registry + supervisor (TSRs that survive).
 *
 * Named background lines persisted in sd:/SERVICES.INI through the shared
 * storage_ini core (no new parser — same shape as APPINFO/FRM/FLOW).
 * `service install` writes the registry; the supervisor (own FreeRTOS
 * timer, P4_CONFIG_SVC_TICK_MS) reaps unexpected exits and re-starts
 * `/restart` services within a per-boot budget; `batch_svc_boot()`
 * re-starts autostart entries after AUTOEXEC.BAT.
 *
 * Restart-on-unexpected-exit is the whole TSR contract: `service stop` is
 * the admin off switch (never restarted, never counted); `taskkill` of a
 * service job counts as unexpected and IS restarted. The pool that runs
 * jobs lives in components/command/ and is reached through the registered
 * batch_bg_ops_t table (same pattern as batch_command_ops_t).
 */

#include "batch.h"

#include <stdio.h>
#include <string.h>

#include "ansi_palette.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "storage.h"

#ifndef SVC_TAG
#define SVC_TAG "svc"
#endif

/** Registry file (resolved to /sdcard at use). */
#define SVC_REG_FILE "sd:/SERVICES.INI"

static svc_entry_t *s_svc = NULL;
static bool s_svc_loaded = false;
static const batch_bg_ops_t *s_bg_ops = NULL;
static TimerHandle_t s_svc_timer = NULL;

/* Forward: lazy supervisor-timer creation (defined below, used by
 * svc_start/svc_boot above it). */
static void svc_ensure_timer(void);

/** Ensure the RAM mirror exists (PSRAM, never BSS — bulk tables stay out
 *  of the boot internal-RAM window). False when out of memory. */
static bool svc_ensure_mirror(void)
{
    if (s_svc == NULL) {
        s_svc = p4heap_calloc_psram(P4_CONFIG_SVC_MAX, sizeof(*s_svc));
    }
    return s_svc != NULL;
}

void batch_register_bg_ops(const batch_bg_ops_t *ops)
{
    s_bg_ops = ops;
}

/** Resolve the registry path (sd:/ alias -> /sdcard/...). */
static bool svc_resolved(char *out, size_t out_size)
{
    return shell_fs_resolve_path(SVC_REG_FILE, out, out_size) == ESP_OK;
}

/** RAM mirror lookup by exact name, or NULL. NULL-safe before allocation. */
static svc_entry_t *svc_find(const char *name)
{
    if (s_svc == NULL) {
        return NULL;
    }
    for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
        if (s_svc[i].used && strcmp(s_svc[i].name, name) == 0) {
            return &s_svc[i];
        }
    }
    return NULL;
}

/** Free one entry's PSRAM line (mirror rebuild/removal). */
static void svc_free_line(svc_entry_t *e)
{
    if (e != NULL && e->line != NULL) {
        p4heap_free(e->line);
        e->line = NULL;
    }
}

/** foreach callback: fold "<name>.<field>" keys into the RAM mirror. */
static bool svc_load_cb(const char *key, const char *value, void *ctx)
{
    char name[P4_CONFIG_SVC_NAME_BYTES];
    const char *dot;

    (void)ctx;
    if (key == NULL || value == NULL) {
        return true;
    }
    dot = strrchr(key, '.');
    if (dot == NULL || (size_t)(dot - key) >= sizeof(name)) {
        return true;
    }
    memcpy(name, key, (size_t)(dot - key));
    name[dot - key] = '\0';
    if (!sync_name_ok(name)) {
        return true;
    }
    {
        svc_entry_t *e = svc_find(name);

        if (e == NULL) {
            for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
                if (!s_svc[i].used) {
                    e = &s_svc[i];
                    memset(e, 0, sizeof(*e));
                    e->used = true;
                    snprintf(e->name, sizeof(e->name), "%s", name);
                    break;
                }
            }
        }
        if (e == NULL) {
            return true;
        }
        if (strcmp(dot + 1, "line") == 0) {
            char *copy = p4heap_strdup_psram(value);

            if (copy != NULL) {
                svc_free_line(e);
                e->line = copy;
            }
        } else if (strcmp(dot + 1, "restart") == 0) {
            e->restart = (strcmp(value, "1") == 0);
        } else if (strcmp(dot + 1, "auto") == 0) {
            e->auto_start = (strcmp(value, "1") == 0);
        }
    }
    return true;
}

/** (Re)build the RAM mirror from the registry file (best effort: an
 *  absent SD card just leaves the mirror empty for a later retry). */
static void svc_load(void)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];

    if (!svc_ensure_mirror()) {
        return;
    }
    if (!svc_resolved(resolved, sizeof(resolved))) {
        return;
    }
    for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
        svc_free_line(&s_svc[i]);
    }
    memset(s_svc, 0, sizeof(*s_svc) * (size_t)P4_CONFIG_SVC_MAX);
    (void)storage_ini_file_foreach(resolved, svc_load_cb, NULL);
    s_svc_loaded = true;
}

svc_entry_t svc_describe(int index)
{
    svc_entry_t empty;

    memset(&empty, 0, sizeof(empty));
    if (s_svc == NULL || index < 0 || index >= (int)P4_CONFIG_SVC_MAX ||
        !s_svc[index].used) {
        return empty;
    }
    return s_svc[index];
}

int svc_count(void)
{
    int n = 0;

    if (s_svc == NULL) {
        return 0;
    }
    for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
        if (s_svc[i].used) {
            n++;
        }
    }
    return n;
}

/** Called from batch_bg_release (same component): a job's slot is going
 *  away — clear any service pointing at it (the supervisor reaps next).
 *  Skips slots that are running again (a restart already claimed the slot
 *  between the old job's exit and this release). */
void svc_slot_released(int slot)
{
    bool live = false;

    if (s_svc == NULL) {
        return;
    }
    if (s_bg_ops != NULL && s_bg_ops->bg_running != NULL) {
        live = s_bg_ops->bg_running(slot);
    }
    if (live) {
        return;
    }
    for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
        if (s_svc[i].used && s_svc[i].slot == slot) {
            s_svc[i].slot = 0;
        }
    }
}

esp_err_t svc_install(const char *name, const char *line, bool restart, bool auto_start)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    svc_entry_t *e;

    if (!sync_name_ok(name) || line == NULL || line[0] == '\0' ||
        strlen(line) >= (size_t)P4_CONFIG_COMMAND_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_svc_loaded) {
        svc_load();
    }
    if (s_svc == NULL) {
        return ESP_ERR_NO_MEM;
    }
    e = svc_find(name);
    if (e == NULL) {
        for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
            if (!s_svc[i].used) {
                e = &s_svc[i];
                memset(e, 0, sizeof(*e));
                e->used = true;
                snprintf(e->name, sizeof(e->name), "%s", name);
                break;
            }
        }
        if (e == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!svc_resolved(resolved, sizeof(resolved))) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        char key[P4_CONFIG_SVC_NAME_BYTES + 8];

        snprintf(key, sizeof(key), "%s.line", name);
        if (storage_ini_file_set(resolved, key, line) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        snprintf(key, sizeof(key), "%s.restart", name);
        if (storage_ini_file_set(resolved, key, restart ? "1" : "0") != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        snprintf(key, sizeof(key), "%s.auto", name);
        if (storage_ini_file_set(resolved, key, auto_start ? "1" : "0") != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    /* A running job keeps its old line (documented); flags apply live.
     * The new line is PSRAM (never BSS — see the struct comment). A failed
     * strdup keeps the old line rather than corrupting a live service. */
    {
        char *copy = p4heap_strdup_psram(line);

        if (copy == NULL) {
            return ESP_ERR_NO_MEM;
        }
        svc_free_line(e);
        e->line = copy;
    }
    e->restart = restart;
    e->auto_start = auto_start;
    return ESP_OK;
}

esp_err_t svc_remove(const char *name)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    svc_entry_t *e;

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_svc_loaded) {
        svc_load();
    }
    e = svc_find(name);
    if (e == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Refused while running — stop it first (explicit beats implicit). */
    if (e->slot > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!svc_resolved(resolved, sizeof(resolved))) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        char key[P4_CONFIG_SVC_NAME_BYTES + 8];

        snprintf(key, sizeof(key), "%s.line", name);
        (void)storage_ini_file_delete(resolved, key);
        snprintf(key, sizeof(key), "%s.restart", name);
        (void)storage_ini_file_delete(resolved, key);
        snprintf(key, sizeof(key), "%s.auto", name);
        (void)storage_ini_file_delete(resolved, key);
    }
    svc_free_line(e);
    memset(e, 0, sizeof(*e));
    return ESP_OK;
}

esp_err_t svc_start(const char *name)
{
    svc_entry_t *e;
    int slot;

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_svc_loaded) {
        svc_load();
    }
    e = svc_find(name);
    if (e == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (e->slot > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bg_ops == NULL || s_bg_ops->bg_start == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    slot = s_bg_ops->bg_start(e->line);
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    e->slot = slot;
    e->supervised = true;
    e->stopped = false;
    e->parked = false;
    /* Something is supervised now: the tick must exist. */
    svc_ensure_timer();
    return ESP_OK;
}

esp_err_t svc_stop(const char *name)
{
    svc_entry_t *e;

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_svc_loaded) {
        svc_load();
    }
    e = svc_find(name);
    if (e == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Admin off switch: never restarted, never counted — even mid-flight
     * (the kill unwinds the job; the tick sees `stopped` and stands down). */
    e->stopped = true;
    e->supervised = false;
    if (e->slot > 0) {
        batch_bg_request_kill(e->slot);
    }
    return ESP_OK;
}

void batch_svc_boot(void)
{
    if (s_bg_ops == NULL || s_bg_ops->bg_start == NULL) {
        return;
    }
    svc_load();
    if (s_svc == NULL) {
        return;
    }
    for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
        if (!s_svc[i].used || !s_svc[i].auto_start || s_svc[i].line == NULL ||
            s_svc[i].line[0] == '\0') {
            continue;
        }
        {
            int slot = s_bg_ops->bg_start(s_svc[i].line);

            /* A failed launch stays supervised: the tick retries (uncounted)
             * instead of silently dropping the autostart. */
            s_svc[i].slot = (slot > 0) ? slot : 0;
            s_svc[i].supervised = true;
            if (slot > 0) {
                /* Something is supervised now: the tick must exist. */
                svc_ensure_timer();
            }
        }
    }
}

void batch_svc_tick(void)
{
    if (s_bg_ops == NULL || s_bg_ops->bg_start == NULL || s_bg_ops->bg_running == NULL) {
        return;
    }
    if (s_svc == NULL) {
        return;
    }
    if (!s_svc_loaded) {
        svc_load();
    }
    for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
        svc_entry_t *e = &s_svc[i];

        if (!e->used || !e->supervised || e->stopped || e->parked) {
            continue;
        }
        if (e->line == NULL || e->line[0] == '\0') {
            continue;
        }
        if (e->slot > 0) {
            /* Reap: a live job stays; a dead one drops its slot and falls
             * through to the (re)start below. */
            if (s_bg_ops->bg_running(e->slot)) {
                continue;
            }
            e->slot = 0;
        }
        /* Down (fresh or reaped): one-shot services come to rest here. */
        if (!e->restart) {
            e->supervised = false;
            continue;
        }
        if (e->restarts >= (int)P4_CONFIG_SVC_RESTART_MAX) {
            e->parked = true;
            /* Parked is terminal for this boot: one error notification and
             * one transcript line (the tick itself stays silent otherwise). */
            shell_header_notify_level("task manager: service parked",
                                      5000, HEADER_NOTIFY_ERR);
            shell_transcript_appendf("service: '%s' parked after %d restarts\n",
                                     e->name, e->restarts);
            continue;
        }
        {
            int slot = s_bg_ops->bg_start(e->line);

            /* Pool full / OTA: not counted — retry next tick. */
            if (slot <= 0) {
                continue;
            }
            e->slot = slot;
            e->restarts++;
            /* One rendezvous per restart for `wait tmgr_svc_restart`
             * automation (auto-clear), plus a visible warning. */
            (void)sync_event_signal(P4_CONFIG_TMGR_EVENT_SVC_RESTART);
            shell_header_notify_level("task manager: service restarted",
                                      3500, HEADER_NOTIFY_WARN);
            shell_transcript_appendf("service: '%s' restarted (restarts %d)\n",
                                     e->name, e->restarts);
        }
    }
}

/** Supervisor timer callback (FreeRTOS timer task): reap + restart. */
static void svc_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    batch_svc_tick();
}

/** Create + arm the supervisor timer on first use (NOT at batch_init: the
 *  FreeRTOS timer task owns an internal-RAM stack, and creating it inside
 *  the boot heap window starves the command worker's contiguous 32 KB
 *  block (same F34 shape). Post-boot heap pressure is lower, so deferring
 *  costs nothing and a serviceless boot costs zero). */
static void svc_ensure_timer(void)
{
    if (s_svc_timer != NULL) {
        return;
    }
    s_svc_timer = xTimerCreate("svc_tick",
                               pdMS_TO_TICKS(P4_CONFIG_SVC_TICK_MS),
                               pdTRUE, NULL, svc_timer_cb);
    if (s_svc_timer != NULL) {
        xTimerStart(s_svc_timer, 0);
    }
}

/** Reset supervisor state (once, from batch_init). The mirror and timer
 *  are created lazily (never inside the boot heap window). */
void batch_svc_init(void)
{
    s_svc_timer = NULL;
    s_svc_loaded = false;
    if (s_svc != NULL) {
        for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
            svc_free_line(&s_svc[i]);
        }
        memset(s_svc, 0, sizeof(*s_svc) * (size_t)P4_CONFIG_SVC_MAX);
    }
}

/* -------------------------------------------------------------------------
 * THE `service` VERB (thin shell over the registry above)
 * ---------------------------------------------------------------------- */

void shell_command_service(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: service list | install <name> <line> [/restart] [/noauto] | "
                          "remove <name> | start <name> | stop <name>");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int n = 0;

        shell_transcript_appendf_ansi(SH_HEAD "Services" SH_RST "\n");
        for (int i = 0; i < (int)P4_CONFIG_SVC_MAX; i++) {
            svc_entry_t e = svc_describe(i);
            const char *state;

            if (!e.used) {
                continue;
            }
            n++;
            if (e.slot > 0) {
                state = "running";
            } else if (e.stopped) {
                state = "stopped";
            } else if (e.parked) {
                state = "parked";
            } else if (e.supervised) {
                state = "pending";
            } else {
                state = "idle";
            }
            shell_transcript_appendf_ansi("  " SH_NUM "%s" SH_RST ": %s%s%s (restarts %d): %s\n",
                                          e.name, state,
                                          e.restart ? " +restart" : "",
                                          e.auto_start ? " +auto" : "",
                                          e.restarts, e.line != NULL ? e.line : "(none)");
        }
        if (n == 0) {
            shell_print_muted("no services installed");
        }
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "install")) {
        bool restart = false;
        bool auto_start = true;
        const char *name = (argc >= 3) ? argv[2] : NULL;
        const char *line = (argc >= 4) ? argv[3] : NULL;

        for (int i = 4; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/restart")) {
                restart = true;
            } else if (shell_text_equals_ignore_case(argv[i], "/noauto")) {
                auto_start = false;
            }
        }
        if (name == NULL || line == NULL) {
            shell_print_usage("Usage: service install <name> <line> [/restart] [/noauto]");
            batch_set_errorlevel(2);
            return;
        }
        {
            esp_err_t rc = svc_install(name, line, restart, auto_start);

            if (rc == ESP_ERR_NO_MEM) {
                shell_print_error("service: registry full (%d max)", P4_CONFIG_SVC_MAX);
            } else if (rc != ESP_OK) {
                shell_print_error("service: cannot install %s", name);
            } else {
                shell_print_ok("service: installed %s", name);
            }
            batch_set_errorlevel(rc == ESP_OK ? 0 : 1);
        }
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "remove")) {
        esp_err_t rc;

        if (argc != 3) {
            shell_print_usage("Usage: service remove <name>");
            batch_set_errorlevel(2);
            return;
        }
        rc = svc_remove(argv[2]);
        if (rc == ESP_ERR_NOT_FOUND) {
            shell_print_error("service: no such service %s", argv[2]);
        } else if (rc == ESP_ERR_INVALID_STATE) {
            shell_print_error("service: %s is running (stop it first)", argv[2]);
        } else if (rc != ESP_OK) {
            shell_print_error("service: cannot remove %s", argv[2]);
        } else {
            shell_print_ok("service: removed %s", argv[2]);
        }
        batch_set_errorlevel(rc == ESP_OK ? 0 : 1);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "start")) {
        esp_err_t rc;

        if (argc != 3) {
            shell_print_usage("Usage: service start <name>");
            batch_set_errorlevel(2);
            return;
        }
        rc = svc_start(argv[2]);
        if (rc == ESP_ERR_NOT_FOUND) {
            shell_print_error("service: no such service %s", argv[2]);
        } else if (rc == ESP_ERR_NO_MEM) {
            shell_print_error("service: no background slots free");
        } else if (rc != ESP_OK) {
            shell_print_error("service: cannot start %s", argv[2]);
        } else {
            shell_print_ok("service: started %s", argv[2]);
        }
        batch_set_errorlevel(rc == ESP_OK ? 0 : 1);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "stop")) {
        esp_err_t rc;

        if (argc != 3) {
            shell_print_usage("Usage: service stop <name>");
            batch_set_errorlevel(2);
            return;
        }
        rc = svc_stop(argv[2]);
        if (rc == ESP_ERR_NOT_FOUND) {
            shell_print_error("service: no such service %s", argv[2]);
        } else if (rc != ESP_OK) {
            shell_print_error("service: cannot stop %s", argv[2]);
        } else {
            shell_print_ok("service: stopped %s", argv[2]);
        }
        batch_set_errorlevel(rc == ESP_OK ? 0 : 1);
        return;
    }
    shell_print_usage("Usage: service list | install <name> <line> [/restart] [/noauto] | "
                      "remove <name> | start <name> | stop <name>");
    batch_set_errorlevel(2);
}

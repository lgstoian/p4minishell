/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file alarm.h
 * @brief SD-persisted alarm / event store + one background checker task.
 *
 * A small, batch-friendly alarm system for P4MiniShell:
 *   - every event lives on the SD card under `sd:/ALARMS/` (INDEX.INI + one
 *     E<id>.INI per event), reusing the storage INI helpers and guarded
 *     sessions with atomic writes and free-space pre-checks
 *   - a single low-rate checker task polls the store and, when an event is
 *     due, posts to the EXISTING surfaces only: the header notification area
 *     (`shell_header_notify`), the LED (`led_notify`), the speaker
 *     (`audio_play_tone`), and optionally a `/run:` batch file — which is
 *     queued onto the command worker through a registered `alarm_host_ops_t`
 *     hook, never run on the checker's stack
 *   - there is no private notification loop and no RAM-only store
 *
 * Layering: this component is a leaf. It REQUIRES only `shell`, `clock`,
 * `storage`, `led`, `audio`, `freertos`, and `esp_timer`. It NEVER includes
 * `command.h` or `batch.h`; the `/run:` batch launch and the command-render
 * helpers are supplied through the registered ops table (registered by
 * `command_init()`), mirroring the `clock_host_ops_t` pattern.
 */

#ifndef P4MINISHELL_ALARM_H
#define P4MINISHELL_ALARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Event model
 * ---------------------------------------------------------------------- */

/** Event flags (stored in the E<id>.INI `flags=` field). */
enum {
    ALARM_FLAG_ENABLED  = 0x01,  /**< The event is armed. */
    ALARM_FLAG_FIRED    = 0x02,  /**< One-shot event has fired (kept until purge). */
    ALARM_FLAG_SILENT   = 0x04,  /**< Suppress sound/LED; header notify only. */
};

/** Actions to take when the event fires (bitmask; stored in `action=`). */
enum {
    ALARM_ACTION_NOTIFY = 0x01,  /**< Show a header notification. */
    ALARM_ACTION_BEEP   = 0x02,  /**< Play a short speaker tone. */
    ALARM_ACTION_LED    = 0x04,  /**< Pulse the RGB LED. */
    ALARM_ACTION_RUN    = 0x08,  /**< Queue the `run=` batch file. */
};

/** Recurrence: none / daily / weekly. Weekly stores a 7-bit weekday mask in
 *  the low bits of `recur` (bit 0 = Sunday .. bit 6 = Saturday). Monthly
 *  fires on `recur_day` of each month (short months skipped), or on the
 *  nth weekday (`recur_nth` 1..5, -1 = last) when set; yearly fires on
 *  `recur_month`/`recur_day`. Zero day/month/nth derives from `when`. */
#define ALARM_RECUR_NONE   0
#define ALARM_RECUR_DAILY  1
#define ALARM_RECUR_MONTHLY 0x40
#define ALARM_RECUR_YEARLY 0x20
#define ALARM_RECUR_WEEKLY 0x80

/** A single alarm event (mirrors one E<id>.INI file). */
typedef struct {
    uint32_t id;                          /**< Monotonic, never reused. */
    time_t   when;                        /**< Next due time (Unix, local). */
    char     title[P4_CONFIG_ALARM_TITLE_BYTES];
    char     msg[P4_CONFIG_ALARM_MSG_BYTES];
    uint8_t  flags;                       /**< ALARM_FLAG_* bits. */
    uint8_t  recur;                       /**< ALARM_RECUR_* / weekly mask. */
    uint8_t  action;                      /**< ALARM_ACTION_* bits. */
    uint8_t  recur_day;                   /**< Monthly/yearly day 1..31 (0 = from `when`). */
    uint8_t  recur_month;                 /**< Yearly month 1..12 (0 = from `when`). */
    int8_t   recur_nth;                   /**< Monthly nth weekday 1..5, -1 = last, 0 = by monthday. */
    /** Sync identity (32 hex chars + NUL). Empty until the first `pim get`
     *  backfill; the loader tolerates older files without it, and the saver
     *  omits it while empty so untouched events stay byte-identical. */
    char     uid[P4_CONFIG_ALARM_UID_BYTES];
    /** Last sync-relevant mutation (Unix). Set by `pim get` backfill, `pim
     *  put` merge, snooze, and the tick fire pass — but only once @p uid is
     *  set, so never-synced events never gain the field. `alarm_enable`
     *  never bumps it (flags are device-local and merge preserves them). */
    time_t   modified;
#if P4_CONFIG_ALARM_ENABLE_RUN_ACTION
    char     run[P4_CONFIG_SD_PATH_BYTES]; /**< Batch file for ALARM_ACTION_RUN. */
#endif
} alarm_event_t;

/** `alarm status` summary. */
typedef struct {
    uint32_t count;                       /**< Stored events (incl. fired). */
    uint32_t enabled;                     /**< Currently armed events. */
    time_t   next_due;                    /**< Next due time, or 0 when none. */
    bool     checker_running;             /**< Whether the checker task is up. */
    bool     catchup_done;                /**< Boot catch-up has run this boot. */
} alarm_status_t;

/* ------------------------------------------------------------------------
 * Host ops (registered by command_init) — the only way this leaf reaches the
 * command worker / shell render layer.
 * ---------------------------------------------------------------------- */

typedef struct {
    /**
     * Queue a command line for the command worker (never run on the checker's
     * stack). Used for the `/run:` batch action: the alarm module posts
     * "call <path>" here and the worker executes it with full expansion,
     * redirection, and ERRORLEVEL semantics.
     */
    void (*execute_async)(char *command);
} alarm_host_ops_t;

/** Register (or clear, with NULL) the alarm host ops. */
void alarm_register_host_ops(const alarm_host_ops_t *ops);

/* ------------------------------------------------------------------------
 * Pure helpers (no SD, no commands — unit-testable)
 * ---------------------------------------------------------------------- */

/** True when a WEEKLY recurrence's weekday mask includes @p wday (0=Sun..6=Sat). */
bool alarm_recur_weekday_matches(uint8_t recur, int wday);

/** Advance a fired recurrence to its next occurrence strictly after @p when. */
time_t alarm_advance_recur(time_t when, uint8_t recur);

/**
 * Next monthly occurrence strictly after @p when (same wall-clock time):
 * on @p day of each month (short months skipped), or on the nth weekday
 * (@p nth 1..5, -1 = last, weekday @p wday 0=Sun..6=Sat) when @p nth != 0.
 * Zero/negative @p day falls back to `when`'s own day-of-month.
 */
time_t alarm_advance_monthly(time_t when, int day, int nth, int wday);

/** Next yearly occurrence strictly after @p when (@p month 1..12, @p day
 *  1..31; zeros fall back to `when`'s own month/day; Feb 29 skips). */
time_t alarm_advance_yearly(time_t when, int month, int day);

/** Advance any recurrence of @p e (monthly/yearly read the event params,
 *  with zero fields derived from `when`; daily/weekly delegate). */
time_t alarm_advance_event(time_t when, const alarm_event_t *e);

/**
 * Parse a local "YYYY-MM-DD" date and "HH:MM[:SS]" time into a Unix timestamp
 * (timezone-aware via mktime, matching the `date`/`time` commands). Returns
 * false on malformed input or an out-of-range date.
 */
bool alarm_parse_datetime(const char *date, const char *time_str, time_t *out);

/* ------------------------------------------------------------------------
 * Lifecycle / store
 * ---------------------------------------------------------------------- */

/**
 * Initialize the alarm store and start the checker task. Loads the event
 * directory, performs the boot catch-up pass when enabled, and arms the
 * checker. Idempotent. Safe to call before the SD card is mounted (the
 * checker simply reports an error until the store is usable).
 */
esp_err_t alarm_init(void);

/** True once alarm_init() has completed at least once. */
bool alarm_is_initialized(void);

/** Report whether the checker task is currently running. */
bool alarm_checker_running(void);

/**
 * Add an event. The title/message must be plain strings; `when` is a local
 * Unix timestamp (already timezone-normalized by the caller using mktime).
 * Writes atomically behind a free-space pre-check. Returns the new id.
 */
esp_err_t alarm_add(const char *title, const char *msg, time_t when,
                    uint8_t recur, uint8_t action, const char *run_path,
                    uint32_t *out_id);

/** Monthly/yearly parameters for alarm_add_ex (0 = derive from `when`). */
typedef struct {
    int day;      /**< Day of month 1..31. */
    int month;    /**< Month 1..12 (yearly). */
    int nth;      /**< Nth weekday 1..5, -1 = last, 0 = by monthday. */
} alarm_recur_params_t;

/**
 * Add an event with explicit monthly/yearly parameters (NULL = derive
 * everything from `when`, identical to alarm_add).
 */
esp_err_t alarm_add_ex(const char *title, const char *msg, time_t when,
                       uint8_t recur, uint8_t action, const char *run_path,
                       const alarm_recur_params_t *params, uint32_t *out_id);

/**
 * Snooze an event: push `when` to now + @p minutes (default 10 when <= 0),
 * clear a FIRED mark, keep recurrence and actions.
 */
esp_err_t alarm_snooze(uint32_t id, int minutes);

/** Read one event into @p out. Returns ESP_ERR_NOT_FOUND when absent. */
esp_err_t alarm_get(uint32_t id, alarm_event_t *out);

/** Iterate stored events in id order; cb returns false to stop. */
esp_err_t alarm_list(bool (*cb)(const alarm_event_t *event, void *ctx), void *ctx);

/**
 * Delete an event. Soft by default: sets fired + disabled (kept until purge).
 * With @p permanent true the event file is removed.
 */
esp_err_t alarm_del(uint32_t id, bool permanent);

/**
 * Delete every event (permanent) and reset the store index (count=0,
 * next_id=1). The batch-first `alarm del all` wipe. Individual `alarm_del`
 * never reuses ids; a full wipe starts a fresh id space.
 */
esp_err_t alarm_del_all(void);

/** Enable or disable an event (writes the flags back). */
esp_err_t alarm_enable(uint32_t id, bool enable);

/**
 * Replace an event wholesale: loads @p id (NOT_FOUND when absent), stores
 * every field of @p ev except the id (which is kept from the stored event),
 * and writes the file back. The sync merge path (`pim put`) for newer-wins
 * updates. Never touches INDEX.INI; the caller's @p ev.modified is kept
 * verbatim (no bump here — bumps happen in snooze/the fire pass).
 */
esp_err_t alarm_set(uint32_t id, const alarm_event_t *ev);

/** Render cap for one canonical event text (id/when/title/msg/flags/recur/
 *  action/run plus the conditional uid/modified lines). Buffers passed to
 *  alarm_event_render() must hold at least this many bytes. */
#define ALARM_EVENT_TEXT_BYTES \
    (P4_CONFIG_ALARM_TITLE_BYTES + P4_CONFIG_ALARM_MSG_BYTES + \
     P4_CONFIG_SD_PATH_BYTES + 512)

/**
 * Render @p e as the canonical E<id>.INI text (the same bytes
 * alarm_event_save() persists). Pure: no SD, unit-testable. The `id=` line
 * is informational; alarm_set() always keeps the stored id. @p size must be
 * at least ALARM_EVENT_TEXT_BYTES.
 */
esp_err_t alarm_event_render(const alarm_event_t *e, char *buf, size_t size);

/**
 * Parse canonical event text (as produced by alarm_event_render()) into
 * @p out. Same field semantics as the file loader: every key but `when` is
 * optional (historical defaults and clamps apply), missing `when` is
 * ESP_ERR_NOT_FOUND, and the event id comes from @p id, never from the
 * text. Pure: no SD, unit-testable. Used by `edit alarm` to validate the
 * edited text before alarm_set().
 */
esp_err_t alarm_event_parse(const char *text, uint32_t id, alarm_event_t *out);

/** Physically remove every soft-deleted (fired + disabled) event. */
esp_err_t alarm_purge(void);

/** Fill @p out with the status summary. */
esp_err_t alarm_status(alarm_status_t *out);

/* ------------------------------------------------------------------------
 * Checker
 * ---------------------------------------------------------------------- */

/**
 * Run one fire pass: find due+enabled events, mark them fired (advancing the
 * `when` of recurring events to the next occurrence), persist, then trigger
 * the configured actions (header notify / beep / LED / queued batch run).
 * Called by the checker task; also callable once at boot for catch-up.
 * Thread-safe (guarded by the store mutex).
 */
void alarm_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_ALARM_H */

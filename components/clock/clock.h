/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#ifndef P4MINISHELL_CLOCK_H
#define P4MINISHELL_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
void time_init(void);
void time_start_sntp(void);
bool time_is_initialized(void);
struct tm time_get_local(void);
struct tm time_get_utc(void);
time_t time_get_unix(void);
uint32_t time_get_uptime_sec(void);
const char *time_get_formatted(void);
const char *time_get_formatted_utc(void);
bool time_is_synchronized(void);

/**
 * True when the system clock holds a usable wall-clock time, whether from an
 * SNTP sync or a manual `date`/`time` set (unix time at or above
 * P4_CONFIG_CLOCK_VALID_EPOCH). The header clock uses this to decide between a
 * real time and the "--:--" placeholder.
 */
bool time_is_set(void);
void time_set_timezone(const char *tz_string);
const char *time_get_timezone(void);

/**
 * Set the timezone from a network-detected UTC offset in seconds (positive =
 * east of UTC) plus an optional human label (e.g. an IANA name). Builds the
 * POSIX TZ string internally ("UTC-2" for +2h, "UTC-5:30" for +5:30) so no
 * hardcoded zone is needed. The label is returned by time_get_timezone_label().
 */
void time_set_utc_offset(int offset_seconds, const char *label);

/** Human label for the active zone (IANA name when known, else the TZ string). */
const char *time_get_timezone_label(void);

/**
 * Force the SNTP client to re-synchronize immediately. Restarts the client so
 * a fresh NTP exchange is issued even when the clock was already synchronized.
 * Safe to call when Wi-Fi is not connected (the client simply waits for the
 * network); must be called after lwIP is up for an immediate exchange.
 */
void time_force_resync(void);

/** Return the configured NTP server hostname. */
const char *time_get_ntp_server(void);

/**
 * Format the system uptime as "Xd Xh Xm Xs" (days included when non-zero) into
 * the caller's buffer.
 *
 * @param buf     Destination buffer.
 * @param buflen  Destination buffer size in bytes.
 */
void time_get_uptime_formatted(char *buf, size_t buflen);

/**
 * Format the local time as "HH:MM" for the header. When the clock has not been
 * synchronized (no NTP exchange yet) @p buf receives "--:--".
 *
 * @param buf     Destination buffer.
 * @param buflen  Destination buffer size in bytes.
 * @return true when the clock is synchronized and @p buf holds a valid time.
 */
bool time_format_hm(char *buf, size_t buflen);

/**
 * Pure formatter for the header clock (unit-testable): writes "HH:MM" from a
 * broken-down time. A NULL @p tm writes "--:--".
 */
void clock_format_hm_snapshot(const struct tm *tm, char *buf, size_t buflen);

/* ========================================================================
 * RTC BACKUP (clock_rtc.c: NVS anchor + optional external chip)
 * ======================================================================== */

/** Outcome of replaying an NVS anchor against the live RTC counter. */
typedef enum {
    CLOCK_RTC_INVALID = 0, /**< Anchor unusable; leave the clock unset. */
    CLOCK_RTC_RESTORED,    /**< Counter kept running; wall time replayed. */
    CLOCK_RTC_STALE        /**< Counter reset (no VBAT?); last-known applied. */
} clock_rtc_restore_t;

/**
 * Pure anchor replay: now = anchor_unix + (rtc_now - anchor_rtc) / 1e6.
 * STALE when the counter reset below the anchor; INVALID when the anchor
 * predates @p valid_epoch or the delta is implausible (> 10 years).
 * Unit-tested; @p unix_out always written (0 on INVALID).
 */
clock_rtc_restore_t clock_rtc_restore_math(int64_t anchor_unix, uint64_t anchor_rtc,
                                           uint64_t rtc_now, int64_t valid_epoch,
                                           int64_t *unix_out);

/** BCD helpers for the external-chip register map (pure, unit-tested). */
uint8_t clock_rtc_bcd_to_bin(uint8_t bcd);
uint8_t clock_rtc_bin_to_bcd(uint8_t bin);

/** Boot-time restore (external chip first when configured, then NVS). */
void clock_rtc_restore(void);

/** Start the periodic NVS anchor timer (once, from time_init). */
void clock_rtc_start_timer(void);

/** Write an anchor for the current wall time (no-op while unset). */
void clock_rtc_anchor_now(void);

/** Note a trustworthy source ("sntp"/"manual"): clears stale, anchors. */
void clock_rtc_note_synced(const char *source);

/** Read wall time from the external chip (ESP_OK + cached presence). */
esp_err_t clock_rtc_ext_read(time_t *unix_out);

/** Push wall time to the external chip (best-effort). */
void clock_rtc_ext_write(time_t unix);

/** Time-source label: none|anchor|stale|manual|sntp|ext|preset. */
const char *clock_rtc_source(void);

/** True when the displayed time is a stale last-known value. */
bool clock_rtc_is_stale(void);

/** Live RTC counter (µs, monotonic while powered incl. VBAT). */
uint64_t clock_rtc_counter_us(void);

/** Seconds since the last anchor write (-1 when never). */
int64_t clock_rtc_anchor_age_sec(void);

/** External-chip presence: -1 unknown, 0 absent/unconfigured, 1 present. */
int clock_rtc_ext_state(void);

/**
 * Host-render ops for the clock command surface.
 *
 * The `date`, `time`, `timezone`, `sntp`/`ntpsync`, and `timer`/`stopwatch`
 * command implementations
 * live in the clock component. They render through this table so the clock
 * component stays a leaf (the shell depends on the clock, not the other way
 * around). The command module registers the table in `command_init()`,
 * mapping each entry onto the matching shell print/record helper; the exact
 * shell output formats are preserved through the wrappers. Every entry must be
 * NULL-checked by the clock command code, so the surface degrades gracefully
 * when the table is not yet registered.
 */
typedef struct {
    /** Append a plain (uncoloured) line, e.g. "date: value out of range". */
    void (*emit_text)(const char *text);
    /** Print a section heading. */
    void (*print_heading)(const char *text);
    /** Print a "label: value" line. */
    void (*print_field)(const char *label, const char *value);
    /** Print a success line. */
    void (*print_ok)(const char *text);
    /** Print an error line. */
    void (*print_error)(const char *text);
    /** Print a warning line. */
    void (*print_warning)(const char *text);
    /** Print a muted/hint line. */
    void (*print_muted)(const char *text);
    /** Print a usage line. */
    void (*print_usage)(const char *text);
    /** Record an error to the shell debug log. */
    void (*record_error)(const char *domain, esp_err_t error, const char *message);
    /** Record a warning to the shell debug log. */
    void (*record_warning)(const char *domain, const char *message);
    /** Case-insensitive string compare (shell quoting semantics). */
    bool (*equals_ignore_case)(const char *left, const char *right);
    /** Store an environment variable (batch-owned table); NULL when unavailable.
     *  @return 0 on success, non-zero on a malformed name or a full table. */
    int (*set_env)(const char *name, const char *value);
} clock_host_ops_t;

/** Register (or clear, with NULL) the host-render ops used by clock commands. */
void clock_register_host_ops(const clock_host_ops_t *ops);

/* ========================================================================
 * CLOCK COMMAND SURFACE (date / time / timezone / sntp)
 * ========================================================================
 * Implemented in components/clock/clock_commands.c and dispatched from the
 * command module. Each follows the DOS-style shape:
 *   - date [MM-DD-YYYY]
 *   - time [HH:MM[:SS]]
 *   - timezone [TZ]
 *   - sntp|ntpsync [sync]
 *   - timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]
 */

/** `date` command: show the clock panel or set the system date. */
void clock_command_date(int argc, char **argv);
/** `time` command: show the clock panel or set the system time. */
void clock_command_time(int argc, char **argv);
/** `sntp`/`ntpsync` command: show NTP status or force a re-sync. */
void clock_command_sntp(int argc, char **argv);
/** `rtc` command: backup status, or `rtc anchor` to force an anchor write. */
void clock_command_rtc(int argc, char **argv);
/** `timezone` command: show or set the timezone. */
void clock_command_timezone(int argc, char **argv);
/** `timer`/`stopwatch` command: start/stop/lap/status named stopwatch runs.
 *  @return 0 ok, 1 state failure (no slot, unknown or idle run), 2 usage. */
int clock_command_timer(int argc, char **argv);

/* ========================================================================
 * STOPWATCH SLOTS (timer / stopwatch command core)
 * ========================================================================
 * Fixed table of named runs over `esp_timer_get_time()`. The `_at` forms
 * take an explicit timestamp so the unit tests stay deterministic; the
 * plain forms sample the hardware timer. All name lookups are
 * case-insensitive; a NULL or empty name selects the "default" run.
 * Return codes mirror the shell ERRORLEVEL contract: 0 ok, 1 state
 * failure, 2 usage (malformed name).
 */

/** Start (or restart from zero) the named run at @p now_us. */
int clock_timer_start_at(const char *name, int64_t now_us);
/** Stop the named run; @p elapsed_ms_out receives the frozen total. */
int clock_timer_stop_at(const char *name, int64_t now_us, int64_t *elapsed_ms_out);
/** Record a lap on the running run; @p elapsed_ms_out receives the split. */
int clock_timer_lap_at(const char *name, int64_t now_us, int64_t *elapsed_ms_out);
/** Read the named run without disturbing it. Any out-pointer may be NULL. */
int clock_timer_status_at(const char *name, int64_t now_us, bool *running_out,
                          int64_t *elapsed_ms_out, uint32_t *laps_out);
/** Number of runs currently held in the table. */
int clock_timer_slot_count(void);
/** Enumerate the held runs in slot order (for `timer status` with no name).
 *  @return true and fills the outputs when @p index selects a live run. */
bool clock_timer_get_slot(int index, char *name_out, size_t name_size,
                          bool *running_out, int64_t *elapsed_ms_out);
/** Hardware-timer wrappers around the `_at` core. */
int clock_timer_start(const char *name);
int clock_timer_stop(const char *name, int64_t *elapsed_ms_out);
int clock_timer_lap(const char *name, int64_t *elapsed_ms_out);
#ifdef __cplusplus
}
#endif
#endif

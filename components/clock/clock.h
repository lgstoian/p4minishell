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
void time_set_timezone(const char *tz_string);
const char *time_get_timezone(void);

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
 * Host-render ops for the clock command surface.
 *
 * The `date`, `time`, `timezone`, and `sntp`/`ntpsync` command implementations
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
 */

/** `date` command: show the clock panel or set the system date. */
void clock_command_date(int argc, char **argv);
/** `time` command: show the clock panel or set the system time. */
void clock_command_time(int argc, char **argv);
/** `sntp`/`ntpsync` command: show NTP status or force a re-sync. */
void clock_command_sntp(int argc, char **argv);
/** `timezone` command: show or set the timezone. */
void clock_command_timezone(int argc, char **argv);
#ifdef __cplusplus
}
#endif
#endif

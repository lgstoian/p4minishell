#include <time.h>
#include "clock.h"
#include "p4minishell_config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

#define CLOCK_TAG P4_CONFIG_SHELL_TAG

static bool s_initialized = false;
static bool s_synchronized = false;
static bool s_sntp_started = false;
static char s_tz_string[P4_CONFIG_TIMEZONE_BYTES] = "UTC";
static char s_tz_label[P4_CONFIG_TIMEZONE_BYTES];
static char s_time_buf[64];
static char s_time_utc_buf[64];
static int64_t s_boot_timestamp_us;

static void clock_sntp_cb(struct timeval *tv)
{
    s_synchronized = true;
    clock_rtc_note_synced("sntp");
    if (tv != NULL) {
        clock_rtc_ext_write(tv->tv_sec);
    }
    ESP_LOGI(CLOCK_TAG, "Time synchronized via NTP");
}

void time_init(void)
{
    if (s_initialized) return;
    s_boot_timestamp_us = esp_timer_get_time();

    /* SNTP init is deferred — esp_sntp_init() requires the lwIP TCP/IP
     * stack to be fully running. networking_init() starts Wi-Fi in a
     * background task, so lwIP may not be ready yet when time_init()
     * is called. The SNTP client will be started when Wi-Fi connects. */
    setenv("TZ", s_tz_string, 1);
    tzset();
    /* Replay wall time across the reboot (external chip, else NVS anchor)
     * before anything else reads the clock. */
    clock_rtc_restore();
    clock_rtc_start_timer();
    s_initialized = true;
    ESP_LOGI(CLOCK_TAG, "Clock module initialized (SNTP deferred)");
}

/** (Re)start the SNTP client against the configured server. */
static void clock_sntp_start_client(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, P4_CONFIG_NTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(clock_sntp_cb);
    esp_sntp_init();
}

/** Start SNTP client — called when lwIP is confirmed ready. */
void time_start_sntp(void)
{
    /* Idempotent: callers poll (header refresh) and must not re-init the
     * client or spawn duplicate SNTP state on every tick. */
    if (s_sntp_started || s_synchronized) return;

    clock_sntp_start_client();
    s_sntp_started = true;
    ESP_LOGI(CLOCK_TAG, "SNTP client started (server %s)", P4_CONFIG_NTP_SERVER);
}

void time_force_resync(void)
{
    /* Stop any running client, clear the sync flag, and restart so a fresh
     * NTP exchange is issued immediately. Safe when not yet initialized. */
    esp_sntp_stop();
    s_synchronized = false;
    clock_sntp_start_client();
    s_sntp_started = true;
    ESP_LOGI(CLOCK_TAG, "SNTP re-synchronization requested (server %s)", P4_CONFIG_NTP_SERVER);
}

const char *time_get_ntp_server(void)
{
    return P4_CONFIG_NTP_SERVER;
}

bool time_is_initialized(void) { return s_initialized; }

struct tm time_get_local(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    return ti;
}

struct tm time_get_utc(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    gmtime_r(&now, &ti);
    return ti;
}

time_t time_get_unix(void) { time_t now; time(&now); return now; }

uint32_t time_get_uptime_sec(void)
{
    int64_t u = esp_timer_get_time() - s_boot_timestamp_us;
    return (uint32_t)(u / 1000000ULL);
}

const char *time_get_formatted(void)
{
    struct tm ti = time_get_local();
    snprintf(s_time_buf, sizeof(s_time_buf), "%04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    return s_time_buf;
}

const char *time_get_formatted_utc(void)
{
    struct tm ti = time_get_utc();
    snprintf(s_time_utc_buf, sizeof(s_time_utc_buf), "%04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    return s_time_utc_buf;
}

bool time_is_synchronized(void) { return s_synchronized; }

bool time_is_set(void)
{
    if (s_synchronized) {
        return true;
    }
    if (clock_rtc_is_stale()) {
        return false; /* last-known time: honest "--:--" until re-synced */
    }
    /* A manual `date`/`time` sets the clock via settimeofday without touching
     * the SNTP flag; treat a plausible wall-clock time as valid. */
    return time(NULL) >= (time_t)P4_CONFIG_CLOCK_VALID_EPOCH;
}

void time_set_timezone(const char *tz)
{
    if (!tz) return;
    snprintf(s_tz_string, sizeof(s_tz_string), "%s", tz);
    setenv("TZ", s_tz_string, 1);
    tzset();
    ESP_LOGI(CLOCK_TAG, "Timezone: %s", s_tz_string);
}

const char *time_get_timezone(void) { return s_tz_string; }

void time_set_utc_offset(int offset_seconds, const char *label)
{
    char tz[P4_CONFIG_TIMEZONE_BYTES];
    int abs_offset = (offset_seconds < 0) ? -offset_seconds : offset_seconds;
    int hours = abs_offset / 3600;
    int minutes = (abs_offset % 3600) / 60;
    /* POSIX TZ offsets are inverted: UTC+2 is written "UTC-2". */
    char sign = (offset_seconds >= 0) ? '-' : '+';

    if (minutes != 0) {
        snprintf(tz, sizeof(tz), "UTC%c%d:%02d", sign, hours, minutes);
    } else {
        snprintf(tz, sizeof(tz), "UTC%c%d", sign, hours);
    }
    if (label != NULL && label[0] != '\0') {
        snprintf(s_tz_label, sizeof(s_tz_label), "%s", label);
    }
    time_set_timezone(tz);
    ESP_LOGI(CLOCK_TAG, "Timezone auto-detected: %s (%s)", tz,
             s_tz_label[0] != '\0' ? s_tz_label : "n/a");
}

const char *time_get_timezone_label(void)
{
    return (s_tz_label[0] != '\0') ? s_tz_label : s_tz_string;
}

void clock_format_hm_snapshot(const struct tm *tm, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return;
    }
    if (tm == NULL) {
        snprintf(buf, buflen, "--:--");
        return;
    }
    snprintf(buf, buflen, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

bool time_format_hm(char *buf, size_t buflen)
{
    struct tm ti;

    if (buf == NULL || buflen == 0) {
        return false;
    }
    if (!time_is_set()) {
        snprintf(buf, buflen, "--:--");
        return false;
    }
    ti = time_get_local();
    clock_format_hm_snapshot(&ti, buf, buflen);
    return true;
}

void time_get_uptime_formatted(char *buf, size_t buflen)
{
    uint32_t sec = time_get_uptime_sec();
    uint32_t days = sec / 86400;
    uint32_t hours = (sec % 86400) / 3600;
    uint32_t mins = (sec % 3600) / 60;
    uint32_t secs = sec % 60;

    if (buf == NULL || buflen == 0) {
        return;
    }
    if (days > 0) {
        snprintf(buf, buflen, "%lu d %02lu:%02lu:%02lu",
                 (unsigned long)days, (unsigned long)hours,
                 (unsigned long)mins, (unsigned long)secs);
    } else {
        snprintf(buf, buflen, "%02lu:%02lu:%02lu",
                 (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
    }
}

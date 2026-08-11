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
static char s_tz_string[P4_CONFIG_TIMEZONE_BYTES] = "UTC";
static char s_time_buf[64];
static char s_time_utc_buf[64];
static int64_t s_boot_timestamp_us;

static void clock_sntp_cb(struct timeval *tv)
{
    (void)tv;
    s_synchronized = true;
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
    if (s_synchronized) return;

    clock_sntp_start_client();
    ESP_LOGI(CLOCK_TAG, "SNTP client started (server %s)", P4_CONFIG_NTP_SERVER);
}

void time_force_resync(void)
{
    /* Stop any running client, clear the sync flag, and restart so a fresh
     * NTP exchange is issued immediately. Safe when not yet initialized. */
    esp_sntp_stop();
    s_synchronized = false;
    clock_sntp_start_client();
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

void time_set_timezone(const char *tz)
{
    if (!tz) return;
    snprintf(s_tz_string, sizeof(s_tz_string), "%s", tz);
    setenv("TZ", s_tz_string, 1);
    tzset();
    ESP_LOGI(CLOCK_TAG, "Timezone: %s", s_tz_string);
}

const char *time_get_timezone(void) { return s_tz_string; }

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

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file portal.c
 * @brief Captive-portal orchestration: start/stop SoftAP + DNS + HTTP.
 *
 * When the portal is active the device runs in APSTA mode:
 *   - SoftAP: a Wi-Fi access point that clients can join
 *   - DNS server: responds to all queries with the AP IP
 *   - HTTP server: serves the portal page and handles credential submission
 *
 * After the user enters credentials the portal shuts down and the device
 * attempts to connect to the home network using the saved credentials.
 */

#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip_addr.h"
#include "p4minishell_config.h"
#include "portal.h"
#include "portal_internal.h"

/* The portal needs networking and storage functions.  A direct include would
 * create a circular dependency, so we declare them via extern. */
extern int networking_wifi_known_count(void);
extern bool networking_wifi_connect_to(const char *ssid, const char *password);
extern bool storage_sd_is_mounted(void);

#define PORTAL_TAG "portal"

/** Private event base: wakes the loop task for timeout/deferred-stop work so
 *  teardown always runs serialized with other Wi-Fi state changes. */
ESP_EVENT_DEFINE_BASE(PORTAL_EVENTS);

typedef enum {
    PORTAL_EVENT_WAKE = 0,
} portal_event_id_t;

static bool s_portal_active;
static esp_netif_t *s_ap_netif;
static uint32_t s_ap_ip;
static int64_t s_start_us;
static esp_timer_handle_t s_timeout_timer;
/** STA TX power saved at portal start, restored at stop (-1 = unknown). */
static int s_saved_tx_power = -1;

/** Deferred stop request (from the HTTP handler task). */
static volatile bool s_stop_requested;

/** Credentials stashed for one post-stop STA connect attempt. */
static bool s_connect_pending;
static char s_connect_ssid[P4_CONFIG_WIFI_SSID_BYTES];
static char s_connect_password[P4_CONFIG_WIFI_PASSWORD_BYTES];

void portal_request_stop(void)
{
    s_stop_requested = true;
}

void portal_request_connect_after_stop(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    snprintf(s_connect_ssid, sizeof(s_connect_ssid), "%s", ssid);
    snprintf(s_connect_password, sizeof(s_connect_password), "%s",
             password != NULL ? password : "");
    s_connect_pending = true;
}

void portal_connect_pending_after_stop(void)
{
    char ssid[P4_CONFIG_WIFI_SSID_BYTES];
    char password[P4_CONFIG_WIFI_PASSWORD_BYTES];

    if (!s_connect_pending) {
        return;
    }
    /* Clear first: exactly-once even if two pollers race here. */
    s_connect_pending = false;
    snprintf(ssid, sizeof(ssid), "%s", s_connect_ssid);
    snprintf(password, sizeof(password), "%s", s_connect_password);
    s_connect_ssid[0] = '\0';
    s_connect_password[0] = '\0';

    ESP_LOGI(PORTAL_TAG, "portal stopped, joining %s", ssid);
    if (!networking_wifi_connect_to(ssid, password)) {
        ESP_LOGW(PORTAL_TAG, "post-portal connect not started; saved credentials apply on next boot");
    }
}

/** Loop-task side of timeout/deferred-stop: teardown, then the pending join. */
static void portal_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (portal_should_stop()) {
        portal_stop();
    }
    portal_connect_pending_after_stop();
}

/** One-shot timeout: flag the stop and wake the loop task to execute it. */
static void portal_timeout_cb(void *arg)
{
    (void)arg;
    s_stop_requested = true;
    if (esp_event_post(PORTAL_EVENTS, PORTAL_EVENT_WAKE, NULL, 0, 0) != ESP_OK) {
        ESP_LOGW(PORTAL_TAG, "timeout fired but loop wake failed; stop runs on next Wi-Fi event");
    }
}

esp_err_t portal_start(void)
{
#if !P4_CONFIG_PORTAL_ENABLE
    ESP_LOGI(PORTAL_TAG, "portal disabled by config");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    if (s_portal_active) {
        return ESP_OK;
    }

    ESP_LOGI(PORTAL_TAG, "starting captive portal");

    /* Create the AP netif.  The DHCP server starts automatically. */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(PORTAL_TAG, "failed to create AP netif");
        return ESP_FAIL;
    }

    /* Switch to APSTA mode FIRST: esp_wifi_set_config(AP) refuses with
     * ESP_ERR_WIFI_MODE while the stack runs STA-only. The STA config is
     * preserved across the mode switch, so a connected station survives. */
    {
        esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);

        if (error != ESP_OK) {
            ESP_LOGE(PORTAL_TAG, "wifi_set_mode(APSTA): %s", esp_err_to_name(error));
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
            return error;
        }
    }

    /* Configure the AP. */
    {
        wifi_config_t ap_config = {
            .ap = {
                .max_connection = P4_CONFIG_PORTAL_AP_MAX_CONN,
                .channel = P4_CONFIG_PORTAL_AP_CHANNEL,
                .authmode = P4_CONFIG_PORTAL_AP_OPEN
                                ? WIFI_AUTH_OPEN
                                : WIFI_AUTH_WPA2_PSK,
            },
        };
        strlcpy((char *)ap_config.ap.ssid, P4_CONFIG_PORTAL_AP_SSID,
                sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(P4_CONFIG_PORTAL_AP_SSID);

        if (!P4_CONFIG_PORTAL_AP_OPEN) {
            strlcpy((char *)ap_config.ap.password, P4_CONFIG_PORTAL_AP_PASSWORD,
                    sizeof(ap_config.ap.password));
        }

        esp_err_t error = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (error != ESP_OK) {
            ESP_LOGE(PORTAL_TAG, "wifi_set_config(AP): %s", esp_err_to_name(error));
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
            return error;
        }
    }

    /* Cap the RF TX power while the AP runs: AP beacons are a continuous
     * current draw and brownout-reboot USB-powered boards at full power
     * (verified on hardware). Restored on stop. Best-effort on hosted
     * stacks that do not implement the RPC. */
    {
        int8_t current = 0;

        if (esp_wifi_get_max_tx_power(&current) == ESP_OK) {
            s_saved_tx_power = current;
        }
        if (esp_wifi_set_max_tx_power(P4_CONFIG_PORTAL_AP_TX_POWER_QDBM) != ESP_OK) {
            ESP_LOGW(PORTAL_TAG, "TX power cap not applied; AP runs at full power");
        }
    }

    /* Extract the AP IP address for the DNS server. */
    {
        esp_netif_ip_info_t ip_info;

        if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
            s_ap_ip = ip_info.ip.addr;
            ESP_LOGI(PORTAL_TAG, "AP IP: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            /* Fallback to the standard SoftAP IP. */
            s_ap_ip = ipaddr_addr("192.168.4.1");
            ESP_LOGW(PORTAL_TAG, "could not read AP IP, using default");
        }
    }

    /* Start the DNS server (redirects all queries to the AP IP). */
    dns_server_start(s_ap_ip);

    /* Start the captive-portal HTTP server. */
    portal_http_start();

    /* Arm the auto-stop timeout (0 = run until stopped explicitly) and the
     * loop-task wake channel that executes it. */
    s_start_us = esp_timer_get_time();
    if (P4_CONFIG_PORTAL_TIMEOUT_SECS > 0) {
        esp_timer_create_args_t args = {
            .callback = portal_timeout_cb,
            .name = "portal_timeout",
        };

        if (s_timeout_timer == NULL &&
            esp_timer_create(&args, &s_timeout_timer) != ESP_OK) {
            ESP_LOGW(PORTAL_TAG, "timeout timer unavailable; portal runs until stopped");
        } else if (s_timeout_timer != NULL) {
            esp_timer_stop(s_timeout_timer);
            if (esp_timer_start_once(s_timeout_timer,
                                     (uint64_t)P4_CONFIG_PORTAL_TIMEOUT_SECS * 1000000ULL) != ESP_OK) {
                ESP_LOGW(PORTAL_TAG, "timeout timer did not start; portal runs until stopped");
            }
        }
    }
    (void)esp_event_handler_register(PORTAL_EVENTS, ESP_EVENT_ANY_ID,
                                     portal_event_handler, NULL);

    s_portal_active = true;
    ESP_LOGI(PORTAL_TAG, "captive portal active: SSID \"%s\"", P4_CONFIG_PORTAL_AP_SSID);
    return ESP_OK;
}

esp_err_t portal_stop(void)
{
#if !P4_CONFIG_PORTAL_ENABLE
    return ESP_OK;
#endif

    if (!s_portal_active) {
        return ESP_OK;
    }

    ESP_LOGI(PORTAL_TAG, "stopping captive portal");

    portal_http_stop();
    dns_server_stop();

    /* Restore the pre-portal TX power cap. */
    if (s_saved_tx_power >= 0) {
        esp_wifi_set_max_tx_power((int8_t)s_saved_tx_power);
        s_saved_tx_power = -1;
    }

    /* Switch back to station-only mode. */
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    if (s_timeout_timer != NULL) {
        esp_timer_stop(s_timeout_timer);
        esp_timer_delete(s_timeout_timer);
        s_timeout_timer = NULL;
    }
    esp_event_handler_unregister(PORTAL_EVENTS, ESP_EVENT_ANY_ID, portal_event_handler);

    s_portal_active = false;
    s_stop_requested = false;
    s_start_us = 0;
    s_connect_pending = false;
    s_connect_ssid[0] = '\0';
    s_connect_password[0] = '\0';
    ESP_LOGI(PORTAL_TAG, "captive portal stopped");
    return ESP_OK;
}

bool portal_is_active(void)
{
    return s_portal_active;
}

bool portal_should_stop(void)
{
    /* A fired timeout also reads as a stop request, so the portal cannot
     * outlive its configuration even if no wake event arrives. */
    if (s_portal_active && !s_stop_requested && P4_CONFIG_PORTAL_TIMEOUT_SECS > 0 &&
        s_start_us > 0 &&
        esp_timer_get_time() - s_start_us >
            (int64_t)P4_CONFIG_PORTAL_TIMEOUT_SECS * 1000000LL) {
        s_stop_requested = true;
    }
    return s_stop_requested;
}

bool portal_should_autostart(void)
{
#if !P4_CONFIG_PORTAL_ENABLE
    return false;
#else
    /* The portal should auto-start when:
     *  1. The SD card is not mounted (no CONFIG.SYS, no known-network file)
     *  2. No known networks are cached in memory
     *  3. The Wi-Fi stack is in STA mode (not already in APSTA) */
    if (s_portal_active) {
        return false;
    }
    if (storage_sd_is_mounted()) {
        return false;
    }
    if (networking_wifi_known_count() > 0) {
        return false;
    }
    return true;
#endif
}

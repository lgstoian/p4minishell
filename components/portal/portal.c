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
extern bool storage_sd_is_mounted(void);

#define PORTAL_TAG "portal"

static bool s_portal_active;
static esp_netif_t *s_ap_netif;
static uint32_t s_ap_ip;

/** Deferred stop request (from the HTTP handler task). */
static volatile bool s_stop_requested;

void portal_request_stop(void)
{
    s_stop_requested = true;
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

    /* Switch to APSTA mode so we can still scan and potentially connect
     * once credentials are entered. */
    {
        esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);

        if (error != ESP_OK) {
            ESP_LOGE(PORTAL_TAG, "wifi_set_mode(APSTA): %s", esp_err_to_name(error));
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
            return error;
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

    /* Switch back to station-only mode. */
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    s_portal_active = false;
    s_stop_requested = false;
    ESP_LOGI(PORTAL_TAG, "captive portal stopped");
    return ESP_OK;
}

bool portal_is_active(void)
{
    return s_portal_active;
}

bool portal_should_stop(void)
{
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

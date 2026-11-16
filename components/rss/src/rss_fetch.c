/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file rss_fetch.c
 * @brief Feed fetching for the RSS reader (see rss.h).
 *
 * HTTP lives entirely in components/networking (the sole owner of the
 * esp_http_client surface); this file only assembles the request shape the
 * reader needs — conditional-GET headers, Basic or Bearer auth — and hands it
 * to networking_http_get_ex(). A 304 is reported as ESP_ERR_NOT_MODIFIED so
 * callers can keep their cached copy.
 */

#include "rss.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "networking.h"

#define RSS_FETCH_LOG_TAG "rss"

#define RSS_MAX_CONDITIONAL_HEADERS 2

/** Render a unix timestamp as an IMF-fixdate (RFC 7231), the only format
 *  If-Modified-Since accepts. Returns false when there is no known date. */
static bool rss_format_http_date(uint64_t unix_seconds, char *out, size_t out_size)
{
    time_t seconds = (time_t)unix_seconds;
    struct tm utc;

    if (unix_seconds == 0 || out == NULL || out_size == 0) {
        return false;
    }
    if (gmtime_r(&seconds, &utc) == NULL) {
        return false;
    }
    out[0] = '\0';
    strftime(out, out_size, "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return out[0] != '\0';
}

esp_err_t rss_fetch(const char *url, const rss_subscription_t *sub,
                    rss_feed_t *feed, char *etag_out, size_t etag_out_size,
                    char *last_modified_out, size_t lm_out_size)
{
    networking_http_header_t headers[RSS_MAX_CONDITIONAL_HEADERS];
    size_t header_count = 0;
    networking_http_result_t result;
    const char *auth_user = NULL;
    const char *auth_pass = NULL;
    const char *bearer = NULL;
    esp_err_t err;

    if (url == NULL || url[0] == '\0' || feed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Conditional GET: let the server answer 304 when the cached copy is
     * current. Only send validators the caller actually has. */
    if (sub != NULL) {
        if (sub->etag[0] != '\0') {
            headers[header_count].name = "If-None-Match";
            headers[header_count].value = sub->etag;
            header_count++;
        }
        if (sub->last_modified[0] != '\0') {
            headers[header_count].name = "If-Modified-Since";
            headers[header_count].value = sub->last_modified;
            header_count++;
        }
        if (sub->auth_type == RSS_AUTH_BASIC) {
            auth_user = (sub->auth_user[0] != '\0') ? sub->auth_user : NULL;
            auth_pass = sub->auth_token;
        } else if (sub->auth_type == RSS_AUTH_BEARER) {
            bearer = (sub->auth_token[0] != '\0') ? sub->auth_token : NULL;
        }
    }

    err = networking_http_get_ex(url,
                                 header_count > 0 ? headers : NULL, header_count,
                                 auth_user, auth_pass, bearer,
                                 &result);

    /* networking reports 304 as ESP_OK with an empty body. */
    if (err == ESP_OK && result.http_status == 304) {
        networking_http_result_free(&result);
        return RSS_ERR_NOT_MODIFIED;
    }
    if (err != ESP_OK) {
        networking_http_result_free(&result);
        return err;
    }

    if (result.body != NULL && result.body_size > P4_CONFIG_RSS_MAX_BODY_BYTES) {
        networking_http_result_free(&result);
        return ESP_ERR_INVALID_SIZE;
    }

    err = rss_parse((const char *)result.body, result.body_size, feed);

    /* Capture the validators before releasing the result: the caller persists
     * them so the next fetch can send a conditional GET instead of re-downloading
     * the whole body. */
    if (etag_out != NULL && etag_out_size > 0) {
        snprintf(etag_out, etag_out_size, "%s", result.etag);
    }
    if (last_modified_out != NULL && lm_out_size > 0) {
        if (!rss_format_http_date(result.last_modified, last_modified_out, lm_out_size)) {
            last_modified_out[0] = '\0';
        }
    }

    networking_http_result_free(&result);
    if (err != ESP_OK) {
        ESP_LOGW(RSS_FETCH_LOG_TAG, "failed to parse feed: %s", url);
        return err;
    }

    return ESP_OK;
}

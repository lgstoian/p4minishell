/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file rss.h
 * @brief RSS 2.0 / Atom 1.0 / JSON Feed reader — public API.
 *
 * CONFIG POLICY (two tiers, no hardcoded limits):
 *
 *   - Compile-time capacities come from p4minishell_config.h (generated from
 *     p4minishell_config.yaml as P4_CONFIG_RSS_*). C cannot size a struct
 *     member at runtime, so these are necessarily compile-time. Nothing in
 *     this header carries its own fallback default; an RSS build with the
 *     config missing is a build error, not a silent default.
 *
 *   - Every behavioural tunable (poll interval, retention, unread handling,
 *     auto-open, save policy, ...) lives in CONFIG.SYS on the SD card and is
 *     read through the `config` command table + the boot directive parser,
 *     exactly like BRIGHTNESS / HEADER / DISPLAY_TIMEOUT.
 */

#ifndef P4MINISHELL_RSS_H
#define P4MINISHELL_RSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * ERRORS
 *
 * ESP-IDF has no "not modified" code, so the reader owns one (its own base,
 * clear of the IDF Wi-Fi/HTTP bases) rather than overloading NOT_FOUND.
 * ======================================================================== */

#define RSS_ERR_BASE          0x9000
/** HTTP 304: the caller's cached copy is current; the feed is unchanged. */
#define RSS_ERR_NOT_MODIFIED  (RSS_ERR_BASE + 1)

/* ========================================================================
 * FORMAT / AUTH
 * ======================================================================== */

typedef enum {
    RSS_FORMAT_UNKNOWN = 0,
    RSS_FORMAT_RSS_2_0,
    RSS_FORMAT_ATOM_1_0,
    RSS_FORMAT_JSON_FEED,
} rss_format_t;

typedef enum {
    RSS_AUTH_NONE = 0,
    RSS_AUTH_BASIC,
    RSS_AUTH_BEARER,
} rss_auth_type_t;

/* ========================================================================
 * ITEMS AND FEEDS
 *
 * Field capacities are P4_CONFIG_RSS_* (see the config policy above). Text
 * fields hold decoded, entity-resolved UTF-8.
 * ======================================================================== */

typedef struct {
    char guid[P4_CONFIG_RSS_ITEM_GUID_BYTES];
    char title[P4_CONFIG_RSS_ITEM_TITLE_BYTES];
    char link[P4_CONFIG_RSS_ITEM_LINK_BYTES];
    char summary[P4_CONFIG_RSS_ITEM_SUMMARY_BYTES];
    char content[P4_CONFIG_RSS_ITEM_CONTENT_BYTES];
    char author[P4_CONFIG_RSS_ITEM_AUTHOR_BYTES];
    char categories[P4_CONFIG_RSS_ITEM_CATEGORIES_BYTES];
    char published[P4_CONFIG_RSS_ITEM_DATE_BYTES];
    bool read;          /**< read/unread state, persisted per feed. */
} rss_item_t;

typedef struct {
    rss_format_t format;
    char title[P4_CONFIG_RSS_FEED_TITLE_BYTES];
    char link[P4_CONFIG_RSS_FEED_LINK_BYTES];
    char description[P4_CONFIG_RSS_FEED_DESC_BYTES];
    char generator[P4_CONFIG_RSS_FEED_GENERATOR_BYTES];
    char updated[P4_CONFIG_RSS_ITEM_DATE_BYTES];
    rss_item_t *items;  /**< Owned array, item_count entries, PSRAM. */
    int item_count;
    int item_capacity;
} rss_feed_t;

/* ========================================================================
 * SUBSCRIPTIONS
 * ======================================================================== */

typedef struct {
    char name[P4_CONFIG_RSS_SUB_NAME_BYTES];     /**< INI section key. */
    char url[P4_CONFIG_RSS_SUB_URL_BYTES];
    char title[P4_CONFIG_RSS_FEED_TITLE_BYTES];  /**< Cached feed title. */
    char last_fetched[P4_CONFIG_RSS_ITEM_DATE_BYTES];
    char etag[P4_CONFIG_RSS_SUB_ETAG_BYTES];      /**< If-None-Match. */
    char last_modified[P4_CONFIG_RSS_SUB_ETAG_BYTES];
    int interval_min;                            /**< CONFIG.SYS default. */
    bool enabled;
    rss_auth_type_t auth_type;
    char auth_user[P4_CONFIG_RSS_SUB_AUTH_BYTES];
    char auth_token[P4_CONFIG_RSS_SUB_AUTH_BYTES]; /**< pass or bearer. */
} rss_subscription_t;

/* ========================================================================
 * PARSE (pure; no I/O — unit-testable on the host)
 * ======================================================================== */

/** Parse RSS 2.0 / Atom 1.0 / JSON Feed, auto-detecting the format.
 *  On ESP_OK @p feed owns an item array the caller releases with
 *  rss_feed_free(). */
esp_err_t rss_parse(const char *data, size_t len, rss_feed_t *feed);

/** Release the item array owned by @p feed and zero it. Safe on a zeroed or
 *  already-freed feed. */
void rss_feed_free(rss_feed_t *feed);

/** Format name for transcripts ("RSS", "Atom", "JSON Feed"). */
const char *rss_format_name(rss_format_t format);

/* ========================================================================
 * FETCH
 *
 * Conditional GET (If-None-Match / If-Modified-Since) and Bearer auth are not
 * expressible through networking_http_get(), which takes only (url, result).
 * RSS uses networking_http_get_ex(), a networking-side extension for custom
 * request headers and Basic/Bearer auth; it lives inside components/networking
 * because that module is the sole owner of the esp_http_client surface.
 * ======================================================================== */

/** Fetch and parse @p url.
 *  @return ESP_OK                 parsed, feed populated
 *          RSS_ERR_NOT_MODIFIED   HTTP 304: cached copy is current
 *          ESP_ERR_INVALID_SIZE   body over P4_CONFIG_RSS_MAX_BODY_BYTES
 *          ESP_ERR_NOT_FOUND      non-2xx
 *          ESP_ERR_INVALID_STATE  no Wi-Fi
 *          ESP_ERR_INVALID_ARG    unparseable feed
 *  @p etag_out / @p last_modified_out may be NULL. */
esp_err_t rss_fetch(const char *url, const rss_subscription_t *sub,
                    rss_feed_t *feed, char *etag_out, size_t etag_out_size,
                    char *last_modified_out, size_t lm_out_size);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_RSS_H */
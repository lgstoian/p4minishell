/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file rss_store.h
 * @module rss
 * @layer leaf
 * @domain net
 * @provides Persistent RSS subscription list, read/unread state and feed poller
 * @status stable
 * @hard-rule ai-context.md#module-layering-rules
 * @test test/main/test_rss_store.c
 * @brief Subscription store for the RSS reader — public API.
 *
 * The store owns three things and nothing else:
 *
 *   1. The subscription list (`sd:/RSS/SUBS.INI`), one pipe-separated record
 *      per feed: NAME|URL|TYPE|USER|TOKEN|ETAG|MODIFIED|FETCHED|ENABLED|
 *      INTERVAL|TITLE. This is the same hand-editable line format
 *      `sd:/WIFI.KNOWN` uses, and it deliberately avoids the flat-INI key
 *      caps (`P4_CONFIG_ENV_NAME_BYTES`/`_VALUE_BYTES`) which a 512-byte feed
 *      URL would overflow.
 *   2. Read/unread state (`sd:/RSS/READING`), NAME|HASH lines. The hash is a
 *      64-bit FNV-1a of the item guid: the table is capped at
 *      P4_CONFIG_RSS_READ_MAX entries in PSRAM, so storing whole guids (up to
 *      256 bytes each) would cost ~50 KB of scarce internal DRAM.
 *   3. The session's last fetched feed, so `rss read` can page through it
 *      without refetching.
 *
 * SAFETY CONTRACT (same shape as wifi_known):
 *   - Every SD operation runs under a guarded session (`shell_sd_begin` /
 *     `shell_sd_end`) and tolerates failure at every step.
 *   - No card, mount failure, missing/corrupt file, read-only or full disk
 *     leaves the store usable and returns a non-OK error; nothing crashes,
 *     freezes or retries forever.
 *   - Tokens are persisted (the feed needs them) but are NEVER printed to the
 *     transcript, history or debug log. `rss list` shows the auth type only.
 */

#ifndef P4MINISHELL_RSS_STORE_H
#define P4MINISHELL_RSS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "networking.h"
#include "rss.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * PURE RECORD EDITORS (no I/O - unit-testable on the host)
 * ======================================================================== */

/** Validate a subscription name: [A-Za-z0-9_-]+ and non-empty. Names are used
 *  in transcript output and as the read-state key, never as a path. */
bool rss_store_name_ok(const char *name);

/** Parse one pipe-separated subscription record. Returns ESP_ERR_INVALID_ARG
 *  on a malformed line (wrong field count, bad auth type, '|' in a field). */
esp_err_t rss_store_parse_record(const char *line, rss_subscription_t *out);

/** Render one subscription record. @return false when it does not fit @p cap. */
bool rss_store_render_record(const rss_subscription_t *sub, char *out, size_t cap);

/** Parse one NAME|HASH read-state record. */
bool rss_store_parse_read_line(const char *line, char *name, size_t name_size,
                               char *hash, size_t hash_size);

/** Render a unix timestamp for the transcript ("YYYY-MM-DD HH:MM", local).
 *  Returns false when the clock is not set. */
bool rss_store_format_time(uint64_t unix_seconds, char *out, size_t out_size);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/** Bind the host render/log callbacks and create the module lock. Safe to call
 *  once from main after networking_init(). */
void rss_store_init(const networking_host_ops_t *ops);

/** Load subscriptions and read state from SD into the RAM mirror.
 *  @return ESP_OK, ESP_ERR_NOT_FOUND when the store/card is absent, or an I/O
 *          error. Any non-OK leaves a usable (possibly empty) store. */
esp_err_t rss_store_load(void);

/** Persist subscriptions and read state atomically (temp file + rename, with
 *  a free-space pre-check). Safe to call when the card is absent. */
esp_err_t rss_store_save(void);

/* ========================================================================
 * SUBSCRIPTIONS
 * ======================================================================== */

/** Number of subscriptions currently held. */
int rss_store_count(void);

/** Copy a subscription by index. @return false when the index is invalid. */
bool rss_store_get(int index, rss_subscription_t *out);

/** Index of the subscription named @p name, or -1. */
int rss_store_find(const char *name);

/** Insert or replace a subscription (matched by name) and persist.
 *  @return ESP_OK, ESP_ERR_NO_MEM when the list is full, or the save error. */
esp_err_t rss_store_add(const rss_subscription_t *sub);

/** Remove a subscription by name and persist. ESP_OK even when absent. */
esp_err_t rss_store_remove(const char *name);

/** Replace an existing subscription (matched by name) and persist. */
esp_err_t rss_store_update(const rss_subscription_t *sub);

/** Set the enabled flag and persist. */
esp_err_t rss_store_set_enabled(const char *name, bool enabled);

/** Set the per-feed poll interval (minutes; 0 = use the CONFIG.SYS default). */
esp_err_t rss_store_set_interval(const char *name, int minutes);

/* ========================================================================
 * FETCH
 * ======================================================================== */

/** Fetch and parse one subscription, persisting its validators (ETag /
 *  Last-Modified) and cached title on success.
 *
 *  When @p feed is non-NULL the parsed feed is handed to the caller (released
 *  with rss_feed_free()) and the session cache is left alone; when @p feed is
 *  NULL the parsed feed becomes the session cache for rss_store_cached_item()
 *  and rss_store_cached_unread(). The poller always passes its own feed so it
 *  never races a reader.
 *  @return ESP_OK, RSS_ERR_NOT_MODIFIED, ESP_ERR_NOT_FOUND when the
 *          subscription is unknown, or a network/parse error. */
esp_err_t rss_store_fetch(const char *name, rss_feed_t *feed);

/** Fetch every enabled subscription in index order, persisting metadata.
 *  Prints a one-line summary per feed through the host transcript hook. */
esp_err_t rss_store_refresh_all(void);

/** Deep-copy one item out of the session cache under the store lock.
 *
 *  A borrowed feed pointer would race the poller (which replaces the cache), so
 *  readers get their own copy instead. The copy is PSRAM-allocated because
 *  rss_item_t is ~11 KB - far too big for a command-worker frame, which a nested
 *  batch would multiply.
 *
 *  @param want  1-based item number, or 0 for the first unread (falling back to
 *               the newest when everything is read).
 *  @return the owned copy (release with free()), or NULL when the feed is not
 *          cached or has no such item. */
rss_item_t *rss_store_cached_item(const char *name, int want, int *index_out);

/** Unread count for the cached feed (0 when nothing is cached for @p name). */
int rss_store_cached_unread(const char *name);

/** Drop the session cache. */
void rss_store_clear_cache(void);

/* ========================================================================
 * READ / UNREAD STATE
 * ======================================================================== */

/** True when the item guid has been read for @p name. */
bool rss_store_is_read(const char *name, const char *guid);

/** Mark an item read and persist. */
void rss_store_mark_read(const char *name, const char *guid);

/** Number of unread items in @p feed for @p name. */
int rss_store_unread_count(const char *name, const rss_feed_t *feed);

/** Forget every read item for @p name and persist. */
esp_err_t rss_store_reset_read(const char *name);

/** Enable/disable read-state persistence (CONFIG.SYS RSS_SAVEREAD). */
void rss_store_set_save_read(bool enable);
bool rss_store_get_save_read(void);

/* ========================================================================
 * TUNABLES (CONFIG.SYS mirrors)
 * ======================================================================== */

void rss_store_set_interval_default(int minutes);
int  rss_store_get_interval_default(void);
void rss_store_set_autopoll(bool enable);
bool rss_store_get_autopoll(void);
void rss_store_set_retention(int items);
int  rss_store_get_retention(void);

/** Apply one CONFIG.SYS directive (RSS_INTERVAL / RSS_AUTOPOLL /
 *  RSS_RETENTION / RSS_SAVEREAD, or their short spellings). The parsing and
 *  validation live here so the `config` table and the boot parser cannot drift.
 *  @return false on an unknown key or an invalid value. */
bool rss_store_apply_directive(const char *key, const char *value);

/** Render a directive's current value (same keys as rss_store_apply_directive). */
bool rss_store_render_directive(const char *key, char *out, size_t out_size);

/* ========================================================================
 * BACKGROUND POLLER
 * ======================================================================== */

/** Start the polling task when RSS_AUTOPOLL is on; a no-op otherwise.
 *  Safe to call more than once. */
void rss_store_start_poller(void);
void rss_store_stop_poller(void);
bool rss_store_poller_running(void);

/* ========================================================================
 * OPML IMPORT / EXCHANGE
 * ======================================================================== */

/** Merge an OPML 2.0 document into the subscription list. Existing names are
 *  left untouched; the return value counts how many were added. */
esp_err_t rss_store_import(const char *path, int *added);

/** Write the subscription list as an OPML 2.0 document. */
esp_err_t rss_store_export(const char *path);

/* ---- rss_opml.c: the interchange format itself ---- */

/** Parse an OPML document into @p out (at most @p max entries), applying the
 *  same name rules as rss_store_add. @p out_count receives how many were
 *  parsed; existing names are skipped. */
esp_err_t rss_opml_parse(const char *text, size_t len,
                         rss_subscription_t *out, int max, int *out_count);

/** Write an OPML 2.0 document for @p count subscriptions to an open file. */
void rss_opml_write(FILE *file, const rss_subscription_t *subs, int count);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_RSS_STORE_H */

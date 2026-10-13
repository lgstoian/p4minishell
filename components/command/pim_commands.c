/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim_commands.c
 * @brief `pim` — serial CardDAV-lite sync over the shared transfer engine.
 *
 * `pim get` renders vCard/iCalendar with sync identity (UID/REV/DTSTAMP)
 * and streams it as one PIMX frame through serial_xfer_stream_buffer (the
 * same engine `send /diag` uses); `pim put` receives a stream through
 * serial_xfer_receive_pump (the same engine `receive` uses, same READY /
 * DONE markers) into a temp file and merges it by uid with newer-wins.
 * Identity, merge, and backfill live in components/pim; this file is usage,
 * framing, and summaries only. `export`/`import` are untouched.
 *
 * Batch-friendly: ERRORLEVEL 0 synced, 1 empty/failed, 2 usage/I-O.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "alarm.h"
#include "batch.h"
#include "command.h"
#include "db.h"
#include "networking.h"
#include "pim.h"
#include "security_commands.h"
#include "shell.h"
#include "storage.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static void shell_command_pim_usage(void)
{
    shell_print_usage("Usage: pim get db <name> | pim get alarms");
    shell_print_usage("       pim put db <name> <size> [/crc] | pim put alarms <size> [/crc]");
    shell_print_usage("       pim net <pull|push|sync> <url> <user> <db> [/p:<pass>]");
}

/* ========================================================================
 * pim get — backfill, render with identity, stream one PIMX frame
 * ======================================================================== */

static int pim_get_db(const char *name)
{
    pim_doc_t doc;
    int rows = 0;
    int backfilled = 0;
    int rc;
    int64_t start_us;
    bool stream_ok = false;

    /* Mirror `export db`: refuse while locked (a locked session cannot
     * exfiltrate private records), include secrets once unlocked. */
    if (!security_can_reveal_private()) {
        shell_print_error("pim: device locked - unlock before syncing secret records");
        return 1;
    }
    rc = pim_db_backfill(name, &backfilled);
    if (rc == 1) {
        shell_print_error("pim: nothing to sync");
        return 1;
    }
    if (rc != 0) {
        shell_print_error("pim: cannot read %s", name);
        return 2;
    }
    if (backfilled > 0) {
        /* Printed before framing: the host scans for the PIMX magic and
         * skips console text, like pull.py skips log lines. */
        shell_print_muted("pim: backfilled %d sync identit%s", backfilled,
                          backfilled == 1 ? "y" : "ies");
    }
    pim_doc_init(&doc);
    if (doc.overflow) {
        shell_print_error("pim: out of memory");
        return 2;
    }
    rc = pim_db_render_vcf_uid(name, &doc, &rows);
    if (rc != 0) {
        if (rc == 1) {
            shell_print_error("pim: nothing to sync");
        } else {
            shell_print_error("pim: cannot read %s", name);
        }
        pim_doc_free(&doc);
        return rc == 1 ? 1 : 2;
    }
    if (doc.overflow) {
        pim_doc_free(&doc);
        shell_print_error("pim: output exceeds %d bytes", P4_CONFIG_DB_EXPORT_MAX_BYTES);
        return 1;
    }
    if (rows == 0) {
        pim_doc_free(&doc);
        shell_print_muted("pim: store is empty");
        return 1;
    }

    start_us = esp_timer_get_time();

    /* The transfer owns the raw console byte stream: suspend the console
     * reader so no host input is misread and no echo interleaves. */
    shell_uart_console_rx_begin();
    stream_ok = serial_xfer_stream_buffer((const uint8_t *)doc.data, doc.used,
                                          P4_CONFIG_PIM_SYNC_MAGIC);
    pim_doc_free(&doc);
    if (stream_ok) {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        shell_uart_console_write_text("\n" P4_CONFIG_SERIAL_TX_DONE_MARKER "\n");
        shell_uart_console_rx_end();
        shell_print_ok("pim: streamed %d contact(s) from %s in %u ms",
                       rows, name, ms);
        return 0;
    }
    shell_uart_console_rx_end();
    shell_print_error("pim: transfer failed");
    return 1;
}

static int pim_get_alarms(void)
{
    pim_doc_t doc;
    int rows = 0;
    int backfilled = 0;
    int rc;
    int64_t start_us;
    bool stream_ok = false;

    /* No lock gate: mirrors `export alarms` (events carry no secret flag). */
    rc = pim_alarms_backfill(&backfilled);
    if (rc != 0) {
        shell_print_error("pim: cannot read alarms");
        return 2;
    }
    if (backfilled > 0) {
        shell_print_muted("pim: backfilled %d sync identit%s", backfilled,
                          backfilled == 1 ? "y" : "ies");
    }
    pim_doc_init(&doc);
    if (doc.overflow) {
        shell_print_error("pim: out of memory");
        return 2;
    }
    rc = pim_alarms_render_ics_uid(&doc, &rows);
    if (rc != 0) {
        shell_print_error("pim: cannot read alarms");
        pim_doc_free(&doc);
        return 2;
    }
    if (doc.overflow) {
        pim_doc_free(&doc);
        shell_print_error("pim: output exceeds %d bytes", P4_CONFIG_DB_EXPORT_MAX_BYTES);
        return 1;
    }
    if (rows == 0) {
        pim_doc_free(&doc);
        shell_print_muted("pim: store is empty");
        return 1;
    }

    start_us = esp_timer_get_time();
    shell_uart_console_rx_begin();
    stream_ok = serial_xfer_stream_buffer((const uint8_t *)doc.data, doc.used,
                                          P4_CONFIG_PIM_SYNC_MAGIC);
    pim_doc_free(&doc);
    if (stream_ok) {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        shell_uart_console_write_text("\n" P4_CONFIG_SERIAL_TX_DONE_MARKER "\n");
        shell_uart_console_rx_end();
        shell_print_ok("pim: streamed %d event(s) in %u ms", rows, ms);
        return 0;
    }
    shell_uart_console_rx_end();
    shell_print_error("pim: transfer failed");
    return 1;
}

/* ========================================================================
 * pim put — receive a stream into a temp file, merge by uid
 * ======================================================================== */

static int pim_put_stream(unsigned long size, bool verify_crc,
                          const char *dbname, bool alarms)
{
    char tmp_path[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    unsigned long cumulative = 0;
    pim_count_t count = {0, 0};
    bool ok = false;
    int64_t start_us;

    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("pim: SD card not present - insert and retry");
        return 1;
    }
    if (!storage_check_free_space((uint64_t)size + 64, 0, "pim")) {
        shell_sd_end(&session, "pim");
        return 1;
    }
    if (storage_temp_path(tmp_path, sizeof(tmp_path), "rx") != ESP_OK) {
        shell_print_error("pim: cannot create temp file");
        shell_sd_end(&session, "pim");
        return 1;
    }
    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        shell_print_error("pim: cannot create %s", tmp_path);
        shell_sd_end(&session, "pim");
        return 1;
    }

    /* Suspend the console reader BEFORE signalling READY so that bytes the
     * host sends the instant it sees the marker cannot be consumed as
     * command lines (same ordering as `receive`). */
    start_us = esp_timer_get_time();
    shell_uart_console_rx_begin();

    /* Ready marker: the host now streams `size` raw bytes, ACK-paced. */
    shell_uart_console_write_text("\n" P4_CONFIG_SERIAL_RX_READY_MARKER "\n");

    ok = serial_xfer_receive_pump(file, size, verify_crc, "pim", &cumulative);

    fclose(file);
    file = NULL;

    if (!ok || cumulative < size) {
        if (verify_crc && cumulative == size) {
            shell_print_error("pim: transfer failed - CRC mismatch or missing trailer (%lu bytes received)",
                              size);
        } else {
            shell_print_error("pim: transfer incomplete (%lu/%lu bytes)",
                              cumulative, size);
        }
        remove(tmp_path);
        shell_sd_end(&session, "pim");
        return 1;
    }

    file = fopen(tmp_path, "rb");
    if (file == NULL) {
        shell_print_error("pim: cannot re-read transfer");
        remove(tmp_path);
        shell_sd_end(&session, "pim");
        return 1;
    }
    if (alarms) {
        ok = pim_ics_merge_stream(file, &count) == 0;
    } else {
        ok = pim_vcf_merge_stream(file, dbname, &count) == 0;
    }
    fclose(file);
    remove(tmp_path);
    shell_sd_end(&session, "pim");
    if (!ok) {
        shell_print_error("pim: cannot parse transfer");
        return 1;
    }
    shell_uart_console_write_text(P4_CONFIG_SERIAL_RX_DONE_MARKER "\n");
    {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        uint32_t rate = ms > 0 ? (uint32_t)((size * 1000u) / ms) : 0u;

        if (count.ok == 0) {
            if (alarms) {
                shell_print_muted("pim: nothing merged (%d skipped)", count.skipped);
            } else {
                shell_print_muted("pim: nothing merged into %s (%d skipped)",
                                  dbname, count.skipped);
            }
            return 1;
        }
        if (alarms) {
            shell_print_ok("pim: merged %d event(s) in %u ms (%lu KB/s, %d skipped)",
                           count.ok, ms, (unsigned long)(rate / 1024u),
                           count.skipped);
        } else {
            shell_print_ok("pim: merged %d record(s) into %s in %u ms (%lu KB/s, %d skipped)",
                           count.ok, dbname, ms, (unsigned long)(rate / 1024u),
                           count.skipped);
        }
    }
    return 0;
}

/* ========================================================================
 * pim net — HTTP CardDAV sync (pull|push|sync) over networking_http_request
 * ========================================================================
 * CardDAV subset against a collection URL (Nextcloud/SabreDAV-style):
 * PROPFIND Depth:1 lists resources, GET fetches each into the serial merge
 * core (newer-wins, same as `pim put db`), PUT pushes local cards rendered
 * with sync identity. No tombstones in v1: deletions do not propagate either
 * way (documented). Credentials travel only in the Basic header, never the
 * URL; an inline /p: is history-masked like crypt, otherwise a secure prompt
 * asks when a key source exists.
 */

#define PIM_NET_URL_BYTES 512
#define PIM_NET_USER_BYTES 96
#define PIM_NET_PASS_BYTES 96
#define PIM_NET_UID_BYTES 128

static const char pim_net_propfind_body[] =
    "<?xml version=\"1.0\"?>"
    "<d:propfind xmlns:d=\"DAV:\">"
    "<d:prop><d:resourcetype/><d:getetag/><d:getcontenttype/></d:prop>"
    "</d:propfind>";

/** Pull-listing accumulator for one PROPFIND scan. */
typedef struct {
    char (*hrefs)[PIM_NET_URL_BYTES];
    int count;
    int cap;
} pim_net_list_t;

static void pim_net_list_add(const char *href, const char *etag_or_null, void *ctx)
{
    pim_net_list_t *list = (pim_net_list_t *)ctx;

    (void)etag_or_null;
    if (list->count >= list->cap) {
        return;
    }
    snprintf(list->hrefs[list->count], PIM_NET_URL_BYTES, "%s", href);
    list->count++;
}

/** Path part of a URL (after authority), "" when absent. */
static const char *pim_net_url_path(const char *url)
{
    const char *scheme;
    const char *slash;

    if (url == NULL) {
        return "";
    }
    scheme = strstr(url, "://");
    if (scheme == NULL) {
        return url;
    }
    slash = strchr(scheme + 3, '/');
    return (slash != NULL) ? slash : "";
}

/** True when @p href addresses the collection itself (self-response) or a
 *  nested collection (trailing '/'): neither is a downloadable card. */
static bool pim_net_href_is_container(const char *collection_url, const char *href)
{
    const char *cpath = pim_net_url_path(collection_url);
    const char *hpath = href;
    char joined[PIM_NET_URL_BYTES];
    size_t clen;
    size_t hlen;

    if (href[0] != '/' && strstr(href, "://") == NULL) {
        /* Relative reference: resolve against the collection first. */
        if (!pim_carddav_join_url(collection_url, href, joined, sizeof(joined))) {
            return true;
        }
        hpath = pim_net_url_path(joined);
    } else if (strstr(href, "://") != NULL) {
        hpath = pim_net_url_path(href);
    }
    clen = strlen(cpath);
    hlen = strlen(hpath);
    while (clen > 0 && cpath[clen - 1] == '/') {
        clen--;
    }
    while (hlen > 0 && hpath[hlen - 1] == '/') {
        hlen--;
    }
    if (clen == hlen && strncmp(cpath, hpath, clen) == 0) {
        return true;
    }
    /* Any trailing-slash remainder is a nested collection. */
    return hlen > 0 && href[strlen(href) - 1] == '/';
}

/** Resolve the password from /p:<pass> (history-masked like crypt).
 *  No interactive prompt: modal password entry over a headless serial link
 *  is racy (verified on hardware), and unattended batch runs need the
 *  credential inline anyway. */
static bool pim_net_password(int argc, char **argv, char *out, size_t out_size)
{
    int i;

    out[0] = '\0';
    for (i = 0; i < argc; i++) {
        if (strncasecmp(argv[i], "/p:", 3) == 0) {
            snprintf(out, out_size, "%s", argv[i] + 3);
            if (out[0] == '\0') {
                break;
            }
            return true;
        }
    }
    shell_print_error("pim: password required (use /p:<pass>)");
    return false;
}

/** Merge one fetched body into @p dbname via a temp file (same merge core as
 *  the serial `pim put db` path). */
static bool pim_net_merge_body(const uint8_t *body, size_t body_size,
                               const char *dbname, pim_count_t *count)
{
    char tmp_path[P4_CONFIG_SD_PATH_BYTES];
    FILE *file;
    bool ok;

    if (storage_temp_path(tmp_path, sizeof(tmp_path), "pimnet") != ESP_OK) {
        return false;
    }
    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        return false;
    }
    if (body_size > 0 && fwrite(body, 1, body_size, file) != body_size) {
        fclose(file);
        remove(tmp_path);
        return false;
    }
    fclose(file);
    file = fopen(tmp_path, "rb");
    if (file == NULL) {
        remove(tmp_path);
        return false;
    }
    ok = pim_vcf_merge_stream(file, dbname, count) == 0;
    fclose(file);
    remove(tmp_path);
    return ok;
}

/** Pull: PROPFIND the collection, GET every card resource, merge newer-wins. */
static int pim_net_pull(const char *url, const char *user, const char *pass,
                        const char *dbname)
{
    networking_http_result_t res;
    pim_net_list_t list;
    char (*hrefs)[PIM_NET_URL_BYTES];
    int pulled = 0;
    int skipped = 0;
    int merged = 0;
    int failed = 0;
    int i;
    esp_err_t error;
    pim_count_t count = {0, 0};

    hrefs = calloc(P4_CONFIG_PIM_NET_MAX_RESOURCES, sizeof(*hrefs));
    if (hrefs == NULL) {
        shell_print_error("pim: out of memory");
        return 1;
    }
    list.hrefs = hrefs;
    list.count = 0;
    list.cap = P4_CONFIG_PIM_NET_MAX_RESOURCES;

    memset(&res, 0, sizeof(res));
    error = networking_http_request(HTTP_METHOD_PROPFIND, url,
                                    "application/xml",
                                    (const uint8_t *)pim_net_propfind_body,
                                    sizeof(pim_net_propfind_body) - 1,
                                    "1", user, pass, &res);
    if (error != ESP_OK) {
        shell_print_error("pim: address-book listing failed (%s)", esp_err_to_name(error));
        networking_http_result_free(&res);
        free(hrefs);
        return 1;
    }
    if (res.http_status != 207) {
        shell_print_error("pim: listing returned HTTP %d (need 207 Multi-Status)", res.http_status);
        networking_http_result_free(&res);
        free(hrefs);
        return 1;
    }
    pim_carddav_scan_multistatus((const char *)res.body, res.body_size,
                                 pim_net_list_add, &list);
    networking_http_result_free(&res);

    for (i = 0; i < list.count; i++) {
        char target[PIM_NET_URL_BYTES];

        if (pim_net_href_is_container(url, list.hrefs[i])) {
            continue;
        }
        if (pulled - skipped >= P4_CONFIG_PIM_NET_MAX_RESOURCES) {
            break;
        }
        if (!pim_carddav_join_url(url, list.hrefs[i], target, sizeof(target))) {
            skipped++;
            continue;
        }
        memset(&res, 0, sizeof(res));
        error = networking_http_request(HTTP_METHOD_GET, target,
                                        NULL, NULL, 0, NULL, user, pass, &res);
        if (error != ESP_OK) {
            shell_print_error("pim: fetch failed for %s (%s)", list.hrefs[i],
                              esp_err_to_name(error));
            networking_http_result_free(&res);
            failed++;
            continue;
        }
        pulled++;
        if (pim_net_merge_body(res.body, res.body_size, dbname, &count)) {
            merged += count.ok;
            skipped += count.skipped;
            count.ok = 0;
            count.skipped = 0;
        } else {
            failed++;
        }
        networking_http_result_free(&res);
        shell_transcript_appendf("pim: got %s\n", list.hrefs[i]);
    }
    free(hrefs);
    shell_print_ok("pim: pulled %d resource(s), merged %d (%d skipped, %d failed)",
                   pulled, merged, skipped, failed);
    return (failed > 0 && merged == 0) ? 1 : 0;
}

/** Push-card visitor: PUT one rendered card under its UID filename. */
typedef struct {
    const char *collection_url;
    const char *user;
    const char *pass;
    int pushed;
    int skipped;
    int failed;
} pim_net_push_t;

static void pim_net_push_card(const char *card, size_t len, void *ctx)
{
    pim_net_push_t *push = (pim_net_push_t *)ctx;
    char uid[PIM_NET_UID_BYTES];
    char target[PIM_NET_URL_BYTES];
    networking_http_result_t res;

    if (pim_carddav_card_uid(card, len, uid, sizeof(uid)) == 0 ||
        !pim_carddav_uid_safe(uid)) {
        push->skipped++;
        return;
    }
    {
        char filename[PIM_NET_UID_BYTES + 8];

        snprintf(filename, sizeof(filename), "%s.vcf", uid);
        if (!pim_carddav_join_url(push->collection_url, filename,
                                  target, sizeof(target))) {
            push->skipped++;
            return;
        }
    }
    memset(&res, 0, sizeof(res));
    if (networking_http_request(HTTP_METHOD_PUT, target, "text/vcard",
                                (const uint8_t *)card, len,
                                NULL, push->user, push->pass, &res) != ESP_OK) {
        push->failed++;
    } else {
        push->pushed++;
        shell_transcript_appendf("pim: put %s\n", uid);
    }
    networking_http_result_free(&res);
}

/** Push: render local cards with sync identity, PUT each under UID.vcf. */
static int pim_net_push(const char *url, const char *user, const char *pass,
                        const char *dbname)
{
    pim_doc_t doc;
    int rows = 0;
    int backfilled = 0;
    int rc;
    pim_net_push_t push;

    if (!security_can_reveal_private()) {
        shell_print_error("pim: device locked - unlock before syncing secret records");
        return 1;
    }
    rc = pim_db_backfill(dbname, &backfilled);
    if (rc == 1) {
        shell_print_error("pim: nothing to sync");
        return 1;
    }
    if (rc != 0) {
        shell_print_error("pim: cannot read %s", dbname);
        return 2;
    }
    pim_doc_init(&doc);
    if (doc.overflow) {
        shell_print_error("pim: out of memory");
        return 2;
    }
    rc = pim_db_render_vcf_uid(dbname, &doc, &rows);
    if (rc != 0 || rows == 0) {
        pim_doc_free(&doc);
        shell_print_muted("pim: store is empty");
        return 1;
    }
    if (doc.overflow) {
        pim_doc_free(&doc);
        shell_print_error("pim: output exceeds %d bytes", P4_CONFIG_DB_EXPORT_MAX_BYTES);
        return 1;
    }
    memset(&push, 0, sizeof(push));
    push.collection_url = url;
    push.user = user;
    push.pass = pass;
    pim_carddav_split_cards(doc.data, doc.used, pim_net_push_card, &push);
    pim_doc_free(&doc);
    shell_print_ok("pim: pushed %d card(s) (%d skipped, %d failed)",
                   push.pushed, push.skipped, push.failed);
    return (push.failed > 0 && push.pushed == 0) ? 1 : 0;
}

/** `pim net <pull|push|sync> <url> <user> <db> [/p:<pass>]`. */
static int pim_net_command(int argc, char **argv)
{
    const char *op;
    const char *url;
    const char *user;
    const char *dbname;
    char pass[PIM_NET_PASS_BYTES];
    shell_sd_session_t session;
    int rc;

    if (argc < 6) {
        shell_command_pim_usage();
        return 2;
    }
    op = argv[2];
    url = argv[3];
    user = argv[4];
    dbname = argv[5];
    if (!shell_text_equals_ignore_case(op, "pull") &&
        !shell_text_equals_ignore_case(op, "push") &&
        !shell_text_equals_ignore_case(op, "sync")) {
        shell_command_pim_usage();
        return 2;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        shell_print_error("pim: URL must start with http:// or https://");
        return 2;
    }
    if (!pim_net_password(argc, argv, pass, sizeof(pass))) {
        return 2;
    }
    if (!security_can_reveal_private()) {
        shell_print_error("pim: device locked - unlock before syncing secret records");
        memset(pass, 0, sizeof(pass));
        return 1;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("pim: SD card not present - insert and retry");
        memset(pass, 0, sizeof(pass));
        return 1;
    }
    if (shell_text_equals_ignore_case(op, "pull")) {
        rc = pim_net_pull(url, user, pass, dbname);
    } else if (shell_text_equals_ignore_case(op, "push")) {
        rc = pim_net_push(url, user, pass, dbname);
    } else {
        rc = pim_net_pull(url, user, pass, dbname);
        if (rc == 0) {
            rc = pim_net_push(url, user, pass, dbname);
        }
    }
    memset(pass, 0, sizeof(pass));
    shell_sd_end(&session, "pim net");
    return rc;
}

int shell_command_pim(int argc, char **argv)
{
    bool alarms = false;
    const char *dbname = NULL;
    unsigned long size = 0;
    bool verify_crc = false;
    char *end = NULL;

    if (argc < 3) {
        shell_command_pim_usage();
        return 2;
    }
    if (shell_text_equals_ignore_case(argv[1], "net")) {
        return pim_net_command(argc, argv);
    }
    if (shell_text_equals_ignore_case(argv[1], "get")) {
        if (argc == 4 && shell_text_equals_ignore_case(argv[2], "db")) {
            return pim_get_db(argv[3]);
        }
        if (argc == 3 &&
            (shell_text_equals_ignore_case(argv[2], "alarms") ||
             shell_text_equals_ignore_case(argv[2], "alarm") ||
             shell_text_equals_ignore_case(argv[2], "cal"))) {
            return pim_get_alarms();
        }
        shell_command_pim_usage();
        return 2;
    }
    if (shell_text_equals_ignore_case(argv[1], "put")) {
        if (shell_text_equals_ignore_case(argv[2], "db")) {
            /* db form: pim put db <name> <size> [/crc] */
            if (argc != 5 && argc != 6) {
                shell_command_pim_usage();
                return 2;
            }
            dbname = argv[3];
            if (argc == 6) {
                if (shell_text_equals_ignore_case(argv[5], "/crc")) {
                    verify_crc = true;
                } else {
                    shell_command_pim_usage();
                    return 2;
                }
            }
            size = strtoul(argv[4], &end, 10);
        } else if (shell_text_equals_ignore_case(argv[2], "alarms") ||
                   shell_text_equals_ignore_case(argv[2], "alarm") ||
                   shell_text_equals_ignore_case(argv[2], "cal")) {
            /* alarms form: pim put alarms <size> [/crc] */
            if (argc != 4 && argc != 5) {
                shell_command_pim_usage();
                return 2;
            }
            alarms = true;
            if (argc == 5) {
                if (shell_text_equals_ignore_case(argv[4], "/crc")) {
                    verify_crc = true;
                } else {
                    shell_command_pim_usage();
                    return 2;
                }
            }
            size = strtoul(argv[3], &end, 10);
        } else {
            shell_command_pim_usage();
            return 2;
        }
        if (end == NULL || *end != '\0' || size == 0 ||
            size > P4_CONFIG_PIM_RX_MAX_BYTES) {
            shell_print_error("pim: size must be 1..%lu bytes",
                              (unsigned long)P4_CONFIG_PIM_RX_MAX_BYTES);
            return 2;
        }
        return pim_put_stream(size, verify_crc, dbname, alarms);
    }
    shell_command_pim_usage();
    return 2;
}

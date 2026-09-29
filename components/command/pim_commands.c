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

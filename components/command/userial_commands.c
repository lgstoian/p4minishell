/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file userial_commands.c
 * @brief `usb userial ...` verbs over the CDC-ACM byte API (components/usb).
 *
 * The wire side of the palmtop Comm story (`tcpterm` is the Wi-Fi side):
 *
 *   usb userial status
 *   usb userial open <vid:pid> [baud=N] [data=8] [parity=N|E|O] [stop=1|2]
 *   usb userial close
 *   usb userial send <text...>            (or the < file / pipe source)
 *   usb userial recv [/n]                 (drain the RX ring)
 *   usb userial term [/t:secs] [/raw]    (ESC exits; VT100 screen by default)
 *
 * The usb component stays a leaf (driver + ring only); every verb, the
 * key-queue pump for `term`, and the SD input sourcing live here, reached
 * from command.c's `usb` arm. `term` renders remote output as a VT100 screen
 * on the TUI grid (SGR colours, cursor motion, erase, alt-screen) via the
 * shared ansi/tui components; `/raw` keeps the legacy sanitized-transcript
 * passthrough. Replies for the one-shot verbs print sanitized like `tcpterm`
 * (ESC passes so remote SGR colours render). ERRORLEVEL: 0 ok,
 * 1 state/IO failure, 2 usage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "command.h"
#include "usb.h"
#include "shell.h"
#include "storage.h"
#include "ansi.h"
#include "tui.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef P4_CONFIG_USERIAL_RING_BYTES
#define P4_CONFIG_USERIAL_RING_BYTES 4096
#endif

#ifndef P4_CONFIG_USERIAL_CHUNK_BYTES
#define P4_CONFIG_USERIAL_CHUNK_BYTES 1024
#endif

#ifndef P4_CONFIG_USERIAL_TERM_IDLE_MS
#define P4_CONFIG_USERIAL_TERM_IDLE_MS 30000
#endif

#ifndef P4_CONFIG_VT100_ENABLE
#define P4_CONFIG_VT100_ENABLE 1
#endif

#ifndef P4_CONFIG_VT100_PENDING_BYTES
#define P4_CONFIG_VT100_PENDING_BYTES 64
#endif

#ifndef P4_CONFIG_TUI_COLS
#define P4_CONFIG_TUI_COLS 80
#endif

#ifndef P4_CONFIG_TUI_ROWS
#define P4_CONFIG_TUI_ROWS 25
#endif

/** Key-poll slice inside the `term` pump loop. */
#define USERIAL_TERM_POLL_MS 50

static void userial_usage(void)
{
    shell_print_usage("Usage: usb userial <status|open|close|send|recv|term> ...");
    shell_transcript_append_text("  usb userial open <vid:pid> [baud=N] [data=8] [parity=N|E|O] [stop=1|2]\n");
    shell_transcript_append_text("  usb userial send <text...>  (or the < file / pipe source)\n");
    shell_transcript_append_text("  usb userial recv [/n]\n");
    shell_transcript_append_text("  usb userial term [/t:secs] [/raw]  (ESC exits; VT100 screen, /raw = legacy transcript)\n");
}

/** Sanitized transcript print (ESC passes for SGR; other controls -> '.'). */
static void userial_print_sanitized(const uint8_t *data, size_t len)
{
    char chunk[256];
    size_t used = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char ch = data[i];
        char emit;
        if (ch == 0x1B || ch == '\n' || ch == '\r' || ch == '\t' ||
            (ch >= 0x20 && ch < 0x7F)) {
            emit = (char)ch;
        } else {
            emit = '.';
        }
        chunk[used++] = emit;
        if (used >= sizeof(chunk) - 1) {
            chunk[used] = '\0';
            shell_transcript_append_text(chunk);
            used = 0;
        }
    }
    if (used > 0) {
        chunk[used] = '\0';
        shell_transcript_append_text(chunk);
    }
}

/** Report a lost link once and close the driver state. */
static bool userial_fail_if_lost(void)
{
    if (userial_link_lost()) {
        userial_close();
        shell_print_error("userial: device disconnected");
        return true;
    }
    return false;
}

static int userial_cmd_status(void)
{
    userial_status_t status;

    if (!userial_get_status(&status)) {
        shell_print_error("userial: driver not installed");
        return 1;
    }
    if (!status.open) {
        shell_print_muted("userial: no device open (usb userial open <vid:pid>)");
        return 1;
    }
    shell_print_field("userial.device", "%04X:%04X", status.vid, status.pid);
    shell_print_field_num("userial.baud", (long)status.coding.baud);
    shell_print_field("userial.line", "%u%c%u",
                      (unsigned)status.coding.data_bits,
                      status.coding.parity == 1 ? 'O' : (status.coding.parity == 2 ? 'E' : 'N'),
                      (unsigned)status.coding.stop_bits);
    shell_print_field_num("userial.waiting", (long)status.waiting);
    shell_print_field_num("userial.dropped", (long)status.dropped);
    if (status.link_lost) {
        shell_print_warning("userial: link lost");
    }
    return 0;
}

static int userial_cmd_open(int argc, char **argv)
{
    userial_coding_t coding;
    const char *baud_s = NULL;
    const char *data_s = NULL;
    const char *parity_s = NULL;
    const char *stop_s = NULL;
    uint16_t vid = 0;
    uint16_t pid = 0;
    int i;

    if (argc < 4) {
        userial_usage();
        return 2;
    }
    if (!userial_parse_id(argv[3], &vid, &pid)) {
        userial_usage();
        return 2;
    }
    for (i = 4; i < argc; i++) {
        const char *arg = argv[i];
        if (strncasecmp(arg, "baud=", 5) == 0) {
            baud_s = arg + 5;
        } else if (strncasecmp(arg, "data=", 5) == 0) {
            data_s = arg + 5;
        } else if (strncasecmp(arg, "parity=", 7) == 0) {
            parity_s = arg + 7;
        } else if (strncasecmp(arg, "stop=", 5) == 0) {
            stop_s = arg + 5;
        } else {
            userial_usage();
            return 2;
        }
    }
    if (!userial_parse_coding(baud_s, data_s, parity_s, stop_s, &coding)) {
        userial_usage();
        return 2;
    }
    if (userial_open(vid, pid, &coding) != 0) {
        shell_print_error("userial: no matching device (check VID:PID and cabling)");
        return 1;
    }
    shell_print_ok("userial: %04X:%04X open at %lu baud", vid, pid,
                   (unsigned long)coding.baud);
    return 0;
}

static int userial_cmd_close(void)
{
    userial_status_t status;

    if (!userial_get_status(&status) || !status.open) {
        shell_print_muted("userial: no device open");
        return 1;
    }
    userial_close();
    shell_print_ok("userial: device closed");
    return 0;
}

static int userial_cmd_send(int argc, char **argv)
{
    uint8_t *payload;
    size_t payload_cap = (size_t)P4_CONFIG_USERIAL_CHUNK_BYTES * 4;
    size_t payload_len = 0;
    int i;

    if (userial_fail_if_lost()) {
        return 1;
    }
    if (!userial_is_open()) {
        shell_print_error("userial: no device open (usb userial open <vid:pid>)");
        return 1;
    }
    payload = heap_caps_malloc(payload_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        payload = malloc(payload_cap);
    }
    if (payload == NULL) {
        shell_print_error("userial: out of memory");
        return 1;
    }
    /* Inline text wins; otherwise the active < file / pipe source feeds. */
    for (i = 3; i < argc; i++) {
        size_t len = strlen(argv[i]);
        size_t need = payload_len + len + (payload_len > 0 ? 1 : 0);
        if (need >= payload_cap) {
            heap_caps_free(payload);
            shell_print_error("userial: request too large");
            return 1;
        }
        if (payload_len > 0) {
            payload[payload_len++] = ' ';
        }
        memcpy(payload + payload_len, argv[i], len);
        payload_len += len;
    }
    if (payload_len == 0) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        if (storage_resolve_input_source(NULL, resolved, sizeof(resolved)) == ESP_OK) {
            shell_sd_session_t session;
            FILE *file = NULL;
            if (shell_sd_begin(&session) == ESP_OK) {
                file = fopen(resolved, "rb");
            }
            if (file != NULL) {
                payload_len = fread(payload, 1, payload_cap - 1, file);
                fclose(file);
            }
            shell_sd_end(&session, "userial");
        }
    }
    if (payload_len == 0) {
        heap_caps_free(payload);
        shell_print_usage("Usage: usb userial send <text...>  (or the < file / pipe source)");
        return 2;
    }
    if (userial_write(payload, payload_len) != 0) {
        heap_caps_free(payload);
        shell_print_error("userial: send failed");
        return 1;
    }
    shell_print_ok("userial: sent %u byte(s)", (unsigned)payload_len);
    heap_caps_free(payload);
    return 0;
}

static int userial_cmd_recv(int argc, char **argv)
{
    uint8_t *buffer;
    size_t got;
    size_t max = P4_CONFIG_USERIAL_RING_BYTES;
    bool no_newline = false;
    int i;

    for (i = 3; i < argc; i++) {
        char *end = NULL;
        long value;

        if (strcasecmp(argv[i], "/n") == 0) {
            no_newline = true;   /* suppress the trailing newline */
            continue;
        }
        value = strtol(argv[i], &end, 10);
        if (end == argv[i] || *end != '\0' || value < 1) {
            userial_usage();
            return 2;
        }
        if ((size_t)value < max) {
            max = (size_t)value;
        }
    }
    if (userial_fail_if_lost()) {
        return 1;
    }
    if (!userial_is_open()) {
        shell_print_error("userial: no device open (usb userial open <vid:pid>)");
        return 1;
    }
    buffer = heap_caps_malloc(max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = malloc(max);
    }
    if (buffer == NULL) {
        shell_print_error("userial: out of memory");
        return 1;
    }
    got = userial_read(buffer, max);
    if (got == 0) {
        heap_caps_free(buffer);
        shell_print_muted("userial: ring empty");
        return 1;
    }
    userial_print_sanitized(buffer, got);
    if (!no_newline) {
        shell_transcript_append_text("\n");
    }
    heap_caps_free(buffer);
    return 0;
}

/* ========================================================================
 * VT100 SCREEN PUMP (`usb userial term` default mode)
 * ========================================================================
 * Remote bytes feed the shared ANSI parser (ansi_process_text_ex), which
 * applies SGR colours and cursor/screen controls (CUP/CUU/CUD/CUF/CUB,
 * ED/EL, save/restore, DECTCEM, alt-screen) to the TUI grid while windows
 * TUI mode is active. Plain-text segments land on the grid through the
 * scroll-aware vt_putc() below (the stock tui_putc() clamps at the last
 * row; a terminal must scroll). No new components, no transfer protocols:
 * `send`/`receive` remain the file-transfer verbs.
 */

static void vt_advance(void)
{
    int row = 1;
    int col = 1;

    tui_get_cursor(&row, &col);
    if (col >= P4_CONFIG_TUI_COLS) {
        if (row >= P4_CONFIG_TUI_ROWS) {
            tui_scroll_up();
            tui_set_cursor(P4_CONFIG_TUI_ROWS, 1);
        } else {
            tui_set_cursor(row + 1, 1);
        }
    } else {
        tui_set_cursor(row, col + 1);
    }
}

static void vt_newline(void)
{
    int row = 1;
    int col = 1;

    tui_get_cursor(&row, &col);
    if (row >= P4_CONFIG_TUI_ROWS) {
        tui_scroll_up();
        tui_set_cursor(P4_CONFIG_TUI_ROWS, 1);
    } else {
        tui_set_cursor(row + 1, 1);
    }
}

static void vt_putc(char ch, uint8_t fg, uint8_t bg)
{
    int row = 1;
    int col = 1;
    char tmp[2];

    if (ch == '\n') {
        vt_newline();
        return;
    }
    if (ch == '\r') {
        tui_get_cursor(&row, &col);
        tui_set_cursor(row, 1);
        return;
    }
    if (ch == '\b' || ch == 0x7F) {
        tui_get_cursor(&row, &col);
        if (col > 1) {
            tui_set_cursor(row, col - 1);
        }
        return;
    }
    if (ch == '\t') {
        int next;

        tui_get_cursor(&row, &col);
        next = ((col - 1) / 8 + 1) * 8 + 1;
        if (next > P4_CONFIG_TUI_COLS) {
            vt_newline();
        } else {
            tui_set_cursor(row, next);
        }
        return;
    }
    if ((unsigned char)ch < 0x20) {
        return; /* swallow BEL and other stray controls */
    }
    tui_get_cursor(&row, &col);
    tmp[0] = ch;
    tmp[1] = '\0';
    tui_print_at(col, row, tmp, fg, bg);
    vt_advance();
}

static void vt_segment_cb(const char *text, const ansi_state_t *state, void *user_data)
{
    uint8_t fg = 16;
    uint8_t bg = 16;

    (void)user_data;
    if (text == NULL) {
        return;
    }
    if (state != NULL) {
        /* Non-default resolved colours (16/256/truecolor alike) quantize to
         * the DOS grid; untouched state keeps the TUI defaults (16). */
        if (state->fg_color != ansi_get_default_fg()) {
            fg = tui_rgb_to_dos(state->fg_color);
        }
        if (state->bg_color != ansi_get_default_bg()) {
            bg = tui_rgb_to_dos(state->bg_color);
        }
    }
    for (const char *p = text; *p != '\0'; p++) {
        vt_putc(*p, fg, bg);
    }
}

/* Feed one RX chunk to the screen, holding a dangling split CSI for the next
 * read. `pending` holds up to P4_CONFIG_VT100_PENDING_BYTES bytes;
 * `text` scratches chunk + pending + NUL. Over-long dangling runs (a remote
 * that never terminates a sequence) are flushed through as-is so memory
 * stays bounded. */
static void vt_feed(const uint8_t *data, size_t len, uint8_t *pending,
                    size_t *pending_len, char *text)
{
    size_t total;
    size_t hold;
    size_t feed;

    if (data == NULL || pending == NULL || pending_len == NULL || text == NULL) {
        return;
    }
    total = *pending_len + len;
    memcpy(text, pending, *pending_len);
    memcpy(text + *pending_len, data, len);
    hold = ansi_csi_trailing((const uint8_t *)text, total);
    if (hold > (size_t)P4_CONFIG_VT100_PENDING_BYTES) {
        hold = 0; /* hostile/never-ending sequence: flush it through */
    }
    feed = total - hold;
    text[feed] = '\0';
    if (feed > 0) {
        ansi_process_text_ex(text, vt_segment_cb, NULL);
    }
    if (hold > 0) {
        memcpy(pending, text + feed, hold);
    }
    *pending_len = hold;
    tui_flush();
}

static int userial_cmd_term(int argc, char **argv)
{
    int64_t idle_us = (int64_t)P4_CONFIG_USERIAL_TERM_IDLE_MS * 1000;
    int64_t deadline_us;
    uint8_t *rxbuf;
    bool raw = false;
    int i;

    for (i = 3; i < argc; i++) {
        const char *arg = argv[i];
        if ((arg[0] == '/' || arg[0] == '-') && strncasecmp(arg + 1, "t:", 2) == 0) {
            char *end = NULL;
            long secs = strtol(arg + 3, &end, 10);
            if (end == arg + 3 || *end != '\0' || secs < 1 || secs > 600) {
                userial_usage();
                return 2;
            }
            idle_us = (int64_t)secs * 1000000;
        } else if (strcasecmp(arg, "/raw") == 0 || strcasecmp(arg, "-raw") == 0) {
            raw = true;
        } else {
            userial_usage();
            return 2;
        }
    }
    if (userial_fail_if_lost()) {
        return 1;
    }
    if (!userial_is_open()) {
        shell_print_error("userial: no device open (usb userial open <vid:pid>)");
        return 1;
    }
    if (!shell_key_input_available()) {
        shell_print_error("userial: no interactive key source (term needs keys)");
        return 1;
    }
    rxbuf = heap_caps_malloc(P4_CONFIG_USERIAL_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rxbuf == NULL) {
        rxbuf = malloc(P4_CONFIG_USERIAL_CHUNK_BYTES);
    }
    if (rxbuf == NULL) {
        shell_print_error("userial: out of memory");
        return 1;
    }
    bool use_vt = !raw && (P4_CONFIG_VT100_ENABLE != 0);
    bool tui_owned = false;
    uint8_t vt_pending[P4_CONFIG_VT100_PENDING_BYTES];
    size_t vt_pending_len = 0;
    char *vt_text = NULL;
    if (use_vt) {
        bool was_active = tui_is_active();
        vt_text = heap_caps_malloc((size_t)P4_CONFIG_USERIAL_CHUNK_BYTES +
                                   (size_t)P4_CONFIG_VT100_PENDING_BYTES + 1,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (vt_text == NULL) {
            vt_text = malloc((size_t)P4_CONFIG_USERIAL_CHUNK_BYTES +
                             (size_t)P4_CONFIG_VT100_PENDING_BYTES + 1);
        }
        if (vt_text == NULL) {
            shell_print_warning("userial: VT screen unavailable, using /raw passthrough");
            use_vt = false;
        } else if (!was_active && !tui_init()) {
            shell_print_warning("userial: VT screen unavailable, using /raw passthrough");
            heap_caps_free(vt_text);
            vt_text = NULL;
            use_vt = false;
        } else {
            /* When TUI was already active (e.g. after `draw`), borrow the
             * grid but leave teardown to its owner. */
            tui_owned = !was_active;
            tui_clear();
            tui_flush();
        }
    }
    if (use_vt) {
        shell_print_ok("userial: terminal open, VT100 screen (ESC exits)");
    } else {
        shell_print_ok("userial: terminal open (ESC exits)");
    }
    shell_key_wait_begin();
    deadline_us = esp_timer_get_time() + idle_us;
    for (;;) {
        char key = '\0';
        size_t got;

        /* Device -> screen (VT100 grid) or transcript (legacy /raw). */
        got = userial_read(rxbuf, P4_CONFIG_USERIAL_CHUNK_BYTES);
        if (got > 0) {
            if (use_vt) {
                vt_feed(rxbuf, got, vt_pending, &vt_pending_len, vt_text);
            } else {
                userial_print_sanitized(rxbuf, got);
            }
            deadline_us = esp_timer_get_time() + idle_us;
        }
        /* Keys -> device (ESC leaves, Enter goes as CR like DOS). */
        if (shell_wait_for_key(USERIAL_TERM_POLL_MS, &key)) {
            uint8_t out = (key == '\n') ? '\r' : (uint8_t)key;
            if (key == 0x1B) {
                break;
            }
            if (userial_write(&out, 1) != 0) {
                if (use_vt) {
                    if (tui_owned) {
                        tui_deinit();
                    }
                    heap_caps_free(vt_text);
                } else {
                    shell_transcript_append_text("\n");
                }
                shell_print_error("userial: send failed");
                shell_key_wait_end();
                heap_caps_free(rxbuf);
                return 1;
            }
            deadline_us = esp_timer_get_time() + idle_us;
        }
        /* Link loss surfaces promptly rather than at the idle timeout. */
        if (userial_link_lost()) {
            if (use_vt) {
                if (tui_owned) {
                    tui_deinit();
                }
                heap_caps_free(vt_text);
            } else {
                shell_transcript_append_text("\n");
            }
            shell_print_error("userial: device disconnected");
            userial_close();
            shell_key_wait_end();
            heap_caps_free(rxbuf);
            return 1;
        }
        if (esp_timer_get_time() >= deadline_us) {
            if (use_vt) {
                if (tui_owned) {
                    tui_deinit();
                }
                heap_caps_free(vt_text);
            } else {
                shell_transcript_append_text("\n");
            }
            shell_print_warning("userial: idle timeout");
            break;
        }
    }
    shell_key_wait_end();
    if (use_vt) {
        if (tui_owned) {
            tui_deinit();
        }
        heap_caps_free(vt_text);
    }
    heap_caps_free(rxbuf);
    shell_print_ok("userial: terminal closed");
    return 0;
}

int shell_command_userial(int argc, char **argv)
{
    if (argc < 3) {
        userial_usage();
        return 2;
    }
    if (strcasecmp(argv[2], "status") == 0) {
        return userial_cmd_status();
    }
    if (strcasecmp(argv[2], "open") == 0) {
        return userial_cmd_open(argc, argv);
    }
    if (strcasecmp(argv[2], "close") == 0) {
        return userial_cmd_close();
    }
    if (strcasecmp(argv[2], "send") == 0) {
        return userial_cmd_send(argc, argv);
    }
    if (strcasecmp(argv[2], "recv") == 0) {
        return userial_cmd_recv(argc, argv);
    }
    if (strcasecmp(argv[2], "term") == 0) {
        return userial_cmd_term(argc, argv);
    }
    userial_usage();
    return 2;
}

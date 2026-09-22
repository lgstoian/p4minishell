/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file serial_commands.c
 * @brief Screenshot and serial-transfer verbs (screenshot/receive/send).
 *
 * Moved verbatim out of command.c in v0.35.4. Binary host<->device
 * transfer over USB-Serial-JTAG plus LVGL screen capture as BMP. The
 * httpget response-body helper stays in command.c with the inline httpget
 * dispatch arm. The single dispatcher in command.c calls these, it never
 * implements them.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "storage.h"
#include "networking.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "command.h"
#include "imagefmt.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include "p4heap.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ========================================================================
 * SCREENSHOT / SCR / CAPTURE
 * ========================================================================
 * Captures the current LVGL screen as a BMP image and either streams it
 * over the UART/USB-Serial-JTAG console with magic markers, or writes
 * it to an SD card file. Uses the LVGL snapshot API for pixel-perfect
 * capture with the LVGL lock held for thread safety.
 */

/**
 * Bounded wait for the LVGL port lock during a capture. The lock is normally
 * free, but a modal's blinking cursor/redraw can hold it briefly; a bounded
 * wait (rather than 0) keeps the host capture from intermittently failing
 * while a modal is open. This also runs on the console-reader task during a
 * modal, where a momentary busy lock is expected.
 */
#define SCREENSHOT_LVGL_LOCK_MS 1000

/**
 * Write a standard 14-byte BMP file header + 40-byte info header for an
 * RGB888 image into the output buffer, then return the header size (54).
 *
 * BMP format (bottom-up, 24-bit RGB888):
 *   - File header: signature 'BM', file size, reserved, pixel data offset
 *   - Info header: header size (40), width, height, planes (1), bpp (24),
 *     compression (0=BI_RGB), image size, resolution, color count
 */
/** Pure BMP header writer (unit-tested): fills 54 header bytes, no I/O. */
int screenshot_write_bmp_headers(uint8_t *buf, uint32_t width, uint32_t height)
{
    return imagefmt_write_bmp_header(buf, width, height);
}

/**
 * Convert one RGB565 row to 24-bit BGR (BMP bottom-up byte order).
 */
static inline void rgb565_to_bmp_row(uint8_t *dst, const uint16_t *src, uint32_t width)
{
    imagefmt_rgb565_to_bgr24(src, dst, (size_t)width);
}

/**
 * `screenshot` / `scr` / `capture` — capture the LVGL screen as a BMP image.
 *
 * Usage:
 *   screenshot              → stream BMP over UART with magic markers
 *   screenshot file.bmp     → save BMP to SD card (current directory)
 *
 * Captures via lv_snapshot_take_to_draw_buf(), converts RGB565 → RGB888 for the BMP,
 * and outputs either to the serial console (with begin/end markers for
 * host-side extraction) or to an SD card file with free-space precheck.
 */

/* Raw byte write straight to the USB-Serial/JTAG TX ring. Unlike fwrite to
 * stdout, this bypasses the console VFS's CRLF newline translation, so binary
 * payloads (send/screenshot frames, receive ACKs) are never corrupted by an
 * inserted \r before every \n byte. Delegates to the shared UART writer, which
 * chunks for the driver's TX ring (4 KB) and completes partial writes under a
 * bounded total wait, so a big payload cannot be truncated under backpressure
 * and a host that stops reading cannot wedge the command worker. */
static bool serial_write_raw(const void *data, size_t len)
{
    /* One shared writer completes partial writes under a bounded total wait
     * (chunked internally for the TX ring), so a full screenshot/send frame is
     * never truncated by transient TX backpressure. */
    return shell_uart_console_write_bytes(data, len,
                                          P4_CONFIG_UART_WRITE_TOTAL_MS);
}

/* Serial binary-stream framing shared by `screenshot`, `send`, and the
 * `receive` protocol: a 4-byte magic + 4-byte little-endian payload size,
 * then the raw bytes. The magic/size lets a host reader frame exactly one
 * payload off the USB-Serial/JTAG console stream without depending on the
 * surrounding transcript text. */
static void serial_write_frame_header(const char *magic4, uint32_t payload_size)
{
    uint8_t hdr[8];

    memcpy(hdr, magic4, 4);
    hdr[4] = (uint8_t)(payload_size & 0xFFu);
    hdr[5] = (uint8_t)((payload_size >> 8) & 0xFFu);
    hdr[6] = (uint8_t)((payload_size >> 16) & 0xFFu);
    hdr[7] = (uint8_t)((payload_size >> 24) & 0xFFu);
    (void)serial_write_raw(hdr, sizeof(hdr));
}

void shell_command_screenshot(int argc, char **argv)
{
    lv_draw_buf_t *draw_buf = NULL;
    void *pixel_data = NULL;
    lv_obj_t *screen;
    uint32_t width = 0;
    uint32_t height = 0;
    bool to_sd = (argc >= 2);

    if (argc > 2) {
        shell_print_usage("Usage: screenshot [filename.bmp]");
        batch_set_errorlevel(2);
        return;
    }

    shell_print_muted("screenshot: capturing current screen...");

    /* Allocate the lv_draw_buf_t structure via the central allocator */
    draw_buf = (lv_draw_buf_t *)p4heap_alloc_any(sizeof(lv_draw_buf_t));
    if (draw_buf == NULL) {
        shell_print_error("screenshot: out of memory for draw buffer structure");
        batch_set_errorlevel(1);
        return;
    }
    memset(draw_buf, 0, sizeof(lv_draw_buf_t));

    /* Take the LVGL lock and do all LVGL operations inside it. Bounded so a
     * modal's brief redraw cannot make the capture fail outright. */
    if (!lvgl_port_lock(SCREENSHOT_LVGL_LOCK_MS)) {
        shell_print_error("screenshot: could not acquire LVGL lock");
        free(draw_buf);
        batch_set_errorlevel(1);
        return;
    }

    /* Repaint any output deferred by the segment batching so the capture
     * sees the current transcript, not a stale label. */
    shell_transcript_flush_now();

    screen = lv_screen_active();
    if (screen == NULL) {
        lvgl_port_unlock();
        free(draw_buf);
        shell_print_error("screenshot: no active LVGL screen");
        batch_set_errorlevel(1);
        return;
    }

    width = lv_display_get_horizontal_resolution(NULL);
    height = lv_display_get_vertical_resolution(NULL);

    if (width == 0 || height == 0) {
        lvgl_port_unlock();
        free(draw_buf);
        shell_print_error("screenshot: invalid display resolution %lux%lu",
                          (unsigned long)width, (unsigned long)height);
        batch_set_errorlevel(1);
        return;
    }

    /* Calculate required buffer size for RGB565 */
    uint32_t data_size = width * height * 2;  /* RGB565 = 2 bytes per pixel */
    uint32_t stride = width * 2;  /* stride in bytes */

    /* Allocate pixel data from PSRAM */
    pixel_data = p4heap_alloc_psram(data_size);
    if (pixel_data == NULL) {
        lvgl_port_unlock();
        free(draw_buf);
        shell_print_error("screenshot: out of memory for pixel data (%lu bytes)",
                          (unsigned long)data_size);
        batch_set_errorlevel(1);
        return;
    }

    /* Initialize the draw buffer with the pre-allocated PSRAM buffer */
    lv_result_t init_result = lv_draw_buf_init(draw_buf, width, height,
                                               LV_COLOR_FORMAT_RGB565,
                                               stride, pixel_data, data_size);
    if (init_result != LV_RESULT_OK) {
        lvgl_port_unlock();
        free(pixel_data);
        free(draw_buf);
        shell_print_error("screenshot: failed to initialize draw buffer");
        batch_set_errorlevel(1);
        return;
    }

    /* Take the snapshot into the pre-created buffer */
    lv_result_t result = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, draw_buf);
    lvgl_port_unlock();

    if (result != LV_RESULT_OK) {
        shell_print_error("screenshot: snapshot capture failed");
        free(pixel_data);
        free(draw_buf);
        batch_set_errorlevel(1);
        return;
    }

    shell_print_ok("screenshot: captured %lux%lu RGB565 (%lu bytes)",
                   (unsigned long)width, (unsigned long)height,
                   (unsigned long)(draw_buf->data_size));

    if (to_sd) {
        /* Write to SD card file */
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        shell_sd_session_t session;

        if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("screenshot: invalid path %s", argv[1]);
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("screenshot: SD card not present");
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Precheck free space: 54-byte header + width*height*3 pixel data */
        {
            uint64_t needed = 54 + (uint64_t)width * (uint64_t)height * 3;
            uint64_t reclaim = storage_get_file_size(resolved);
            if (!storage_check_free_space(needed, reclaim, "screenshot")) {
                shell_sd_end(&session, "screenshot");
                free(pixel_data);
        free(draw_buf);
                batch_set_errorlevel(1);
                return;
            }
        }

        FILE *f = fopen(resolved, "wb");
        if (f == NULL) {
            shell_print_error("screenshot: cannot create %s", resolved);
            shell_sd_end(&session, "screenshot");
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Write BMP headers */
        uint8_t headers[54];
        int hdr_size = screenshot_write_bmp_headers(headers, width, height);
        size_t written = fwrite(headers, 1, hdr_size, f);
        if (written != (size_t)hdr_size) {
            shell_print_error("screenshot: failed to write BMP header");
            fclose(f);
            remove(resolved);
            shell_sd_end(&session, "screenshot");
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Convert RGB565 to RGB888 row-by-row, bottom-up (BMP convention).
         * Allocate a row buffer on heap to avoid stack pressure. */
        uint32_t row_bytes = width * 3;
        uint8_t *row_buf = p4heap_alloc_psram(row_bytes);
        if (row_buf == NULL) {
            shell_print_error("screenshot: out of memory for row buffer");
            fclose(f);
            remove(resolved);
            shell_sd_end(&session, "screenshot");
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        bool write_ok = true;
        int32_t y;
        for (y = height - 1; y >= 0; y--) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)draw_buf->data +
                                                       (uint32_t)y * draw_buf->header.stride);
            rgb565_to_bmp_row(row_buf, row, width);
            if (fwrite(row_buf, 1, row_bytes, f) != row_bytes) {
                write_ok = false;
                break;
            }
        }

        free(row_buf);
        fclose(f);
        shell_sd_end(&session, "screenshot");

        if (!write_ok) {
            shell_print_error("screenshot: write failed at row, removing partial file");
            remove(resolved);
            free(pixel_data);
        free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        shell_print_ok("screenshot: saved %s (%lu bytes)", resolved,
                       (unsigned long)(54 + (uint64_t)width * height * 3));
        free(pixel_data);
        free(draw_buf);
        batch_set_errorlevel(0);
        return;
    }

    /* Stream raw BMP binary to UART/USB-Serial-JTAG console.
     * Protocol: 4-byte magic "BMPX" + 4-byte little-endian size + raw data.
     * The host reads the magic, then the size, then exactly that many bytes. */
    {
        size_t total_size = 54 + (size_t)width * height * 3;
        shell_print_muted("screenshot: streaming %lu bytes to serial...", (unsigned long)total_size);

        /* Build the full BMP in memory first */
        uint8_t *bmp_data = p4heap_alloc_psram(total_size);
        if (bmp_data == NULL) {
            shell_print_error("screenshot: out of memory for BMP buffer");
            free(pixel_data);
            free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        /* Write BMP header */
        screenshot_write_bmp_headers(bmp_data, width, height);

        /* Convert RGB565 to RGB888 row-by-row, bottom-up (BMP convention) */
        uint32_t row_bytes = width * 3;
        uint8_t *row_buf = p4heap_alloc_psram(row_bytes);
        if (row_buf == NULL) {
            shell_print_error("screenshot: out of memory for row buffer");
            free(bmp_data);
            free(pixel_data);
            free(draw_buf);
            batch_set_errorlevel(1);
            return;
        }

        int32_t y;
        for (y = height - 1; y >= 0; y--) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)draw_buf->data +
                                                       (uint32_t)y * draw_buf->header.stride);
            rgb565_to_bmp_row(row_buf, row, width);
            memcpy(bmp_data + 54 + (height - 1 - y) * row_bytes, row_buf, row_bytes);
        }
        free(row_buf);

        /* The raw stream owns the console byte stream: suspend the console
         * reader (as `send`/`receive` do) so no host input is misread and no
         * console echo interleaves with the BMP payload. */
        shell_uart_console_rx_begin();
        serial_write_frame_header(P4_CONFIG_SCREENSHOT_BMP_MAGIC, (uint32_t)total_size);
        /* Send raw BMP data (raw driver write: no CRLF translation). */
        size_t written = serial_write_raw(bmp_data, total_size) ? total_size : 0;
        shell_uart_console_rx_end();

        free(bmp_data);

        shell_print_ok("screenshot: streamed %lu bytes to serial",
                       (unsigned long)written);
    }

    /* Free the pixel data and draw buffer structure */
    if (pixel_data != NULL) {
        free(pixel_data);
    }
    if (draw_buf != NULL) {
        free(draw_buf);
    }
    batch_set_errorlevel(0);
}

/* ========================================================================
 * SERIAL FILE TRANSFER (receive / send)
 * ========================================================================
 * `receive` pushes a binary from the host into an SD file; `send` streams an
 * SD file (or a compact diagnostic report) back to the host. Both run over the
 * USB-Serial/JTAG console and both suspend the console reader for the
 * duration, so the raw byte stream is never mistaken for command lines and no
 * console echo interleaves with the payload. Both set ERRORLEVEL: 0 success,
 * 1 transfer/IO error (or CRC mismatch), 2 usage.
 *
 * receive protocol (ACK-paced: the device's USB RX ring drops bytes under a
 * burst, so the host only sends what the ACK count confirms was accepted):
 *   host   -> "receive <path> <size> [/crc]\n"
 *   device -> "\n" P4_CONFIG_SERIAL_RX_READY_MARKER "\n"
 *   loop: device reads up to P4_CONFIG_SERIAL_XFER_CHUNK_BYTES, writes SD,
 *         device -> "RX <cumulative>\n", host sends the remaining delta
 *   until cumulative == size
 *   optional (only with /crc): host -> 4-byte little-endian CRC-32 trailer
 *   device -> P4_CONFIG_SERIAL_RX_DONE_MARKER (or an error, then the partial
 *   file is removed)
 *
 * send protocol (framed payload; the size in the header tells the host how
 * many bytes to read):
 *   host   -> "send <path> [offset] [count]\n"   or   "send /diag\n"
 *   device -> "SDFX" + 4-byte little-endian payload size + raw bytes
 *   device -> "\n" P4_CONFIG_SERIAL_TX_DONE_MARKER "\n"
 *
 * `send <path> [offset] [count]` streams a byte range of a file (defaults:
 * whole file), bounded by P4_CONFIG_SERIAL_SEND_MAX_BYTES. `send /diag`
 * streams a bounded text report of version/heap/uptime/tasks/wifi for host
 * side scripting.
 */

/* Incremental CRC-32 (IEEE 802.3, reflected poly 0xEDB88320). Matches the
 * value zlib's crc32() reports for the same bytes: start the accumulator at
 * 0xFFFFFFFF and invert the result when the transfer completes. Shared with
 * the `crc32`/`asset` verbs (asset_commands.c) so the firmware holds exactly
 * one CRC implementation. */
uint32_t shell_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    while (len-- > 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* Write the 8-byte `send` frame header: 4-byte magic + 4-byte little-endian
 * payload size. (The generic serial_write_frame_header() above is shared with
 * `screenshot`.) */
static void serial_send_frame_header(uint32_t payload_size)
{
    serial_write_frame_header(P4_CONFIG_SERIAL_SEND_MAGIC, payload_size);
}

/* Bounded append helper for the `send /diag` report. */
static void serial_diag_line(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    int n;
    va_list args;

    if (buf == NULL || *pos >= cap) {
        return;
    }
    va_start(args, fmt);
    n = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);
    if (n > 0) {
        /* vsnprintf returns the length it WOULD have written; on truncation
         * keep *pos at the last writable byte so callers can never use a
         * length that runs past the buffer. */
        if (*pos + (size_t)n >= cap) {
            *pos = cap - 1;
        } else {
            *pos += (size_t)n;
        }
    }
}

void shell_command_receive(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    char tmp_path[P4_CONFIG_SD_PATH_BYTES + 8];
    shell_sd_session_t session;
    FILE *file = NULL;
    unsigned long size = 0;
    unsigned long cumulative = 0;
    bool verify_crc = false;
    char *end = NULL;
    esp_err_t error;
    int64_t start_us;
    uint32_t crc = 0xFFFFFFFFu;

    if (argc != 3 && argc != 4) {
        shell_print_usage("Usage: receive <path> <size> [/crc]");
        batch_set_errorlevel(2);
        return;
    }
    if (argc == 4) {
        if (shell_text_equals_ignore_case(argv[3], "/crc")) {
            verify_crc = true;
        } else {
            shell_print_usage("Usage: receive <path> <size> [/crc]");
            batch_set_errorlevel(2);
            return;
        }
    }

    size = strtoul(argv[2], &end, 10);
    if (*end != '\0' || size == 0 || size > P4_CONFIG_SERIAL_RX_MAX_BYTES) {
        shell_print_error("receive: size must be 1..%lu bytes",
                          (unsigned long)P4_CONFIG_SERIAL_RX_MAX_BYTES);
        batch_set_errorlevel(2);
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved, sizeof(resolved));
    if (error != ESP_OK) {
        shell_print_error("receive: invalid path %s", argv[1]);
        batch_set_errorlevel(2);
        return;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.rx", resolved);

    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("receive: SD card not present - insert and retry");
        batch_set_errorlevel(1);
        return;
    }

    {
        uint64_t reclaim = storage_get_file_size(resolved);

        if (!storage_check_free_space(size + 64, reclaim, "receive")) {
            shell_sd_end(&session, "receive");
            batch_set_errorlevel(1);
            return;
        }
    }

    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        shell_print_error("receive: cannot create %s", tmp_path);
        shell_sd_end(&session, "receive");
        batch_set_errorlevel(1);
        return;
    }

    /* Suspend the console reader BEFORE signalling READY so that bytes the
     * host sends the instant it sees the marker cannot be consumed as command
     * lines by the (not yet suspended) console task. */
    start_us = esp_timer_get_time();
    shell_uart_console_rx_begin();

    /* Ready marker: the host now streams `size` raw bytes, ACK-paced. */
    shell_uart_console_write_text("\n" P4_CONFIG_SERIAL_RX_READY_MARKER "\n");

    {
        uint8_t *buf = malloc(P4_CONFIG_SERIAL_XFER_CHUNK_BYTES);
        bool ok = (buf != NULL);
        unsigned long idle_ms = 0;

        if (buf == NULL) {
            shell_print_error("receive: out of memory");
        } else {
            while (cumulative < size && ok) {
                size_t want = size - cumulative;
                size_t got;

                if (want > P4_CONFIG_SERIAL_XFER_CHUNK_BYTES) {
                    want = P4_CONFIG_SERIAL_XFER_CHUNK_BYTES;
                }

                /* Read until the chunk is full, or the host goes idle. Reads
                 * go through the USB-Serial/JTAG driver ring (installed with a
                 * large RX buffer in shell_uart_console_start) so bursts do
                 * not overflow; the 100 ms window gives a bounded idle check. */
                got = 0;
                while (got < want && ok) {
                    int r = usb_serial_jtag_read_bytes(buf + got, want - got,
                                                       pdMS_TO_TICKS(100));

                    if (r < 0 || r == 0) {
                        idle_ms += 100;
                        if (idle_ms >= P4_CONFIG_SERIAL_XFER_IDLE_TIMEOUT_MS) {
                            shell_print_error("receive: timed out waiting for data");
                            ok = false;
                            break;
                        }
                        continue;
                    }
                    idle_ms = 0;
                    got += (size_t)r;
                }

                if (ok && got > 0) {
                    if (fwrite(buf, 1, got, file) != got) {
                        shell_print_error("receive: SD write failed");
                        ok = false;
                        break;
                    }
                    crc = shell_crc32_update(crc, buf, got);
                    cumulative += got;

                    /* ACK: report cumulative bytes so the host knows how much
                     * of this chunk was accepted and sends the right delta
                     * next. */
                    {
                        char ack[48];
                        int ack_len = snprintf(ack, sizeof(ack), "RX %lu\n", cumulative);

                        (void)serial_write_raw(ack, (size_t)ack_len);
                    }
                }
            }

            if (ok && verify_crc) {
                /* The host appended a 4-byte little-endian CRC-32 trailer. */
                uint8_t crc_bytes[4];
                size_t got_crc = 0;
                unsigned long crc_idle_ms = 0;

                while (got_crc < sizeof(crc_bytes) && ok) {
                    int r = usb_serial_jtag_read_bytes(crc_bytes + got_crc,
                                                       sizeof(crc_bytes) - got_crc,
                                                       pdMS_TO_TICKS(100));

                    if (r < 0 || r == 0) {
                        crc_idle_ms += 100;
                        if (crc_idle_ms >= P4_CONFIG_SERIAL_XFER_IDLE_TIMEOUT_MS) {
                            shell_print_error("receive: timed out waiting for CRC trailer");
                            ok = false;
                            break;
                        }
                        continue;
                    }
                    crc_idle_ms = 0;
                    got_crc += (size_t)r;
                }
                if (ok) {
                    uint32_t expected = (uint32_t)crc_bytes[0]
                        | ((uint32_t)crc_bytes[1] << 8)
                        | ((uint32_t)crc_bytes[2] << 16)
                        | ((uint32_t)crc_bytes[3] << 24);
                    uint32_t computed = ~crc;

                    if (computed != expected) {
                        shell_print_error("receive: CRC mismatch (expected %08lx, computed %08lx)",
                                          (unsigned long)expected, (unsigned long)computed);
                        ok = false;
                    }
                }
            }
            free(buf);
        }

        /* Drain any bytes still buffered in the USB ring so the resumed
         * console task never sees leftover binary as a command line. */
        {
            uint8_t drain_buf[64];

            while (usb_serial_jtag_read_bytes(drain_buf, sizeof(drain_buf), 0) > 0) {
            }
        }

        shell_uart_console_rx_end();

        fclose(file);
        file = NULL;

        if (!ok || cumulative < size) {
            if (verify_crc && cumulative == size) {
                shell_print_error("receive: transfer failed - CRC mismatch or missing trailer (%lu bytes received)",
                                  size);
            } else {
                shell_print_error("receive: transfer incomplete (%lu/%lu bytes)",
                                  cumulative, size);
            }
            remove(tmp_path);
            shell_sd_end(&session, "receive");
            batch_set_errorlevel(1);
            return;
        }
    }

    /* Atomic replace: FATFS f_rename refuses to overwrite, so remove first. */
    remove(resolved);
    if (rename(tmp_path, resolved) != 0) {
        int err = errno;

        remove(tmp_path);
        shell_print_error("receive: failed to finalize %s (errno %d: %s)",
                          resolved, err, strerror(err));
        shell_sd_end(&session, "receive");
        batch_set_errorlevel(1);
        return;
    }

    shell_sd_end(&session, "receive");
    shell_uart_console_write_text(P4_CONFIG_SERIAL_RX_DONE_MARKER "\n");
    {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        uint32_t rate = ms > 0 ? (uint32_t)((size * 1000u) / ms) : 0u;

        shell_print_ok("receive: wrote %lu bytes to %s in %u ms (%lu KB/s)",
                       size, resolved, ms, (unsigned long)(rate / 1024u));
    }
    batch_set_errorlevel(0);
}

void shell_command_send(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    bool diag = false;
    bool stream_ok = false;
    uint32_t payload_size = 0;
    int64_t start_us;

    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "/diag")) {
        diag = true;
    } else if (argc < 2 || argc > 4) {
        shell_print_usage("Usage: send <path> [offset] [count]  |  send /diag");
        batch_set_errorlevel(2);
        return;
    }

    start_us = esp_timer_get_time();

    /* The transfer owns the raw console byte stream: suspend the console
     * reader so no host input is misread and no console echo interleaves with
     * the framed payload. */
    shell_uart_console_rx_begin();

    if (diag) {
        char *report = malloc(P4_CONFIG_SERIAL_DIAG_BYTES);
        size_t pos = 0;

        if (report == NULL) {
            shell_uart_console_rx_end();
            shell_print_error("send: out of memory for diagnostic report");
            batch_set_errorlevel(1);
            return;
        }

        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "P4MiniShell %s\n", P4_CONFIG_VERSION_STRING);
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "board %s\n", BOARD_CFG_NAME);
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "idf %s\n", esp_get_idf_version());
        {
            esp_chip_info_t chip_info;

            esp_chip_info(&chip_info);
            serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                             "chip %s rev %u, %u cores\n",
                             CONFIG_IDF_TARGET, chip_info.revision,
                             (unsigned int)chip_info.cores);
        }
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "heap_free %lu\n",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "heap_internal %lu\n",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "heap_psram %lu\n",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "uptime_s %llu\n",
                         (unsigned long long)(esp_timer_get_time() / 1000000));
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "tasks %u\n", (unsigned int)uxTaskGetNumberOfTasks());
        {
            char cwd_buf[P4_CONFIG_SD_PATH_BYTES];

            shell_get_cwd_for_prompt(cwd_buf, sizeof(cwd_buf));
            serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                             "cwd %s\n", cwd_buf);
        }
        serial_diag_line(report, P4_CONFIG_SERIAL_DIAG_BYTES, &pos,
                         "wifi %s\n", networking_wifi_state_string());

        payload_size = (uint32_t)pos;
        serial_send_frame_header(payload_size);
        if (payload_size > 0) {
            (void)serial_write_raw(report, payload_size);
        }
        /* CRC-32 trailer over the report (frame protocol parity with send). */
        {
            uint32_t final_crc = ~shell_crc32_update(0xFFFFFFFFu,
                                                      (const uint8_t *)report,
                                                      (size_t)pos);
            uint8_t trailer[4];

            trailer[0] = (uint8_t)(final_crc & 0xFFu);
            trailer[1] = (uint8_t)((final_crc >> 8) & 0xFFu);
            trailer[2] = (uint8_t)((final_crc >> 16) & 0xFFu);
            trailer[3] = (uint8_t)((final_crc >> 24) & 0xFFu);
            (void)serial_write_raw(trailer, sizeof(trailer));
        }
        free(report);
        stream_ok = true;
    } else {
        size_t offset = 0;
        size_t count = P4_CONFIG_SERIAL_SEND_MAX_BYTES;
        long file_size_long = -1;
        char *end = NULL;

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_uart_console_rx_end();
            shell_print_error("send: SD card not present - insert and retry");
            batch_set_errorlevel(1);
            return;
        }

        if (shell_fs_resolve_path(argv[1], resolved, sizeof(resolved)) != ESP_OK) {
            shell_uart_console_rx_end();
            shell_sd_end(&session, "send");
            shell_print_error("send: invalid path %s", argv[1]);
            batch_set_errorlevel(1);
            return;
        }

        if (argc >= 3) {
            offset = strtoul(argv[2], &end, 10);
            if (*end != '\0') {
                shell_uart_console_rx_end();
                shell_sd_end(&session, "send");
                shell_print_error("send: invalid offset %s", argv[2]);
                batch_set_errorlevel(1);
                return;
            }
        }
        if (argc >= 4) {
            count = strtoul(argv[3], &end, 10);
            if (*end != '\0') {
                shell_uart_console_rx_end();
                shell_sd_end(&session, "send");
                shell_print_error("send: invalid count %s", argv[3]);
                batch_set_errorlevel(1);
                return;
            }
        }

        file = fopen(resolved, "rb");
        if (file == NULL) {
            shell_uart_console_rx_end();
            shell_sd_end(&session, "send");
            shell_print_error("send: cannot open %s", resolved);
            batch_set_errorlevel(1);
            return;
        }

        if (fseek(file, 0, SEEK_END) == 0) {
            file_size_long = ftell(file);
        }
        if (fseek(file, 0, SEEK_SET) != 0 || file_size_long < 0) {
            fclose(file);
            shell_uart_console_rx_end();
            shell_sd_end(&session, "send");
            shell_print_error("send: cannot size %s", resolved);
            batch_set_errorlevel(1);
            return;
        }

        /* Clamp the requested byte range to the file and the hard bound. */
        if (offset >= (size_t)file_size_long) {
            count = 0;
        } else {
            size_t available = (size_t)file_size_long - offset;

            if (count > available) {
                count = available;
            }
            if (count > P4_CONFIG_SERIAL_SEND_MAX_BYTES) {
                count = P4_CONFIG_SERIAL_SEND_MAX_BYTES;
            }
        }

        payload_size = (uint32_t)count;
        serial_send_frame_header(payload_size);

        {
            uint32_t crc = 0xFFFFFFFFu;

            if (count > 0) {
                uint8_t *buf = malloc(P4_CONFIG_SERIAL_XFER_CHUNK_BYTES);
                size_t remaining = count;

                if (buf == NULL) {
                    stream_ok = false;
                } else {
                    stream_ok = true;
                    if (fseek(file, (long)offset, SEEK_SET) != 0) {
                        stream_ok = false;
                    }
                    while (stream_ok && remaining > 0) {
                        size_t want = remaining > P4_CONFIG_SERIAL_XFER_CHUNK_BYTES
                                          ? P4_CONFIG_SERIAL_XFER_CHUNK_BYTES
                                          : remaining;
                        size_t got = fread(buf, 1, want, file);

                        if (got == 0) {
                            stream_ok = false;
                            break;
                        }
                        if (!serial_write_raw(buf, got)) {
                            stream_ok = false;
                            break;
                        }
                        crc = shell_crc32_update(crc, buf, got);
                        remaining -= got;
                    }
                    free(buf);
                }
            } else {
                stream_ok = true;
            }

            /* Append the 4-byte little-endian CRC-32 trailer over the payload
             * (empty payload => CRC of nothing = 0x00000000) so the host can
             * verify the frame was not corrupted or interleaved. */
            if (stream_ok) {
                uint32_t final_crc = ~crc;
                uint8_t trailer[4];

                trailer[0] = (uint8_t)(final_crc & 0xFFu);
                trailer[1] = (uint8_t)((final_crc >> 8) & 0xFFu);
                trailer[2] = (uint8_t)((final_crc >> 16) & 0xFFu);
                trailer[3] = (uint8_t)((final_crc >> 24) & 0xFFu);
                if (!serial_write_raw(trailer, sizeof(trailer))) {
                    stream_ok = false;
                }
            }
        }

        fclose(file);
        file = NULL;
        shell_sd_end(&session, "send");
    }

    if (stream_ok) {
        shell_uart_console_write_text("\n" P4_CONFIG_SERIAL_TX_DONE_MARKER "\n");
        shell_uart_console_rx_end();
        {
            uint32_t ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
            uint32_t rate = ms > 0 ? (uint32_t)(((uint32_t)payload_size * 1000u) / ms) : 0u;

            shell_print_ok("send: streamed %lu bytes in %u ms (%lu KB/s)",
                           (unsigned long)payload_size, ms,
                           (unsigned long)(rate / 1024u));
        }
        batch_set_errorlevel(0);
    } else {
        shell_uart_console_rx_end();
        shell_print_error("send: transfer failed");
        batch_set_errorlevel(1);
    }
}

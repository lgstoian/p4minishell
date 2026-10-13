/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file strutil.c
 * @brief Small shared string helpers used by several leaf modules.
 *
 * These were duplicated verbatim in networking/bluetooth/usb/c6ota; this leaf
 * (no IDF component dependencies) lets every one of them share one copy
 * without introducing a layering cycle.
 */

#include "strutil.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

bool strutil_text_equals_ignore_case(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

int strutil_split_args(char *text, char **argv, int max_args)
{
    int argc = 0;
    char *cursor = text;

    while (cursor != NULL && *cursor != '\0' && argc < max_args) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        argv[argc++] = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor = '\0';
            cursor++;
        }
    }

    return argc;
}

int strutil_format_hex_row(char *out, size_t out_size, uint32_t base_addr,
                           const uint8_t *data, size_t len)
{
    /* 8 offset + 2 spaces + 16*3 hex + " |" + 16 ascii + "|\n" + NUL. */
    char scratch[10 + 16 * 3 + 2 + STRUTIL_HEX_ROW_BYTES + 2 + 1];
    size_t pos = 0;
    size_t index;
    int written;

    if (out == NULL || out_size == 0 || data == NULL) {
        return -1;
    }
    if (len > STRUTIL_HEX_ROW_BYTES) {
        len = STRUTIL_HEX_ROW_BYTES;
    }

    written = snprintf(scratch + pos, sizeof(scratch) - pos, "%08X  ",
                       (unsigned int)base_addr);
    if (written < 0) {
        return -1;
    }
    pos += (size_t)written;

    for (index = 0; index < STRUTIL_HEX_ROW_BYTES; index++) {
        if (index < len) {
            written = snprintf(scratch + pos, sizeof(scratch) - pos, "%02X ",
                               (unsigned int)data[index]);
        } else {
            written = snprintf(scratch + pos, sizeof(scratch) - pos, "   ");
        }
        if (written < 0) {
            return -1;
        }
        pos += (size_t)written;
    }

    written = snprintf(scratch + pos, sizeof(scratch) - pos, " |");
    if (written < 0) {
        return -1;
    }
    pos += (size_t)written;

    for (index = 0; index < len; index++) {
        /* data is uint8_t: 0x80-0xFF must stay unsigned. A signed char
         * would make these negative, so the 128..255 test below could never
         * be true and every extended byte would fall through to '.'. */
        unsigned char c = data[index];

        /* CP437 printable range: 0x20-0xFF except 0x7F DEL.
         * Control chars (0x00-0x1F, 0x7F) show as '.'. Written as a lower
         * bound plus a 0x7F exclusion because `c <= 255` on an unsigned char
         * is always true and trips -Wtype-limits. */
        if (c >= 32 && c != 127) {
            scratch[pos++] = (char)c;
        } else {
            scratch[pos++] = '.';
        }
    }
    scratch[pos++] = '|';
    scratch[pos++] = '\n';
    scratch[pos] = '\0';

    if (pos + 1 > out_size) {
        return -1;
    }
    for (index = 0; index <= pos; index++) {
        out[index] = scratch[index];
    }
    return (int)pos;
}

uint32_t strutil_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return crc;
    }
    while (len-- > 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

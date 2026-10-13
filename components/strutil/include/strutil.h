/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file strutil.h
 * @brief Shared string helpers (case-insensitive compare, argument split,
 * hexdump row formatting).
 *
 * Leaf component with no IDF dependencies so any module (including the USB
 * leaf) can share one implementation instead of carrying its own copy.
 */

#ifndef STRUTIL_H
#define STRUTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Case-insensitive equality; false when either pointer is NULL. */
bool strutil_text_equals_ignore_case(const char *left, const char *right);

/** Whitespace-split @p text in place into @p argv (up to @p max_args).
 *  Returns the token count. */
int strutil_split_args(char *text, char **argv, int max_args);

/** Bytes per formatted hexdump row (matches the hexview modal surface). */
#define STRUTIL_HEX_ROW_BYTES 16

/** Format one hexdump row: `%08X  ` + 16 `%02X ` groups (blank-padded when
 *  short) + ` |` + printable ASCII (`.` for the rest) + `|`. @p base_addr is
 *  the address printed for @p data[0]; @p len (0..16) is the valid count.
 *  Returns the bytes written excluding NUL, or -1 when @p out is too small.
 *  Pure, unit-tested; shared by the hexview modal and the `debug` verbs so
 *  both render byte-identical rows. */
int strutil_format_hex_row(char *out, size_t out_size, uint32_t base_addr,
                           const uint8_t *data, size_t len);

/** Incremental CRC-32 (IEEE 802.3, reflected poly 0xEDB88320). Matches the
 *  value zlib's crc32() reports for the same bytes: start the accumulator
 *  at 0xFFFFFFFF and invert the result when done. The single firmware CRC
 *  (serial framing, `crc32`/`asset` verbs, `himem crc`) — callers share
 *  this, never a second copy. NULL data with nonzero len folds nothing. */
uint32_t strutil_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* STRUTIL_H */

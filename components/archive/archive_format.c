/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file archive_format.c
 * @module archive
 * @layer leaf
 * @domain fs
 * @provides pure USTAR fields, memory scan, machine listing and bounds checks
 * @status experimental
 * @test test/main/test_video.c, tools/host_video_gate.py
 * @hard-rule ai-context.md#module-layering-rules
 * @brief The one USTAR header implementation, shared by firmware and host gate.
 *
 * The streaming reader in archive.c and the in-memory scanner used by the host
 * gate both call archive_parse_header() here: there is no second tar parser.
 * CRC-32 delegates to strutil_crc32_update(), the firmware's single CRC.
 */

#include "archive.h"
#include "archive_format_internal.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/** CRC-32 is owned by strutil; this wrapper keeps the archive API stable. */
uint32_t archive_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    return strutil_crc32_update(crc, data, len);
}

void archive_octal(uint64_t value, char *out, size_t out_size)
{
    char tmp[24];
    size_t field;
    size_t len;
    size_t src = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%llo", (unsigned long long)value);
    len = strlen(tmp);
    field = out_size - 1;

    if (len > field) {
        src = len - field;
        len = field;
    } else {
        memset(out, '0', field - len);
    }
    memcpy(out + (field - len), tmp + src, len);
    out[field] = '\0';
}

bool archive_unoctal(const char *in, size_t in_size, uint64_t *out)
{
    uint64_t value = 0;
    size_t i = 0;

    if (in == NULL || out == NULL || in_size == 0) {
        return false;
    }
    while (i < in_size && (in[i] == ' ' || in[i] == '\0')) {
        i++;
    }
    if (i >= in_size) {
        return false;
    }
    for (; i < in_size && in[i] >= '0' && in[i] <= '7'; i++) {
        value = value * 8 + (uint64_t)(in[i] - '0');
    }
    *out = value;
    return true;
}

void archive_copy_path(char *dst, size_t dst_size, const char *src)
{
    size_t n;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/** Checksum over the header with the chksum field counted as spaces. */
static uint64_t archive_header_sum(const uint8_t block[ARCHIVE_BLOCK])
{
    uint64_t sum = 0;

    for (int i = 0; i < ARCHIVE_BLOCK; i++) {
        if (i >= USTAR_CHKSUM && i < USTAR_CHKSUM + USTAR_CHKSUM_LEN) {
            sum += (uint64_t)' ';
        } else {
            sum += block[i];
        }
    }
    return sum;
}

void archive_format_header(const archive_entry_t *e, uint8_t block[ARCHIVE_BLOCK])
{
    char name[USTAR_NAME_LEN + 1];
    char prefix[USTAR_PREFIX_LEN + 1];
    const char *slash;
    size_t pathlen;

    if (e == NULL || block == NULL) {
        return;
    }
    memset(block, 0, ARCHIVE_BLOCK);
    pathlen = strlen(e->path);
    slash = strrchr(e->path, '/');
    if (pathlen <= USTAR_NAME_LEN &&
        (slash == NULL || slash[1] == '\0' || e->is_dir)) {
        memcpy(block + USTAR_NAME, e->path,
               pathlen < USTAR_NAME_LEN ? pathlen : USTAR_NAME_LEN);
    } else if (slash != NULL) {
        size_t prelen = (size_t)(slash - e->path);
        size_t namelen = pathlen - prelen - 1;
        if (prelen > USTAR_PREFIX_LEN) {
            prelen = USTAR_PREFIX_LEN;
        }
        if (namelen > USTAR_NAME_LEN) {
            namelen = USTAR_NAME_LEN;
        }
        memcpy(name, slash + 1, namelen);
        name[namelen] = '\0';
        memcpy(prefix, e->path, prelen);
        prefix[prelen] = '\0';
        memcpy(block + USTAR_NAME, name, namelen);
        memcpy(block + USTAR_PREFIX, prefix, prelen);
    } else {
        return;
    }
    archive_octal(e->is_dir ? 0755 : 0644, (char *)block + USTAR_MODE, 8);
    archive_octal(0, (char *)block + USTAR_UID, 8);
    archive_octal(0, (char *)block + 116, 8);
    archive_octal(e->size, (char *)block + USTAR_SIZE, USTAR_SIZE_LEN);
    archive_octal((uint64_t)(e->mtime >= 0 ? e->mtime : 0),
                  (char *)block + USTAR_MTIME, 12);
    block[USTAR_TYPE] = e->is_dir ? '5' : '0';
    memcpy(block + USTAR_MAGIC, "ustar", 5);
    block[USTAR_MAGIC + 5] = '\0';
    memcpy(block + 263, "00", 2);
    memcpy(block + 265, "root", 4);
    memcpy(block + 297, "root", 4);
    {
        uint64_t sum = archive_header_sum(block);
        archive_octal(sum, (char *)block + USTAR_CHKSUM, 7);
        block[USTAR_CHKSUM + 6] = '\0';
        block[USTAR_CHKSUM + 7] = ' ';
    }
}

int archive_parse_header(const uint8_t block[ARCHIVE_BLOCK], archive_entry_t *out)
{
    static const uint8_t zero[ARCHIVE_BLOCK] = {0};
    uint64_t stored = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;
    char name[USTAR_NAME_LEN + 1];
    char prefix[USTAR_PREFIX_LEN + 1];

    if (block == NULL || out == NULL) {
        return -1;
    }
    if (memcmp(block, zero, ARCHIVE_BLOCK) == 0) {
        return 0;
    }
    if (!archive_unoctal((const char *)block + USTAR_CHKSUM, USTAR_CHKSUM_LEN,
                         &stored) ||
        stored != archive_header_sum(block)) {
        return -1;
    }
    if (!archive_unoctal((const char *)block + USTAR_SIZE, USTAR_SIZE_LEN,
                         &size) ||
        !archive_unoctal((const char *)block + USTAR_MTIME, 12, &mtime)) {
        return -1;
    }
    memcpy(name, block + USTAR_NAME, USTAR_NAME_LEN);
    name[USTAR_NAME_LEN] = '\0';
    memcpy(prefix, block + USTAR_PREFIX, USTAR_PREFIX_LEN);
    prefix[USTAR_PREFIX_LEN] = '\0';
    if (prefix[0] != '\0') {
        char joined[USTAR_PREFIX_LEN + 1 + USTAR_NAME_LEN + 1];
        snprintf(joined, sizeof(joined), "%s/%s", prefix, name);
        archive_copy_path(out->path, sizeof(out->path), joined);
    } else {
        archive_copy_path(out->path, sizeof(out->path), name);
    }
    out->size = size;
    out->mtime = (int64_t)mtime;
    out->is_dir = (block[USTAR_TYPE] == '5');
    out->crc = 0;
    out->crc_known = false;
    out->format = ARCHIVE_FORMAT_USTAR;
    return 1;
}

bool archive_path_safe(const char *path)
{
    const char *p;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (path[0] == '/') {
        return false;
    }
    p = path;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t len = (slash != NULL) ? (size_t)(slash - p) : strlen(p);

        if (len == 2 && p[0] == '.' && p[1] == '.') {
            return false;
        }
        if (slash == NULL) {
            break;
        }
        p = slash + 1;
    }
    return true;
}

bool archive_entry_fits(const char *entry)
{
    size_t len;
    const char *slash;

    if (entry == NULL) {
        return false;
    }
    len = strlen(entry);
    if (len <= USTAR_NAME_LEN) {
        return true;
    }
    slash = strrchr(entry, '/');
    while (slash != NULL) {
        size_t prelen = (size_t)(slash - entry);
        size_t namelen = len - prelen - 1;
        if (prelen <= USTAR_PREFIX_LEN && namelen >= 1 &&
            namelen <= USTAR_NAME_LEN) {
            return true;
        }
        if (prelen == 0) {
            break;
        }
        {
            const char *prev = NULL;
            for (const char *q = entry; q < slash; q++) {
                if (*q == '/') {
                    prev = q;
                }
            }
            slash = prev;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Machine listing, bounds checks and the one in-memory USTAR scan
 * ------------------------------------------------------------------------ */

void archive_machine_sanitize(char *text)
{
    if (text == NULL) {
        return;
    }
    for (; *text != '\0'; text++) {
        if (*text == '|' || *text == '\r' || *text == '\n') {
            *text = '/';
        }
    }
}

int archive_format_machine_line(const archive_entry_t *entry, char *out,
                                size_t out_size)
{
    char path[ARCHIVE_PATH_MAX + 1];
    int written;

    if (entry == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    archive_copy_path(path, sizeof(path), entry->path);
    archive_machine_sanitize(path);
    written = snprintf(out, out_size, "%s|%llu|%lld|%s|%d",
                       entry->is_dir ? "d" : "f",
                       (unsigned long long)entry->size,
                       (long long)entry->mtime,
                       entry->crc_known ? "crc" : "-",
                       entry->crc_known ? (int)entry->crc : 0);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    /* Append the sanitized path after the numeric prefix. */
    {
        size_t used = (size_t)written;
        size_t left = out_size - used;

        written = snprintf(out + used, left, "|%s", path);
        if (written < 0 || (size_t)written >= left) {
            return -1;
        }
        used += (size_t)written;
        return (int)used;
    }
}

bool archive_entry_bounds_ok(const archive_entry_t *entry, uint64_t max_size)
{
    if (entry == NULL || !archive_path_safe(entry->path) ||
        strlen(entry->path) > ARCHIVE_PATH_MAX || entry->size > max_size ||
        entry->mtime > (int64_t)0x3FFFFFFFFFFFFFFFLL ||
        entry->mtime < -(int64_t)0x3FFFFFFFFFFFFFFFLL) {
        return false;
    }
    return true;
}

esp_err_t archive_ustar_scan_mem(const uint8_t *data, size_t len,
                                 archive_list_cb_t cb, void *ctx)
{
    size_t offset = 0;

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    while (offset + ARCHIVE_BLOCK <= len) {
        archive_entry_t entry;
        uint64_t skip;
        int rc = archive_parse_header(data + offset, &entry);

        if (rc < 0) {
            return ESP_FAIL;
        }
        if (rc == 0) {
            return ESP_OK;      /* two zero blocks end a USTAR archive */
        }
        if (cb != NULL && !cb(&entry, ctx)) {
            return ESP_OK;
        }
        skip = ((entry.size + ARCHIVE_BLOCK - 1) / ARCHIVE_BLOCK) *
               ARCHIVE_BLOCK;
        if (skip > len - offset - ARCHIVE_BLOCK) {
            return ESP_FAIL;
        }
        offset += ARCHIVE_BLOCK + (size_t)skip;
    }
    return ESP_FAIL;            /* truncated: no end marker */
}

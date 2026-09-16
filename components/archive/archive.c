/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file archive.c
 * @brief USTAR (.p4a) backup archives: store-only tar + CRC manifest.
 *
 * Create streams sources (files, or directory trees via the heap-scratch
 * walker from the `dir /s` / `xcopy` family) into `<archive>.tmp`, appends
 * the `P4CRC.MANIFEST` trailer, and renames over the target. Extract and
 * verify stream back the same way; every member lands through temp+rename
 * and unsafe paths (`/..` escapes, absolute names) are refused.
 */

#include "archive.h"
#include "storage.h"
#include "shell.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>

#define ARCHIVE_TAG "archive"
#define ARCHIVE_BLOCK 512

#ifndef P4_CONFIG_ARCHIVE_MAX_ENTRIES
#define P4_CONFIG_ARCHIVE_MAX_ENTRIES 512
#endif

#ifndef P4_CONFIG_ARCHIVE_CHUNK_BYTES
#define P4_CONFIG_ARCHIVE_CHUNK_BYTES 4096
#endif

#ifndef P4_CONFIG_SD_PATH_BYTES
#define P4_CONFIG_SD_PATH_BYTES 320
#endif

#ifndef P4_CONFIG_DIR_RECURSE_DEPTH_MAX
#define P4_CONFIG_DIR_RECURSE_DEPTH_MAX 8
#endif

/* Offsets into the 512-byte USTAR header. */
#define USTAR_NAME 0
#define USTAR_NAME_LEN 100
#define USTAR_MODE 100
#define USTAR_UID 108
#define USTAR_SIZE 124
#define USTAR_SIZE_LEN 12
#define USTAR_MTIME 136
#define USTAR_CHKSUM 148
#define USTAR_CHKSUM_LEN 8
#define USTAR_TYPE 156
#define USTAR_MAGIC 257
#define USTAR_PREFIX 345
#define USTAR_PREFIX_LEN 155

/* ========================================================================
 * Pure format core
 * ======================================================================== */

/** Bounded member-path copy (defined with the path helpers below). */
static void archive_copy_path(char *dst, size_t dst_size, const char *src);

uint32_t archive_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
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

void archive_octal(uint64_t value, char *out, size_t out_size)
{
    char tmp[24];   /* 64-bit octal is at most 22 digits + NUL */
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
        /* Value wider than the field: keep the least-significant digits. */
        src = len - field;
        len = field;
    } else {
        /* Left-pad with zeros to fill the fixed-width USTAR field. */
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
    if (pathlen <= USTAR_NAME_LEN && (slash == NULL || slash[1] == '\0' || e->is_dir)) {
        /* Short name (directories keep their trailing slash in name). */
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
        /* Unrepresentable here; the caller skips such entries. */
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
    if (!archive_unoctal((const char *)block + USTAR_CHKSUM, USTAR_CHKSUM_LEN, &stored) ||
        stored != archive_header_sum(block)) {
        return -1;
    }
    if (!archive_unoctal((const char *)block + USTAR_SIZE, USTAR_SIZE_LEN, &size) ||
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
    /* Refuse any `..` component (exact match between slashes). */
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

/* ========================================================================
 * Small helpers (sessions, paths, time)
 * ======================================================================== */

/** Heap helper: PSRAM first, plain malloc fallback. Freed with heap_caps_free. */
static void *archive_alloc(size_t size){
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(size);
    }
    return p;
}

/** Bounded member-path copy (explicit truncation, no -Wformat-truncation). */
static void archive_copy_path(char *dst, size_t dst_size, const char *src)
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

/** FAT date/time pair to unix seconds (device-local, like the alarm store). */
static int64_t archive_fat_to_unix(uint16_t fdate, uint16_t ftime)
{
    struct tm tmv;

    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = (((fdate >> 9) & 127) + 1980) - 1900;
    tmv.tm_mon = (int)(((fdate >> 5) & 15)) - 1;
    tmv.tm_mday = (int)(fdate & 31);
    tmv.tm_hour = (int)((ftime >> 11) & 31);
    tmv.tm_min = (int)((ftime >> 5) & 63);
    tmv.tm_sec = (int)(ftime & 31) * 2;
    tmv.tm_isdst = -1;
    if (tmv.tm_mon < 0 || tmv.tm_mon > 11 || tmv.tm_mday < 1 || tmv.tm_mday > 31) {
        return 0;
    }
    {
        time_t t = mktime(&tmv);
        return (t == (time_t)-1) ? 0 : (int64_t)t;
    }
}

/** mkdir -p over a resolved VFS directory. Missing parents are created. */
static esp_err_t archive_mkdir_p(const char *vfs_dir)
{
    char *tmp = archive_alloc(P4_CONFIG_SD_PATH_BYTES);
    esp_err_t error = ESP_OK;

    if (tmp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(tmp, P4_CONFIG_SD_PATH_BYTES, "%s", vfs_dir);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                error = ESP_FAIL;
                break;
            }
            *p = '/';
        }
    }
    if (error == ESP_OK && mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        error = ESP_FAIL;
    }
    heap_caps_free(tmp);
    return error;
}

/** Parent directory of @p vfs_path (no trailing slash). */
static void archive_parent_dir(const char *vfs_path, char *out, size_t out_size)
{
    char *slash;

    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", vfs_path);
    slash = strrchr(out, '/');
    if (slash != NULL && slash != out) {
        *slash = '\0';
    }
}

/* ========================================================================
 * Create: size pre-walk + streaming writer
 * ======================================================================== */

typedef struct {
    uint64_t bytes;
    int members;
    bool abort;
} archive_size_ctx_t;

typedef struct {
    char vfs[P4_CONFIG_SD_PATH_BYTES];
    char fatfs[P4_CONFIG_SD_PATH_BYTES];
    char child_vfs[P4_CONFIG_SD_PATH_BYTES];
    FF_DIR dir;
    FILINFO info;
} archive_walk_scratch_t;

static void archive_size_walk(const char *vfs, int depth, archive_size_ctx_t *ctx)
{
    archive_walk_scratch_t *s;
    FRESULT result;

    if (depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX || ctx->abort) {
        return;
    }
    s = archive_alloc(sizeof(*s));
    if (s == NULL) {
        ctx->abort = true;
        return;
    }
    if (shell_sd_vfs_to_fatfs_path(vfs, s->fatfs, sizeof(s->fatfs)) != ESP_OK) {
        heap_caps_free(s);
        return;
    }
    if (f_opendir(&s->dir, s->fatfs) != FR_OK) {
        heap_caps_free(s);
        return;
    }
    for (;;) {
        result = f_readdir(&s->dir, &s->info);
        if (result != FR_OK || s->info.fname[0] == '\0') {
            break;
        }
        if (strcmp(s->info.fname, ".") == 0 || strcmp(s->info.fname, "..") == 0) {
            continue;
        }
        if (ctx->members >= P4_CONFIG_ARCHIVE_MAX_ENTRIES) {
            break;
        }
        ctx->members++;
        if ((s->info.fattrib & AM_DIR) != 0) {
            ctx->bytes += ARCHIVE_BLOCK;
            if (snprintf(s->child_vfs, sizeof(s->child_vfs), "%s/%s",
                         vfs, s->info.fname) < (int)sizeof(s->child_vfs)) {
                archive_size_walk(s->child_vfs, depth + 1, ctx);
            }
        } else {
            ctx->bytes += ARCHIVE_BLOCK +
                          ((uint64_t)s->info.fsize + ARCHIVE_BLOCK - 1) /
                          ARCHIVE_BLOCK * ARCHIVE_BLOCK;
        }
    }
    f_closedir(&s->dir);
    heap_caps_free(s);
}

typedef struct {
    FILE *out;
    FILE *manifest;
    archive_stats_t *stats;
    uint8_t *chunk;
    char entry[P4_CONFIG_SD_PATH_BYTES];
    bool failed;
} archive_write_ctx_t;

/** One subdirectory deferred until its parent FF_DIR is closed, so the
 *  recursive create-walker holds only one directory handle per level. */
typedef struct archive_pending_dir {
    struct archive_pending_dir *next;
    char vfs[P4_CONFIG_SD_PATH_BYTES];
    char entry[P4_CONFIG_SD_PATH_BYTES];
} archive_pending_dir_t;

/** Write @p size zero bytes (padding / zero blocks). False on I/O error. */
static bool archive_write_zeros(FILE *out, uint64_t size, uint8_t *chunk, size_t chunk_size)
{
    memset(chunk, 0, chunk_size);
    while (size > 0) {
        size_t n = (size > chunk_size) ? chunk_size : (size_t)size;
        if (fwrite(chunk, 1, n, out) != n) {
            return false;
        }
        size -= n;
    }
    return true;
}

/** Stream one regular file as @p entry (header + data + pad + manifest line). */
static void archive_write_file(archive_write_ctx_t *ctx, const char *vfs_path,
                               const archive_entry_t *e)
{
    FILE *in;
    uint64_t left;
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t header[ARCHIVE_BLOCK];

    in = fopen(vfs_path, "rb");
    if (in == NULL) {
        ctx->stats->skipped++;
        return;
    }
    archive_format_header(e, header);
    if (fwrite(header, 1, ARCHIVE_BLOCK, ctx->out) != ARCHIVE_BLOCK) {
        fclose(in);
        ctx->failed = true;
        return;
    }
    left = e->size;
    while (left > 0 && !ctx->failed) {
        size_t want = (left > P4_CONFIG_ARCHIVE_CHUNK_BYTES)
                          ? P4_CONFIG_ARCHIVE_CHUNK_BYTES : (size_t)left;
        size_t got = fread(ctx->chunk, 1, want, in);
        if (got == 0) {
            ctx->failed = true;
            break;
        }
        crc = archive_crc32_update(crc, ctx->chunk, got);
        if (fwrite(ctx->chunk, 1, got, ctx->out) != got) {
            ctx->failed = true;
            break;
        }
        left -= got;
    }
    fclose(in);
    if (ctx->failed) {
        return;
    }
    {
        uint64_t pad = (ARCHIVE_BLOCK - (e->size % ARCHIVE_BLOCK)) % ARCHIVE_BLOCK;
        if (!archive_write_zeros(ctx->out, pad, ctx->chunk, P4_CONFIG_ARCHIVE_CHUNK_BYTES)) {
            ctx->failed = true;
            return;
        }
    }
    crc ^= 0xFFFFFFFFu;
    fprintf(ctx->manifest, "%08X %llu %s\n",
            (unsigned)crc, (unsigned long long)e->size, e->path);
    ctx->stats->files++;
    ctx->stats->bytes += e->size;
}

/** Split @p entry into USTAR name/prefix form. False when unrepresentable. */
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
        if (prelen <= USTAR_PREFIX_LEN && namelen >= 1 && namelen <= USTAR_NAME_LEN) {
            return true;
        }
        if (prelen == 0) {
            break;
        }
        /* Try an earlier split (walk backwards for the next slash). */
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

static void archive_create_walk(archive_write_ctx_t *ctx, const char *vfs,
                                const char *entry, int depth)
{
    archive_walk_scratch_t *s;
    archive_pending_dir_t *pending = NULL;
    FRESULT result;

    if (depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX || ctx->failed) {
        return;
    }
    if (ctx->stats->files + ctx->stats->dirs + ctx->stats->skipped >=
        P4_CONFIG_ARCHIVE_MAX_ENTRIES) {
        return;
    }
    s = archive_alloc(sizeof(*s));
    if (s == NULL) {
        ctx->failed = true;
        return;
    }
    if (shell_sd_vfs_to_fatfs_path(vfs, s->fatfs, sizeof(s->fatfs)) != ESP_OK) {
        heap_caps_free(s);
        ctx->stats->skipped++;
        return;
    }
    if (f_opendir(&s->dir, s->fatfs) != FR_OK) {
        heap_caps_free(s);
        ctx->stats->skipped++;
        return;
    }
    for (;;) {
        archive_entry_t e;
        char child_entry[P4_CONFIG_SD_PATH_BYTES];

        if (ctx->failed || ctx->stats->files + ctx->stats->dirs + ctx->stats->skipped >=
            P4_CONFIG_ARCHIVE_MAX_ENTRIES) {
            break;
        }
        result = f_readdir(&s->dir, &s->info);
        if (result != FR_OK || s->info.fname[0] == '\0') {
            break;
        }
        if (strcmp(s->info.fname, ".") == 0 || strcmp(s->info.fname, "..") == 0) {
            continue;
        }
        if (snprintf(s->child_vfs, sizeof(s->child_vfs), "%s/%s",
                     vfs, s->info.fname) >= (int)sizeof(s->child_vfs) ||
            snprintf(child_entry, sizeof(child_entry), "%s/%s",
                     entry, s->info.fname) >= (int)sizeof(child_entry)) {
            ctx->stats->skipped++;
            continue;
        }
        if (!archive_entry_fits(child_entry)) {
            ctx->stats->skipped++;
            continue;
        }
        memset(&e, 0, sizeof(e));
        archive_copy_path(e.path, sizeof(e.path), child_entry);
        e.mtime = archive_fat_to_unix(s->info.fdate, s->info.ftime);
        if ((s->info.fattrib & AM_DIR) != 0) {
            size_t elen = strlen(e.path);
            uint8_t header[ARCHIVE_BLOCK];
            archive_pending_dir_t *node;
            if (elen + 1 < sizeof(e.path)) {
                e.path[elen] = '/';
                e.path[elen + 1] = '\0';
            }
            e.is_dir = true;
            archive_format_header(&e, header);
            if (fwrite(header, 1, ARCHIVE_BLOCK, ctx->out) != ARCHIVE_BLOCK) {
                ctx->failed = true;
                break;
            }
            ctx->stats->dirs++;
            /* Defer the descent until this directory is closed. */
            node = archive_alloc(sizeof(*node));
            if (node == NULL) {
                ctx->failed = true;
                break;
            }
            snprintf(node->vfs, sizeof(node->vfs), "%s", s->child_vfs);
            snprintf(node->entry, sizeof(node->entry), "%s", child_entry);
            node->next = pending;
            pending = node;
        } else {
            e.size = s->info.fsize;
            archive_write_file(ctx, s->child_vfs, &e);
        }
    }
    f_closedir(&s->dir);
    heap_caps_free(s);

    /* Descend only after the parent handle is released. */
    while (pending != NULL) {
        archive_pending_dir_t *node = pending;
        pending = node->next;
        if (!ctx->failed) {
            archive_create_walk(ctx, node->vfs, node->entry, depth + 1);
        }
        heap_caps_free(node);
    }
}

esp_err_t archive_create(const char *archive_path, const char *const *sources,
                         int source_count, archive_stats_t *stats)
{
    char tmp[P4_CONFIG_SD_PATH_BYTES + 16];
    char manifest_tmp[P4_CONFIG_SD_PATH_BYTES + 16];
    shell_sd_session_t session;
    FILE *out = NULL;
    FILE *manifest = NULL;
    archive_write_ctx_t ctx;
    archive_size_ctx_t size = {0, 0, false};
    uint8_t *chunk = NULL;
    uint64_t estimate;
    uint64_t reclaim = 0;
    int i;

    if (archive_path == NULL || sources == NULL || source_count <= 0 || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stats, 0, sizeof(*stats));
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", archive_path);
    snprintf(manifest_tmp, sizeof(manifest_tmp), "%s.mfst", archive_path);

    /* Size pre-walk for the free-space guard (headers + padded data). */
    for (i = 0; i < source_count && !size.abort; i++) {
        FILINFO info;
        char fatfs[P4_CONFIG_SD_PATH_BYTES];
        if (shell_sd_vfs_to_fatfs_path(sources[i], fatfs, sizeof(fatfs)) != ESP_OK) {
            stats->skipped++;
            continue;
        }
        if (f_stat(fatfs, &info) != FR_OK) {
            stats->skipped++;
            continue;
        }
        if ((info.fattrib & AM_DIR) != 0) {
            size.members++;
            size.bytes += ARCHIVE_BLOCK;
            archive_size_walk(sources[i], 0, &size);
        } else {
            size.members++;
            size.bytes += ARCHIVE_BLOCK +
                          ((uint64_t)info.fsize + ARCHIVE_BLOCK - 1) /
                          ARCHIVE_BLOCK * ARCHIVE_BLOCK;
        }
        if (size.members >= P4_CONFIG_ARCHIVE_MAX_ENTRIES) {
            break;
        }
    }
    estimate = size.bytes + ARCHIVE_BLOCK * 4; /* manifest + end blocks + slack */
    {
        struct stat st;
        if (stat(archive_path, &st) == 0 && st.st_size > 0) {
            reclaim = (uint64_t)st.st_size;
        }
    }
    if (!storage_check_free_space(estimate, reclaim, "archive")) {
        shell_sd_end(&session, "archive");
        return ESP_ERR_NO_MEM;
    }

    chunk = archive_alloc(P4_CONFIG_ARCHIVE_CHUNK_BYTES);
    if (chunk == NULL) {
        shell_sd_end(&session, "archive");
        return ESP_ERR_NO_MEM;
    }
    out = fopen(tmp, "wb");
    manifest = fopen(manifest_tmp, "w");
    if (out == NULL || manifest == NULL) {
        if (out != NULL) {
            fclose(out);
        }
        if (manifest != NULL) {
            fclose(manifest);
        }
        remove(tmp);
        remove(manifest_tmp);
        heap_caps_free(chunk);
        shell_sd_end(&session, "archive");
        return ESP_FAIL;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.manifest = manifest;
    ctx.stats = stats;
    ctx.chunk = chunk;

    for (i = 0; i < source_count && !ctx.failed; i++) {
        FILINFO info;
        char fatfs[P4_CONFIG_SD_PATH_BYTES];
        const char *base;
        archive_entry_t e;

        if (shell_sd_vfs_to_fatfs_path(sources[i], fatfs, sizeof(fatfs)) != ESP_OK) {
            stats->skipped++;
            continue;
        }
        if (f_stat(fatfs, &info) != FR_OK) {
            stats->skipped++;
            continue;
        }
        base = strrchr(sources[i], '/');
        base = (base != NULL) ? base + 1 : sources[i];
        if (base[0] == '\0' || !archive_entry_fits(base)) {
            stats->skipped++;
            continue;
        }
        if (stats->files + stats->dirs + stats->skipped >= P4_CONFIG_ARCHIVE_MAX_ENTRIES) {
            break;
        }
        memset(&e, 0, sizeof(e));
        archive_copy_path(e.path, sizeof(e.path), base);
        e.mtime = archive_fat_to_unix(info.fdate, info.ftime);
        if ((info.fattrib & AM_DIR) != 0) {
            size_t elen = strlen(e.path);
            uint8_t header[ARCHIVE_BLOCK];
            if (elen + 1 < sizeof(e.path)) {
                e.path[elen] = '/';
                e.path[elen + 1] = '\0';
            }
            e.is_dir = true;
            archive_format_header(&e, header);
            if (fwrite(header, 1, ARCHIVE_BLOCK, out) != ARCHIVE_BLOCK) {
                ctx.failed = true;
                break;
            }
            stats->dirs++;
            archive_create_walk(&ctx, sources[i], base, 0);
        } else {
            e.size = info.fsize;
            archive_write_file(&ctx, sources[i], &e);
        }
    }

    fclose(manifest);
    manifest = NULL;
    if (!ctx.failed) {
        /* Append the manifest as the final member, then close the archive. */
        struct stat mst;
        if (stat(manifest_tmp, &mst) == 0) {
            archive_entry_t me;
            FILE *min = fopen(manifest_tmp, "rb");
            uint8_t header[ARCHIVE_BLOCK];
            uint64_t left;
            memset(&me, 0, sizeof(me));
            snprintf(me.path, sizeof(me.path), "%s", ARCHIVE_MANIFEST_NAME);
            me.size = (uint64_t)mst.st_size;
            me.mtime = (int64_t)time(NULL);
            archive_format_header(&me, header);
            if (min == NULL ||
                fwrite(header, 1, ARCHIVE_BLOCK, out) != ARCHIVE_BLOCK) {
                if (min != NULL) {
                    fclose(min);
                }
                ctx.failed = true;
            } else {
                left = me.size;
                while (left > 0 && !ctx.failed) {
                    size_t want = (left > P4_CONFIG_ARCHIVE_CHUNK_BYTES)
                                      ? P4_CONFIG_ARCHIVE_CHUNK_BYTES : (size_t)left;
                    size_t got = fread(chunk, 1, want, min);
                    if (got == 0) {
                        ctx.failed = true;
                        break;
                    }
                    if (fwrite(chunk, 1, got, out) != got) {
                        ctx.failed = true;
                        break;
                    }
                    left -= got;
                }
                fclose(min);
                if (!ctx.failed) {
                    uint64_t pad = (ARCHIVE_BLOCK - (me.size % ARCHIVE_BLOCK)) % ARCHIVE_BLOCK;
                    if (!archive_write_zeros(out, pad, chunk, P4_CONFIG_ARCHIVE_CHUNK_BYTES)) {
                        ctx.failed = true;
                    }
                }
            }
        } else {
            ctx.failed = true;
        }
    }
    if (!ctx.failed) {
        uint8_t zeros[ARCHIVE_BLOCK];
        memset(zeros, 0, sizeof(zeros));
        if (fwrite(zeros, 1, ARCHIVE_BLOCK, out) != ARCHIVE_BLOCK ||
            fwrite(zeros, 1, ARCHIVE_BLOCK, out) != ARCHIVE_BLOCK ||
            fflush(out) != 0 || fclose(out) != 0) {
            out = NULL;
            ctx.failed = true;
        } else {
            out = NULL;
        }
    }
    if (out != NULL) {
        fclose(out);
    }
    heap_caps_free(chunk);
    remove(manifest_tmp);
    if (ctx.failed || (stats->files == 0 && stats->dirs == 0)) {
        remove(tmp);
        shell_sd_end(&session, "archive");
        return (stats->files == 0 && stats->dirs == 0 && !ctx.failed)
                   ? ESP_ERR_NOT_FOUND
                   : ESP_FAIL;
    }
    /* FATFS rename refuses to overwrite: remove-then-rename like the store. */
    remove(archive_path);
    if (rename(tmp, archive_path) != 0) {
        remove(tmp);
        shell_sd_end(&session, "archive");
        return ESP_FAIL;
    }
    shell_sd_end(&session, "archive");
    return ESP_OK;
}

/* ========================================================================
 * Reader core (list / extract / verify share the stream)
 * ======================================================================== */

typedef struct {
    FILE *f;
    uint8_t block[ARCHIVE_BLOCK];
    char longname[256];
    bool have_longname;
    bool ended;
} archive_reader_t;

/** Skip @p size payload bytes (rounded up to blocks). False on I/O error.
 *  Only correct for member DATA sizes (the rounding consumes the pad). */
static bool archive_reader_skip(archive_reader_t *r, uint64_t size)
{
    uint64_t blocks = (size + ARCHIVE_BLOCK - 1) / ARCHIVE_BLOCK;
    uint64_t skip = blocks * ARCHIVE_BLOCK;
    if (skip == 0) {
        return true;
    }
    if (skip <= (uint64_t)LONG_MAX && fseek(r->f, (long)skip, SEEK_CUR) == 0) {
        return true;
    }
    {
        uint8_t drain[512];
        while (skip > 0) {
            size_t want = (skip > sizeof(drain)) ? sizeof(drain) : (size_t)skip;
            if (fread(drain, 1, want, r->f) != want) {
                return false;
            }
            skip -= want;
        }
    }
    return true;
}

/** Skip exactly @p size bytes (for padding). False on I/O error. */
static bool archive_reader_skip_exact(archive_reader_t *r, uint64_t size)
{
    if (size == 0) {
        return true;
    }
    if (size <= (uint64_t)LONG_MAX && fseek(r->f, (long)size, SEEK_CUR) == 0) {
        return true;
    }
    {
        uint8_t drain[512];
        while (size > 0) {
            size_t want = (size > sizeof(drain)) ? sizeof(drain) : (size_t)size;
            if (fread(drain, 1, want, r->f) != want) {
                return false;
            }
            size -= want;
        }
    }
    return true;
}

/**
 * Next member: 1 with @p e filled, 0 at end-of-archive, -1 on corruption.
 * GNU 'L' longnames override the following header; 'x'/'g' pax headers and
 * unknown typeflags are skipped with their data.
 */
static int archive_reader_next(archive_reader_t *r, archive_entry_t *e)
{
    for (;;) {
        int rc;
        size_t got = fread(r->block, 1, ARCHIVE_BLOCK, r->f);
        if (got == 0) {
            return 0; /* EOF ends the archive too */
        }
        if (got != ARCHIVE_BLOCK) {
            return -1;
        }
        rc = archive_parse_header(r->block, e);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }
        {
            char type = (char)r->block[USTAR_TYPE];
            if (type == 'L' || type == 'K') {
                /* GNU longname: data blocks carry the real name. */
                uint64_t total = ((e->size + ARCHIVE_BLOCK - 1) / ARCHIVE_BLOCK) *
                                 ARCHIVE_BLOCK;
                size_t want = (e->size < sizeof(r->longname) - 1)
                                  ? (size_t)e->size : sizeof(r->longname) - 1;
                if (want > 0 && fread(r->longname, 1, want, r->f) != want) {
                    return -1;
                }
                r->longname[want] = '\0';
                r->have_longname = (want > 0);
                if (!archive_reader_skip_exact(r, total - want)) {
                    return -1;
                }
                continue;
            }
            if (type == 'x' || type == 'g') {
                if (!archive_reader_skip(r, e->size)) {
                    return -1;
                }
                continue;
            }
            if (type != '0' && type != '\0' && type != '5') {
                if (!archive_reader_skip(r, e->size)) {
                    return -1;
                }
                continue;
            }
            if (r->have_longname) {
                snprintf(e->path, sizeof(e->path), "%s", r->longname);
                r->have_longname = false;
            }
            return 1;
        }
    }
}

esp_err_t archive_list(const char *archive_path, archive_list_cb_t cb, void *ctx)
{
    shell_sd_session_t session;
    archive_reader_t r;
    esp_err_t error = ESP_OK;

    if (archive_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&r, 0, sizeof(r));
    r.f = fopen(archive_path, "rb");
    if (r.f == NULL) {
        shell_sd_end(&session, "archive");
        return ESP_ERR_NOT_FOUND;
    }
    for (;;) {
        archive_entry_t e;
        int rc = archive_reader_next(&r, &e);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            error = ESP_FAIL;
            break;
        }
        if (!archive_reader_skip(&r, e.size)) {
            error = ESP_FAIL;
            break;
        }
        if (strcmp(e.path, ARCHIVE_MANIFEST_NAME) == 0) {
            continue;
        }
        if (cb != NULL && !cb(&e, ctx)) {
            break;
        }
    }
    fclose(r.f);
    shell_sd_end(&session, "archive");
    return error;
}

/** Stream one member's data to @p out while running the CRC. False on error. */
static bool archive_stream_to(FILE *in, FILE *out, uint64_t size, uint8_t *chunk,
                              size_t chunk_size, uint32_t *crc_io)
{
    uint64_t left = size;
    uint32_t crc = (crc_io != NULL) ? *crc_io : 0xFFFFFFFFu;

    while (left > 0) {
        size_t want = (left > chunk_size) ? chunk_size : (size_t)left;
        size_t got = fread(chunk, 1, want, in);
        if (got == 0) {
            return false;
        }
        crc = archive_crc32_update(crc, chunk, got);
        if (out != NULL && fwrite(chunk, 1, got, out) != got) {
            return false;
        }
        left -= got;
    }
    if (crc_io != NULL) {
        *crc_io = crc;
    }
    return true;
}

esp_err_t archive_extract(const char *archive_path, const char *dest_dir,
                          archive_stats_t *stats)
{
    shell_sd_session_t session;
    archive_reader_t r;
    uint8_t *chunk = NULL;
    esp_err_t error = ESP_OK;

    if (archive_path == NULL || dest_dir == NULL || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stats, 0, sizeof(*stats));
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (archive_mkdir_p(dest_dir) != ESP_OK) {
        shell_sd_end(&session, "archive");
        return ESP_FAIL;
    }
    chunk = archive_alloc(P4_CONFIG_ARCHIVE_CHUNK_BYTES);
    if (chunk == NULL) {
        shell_sd_end(&session, "archive");
        return ESP_ERR_NO_MEM;
    }
    memset(&r, 0, sizeof(r));
    r.f = fopen(archive_path, "rb");
    if (r.f == NULL) {
        heap_caps_free(chunk);
        shell_sd_end(&session, "archive");
        return ESP_ERR_NOT_FOUND;
    }
    for (;;) {
        archive_entry_t e;
        int rc = archive_reader_next(&r, &e);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            error = ESP_FAIL;
            break;
        }
        if (strcmp(e.path, ARCHIVE_MANIFEST_NAME) == 0) {
            if (!archive_reader_skip(&r, e.size)) {
                error = ESP_FAIL;
                break;
            }
            continue;
        }
        if (!archive_path_safe(e.path) ||
            stats->files + stats->dirs + stats->skipped >= P4_CONFIG_ARCHIVE_MAX_ENTRIES) {
            if (!archive_reader_skip(&r, e.size)) {
                error = ESP_FAIL;
                break;
            }
            stats->skipped++;
            continue;
        }
        {
            char dest[P4_CONFIG_SD_PATH_BYTES];
            if (snprintf(dest, sizeof(dest), "%s/%s", dest_dir, e.path) >=
                (int)sizeof(dest)) {
                if (!archive_reader_skip(&r, e.size)) {
                    error = ESP_FAIL;
                    break;
                }
                stats->skipped++;
                continue;
            }
            if (e.is_dir) {
                if (!archive_reader_skip(&r, e.size)) {
                    error = ESP_FAIL;
                    break;
                }
                if (archive_mkdir_p(dest) != ESP_OK) {
                    stats->skipped++;
                } else {
                    stats->dirs++;
                }
                continue;
            }
            {
                char parent[P4_CONFIG_SD_PATH_BYTES];
                char tmp[P4_CONFIG_SD_PATH_BYTES + 16];
                FILE *out = NULL;
                bool ok = false;

                archive_parent_dir(dest, parent, sizeof(parent));
                if (archive_mkdir_p(parent) != ESP_OK) {
                    if (!archive_reader_skip(&r, e.size)) {
                        error = ESP_FAIL;
                        break;
                    }
                    stats->skipped++;
                    continue;
                }
                /* Free-space guard per member (reclaim an existing file). */
                {
                    struct stat st;
                    uint64_t reclaim = 0;
                    if (stat(dest, &st) == 0 && st.st_size > 0) {
                        reclaim = (uint64_t)st.st_size;
                    }
                    if (!storage_check_free_space(e.size + 512, reclaim, "archive")) {
                        if (!archive_reader_skip(&r, e.size)) {
                            error = ESP_FAIL;
                            break;
                        }
                        stats->skipped++;
                        continue;
                    }
                }
                snprintf(tmp, sizeof(tmp), "%s.rx", dest);
                out = fopen(tmp, "wb");
                if (out == NULL) {
                    if (!archive_reader_skip(&r, e.size)) {
                        error = ESP_FAIL;
                        break;
                    }
                    stats->skipped++;
                    continue;
                }
                ok = archive_stream_to(r.f, out, e.size, chunk,
                                       P4_CONFIG_ARCHIVE_CHUNK_BYTES, NULL);
                if (fflush(out) != 0 || fclose(out) != 0) {
                    ok = false;
                }
                out = NULL;
                {
                    uint64_t pad = (ARCHIVE_BLOCK - (e.size % ARCHIVE_BLOCK)) % ARCHIVE_BLOCK;
                    if (!archive_reader_skip_exact(&r, pad)) {
                        ok = false;
                    }
                }
                if (!ok) {
                    remove(tmp);
                    error = ESP_FAIL;
                    break;
                }
                remove(dest);
                if (rename(tmp, dest) != 0) {
                    remove(tmp);
                    stats->skipped++;
                    continue;
                }
                stats->files++;
                stats->bytes += e.size;
            }
        }
    }
    fclose(r.f);
    heap_caps_free(chunk);
    shell_sd_end(&session, "archive");
    return error;
}

/* ========================================================================
 * Verify (manifest-driven, two tables in one pass)
 * ======================================================================== */

typedef struct {
    char path[ARCHIVE_PATH_MAX + 1];
    uint64_t size;
    uint32_t crc;
} archive_manifest_row_t;

/**
 * Parse one manifest line ("crc32hex size path"; the path may hold spaces).
 * False on malformed input.
 */
static bool archive_manifest_parse(const char *line, uint32_t *crc_out,
                                   uint64_t *size_out, char *path_out, size_t path_size)
{
    const char *p = line;
    uint32_t crc = 0;
    uint64_t size = 0;
    int i;

    if (line == NULL || crc_out == NULL || size_out == NULL ||
        path_out == NULL || path_size == 0) {
        return false;
    }
    for (i = 0; i < 8; i++) {
        char c = *p++;
        crc <<= 4;
        if (c >= '0' && c <= '9') {
            crc |= (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            crc |= (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            crc |= (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    if (*p++ != ' ') {
        return false;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        size = size * 10 + (uint64_t)(*p - '0');
        p++;
    }
    if (*p++ != ' ' || *p == '\0') {
        return false;
    }
    {
        size_t n = strlen(p);
        if (n > path_size - 1) {
            n = path_size - 1;
        }
        memcpy(path_out, p, n);
        path_out[n] = '\0';
    }
    *crc_out = crc;
    *size_out = size;
    return true;
}

esp_err_t archive_verify(const char *archive_path, archive_stats_t *stats)
{
    shell_sd_session_t session;
    archive_reader_t r;
    uint8_t *chunk = NULL;
    archive_manifest_row_t *computed = NULL;
    int computed_count = 0;
    int manifest_count = 0;
    int matched = 0;
    bool saw_manifest = false;
    esp_err_t error = ESP_OK;

    if (archive_path == NULL || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stats, 0, sizeof(*stats));
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    chunk = archive_alloc(P4_CONFIG_ARCHIVE_CHUNK_BYTES);
    computed = archive_alloc(sizeof(*computed) * P4_CONFIG_ARCHIVE_MAX_ENTRIES);
    if (chunk == NULL || computed == NULL) {
        if (chunk != NULL) {
            heap_caps_free(chunk);
        }
        if (computed != NULL) {
            heap_caps_free(computed);
        }
        shell_sd_end(&session, "archive");
        return ESP_ERR_NO_MEM;
    }
    memset(&r, 0, sizeof(r));
    r.f = fopen(archive_path, "rb");
    if (r.f == NULL) {
        heap_caps_free(chunk);
        heap_caps_free(computed);
        shell_sd_end(&session, "archive");
        return ESP_ERR_NOT_FOUND;
    }
    for (;;) {
        archive_entry_t e;
        int rc = archive_reader_next(&r, &e);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            error = ESP_FAIL;
            break;
        }
        if (e.is_dir) {
            if (!archive_reader_skip(&r, e.size)) {
                error = ESP_FAIL;
                break;
            }
            stats->dirs++;
            continue;
        }
        if (strcmp(e.path, ARCHIVE_MANIFEST_NAME) == 0) {
            /* Manifest: compare every line against the computed table. */
            uint64_t left = e.size;
            char *line = archive_alloc(512);
            size_t llen = 0;
            saw_manifest = true;
            if (line == NULL) {
                error = ESP_FAIL;
                break;
            }
            while (left > 0) {
                size_t want = (left > P4_CONFIG_ARCHIVE_CHUNK_BYTES)
                                  ? P4_CONFIG_ARCHIVE_CHUNK_BYTES : (size_t)left;
                size_t got = fread(chunk, 1, want, r.f);
                size_t k;
                if (got == 0) {
                    error = ESP_FAIL;
                    break;
                }
                left -= got;
                for (k = 0; k < got; k++) {
                    if (chunk[k] == '\n') {
                        line[llen] = '\0';
                        manifest_count++;
                        {
                            uint32_t crc = 0;
                            uint64_t size = 0;
                            char mpath[ARCHIVE_PATH_MAX + 1];
                            bool found = false;
                            if (archive_manifest_parse(line, &crc, &size, mpath,
                                                       sizeof(mpath))) {
                                for (int t = 0; t < computed_count; t++) {
                                    if (strcmp(computed[t].path, mpath) == 0 &&
                                        computed[t].size == size) {
                                        found = true;
                                        if (computed[t].crc == crc) {
                                            matched++;
                                        } else {
                                            error = ESP_FAIL;
                                        }
                                        break;
                                    }
                                }
                            }
                            if (!found) {
                                error = ESP_FAIL;
                            }
                        }
                        llen = 0;
                    } else if (llen + 1 < 512) {
                        line[llen++] = (char)chunk[k];
                    }
                }
                if (error != ESP_OK) {
                    break;
                }
            }
            heap_caps_free(line);
            if (error != ESP_OK) {
                break;
            }
            {
                uint64_t pad = (ARCHIVE_BLOCK - (e.size % ARCHIVE_BLOCK)) % ARCHIVE_BLOCK;
                if (!archive_reader_skip_exact(&r, pad)) {
                    error = ESP_FAIL;
                    break;
                }
            }
            continue;
        }
        /* Regular file: hash it, remember for the manifest pass. */
        if (!archive_path_safe(e.path)) {
            if (!archive_reader_skip(&r, e.size)) {
                error = ESP_FAIL;
                break;
            }
            stats->skipped++;
            continue;
        }
        {
            uint32_t crc = 0xFFFFFFFFu;
            if (!archive_stream_to(r.f, NULL, e.size, chunk,
                                   P4_CONFIG_ARCHIVE_CHUNK_BYTES, &crc)) {
                error = ESP_FAIL;
                break;
            }
            crc ^= 0xFFFFFFFFu;
            if (computed_count < P4_CONFIG_ARCHIVE_MAX_ENTRIES) {
                snprintf(computed[computed_count].path,
                         sizeof(computed[computed_count].path), "%s", e.path);
                computed[computed_count].size = e.size;
                computed[computed_count].crc = crc;
                computed_count++;
            } else {
                stats->skipped++;
            }
            {
                uint64_t pad = (ARCHIVE_BLOCK - (e.size % ARCHIVE_BLOCK)) % ARCHIVE_BLOCK;
                if (!archive_reader_skip_exact(&r, pad)) {
                    error = ESP_FAIL;
                    break;
                }
            }
            stats->files++;
            stats->bytes += e.size;
        }
    }
    fclose(r.f);
    heap_caps_free(chunk);
    heap_caps_free(computed);
    shell_sd_end(&session, "archive");
    if (error == ESP_OK && !saw_manifest) {
        error = ESP_FAIL; /* foreign tar: nothing to check against */
    }
    if (error == ESP_OK && matched != manifest_count) {
        error = ESP_FAIL; /* manifest lines without a matching member */
    }
    return error;
}

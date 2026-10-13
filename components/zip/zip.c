/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file zip.c
 * @brief Minimal ZIP reader for EPUB extraction.
 *
 * A bounded, streaming ZIP reader tailored for EPUB extraction:
 *  - Reads central directory from the end of the file
 *  - Supports stored (0) and deflate (8) compression methods
 *  - Extracts individual entries by name or index
 *  - Bounded memory usage (streaming extraction, no full archive in RAM)
 *  - No ZIP64 support (EPUBs are small); no encryption; no spanning
 */

#include "zip.h"
#include "p4minishell_config.h"
#include "strutil.h"
#include "storage.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <zlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

static const char *ZIP_TAG = "zip";

/* ------------------------------------------------------------------------- */
/* Constants & limits                                                        */
/* ------------------------------------------------------------------------- */

#define ZIP_LOCAL_HEADER_SIZE    30
#define ZIP_CENTRAL_DIR_SIZE     46
#define ZIP_EOCD_SIZE            22
#define ZIP_MAX_EOCD_SEARCH      65536  /* Max bytes to scan from end for EOCD */
#define ZIP_CHUNK_SIZE           4096
#define ZIP_MAX_NAME_LEN         256
#define ZIP_MAX_EXTRA_LEN        1024
#define ZIP_MAX_COMMENT_LEN      256

/* ZIP signature constants */
#define ZIP_LOCAL_HEADER_SIG     0x04034b50
#define ZIP_CENTRAL_DIR_SIG      0x02014b50
#define ZIP_EOCD_SIG             0x06054b50

#define ZIP_METHOD_STORED        0
#define ZIP_METHOD_DEFLATE       8

/* ------------------------------------------------------------------------- */
/* Internal structures                                                       */
/* ------------------------------------------------------------------------- */

typedef struct zip_entry_internal {
    char path[ZIP_PATH_MAX + 1];
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t compression_method;
    uint32_t crc32;
    uint32_t local_header_offset;
    bool is_dir;
} zip_entry_internal_t;

struct zip_handle_s {
    FILE *file;
    char archive_path[256];
    zip_entry_internal_t *entries;
    int entry_count;
    int entry_capacity;
};

typedef struct zip_handle_s zip_handle_t;

/* ------------------------------------------------------------------------- */
/* Utility: CRC-32 (IEEE 802.3)                                             */
/* ------------------------------------------------------------------------- */


/* ------------------------------------------------------------------------- */
/* LE integer helpers                                                        */
/* ------------------------------------------------------------------------- */

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ------------------------------------------------------------------------- */
/* ZIP header parsing (pure, no I/O)                                         */
/* ------------------------------------------------------------------------- */

bool zip_parse_local_header(const uint8_t *buf, size_t len,
                            uint16_t *method_out, uint32_t *crc32_out,
                            uint32_t *compressed_size_out, uint32_t *uncompressed_size_out,
                            uint16_t *name_len_out, uint16_t *extra_len_out)
{
    if (len < ZIP_LOCAL_HEADER_SIZE) return false;
    if (le32(buf) != ZIP_LOCAL_HEADER_SIG) return false;

    uint16_t method = le16(buf + 8);
    uint32_t crc32 = le32(buf + 14);
    uint32_t csize = le32(buf + 18);
    uint32_t usize = le32(buf + 22);
    uint16_t nlen = le16(buf + 26);
    uint16_t elen = le16(buf + 28);

    if (method_out) *method_out = method;
    if (crc32_out) *crc32_out = crc32;
    if (compressed_size_out) *compressed_size_out = csize;
    if (uncompressed_size_out) *uncompressed_size_out = usize;
    if (name_len_out) *name_len_out = nlen;
    if (extra_len_out) *extra_len_out = elen;
    return true;
}

bool zip_parse_central_dir(const uint8_t *buf, size_t len,
                           uint16_t *method_out, uint32_t *crc32_out,
                           uint32_t *compressed_size_out, uint32_t *uncompressed_size_out,
                           uint16_t *name_len_out, uint16_t *extra_len_out, uint16_t *comment_len_out,
                           uint32_t *local_offset_out, bool *is_dir_out, char *name_out, size_t name_cap)
{
    if (len < ZIP_CENTRAL_DIR_SIZE) return false;
    if (le32(buf) != ZIP_CENTRAL_DIR_SIG) return false;

    uint16_t method = le16(buf + 10);
    uint32_t crc32 = le32(buf + 16);
    uint32_t csize = le32(buf + 20);
    uint32_t usize = le32(buf + 24);
    uint16_t nlen = le16(buf + 28);
    uint16_t elen = le16(buf + 30);
    uint16_t clen = le16(buf + 32);
    uint32_t loff = le32(buf + 42);
    const char *name = (const char *)(buf + ZIP_CENTRAL_DIR_SIZE);

    if (method_out) *method_out = method;
    if (crc32_out) *crc32_out = crc32;
    if (compressed_size_out) *compressed_size_out = csize;
    if (uncompressed_size_out) *uncompressed_size_out = usize;
    if (name_len_out) *name_len_out = nlen;
    if (extra_len_out) *extra_len_out = elen;
    if (comment_len_out) *comment_len_out = clen;
    if (local_offset_out) *local_offset_out = loff;
    if (is_dir_out) *is_dir_out = (nlen > 0 && name[nlen - 1] == '/');
    if (name_out && name_cap > 0 && nlen > 0) {
        size_t cp = (nlen < name_cap - 1) ? nlen : name_cap - 1;
        memcpy(name_out, name, cp);
        name_out[cp] = '\0';
    }
    return true;
}

bool zip_parse_eocd(const uint8_t *buf, size_t len,
                    uint16_t *disk_num, uint16_t *disk_cd_num,
                    uint16_t *cd_entries_this_disk, uint16_t *cd_entries_total,
                    uint32_t *cd_size, uint32_t *cd_offset, uint16_t *comment_len)
{
    if (len < ZIP_EOCD_SIZE) return false;
    if (le32(buf) != ZIP_EOCD_SIG) return false;

    if (disk_num) *disk_num = le16(buf + 4);
    if (disk_cd_num) *disk_cd_num = le16(buf + 6);
    if (cd_entries_this_disk) *cd_entries_this_disk = le16(buf + 8);
    if (cd_entries_total) *cd_entries_total = le16(buf + 10);
    if (cd_size) *cd_size = le32(buf + 12);
    if (cd_offset) *cd_offset = le32(buf + 16);
    if (comment_len) *comment_len = le16(buf + 20);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Path safety                                                               */
/* ------------------------------------------------------------------------- */

bool zip_path_safe(const char *path)
{
    if (!path || !*path) return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) return false;
        if (p[0] == ':' || p[0] == '*' || p[0] == '?' || p[0] == '"' || p[0] == '<' || p[0] == '>' || p[0] == '|') return false;
        p++;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* ZIP handle management                                                     */
/* ------------------------------------------------------------------------- */

static zip_handle_t *zip_handle_alloc(void)
{
    zip_handle_t *h = calloc(1, sizeof(zip_handle_t));
    if (!h) return NULL;
    h->entry_capacity = 64;
    h->entries = calloc(h->entry_capacity, sizeof(zip_entry_internal_t));
    if (!h->entries) { free(h); return NULL; }
    return h;
}

static void zip_handle_free(zip_handle_t *h)
{
    if (!h) return;
    if (h->file) fclose(h->file);
    free(h->entries);
    free(h);
}

static int zip_ensure_capacity(zip_handle_t *h, int needed)
{
    if (h->entry_count + needed <= h->entry_capacity) return 0;
    int new_cap = h->entry_capacity * 2;
    while (h->entry_count + needed > new_cap) new_cap *= 2;
    zip_entry_internal_t *new_entries = realloc(h->entries, new_cap * sizeof(zip_entry_internal_t));
    if (!new_entries) return -1;
    h->entries = new_entries;
    h->entry_capacity = new_cap;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* EOCD search                                                               */
/* ------------------------------------------------------------------------- */

static bool zip_find_eocd(FILE *f, uint32_t *cd_offset_out, uint16_t *cd_entries_total)
{
    long fsize;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    if (fsize < ZIP_EOCD_SIZE) return false;

    size_t search_len = (fsize > ZIP_MAX_EOCD_SEARCH) ? ZIP_MAX_EOCD_SEARCH : (size_t)fsize;
    uint8_t *buf = malloc(search_len);
    if (!buf) return false;

    fseek(f, fsize - search_len, SEEK_SET);
    size_t read = fread(buf, 1, search_len, f);
    if (read != search_len) { free(buf); return false; }

    /* Scan backwards for EOCD signature */
    for (size_t i = read; i >= ZIP_EOCD_SIZE; i--) {
        if (buf[i - 4] == 0x50 && buf[i - 3] == 0x4b && buf[i - 2] == 0x05 && buf[i - 1] == 0x06) {
            /* Decode straight from the located EOCD record. The pure
             * zip_parse_eocd() helper exists for header parsing, but here the
             * signature is already found and only two fields are wanted. */
            uint16_t cd_entries_total_val = le16(&buf[i - 4 + 10]);
            uint32_t cd_offset_val = le32(&buf[i - 4 + 16]);
            if (cd_offset_out) *cd_offset_out = cd_offset_val;
            if (cd_entries_total) *cd_entries_total = cd_entries_total_val;
            free(buf);
            return true;
        }
    }
    free(buf);
    return false;
}

/* ------------------------------------------------------------------------- */
/* Central directory reading                                                 */
/* ------------------------------------------------------------------------- */

static bool zip_read_central_dir(zip_handle_t *h)
{
    uint32_t cd_offset = 0;
    uint16_t entries_total = 0;

    /* zip_find_eocd() reports the count as uint16_t (it is read straight from
     * the EOCD record), so take it through a matching local rather than
     * handing it &h->entry_count (an int *). */
    if (!zip_find_eocd(h->file, &cd_offset, &entries_total)) {
        ESP_LOGE(ZIP_TAG, "EOCD not found");
        return false;
    }
    h->entry_count = entries_total;

    if (h->entry_count == 0) return true;

    if (zip_ensure_capacity(h, h->entry_count) != 0) return false;

    fseek(h->file, cd_offset, SEEK_SET);
    uint8_t *buf = malloc(ZIP_CENTRAL_DIR_SIZE + ZIP_MAX_NAME_LEN + ZIP_MAX_EXTRA_LEN + ZIP_MAX_COMMENT_LEN);
    if (!buf) return false;

    int read_count = 0;
    for (int i = 0; i < h->entry_count; i++) {
        if (fread(buf, 1, ZIP_CENTRAL_DIR_SIZE, h->file) != ZIP_CENTRAL_DIR_SIZE) break;
        if (le32(buf) != ZIP_CENTRAL_DIR_SIG) break;

        uint16_t name_len = le16(buf + 28);
        uint16_t extra_len = le16(buf + 30);
        uint16_t comment_len = le16(buf + 32);

        if (name_len > ZIP_MAX_NAME_LEN) {
            /* Skip over-long name */
            fseek(h->file, extra_len + comment_len, SEEK_CUR);
            h->entry_count--;
            continue;
        }

        if (fread(buf + ZIP_CENTRAL_DIR_SIZE, 1, name_len + extra_len + comment_len, h->file) != name_len + extra_len + comment_len) break;

        zip_entry_internal_t *e = &h->entries[read_count];
        memset(e, 0, sizeof(*e));

        e->compression_method = le16(buf + 10);
        e->crc32 = le32(buf + 16);
        e->compressed_size = le32(buf + 20);
        e->uncompressed_size = le32(buf + 24);
        /* name_len / extra_len / comment_len were read from this entry above
         * (they drive the fread of the variable-length tail); assigning here
         * avoids shadowing them with a second declaration in the same scope. */
        name_len = le16(buf + 28);
        extra_len = le16(buf + 30);
        comment_len = le16(buf + 32);
        e->local_header_offset = le32(buf + 42);

        const char *name = (const char *)(buf + ZIP_CENTRAL_DIR_SIZE);
        size_t cp = (name_len < ZIP_PATH_MAX) ? name_len : ZIP_PATH_MAX;
        memcpy(e->path, name, cp);
        e->path[cp] = '\0';
        e->is_dir = (cp > 0 && e->path[cp - 1] == '/');

        read_count++;
    }

    h->entry_count = read_count;
    free(buf);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t zip_open(const char *archive_path, void **out_handle)
{
    if (!archive_path || !out_handle) return ESP_ERR_INVALID_ARG;

    /* Resolve VFS path via shell_sd_resolve_path if needed */
    char resolved[256];
    if (!shell_sd_resolve_path(archive_path, resolved, sizeof(resolved))) {
        strlcpy(resolved, archive_path, sizeof(resolved));
    }

    zip_handle_t *h = zip_handle_alloc();
    if (!h) return ESP_ERR_NO_MEM;

    h->file = fopen(resolved, "rb");
    if (!h->file) {
        zip_handle_free(h);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(h->archive_path, resolved, sizeof(h->archive_path));

    if (!zip_read_central_dir(h)) {
        zip_handle_free(h);
        return ESP_FAIL;
    }

    *out_handle = h;
    return ESP_OK;
}

void zip_close(void *handle)
{
    zip_handle_free((zip_handle_t *)handle);
}

int zip_get_count(void *handle)
{
    zip_handle_t *h = (zip_handle_t *)handle;
    return h ? h->entry_count : -1;
}

esp_err_t zip_get_entry(void *handle, int index, zip_entry_t *out_entry)
{
    zip_handle_t *h = (zip_handle_t *)handle;
    if (!h || index < 0 || index >= h->entry_count || !out_entry) return ESP_ERR_INVALID_ARG;

    zip_entry_internal_t *e = &h->entries[index];
    strlcpy(out_entry->path, e->path, sizeof(out_entry->path));
    out_entry->compressed_size = e->compressed_size;
    out_entry->uncompressed_size = e->uncompressed_size;
    out_entry->compression_method = e->compression_method;
    out_entry->crc32 = e->crc32;
    out_entry->local_header_offset = e->local_header_offset;
    out_entry->is_dir = e->is_dir;
    return ESP_OK;
}

esp_err_t zip_find_entry(void *handle, const char *path, zip_entry_t *out_entry)
{
    zip_handle_t *h = (zip_handle_t *)handle;
    if (!h || !path || !out_entry) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < h->entry_count; i++) {
        if (strcmp(h->entries[i].path, path) == 0) {
            zip_entry_internal_t *e = &h->entries[i];
            strlcpy(out_entry->path, e->path, sizeof(out_entry->path));
            out_entry->compressed_size = e->compressed_size;
            out_entry->uncompressed_size = e->uncompressed_size;
            out_entry->compression_method = e->compression_method;
            out_entry->crc32 = e->crc32;
            out_entry->local_header_offset = e->local_header_offset;
            out_entry->is_dir = e->is_dir;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------------- */
/* Extraction                                                                */
/* ------------------------------------------------------------------------- */

static esp_err_t zip_extract_stored(FILE *f, const zip_entry_internal_t *e, const char *dest_path)
{
    FILE *out = fopen(dest_path, "wb");
    if (!out) return ESP_FAIL;

    uint8_t *buf = malloc(ZIP_CHUNK_SIZE);
    if (!buf) { fclose(out); return ESP_ERR_NO_MEM; }

    uint32_t remaining = e->compressed_size;
    uint32_t crc = 0;
    while (remaining > 0) {
        size_t to_read = (remaining > ZIP_CHUNK_SIZE) ? ZIP_CHUNK_SIZE : remaining;
        size_t read = fread(buf, 1, to_read, f);
        if (read != to_read) { free(buf); fclose(out); return ESP_FAIL; }
        crc = strutil_crc32_update(crc, buf, read);
        fwrite(buf, 1, read, out);
        remaining -= read;
    }

    free(buf);
    fclose(out);

    if (crc != e->crc32) {
        ESP_LOGW(ZIP_TAG, "CRC mismatch for %s: expected 0x%08x, got 0x%08x", e->path, e->crc32, crc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t zip_extract_deflate(FILE *f, const zip_entry_internal_t *e, const char *dest_path)
{
    FILE *out = fopen(dest_path, "wb");
    if (!out) return ESP_FAIL;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;

    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) { /* -MAX_WBITS for raw deflate */
        fclose(out);
        return ESP_FAIL;
    }

    uint8_t *in_buf = malloc(ZIP_CHUNK_SIZE);
    uint8_t *out_buf = malloc(ZIP_CHUNK_SIZE);
    if (!in_buf || !out_buf) { free(in_buf); free(out_buf); inflateEnd(&strm); fclose(out); return ESP_ERR_NO_MEM; }

    uint32_t remaining = e->compressed_size;
    uint32_t crc = 0;
    int ret = Z_OK;

    while (remaining > 0 || strm.avail_in > 0) {
        if (strm.avail_in == 0 && remaining > 0) {
            size_t to_read = (remaining > ZIP_CHUNK_SIZE) ? ZIP_CHUNK_SIZE : remaining;
            size_t read = fread(in_buf, 1, to_read, f);
            if (read == 0 && remaining > 0) { ret = Z_DATA_ERROR; break; }
            strm.avail_in = read;
            strm.next_in = in_buf;
            remaining -= read;
        }

        strm.avail_out = ZIP_CHUNK_SIZE;
        strm.next_out = out_buf;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR) break;

        size_t have = ZIP_CHUNK_SIZE - strm.avail_out;
        if (have > 0) {
            crc = strutil_crc32_update(crc, out_buf, have);
            fwrite(out_buf, 1, have, out);
        }

        if (ret == Z_STREAM_END) break;
    }

    free(in_buf);
    free(out_buf);
    inflateEnd(&strm);
    fclose(out);

    if (ret != Z_STREAM_END) return ESP_FAIL;
    if (crc != e->crc32) {
        ESP_LOGW(ZIP_TAG, "CRC mismatch for %s: expected 0x%08x, got 0x%08x", e->path, e->crc32, crc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t zip_extract_entry_internal(zip_handle_t *h, const zip_entry_internal_t *e, const char *dest_path)
{
    if (e->is_dir) return ESP_OK; /* Directories created by extract_all */

    /* Seek to local file header */
    fseek(h->file, e->local_header_offset, SEEK_SET);
    uint8_t header[ZIP_LOCAL_HEADER_SIZE + ZIP_MAX_NAME_LEN + ZIP_MAX_EXTRA_LEN];
    if (fread(header, 1, ZIP_LOCAL_HEADER_SIZE, h->file) != ZIP_LOCAL_HEADER_SIZE) return ESP_FAIL;
    if (le32(header) != ZIP_LOCAL_HEADER_SIG) return ESP_FAIL;

    uint16_t name_len = le16(header + 26);
    uint16_t extra_len = le16(header + 28);

    /* Skip name + extra */
    fseek(h->file, name_len + extra_len, SEEK_CUR);

    /* Dispatch by compression method */
    if (e->compression_method == ZIP_METHOD_STORED) {
        return zip_extract_stored(h->file, e, dest_path);
    } else if (e->compression_method == ZIP_METHOD_DEFLATE) {
        return zip_extract_deflate(h->file, e, dest_path);
    } else {
        ESP_LOGE(ZIP_TAG, "Unsupported compression method %d for %s", e->compression_method, e->path);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t zip_extract_entry(void *handle, const zip_entry_t *entry, const char *dest_path)
{
    zip_handle_t *h = (zip_handle_t *)handle;
    if (!h || !entry || !dest_path) return ESP_ERR_INVALID_ARG;

    /* Find matching internal entry */
    zip_entry_internal_t *e = NULL;
    for (int i = 0; i < h->entry_count; i++) {
        if (strcmp(h->entries[i].path, entry->path) == 0) {
            e = &h->entries[i];
            break;
        }
    }
    if (!e) return ESP_ERR_NOT_FOUND;

    /* Ensure parent directories exist */
    char dir[256];
    const char *last_slash = strrchr(dest_path, '/');
    if (last_slash) {
        size_t len = last_slash - dest_path;
        strlcpy(dir, dest_path, len + 1);
        storage_mkdir_p(dir);
    }

    return zip_extract_entry_internal(h, e, dest_path);
}

esp_err_t zip_extract_all(void *handle, const char *dest_dir, zip_stats_t *stats)
{
    zip_handle_t *h = (zip_handle_t *)handle;
    if (!h || !dest_dir) return ESP_ERR_INVALID_ARG;

    zip_stats_t local_stats = {0};
    if (!stats) stats = &local_stats;

    storage_mkdir_p(dest_dir);

    for (int i = 0; i < h->entry_count; i++) {
        zip_entry_internal_t *e = &h->entries[i];
        if (!zip_path_safe(e->path)) { stats->skipped++; continue; }

        char dest_path[512];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, e->path);

        if (e->is_dir) {
            storage_mkdir_p(dest_path);
            stats->entries++;
            continue;
        }

        /* Ensure parent dir */
        char *last_slash = strrchr(dest_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            storage_mkdir_p(dest_path);
            *last_slash = '/';
        }

        esp_err_t err = zip_extract_entry_internal(h, &h->entries[i], dest_path);
        if (err != ESP_OK) {
            stats->skipped++;
            continue;
        }
        stats->entries++;
        stats->bytes += e->uncompressed_size;
    }
    return ESP_OK;
}

esp_err_t zip_list(void *handle, zip_list_cb_t cb, void *ctx)
{
    zip_handle_t *h = (zip_handle_t *)handle;
    if (!h || !cb) return ESP_ERR_INVALID_ARG;

    zip_entry_t entry;
    for (int i = 0; i < h->entry_count; i++) {
        zip_entry_internal_t *e = &h->entries[i];
        strlcpy(entry.path, e->path, sizeof(entry.path));
        entry.compressed_size = e->compressed_size;
        entry.uncompressed_size = e->uncompressed_size;
        entry.compression_method = e->compression_method;
        entry.crc32 = e->crc32;
        entry.local_header_offset = e->local_header_offset;
        entry.is_dir = e->is_dir;
        if (!cb(&entry, ctx)) break;
    }
    return ESP_OK;
}
/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file zip_writer.c
 * @brief Stored-entry ZIP writing (see zip_writer.h).
 *
 * Only what an EPUB needs: local headers, a central directory and an end
 * record. Every entry is STORED (method 0), sizes are 32-bit (documents are
 * far below 4 GB) and CRC-32 comes from `strutil_crc32_update` - the single
 * firmware CRC - so there is no second CRC table.
 */

#include "zip_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strutil.h"

#define ZIP_LOCAL_SIG    0x04034b50u
#define ZIP_CENTRAL_SIG  0x02014b50u
#define ZIP_EOCD_SIG     0x06054b50u
#define ZIP_NAME_MAX     96

typedef struct {
    char name[ZIP_NAME_MAX];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
} zip_writer_entry_t;

struct zip_writer {
    FILE *file;
    char path[192];
    zip_writer_entry_t entries[ZIP_WRITER_MAX_ENTRIES];
    int count;
    uint32_t offset;
    bool failed;
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t zip_crc(const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    return ~strutil_crc32_update(0xFFFFFFFFu, (const uint8_t *)data, len);
}

esp_err_t zip_writer_create(const char *path, zip_writer_t **out_writer)
{
    zip_writer_t *writer;

    if (path == NULL || path[0] == '\0' || out_writer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_writer = NULL;
    if (strlen(path) >= sizeof(writer->path)) {
        return ESP_ERR_INVALID_ARG;
    }
    writer = calloc(1, sizeof(*writer));
    if (writer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(writer->path, sizeof(writer->path), "%s", path);
    writer->file = fopen(path, "wb");
    if (writer->file == NULL) {
        free(writer);
        return ESP_FAIL;
    }
    *out_writer = writer;
    return ESP_OK;
}

esp_err_t zip_writer_add(zip_writer_t *writer, const char *name,
                         const void *data, size_t len)
{
    uint8_t header[30];
    zip_writer_entry_t *entry;
    size_t name_len;

    if (writer == NULL || writer->file == NULL || name == NULL ||
        writer->failed) {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->count >= ZIP_WRITER_MAX_ENTRIES || len > 0xFFFFFFFFu) {
        return ESP_ERR_NO_MEM;
    }
    name_len = strlen(name);
    if (name_len == 0 || name_len >= ZIP_NAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    entry = &writer->entries[writer->count];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, name, name_len);
    entry->crc = zip_crc(data, len);
    entry->size = (uint32_t)len;
    entry->offset = writer->offset;

    memset(header, 0, sizeof(header));
    put32(header + 0, ZIP_LOCAL_SIG);
    put16(header + 4, 20);            /* version needed */
    put16(header + 6, 0);             /* flags: no data descriptor */
    put16(header + 8, 0);             /* method: stored */
    put16(header + 10, 0);            /* mod time */
    put16(header + 12, 0x21);         /* mod date (1980-01-01) */
    put32(header + 14, entry->crc);
    put32(header + 18, entry->size);
    put32(header + 22, entry->size);
    put16(header + 26, (uint16_t)name_len);
    put16(header + 28, 0);            /* extra length: EPUB needs none */

    if (fwrite(header, 1, sizeof(header), writer->file) != sizeof(header) ||
        fwrite(name, 1, name_len, writer->file) != name_len ||
        (len > 0 && fwrite(data, 1, len, writer->file) != len)) {
        writer->failed = true;
        return ESP_FAIL;
    }
    writer->offset += (uint32_t)(sizeof(header) + name_len + len);
    writer->count++;
    return ESP_OK;
}

esp_err_t zip_writer_close(zip_writer_t *writer)
{
    uint8_t central[46];
    uint32_t central_size = 0;
    uint32_t central_offset;
    uint8_t eocd[22];
    esp_err_t result = ESP_OK;

    if (writer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->file == NULL || writer->failed) {
        zip_writer_abort(writer);
        return ESP_FAIL;
    }
    central_offset = writer->offset;
    for (int i = 0; i < writer->count; i++) {
        const zip_writer_entry_t *entry = &writer->entries[i];
        size_t name_len = strlen(entry->name);

        memset(central, 0, sizeof(central));
        put32(central + 0, ZIP_CENTRAL_SIG);
        put16(central + 4, 20);       /* version made by */
        put16(central + 6, 20);       /* version needed */
        put16(central + 8, 0);        /* flags */
        put16(central + 10, 0);       /* method: stored */
        put16(central + 12, 0);       /* mod time */
        put16(central + 14, 0x21);    /* mod date */
        put32(central + 16, entry->crc);
        put32(central + 20, entry->size);
        put32(central + 24, entry->size);
        put16(central + 28, (uint16_t)name_len);
        put16(central + 30, 0);       /* extra */
        put16(central + 32, 0);       /* comment */
        put16(central + 34, 0);       /* disk */
        put16(central + 36, 0);       /* internal attrs */
        put32(central + 38, 0);       /* external attrs */
        put32(central + 42, entry->offset);

        if (fwrite(central, 1, sizeof(central), writer->file) != sizeof(central) ||
            fwrite(entry->name, 1, name_len, writer->file) != name_len) {
            result = ESP_FAIL;
            break;
        }
        central_size += (uint32_t)(sizeof(central) + name_len);
    }
    if (result == ESP_OK) {
        memset(eocd, 0, sizeof(eocd));
        put32(eocd + 0, ZIP_EOCD_SIG);
        put16(eocd + 4, 0);           /* this disk */
        put16(eocd + 6, 0);           /* central dir disk */
        put16(eocd + 8, (uint16_t)writer->count);
        put16(eocd + 10, (uint16_t)writer->count);
        put32(eocd + 12, central_size);
        put32(eocd + 16, central_offset);
        put16(eocd + 20, 0);          /* comment length */
        if (fwrite(eocd, 1, sizeof(eocd), writer->file) != sizeof(eocd)) {
            result = ESP_FAIL;
        }
    }
    if (fclose(writer->file) != 0) {
        result = ESP_FAIL;
    }
    writer->file = NULL;
    if (result != ESP_OK) {
        remove(writer->path);
    }
    free(writer);
    return result;
}

void zip_writer_abort(zip_writer_t *writer)
{
    if (writer == NULL) {
        return;
    }
    if (writer->file != NULL) {
        fclose(writer->file);
        writer->file = NULL;
    }
    remove(writer->path);
    free(writer);
}

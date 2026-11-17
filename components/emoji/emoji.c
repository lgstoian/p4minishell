/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file emoji.c
 * @brief Emoji pack loader (see emoji.h).
 */

#include "emoji.h"

#include "emoji_core.h"
#include "imgdec.h"
#include "p4heap.h"
#include "storage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t cp;
    uint8_t cells;
    char file[64];
} emoji_entry_t;

typedef struct {
    uint32_t cp;
    int cells;
    gfx_surface_t surface;
    bool used;
} emoji_cache_slot_t;

static emoji_entry_t *s_entries = NULL;   /* PSRAM, EMOJI_PACK_MAX_ENTRIES */
static int s_entry_count = 0;
static char s_source[8] = "";
static bool s_loaded = false;
static emoji_cache_slot_t s_cache[EMOJI_PACK_CACHE_SLOTS];
static int s_cache_next = 0;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** Parse an INI key: `HEX` or `HEX.CELLS`. */
static bool parse_key(const char *key, uint32_t *cp, bool *is_cells)
{
    uint32_t value = 0;
    int digits = 0;

    if (key == NULL || *key == '\0') {
        return false;
    }
    while (*key != '\0' && *key != '.') {
        int v = hex_value(*key);

        if (v < 0 || digits >= 6) {
            return false;
        }
        value = (value << 4) | (uint32_t)v;
        digits++;
        key++;
    }
    if (digits == 0) {
        return false;
    }
    *cp = value;
    *is_cells = (*key == '.');
    return true;
}

static emoji_entry_t *find_entry(uint32_t cp)
{
    for (int i = 0; i < s_entry_count; i++) {
        if (s_entries[i].cp == cp) {
            return &s_entries[i];
        }
    }
    return NULL;
}

typedef struct {
    const char *dir;
    int added;
} load_ctx_t;

static bool load_line(const char *key, const char *value, void *ctx)
{
    load_ctx_t *load = (load_ctx_t *)ctx;
    uint32_t cp = 0;
    bool is_cells = false;
    emoji_entry_t *entry;

    (void)load->dir;
    if (!parse_key(key, &cp, &is_cells)) {
        return true;   /* ignore unrelated keys, keep scanning */
    }
    entry = find_entry(cp);
    if (is_cells) {
        if (entry != NULL) {
            long cells = strtol(value, NULL, 10);

            if (cells < 1) cells = 1;
            if (cells > EMOJI_PACK_MAX_CELLS) cells = EMOJI_PACK_MAX_CELLS;
            entry->cells = (uint8_t)cells;
        }
        return true;
    }
    if (value[0] == '\0' || strchr(value, '/') != NULL ||
        strchr(value, '\\') != NULL) {
        return true;   /* pack files stay inside EMOJI/ */
    }
    if (entry == NULL) {
        if (s_entry_count >= EMOJI_PACK_MAX_ENTRIES) {
            return false;   /* documented cap */
        }
        entry = &s_entries[s_entry_count++];
        memset(entry, 0, sizeof(*entry));
        entry->cp = cp;
        entry->cells = EMOJI_PACK_MAX_CELLS;
    }
    snprintf(entry->file, sizeof(entry->file), "%s", value);
    if (load->added++ > 512) {
        return false;   /* hostile file guard */
    }
    return true;
}

/** Try one volume: `<root>/EMOJI/EMOJI.INI`. */
static bool load_volume(const char *root)
{
    char path[64];
    load_ctx_t load;

    snprintf(path, sizeof(path), "%s/EMOJI/EMOJI.INI", root);
    memset(&load, 0, sizeof(load));
    if (storage_ini_file_foreach(path, load_line, &load) != ESP_OK) {
        return false;
    }
    if (load.added == 0) {
        return false;
    }
    snprintf(s_source, sizeof(s_source), "%s",
             strcmp(root, "usb:/") == 0 ? "usb" : "sd");
    return true;
}

void emoji_pack_init(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    s_entries = p4heap_alloc_psram(sizeof(emoji_entry_t) * EMOJI_PACK_MAX_ENTRIES);
    if (s_entries == NULL) {
        return;
    }
    memset(s_entries, 0, sizeof(emoji_entry_t) * EMOJI_PACK_MAX_ENTRIES);
    s_entry_count = 0;
    s_source[0] = '\0';
    if (load_volume("sd:/") || load_volume("usb:/")) {
        return;   /* SD wins; USB is the fallback */
    }
}

void emoji_pack_free(void)
{
    for (int i = 0; i < EMOJI_PACK_CACHE_SLOTS; i++) {
        if (s_cache[i].used) {
            gfx_surface_free(&s_cache[i].surface);
            s_cache[i].used = false;
        }
    }
    if (s_entries != NULL) {
        p4heap_free(s_entries);
        s_entries = NULL;
    }
    s_entry_count = 0;
    s_source[0] = '\0';
    s_loaded = false;
}

void emoji_pack_reload(void)
{
    emoji_pack_free();
    emoji_pack_init();
}

int emoji_pack_count(void)
{
    return s_entry_count;
}

const char *emoji_pack_source(void)
{
    return s_source;
}

bool emoji_pack_lookup(uint32_t cp, char *path, size_t path_size, int *cells)
{
    emoji_entry_t *entry;

    emoji_pack_init();
    entry = find_entry(cp);
    if (entry == NULL) {
        return false;
    }
    if (path != NULL && path_size > 0) {
        snprintf(path, path_size, "%s/EMOJI/%s",
                 strcmp(s_source, "usb") == 0 ? "usb:/" : "sd:/", entry->file);
    }
    if (cells != NULL) {
        *cells = entry->cells;
    }
    return true;
}

/** Read a whole binary file into a PSRAM buffer (NULL when missing/too big). */
static uint8_t *read_file(const char *path, size_t *size_out)
{
    char resolved[256];
    FILE *f;
    long size;
    uint8_t *buf;

    if (storage_resolve_input_source(path, resolved, sizeof(resolved)) != ESP_OK) {
        return NULL;
    }
    f = fopen(resolved, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size <= 0 || size > (long)GFX_BMP_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = p4heap_alloc_psram((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        p4heap_free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)size;
    return buf;
}

const gfx_surface_t *emoji_pack_image(uint32_t cp, int *cells)
{
    char path[128];
    int width_cells = EMOJI_PACK_MAX_CELLS;
    size_t len = 0;
    uint8_t *data;
    gfx_surface_t surface;
    imgdec_image_t imd;

    if (!emoji_pack_lookup(cp, path, sizeof(path), &width_cells)) {
        return NULL;
    }
    if (cells != NULL) {
        *cells = width_cells;
    }
    for (int i = 0; i < EMOJI_PACK_CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].cp == cp) {
            return &s_cache[i].surface;
        }
    }
    data = read_file(path, &len);
    if (data == NULL) {
        return NULL;
    }
    /* One cell grid is 8x8 pixels for the ramp; the draw path samples it to
     * whatever the live font cell is. */
    if (!imgdec_decode_scaled(data, len, width_cells * 8, 8, &imd)) {
        p4heap_free(data);
        return NULL;
    }
    p4heap_free(data);
    memset(&surface, 0, sizeof(surface));
    surface.px = imd.pixels;
    surface.w = imd.width;
    surface.h = imd.height;

    {
        int slot = s_cache_next;

        s_cache_next = (s_cache_next + 1) % EMOJI_PACK_CACHE_SLOTS;
        if (s_cache[slot].used) {
            gfx_surface_free(&s_cache[slot].surface);
        }
        s_cache[slot].cp = cp;
        s_cache[slot].cells = width_cells;
        s_cache[slot].surface = surface;
        s_cache[slot].used = true;
        return &s_cache[slot].surface;
    }
}

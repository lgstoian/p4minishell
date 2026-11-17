/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file emoji.c
 * @brief Emoji pack loader (see emoji.h).
 */

#include "emoji.h"
#include "imagefile.h"

#include "esp_log.h"

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

/* LVGL label glyphs are square and role-sized, so they need their own decode
 * size. Four slots match the TUI cache budget. */
typedef struct {
    uint32_t cp;
    int px;
    uint16_t *pixels;
    lv_image_dsc_t dsc;
    bool used;
} emoji_lvgl_slot_t;

static emoji_entry_t *s_entries = NULL;   /* PSRAM, EMOJI_PACK_MAX_ENTRIES */
static int s_entry_count = 0;
static char s_source[8] = "";
static bool s_loaded = false;
static emoji_cache_slot_t s_cache[EMOJI_PACK_CACHE_SLOTS];
static int s_cache_next = 0;
static emoji_lvgl_slot_t s_lvgl_cache[EMOJI_PACK_CACHE_SLOTS];
static int s_lvgl_next = 0;

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
        if (s_lvgl_cache[i].used) {
            p4heap_free(s_lvgl_cache[i].pixels);
            s_lvgl_cache[i].used = false;
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

bool emoji_pack_entry_at(int index, uint32_t *cp, char *file, size_t file_size,
                         int *cells)
{
    emoji_pack_init();
    if (index < 0 || index >= s_entry_count) {
        return false;
    }
    if (cp != NULL) {
        *cp = s_entries[index].cp;
    }
    if (file != NULL && file_size > 0) {
        snprintf(file, file_size, "%s", s_entries[index].file);
    }
    if (cells != NULL) {
        *cells = s_entries[index].cells;
    }
    return true;
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
        snprintf(path, path_size, "%sEMOJI/%s",
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
    gfx_surface_t full;

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
     * whatever the live font cell is. The shared decoder owns the
     * BMP-vs-PNG/JPEG choice (gfx_image_decode_mem). */
    /* A TUI cell shows one ramp glyph, so a `cells`-wide inline emoji has only
     * `cells` samples. Decode the icon once at a readable grid, then pick the
     * brightest pixel of each cell block: a plain nearest-neighbour decode to
     * 2x1 samples the icon's top-left background and renders nothing. */
    if (!imagefile_decode_mem(data, len, width_cells * 8, 8, &full)) {
            p4heap_free(data);
        return NULL;
    }
    p4heap_free(data);
    if (!gfx_surface_alloc(&surface, width_cells, 1)) {
        gfx_surface_free(&full);
        return NULL;
    }
    for (int cx = 0; cx < width_cells; cx++) {
        uint16_t best = 0;
        int best_lum = -1;

        for (int y = 0; y < full.h; y++) {
            for (int x = cx * full.w / width_cells;
                 x < (cx + 1) * full.w / width_cells && x < full.w; x++) {
                uint16_t px = full.px[(size_t)y * (size_t)full.w + (size_t)x];
                int r = ((px >> 11) & 0x1F) << 3;
                int g = ((px >> 5) & 0x3F) << 2;
                int b = (px & 0x1F) << 3;
                int lum = (r * 30 + g * 59 + b * 11) / 100;

                if (lum > best_lum) {
                    best_lum = lum;
                    best = px;
                }
            }
        }
        surface.px[cx] = best;
    }
    gfx_surface_free(&full);

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

const lv_image_dsc_t *emoji_pack_lvgl_image(uint32_t cp, int px)
{
    char path[128];
    int cells = EMOJI_PACK_MAX_CELLS;
    size_t len = 0;
    uint8_t *data;
    gfx_surface_t surface;

    if (px < 8) px = 8;
    if (px > 64) px = 64;
    if (!emoji_pack_lookup(cp, path, sizeof(path), &cells)) {
        ESP_LOGD("emoji_pack", "lvgl cp=%lx: not in pack", (unsigned long)cp);
        return NULL;
    }
    (void)cells;
    for (int i = 0; i < EMOJI_PACK_CACHE_SLOTS; i++) {
        if (s_lvgl_cache[i].used && s_lvgl_cache[i].cp == cp &&
            s_lvgl_cache[i].px == px) {
            return &s_lvgl_cache[i].dsc;
        }
    }
    data = read_file(path, &len);
    if (data == NULL) {
        ESP_LOGD("emoji_pack", "lvgl cp=%lx: read %s failed", (unsigned long)cp, path);
        return NULL;
    }
    if (!imagefile_decode_mem(data, len, px, px, &surface)) {
        ESP_LOGD("emoji_pack", "lvgl cp=%lx: decode %u bytes failed",
                 (unsigned long)cp, (unsigned)len);
        p4heap_free(data);
        return NULL;
    }
    ESP_LOGD("emoji_pack", "lvgl cp=%lx: %dx%d ok", (unsigned long)cp, surface.w,
             surface.h);
    p4heap_free(data);

    {
        int slot = s_lvgl_next;
        emoji_lvgl_slot_t *entry;

        s_lvgl_next = (s_lvgl_next + 1) % EMOJI_PACK_CACHE_SLOTS;
        entry = &s_lvgl_cache[slot];
        if (entry->used) {
            p4heap_free(entry->pixels);
        }
        memset(entry, 0, sizeof(*entry));
        entry->cp = cp;
        entry->px = px;
        entry->pixels = surface.px;
        entry->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        entry->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        entry->dsc.header.w = (uint32_t)surface.w;
        entry->dsc.header.h = (uint32_t)surface.h;
        entry->dsc.header.stride = (uint32_t)(surface.w * 2);
        entry->dsc.data_size = (uint32_t)(surface.w * surface.h * 2);
        entry->dsc.data = (const uint8_t *)surface.px;
        entry->used = true;
        return &entry->dsc;
    }
}

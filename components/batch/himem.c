/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file himem.c
 * @brief XMS-style extended memory: handle blocks over PSRAM for batch apps.
 *
 * DOS extended-memory tricks without the A20 dance: named handle blocks above
 * the 64-variable environment ceiling, for staging bulk data (file slices,
 * computed tables) without touching the SD card. One handle table over the
 * single p4heap PSRAM allocator (never a second heap); blocks never move, so
 * XMS locks guard against free rather than promising relocation.
 *
 * Ownership is the process-isolation half: every block records its task
 * slot, one background job's holdings are capped by
 * P4_CONFIG_HIMEM_JOB_QUOTA_BYTES, and `batch_bg_release()` frees whatever
 * a job still holds (locks do not survive job exit — no TSRs, no leaks).
 * The environment overlay in batch.c is the other half (private `set`).
 */

#include "batch.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ansi_palette.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "shell.h"
#include "storage.h"
#include "strutil.h"

#ifndef HIMEM_TAG
#define HIMEM_TAG "himem"
#endif

/** One extended-memory block. */
typedef struct {
    bool used;
    uint8_t *ptr;      /**< PSRAM block (zeroed at alloc). */
    size_t size;
    int owner_slot;    /**< 0 = foreground, N = bg slot N. */
    uint32_t lock;     /**< XMS lock guard: locked blocks refuse free. */
} himem_block_t;

/* Bulk tables live in PSRAM (allocated on first use), never BSS: every
 * internal-RAM byte counts in the boot window that the command worker
 * (32 KB contiguous) and the Wi-Fi task (12 KB) need. */
static himem_block_t *s_himem = NULL;
static size_t *s_himem_slot_bytes = NULL;

/** Ensure the tables exist (PSRAM). False when out of memory. */
static bool himem_ensure_tables(void)
{
    if (s_himem == NULL) {
        s_himem = p4heap_calloc_psram(P4_CONFIG_HIMEM_HANDLES, sizeof(*s_himem));
        if (s_himem == NULL) {
            return false;
        }
    }
    if (s_himem_slot_bytes == NULL) {
        s_himem_slot_bytes = p4heap_calloc_psram((size_t)1 + (size_t)P4_CONFIG_BG_TASKS,
                                                 sizeof(*s_himem_slot_bytes));
        if (s_himem_slot_bytes == NULL) {
            return false;
        }
    }
    return true;
}

/** True for a live handle index. NULL-safe before the tables exist. */
static bool himem_handle_valid(int h)
{
    return s_himem != NULL && h >= 0 && h < (int)P4_CONFIG_HIMEM_HANDLES && s_himem[h].used;
}

/** True when @p slot may act on handle @p h (owner, or the foreground
 *  supervisor acting over any job's block). */
static bool himem_may_access(int h, int slot)
{
    return himem_handle_valid(h) &&
           (slot == 0 || slot == s_himem[h].owner_slot);
}

int himem_alloc(size_t size, int slot)
{
    int h;

    if (size == 0 || size > (size_t)P4_CONFIG_HIMEM_MAX_BYTES) {
        return -1;
    }
    if (slot < 0 || slot > (int)P4_CONFIG_BG_TASKS) {
        return -1;
    }
    if (!himem_ensure_tables()) {
        return -1;
    }
    if (slot > 0 &&
        s_himem_slot_bytes[slot] + size > (size_t)P4_CONFIG_HIMEM_JOB_QUOTA_BYTES) {
        return -1;
    }
    for (h = 0; h < (int)P4_CONFIG_HIMEM_HANDLES; h++) {
        if (!s_himem[h].used) {
            s_himem[h].ptr = p4heap_calloc_psram(1, size);
            if (s_himem[h].ptr == NULL) {
                return -1;
            }
            s_himem[h].used = true;
            s_himem[h].size = size;
            s_himem[h].owner_slot = slot;
            s_himem[h].lock = 0;
            /* Foreground holdings are uncapped but still tracked, so
             * `himem info` accounts every byte. */
            s_himem_slot_bytes[slot] += size;
            return h;
        }
    }
    return -1;
}

esp_err_t himem_free(int h, int slot)
{
    if (!himem_handle_valid(h)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!himem_may_access(h, slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_himem[h].lock > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_himem_slot_bytes[s_himem[h].owner_slot] -= s_himem[h].size;
    p4heap_free(s_himem[h].ptr);
    memset(&s_himem[h], 0, sizeof(s_himem[h]));
    return ESP_OK;
}

int himem_lock(int h, int slot)
{
    if (!himem_may_access(h, slot)) {
        return -1;
    }
    if (s_himem[h].lock >= UINT32_MAX) {
        return -1;
    }
    s_himem[h].lock++;
    return (int)s_himem[h].lock;
}

int himem_unlock(int h, int slot)
{
    if (!himem_may_access(h, slot)) {
        return -1;
    }
    if (s_himem[h].lock == 0) {
        return -1;
    }
    s_himem[h].lock--;
    return (int)s_himem[h].lock;
}

esp_err_t himem_peek(int h, size_t offset, size_t count,
                     const uint8_t **ptr_out, size_t *len_out)
{
    if (!himem_handle_valid(h)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset > s_himem[h].size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset == s_himem[h].size) {
        /* Exact end: only the explicit to-end request (count 0) is a valid
         * empty range; a nonzero count has nothing to clamp to. */
        if (count != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        count = 0;
    } else if (count == 0 || offset + count > s_himem[h].size) {
        count = s_himem[h].size - offset;
    }
    if (ptr_out != NULL) {
        *ptr_out = s_himem[h].ptr + offset;
    }
    if (len_out != NULL) {
        *len_out = count;
    }
    return ESP_OK;
}

esp_err_t himem_fill(int h, int slot, uint8_t byte, size_t offset, size_t count)
{
    const uint8_t *ptr = NULL;
    size_t len = 0;

    if (!himem_may_access(h, slot)) {
        return himem_handle_valid(h) ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    if (himem_peek(h, offset, count, &ptr, &len) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    memset((void *)ptr, byte, len);
    return ESP_OK;
}

himem_handle_info_t himem_describe(int h)
{
    himem_handle_info_t info;

    memset(&info, 0, sizeof(info));
    if (s_himem == NULL || h < 0 || h >= (int)P4_CONFIG_HIMEM_HANDLES) {
        return info;
    }
    info.used = s_himem[h].used;
    info.size = s_himem[h].size;
    info.owner_slot = s_himem[h].owner_slot;
    info.locked = s_himem[h].lock > 0;
    return info;
}

size_t himem_slot_bytes(int slot)
{
    if (s_himem_slot_bytes == NULL || slot < 0 || slot > (int)P4_CONFIG_BG_TASKS) {
        return 0;
    }
    return s_himem_slot_bytes[slot];
}

void himem_free_owned(int slot)
{
    int h;

    if (s_himem == NULL || slot < 0 || slot > (int)P4_CONFIG_BG_TASKS) {
        return;
    }
    /* Job exit frees everything, locked or not: locks guard against an
     * accidental `himem free` mid-job, never against cleanup. */
    for (h = 0; h < (int)P4_CONFIG_HIMEM_HANDLES; h++) {
        if (s_himem[h].used && s_himem[h].owner_slot == slot) {
            s_himem_slot_bytes[slot] -= s_himem[h].size;
            p4heap_free(s_himem[h].ptr);
            memset(&s_himem[h], 0, sizeof(s_himem[h]));
        }
    }
    s_himem_slot_bytes[slot] = 0;
}

/**
 * Parse a byte count: plain decimal, `0x` hex, or a `K`/`M` suffix
 * (1024/1048576, case-insensitive, DOS convention). Pure, unit-tested.
 *
 * @return The byte count, or 0 for empty/garbage/overflow (callers treat 0
 *         as invalid — a zero-byte block is never allocated).
 */
uint64_t himem_parse_size(const char *text)
{
    uint64_t value = 0;
    uint64_t mult = 1;
    const char *p;

    if (text == NULL || *text == '\0') {
        return 0;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        for (p = text + 2; *p != '\0'; p++) {
            unsigned digit;

            if (*p >= '0' && *p <= '9') {
                digit = (unsigned)(*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                digit = (unsigned)(*p - 'a') + 10u;
            } else if (*p >= 'A' && *p <= 'F') {
                digit = (unsigned)(*p - 'A') + 10u;
            } else {
                return 0;
            }
            if (value > (UINT64_MAX - digit) / 16u) {
                return 0;
            }
            value = value * 16u + digit;
        }
        return value;
    }
    for (p = text; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            if (value > (UINT64_MAX - 9u) / 10u) {
                return 0;
            }
            value = value * 10u + (uint64_t)(*p - '0');
        } else if ((*p == 'K' || *p == 'k') && p[1] == '\0') {
            mult = 1024u;
        } else if ((*p == 'M' || *p == 'm') && p[1] == '\0') {
            mult = 1024u * 1024u;
        } else {
            return 0;
        }
    }
    if (value > UINT64_MAX / mult) {
        return 0;
    }
    return value * mult;
}

/**
 * Parse a decimal handle number (pure, unit-tested). @return The handle,
 * or -1 for empty/garbage/out-of-range.
 */
int himem_parse_handle(const char *text)
{
    long h = -1;
    const char *p;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    h = 0;
    for (p = text; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        h = h * 10 + (*p - '0');
        if (h >= (long)P4_CONFIG_HIMEM_HANDLES) {
            return -1;
        }
    }
    return (int)h;
}

/* -------------------------------------------------------------------------
 * FILE TRANSFER (load/save stage through the block, not the SD card)
 * ---------------------------------------------------------------------- */

/**
 * Load a file prefix into the block at @p offset (pure VFS fread, the same
 * direct-into-PSRAM shape as the batch RAM image — no DMA bounce needed).
 * @return ESP_OK with @p loaded_out set, or a failure code (block left
 *         untouched on failure).
 */
static esp_err_t himem_load(int h, int slot, const char *path, size_t offset,
                            size_t *loaded_out)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    FILE *file = NULL;
    long fsize = 0;
    size_t want = 0;
    size_t got = 0;

    if (!himem_may_access(h, slot)) {
        return himem_handle_valid(h) ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    if (offset > s_himem[h].size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path == NULL || shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (fsize = ftell(file)) < 0) {
        fclose(file);
        return ESP_ERR_INVALID_STATE;
    }
    fseek(file, 0, SEEK_SET);
    want = s_himem[h].size - offset;
    if ((size_t)fsize < want) {
        want = (size_t)fsize;
    }
    got = fread(s_himem[h].ptr + offset, 1, want, file);
    fclose(file);
    if (got != want) {
        return ESP_ERR_INVALID_STATE;
    }
    if (loaded_out != NULL) {
        *loaded_out = got;
    }
    return ESP_OK;
}

/**
 * Save [offset, offset+count) to @p path (count 0 = to end of block).
 * @return ESP_OK with @p saved_out set, or a failure code.
 */
static esp_err_t himem_save(int h, const char *path, size_t offset, size_t count,
                            size_t *saved_out)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    const uint8_t *ptr = NULL;
    size_t len = 0;
    FILE *file = NULL;
    size_t wrote = 0;

    if (himem_peek(h, offset, count, &ptr, &len) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path == NULL || shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    file = fopen(resolved, "wb");
    if (file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    wrote = (len > 0) ? fwrite(ptr, 1, len, file) : 0;
    fclose(file);
    if (wrote != len) {
        return ESP_ERR_INVALID_STATE;
    }
    if (saved_out != NULL) {
        *saved_out = wrote;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * THE `himem` VERB
 * ---------------------------------------------------------------------- */

static void himem_cmd_info(void)
{
    int h;
    int used = 0;
    size_t bytes = 0;

    shell_transcript_appendf_ansi(SH_HEAD "Extended memory" SH_RST "\n");
    for (h = 0; h < (int)P4_CONFIG_HIMEM_HANDLES; h++) {
        himem_handle_info_t info = himem_describe(h);

        if (!info.used) {
            continue;
        }
        used++;
        bytes += info.size;
        shell_transcript_appendf_ansi("  " SH_NUM "%d" SH_RST ": %u bytes%s%s\n",
                                      h, (unsigned)info.size,
                                      info.owner_slot == 0 ? "" : " (job)",
                                      info.locked ? " locked" : "");
    }
    shell_print_field("handles", "%d/%d used", used, P4_CONFIG_HIMEM_HANDLES);
    shell_print_field("bytes", "%u total", (unsigned)bytes);
    shell_print_field("job quota", "%u bytes/job", (unsigned)P4_CONFIG_HIMEM_JOB_QUOTA_BYTES);
    batch_set_errorlevel(0);
}

void shell_command_himem(int argc, char **argv)
{
    int slot = batch_bg_current_slot();

    if (argc < 2) {
        himem_cmd_info();
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "info")) {
        himem_cmd_info();
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "alloc")) {
        const char *varname = NULL;
        uint64_t bytes = 0;
        int h = -1;
        int i;

        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '/' && (argv[i][1] == 'v' || argv[i][1] == 'V') &&
                argv[i][2] == ':' && argv[i][3] != '\0') {
                varname = argv[i] + 3;
            } else if (bytes == 0) {
                bytes = himem_parse_size(argv[i]);
            }
        }
        if (bytes == 0 || bytes > (uint64_t)P4_CONFIG_HIMEM_MAX_BYTES) {
            shell_print_usage("Usage: himem alloc <bytes>[K|M] [/v:VAR]");
            batch_set_errorlevel(2);
            return;
        }
        h = himem_alloc((size_t)bytes, slot);
        if (h < 0) {
            shell_print_error("himem: alloc failed (%u bytes%s)",
                              (unsigned)bytes,
                              slot > 0 ? ", job quota or table full" : ", table full or no PSRAM");
            batch_set_errorlevel(1);
            return;
        }
        if (varname != NULL) {
            char text[16];

            snprintf(text, sizeof(text), "%d", h);
            if (shell_env_set(varname, text) != ESP_OK) {
                shell_print_error("himem: cannot set %s", varname);
                batch_set_errorlevel(1);
                return;
            }
        }
        shell_print_ok("himem: handle %d (%u bytes)", h, (unsigned)bytes);
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "free")) {
        int h = (argc >= 3) ? himem_parse_handle(argv[2]) : -1;

        if (h < 0) {
            shell_print_usage("Usage: himem free <handle>");
            batch_set_errorlevel(2);
            return;
        }
        if (himem_free(h, slot) != ESP_OK) {
            shell_print_error("himem: cannot free %d (bad, foreign, or locked)", h);
            batch_set_errorlevel(1);
            return;
        }
        shell_print_ok("himem: freed %d", h);
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "lock") ||
        shell_text_equals_ignore_case(argv[1], "unlock")) {
        bool lock = (argv[1][0] == 'l' || argv[1][0] == 'L');
        int h = (argc >= 3) ? himem_parse_handle(argv[2]) : -1;
        int count = -1;

        if (h < 0) {
            shell_print_usage("Usage: himem lock|unlock <handle>");
            batch_set_errorlevel(2);
            return;
        }
        count = lock ? himem_lock(h, slot) : himem_unlock(h, slot);
        if (count < 0) {
            shell_print_error("himem: cannot %s %d", lock ? "lock" : "unlock", h);
            batch_set_errorlevel(1);
            return;
        }
        shell_print_ok("himem: %d lock count %d", h, count);
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "fill")) {
        int h = (argc >= 3) ? himem_parse_handle(argv[2]) : -1;
        uint64_t byte = (argc >= 4) ? himem_parse_size(argv[3]) : (uint64_t)256;
        uint64_t offset = (argc >= 5) ? himem_parse_size(argv[4]) : 0;
        uint64_t count = (argc >= 6) ? himem_parse_size(argv[5]) : 0;

        if (h < 0 || byte > 255u) {
            shell_print_usage("Usage: himem fill <handle> <byte> [offset] [count]");
            batch_set_errorlevel(2);
            return;
        }
        if (himem_fill(h, slot, (uint8_t)byte, (size_t)offset, (size_t)count) != ESP_OK) {
            shell_print_error("himem: fill %d out of range or foreign", h);
            batch_set_errorlevel(1);
            return;
        }
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "load")) {
        int h = (argc >= 3) ? himem_parse_handle(argv[2]) : -1;
        const char *path = (argc >= 4) ? argv[3] : NULL;
        uint64_t offset = (argc >= 5) ? himem_parse_size(argv[4]) : 0;
        size_t loaded = 0;

        if (h < 0 || path == NULL) {
            shell_print_usage("Usage: himem load <handle> <file> [offset]");
            batch_set_errorlevel(2);
            return;
        }
        if (himem_load(h, slot, path, (size_t)offset, &loaded) != ESP_OK) {
            shell_print_error("himem: cannot load %s into %d", path, h);
            batch_set_errorlevel(1);
            return;
        }
        shell_print_ok("himem: loaded %u bytes into %d", (unsigned)loaded, h);
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "save")) {
        int h = (argc >= 3) ? himem_parse_handle(argv[2]) : -1;
        const char *path = (argc >= 4) ? argv[3] : NULL;
        uint64_t offset = (argc >= 5) ? himem_parse_size(argv[4]) : 0;
        uint64_t count = (argc >= 6) ? himem_parse_size(argv[5]) : 0;
        size_t saved = 0;

        if (h < 0 || path == NULL) {
            shell_print_usage("Usage: himem save <handle> <file> [offset] [count]");
            batch_set_errorlevel(2);
            return;
        }
        if (himem_save(h, path, (size_t)offset, (size_t)count, &saved) != ESP_OK) {
            shell_print_error("himem: cannot save %d to %s", h, path);
            batch_set_errorlevel(1);
            return;
        }
        shell_print_ok("himem: saved %u bytes from %d", (unsigned)saved, h);
        batch_set_errorlevel(0);
        return;
    }
    if (shell_text_equals_ignore_case(argv[1], "crc")) {
        int h = (argc >= 3) ? himem_parse_handle(argv[2]) : -1;
        uint64_t offset = (argc >= 4) ? himem_parse_size(argv[3]) : 0;
        uint64_t count = (argc >= 5) ? himem_parse_size(argv[4]) : 0;
        const uint8_t *ptr = NULL;
        size_t len = 0;
        uint32_t crc;

        if (h < 0) {
            shell_print_usage("Usage: himem crc <handle> [offset] [count]");
            batch_set_errorlevel(2);
            return;
        }
        if (himem_peek(h, (size_t)offset, (size_t)count, &ptr, &len) != ESP_OK) {
            shell_print_error("himem: crc %d out of range", h);
            batch_set_errorlevel(1);
            return;
        }
        crc = ~strutil_crc32_update(0xFFFFFFFFu, ptr, len);
        shell_transcript_appendf("himem.crc: %08X\n", (unsigned)crc);
        batch_set_errorlevel(0);
        return;
    }
    shell_print_usage("Usage: himem [info] | alloc <bytes>[K|M] [/v:VAR] | free <h> | "
                      "lock|unlock <h> | fill <h> <byte> [off] [n] | load <h> <file> [off] | "
                      "save <h> <file> [off] [n] | crc <h> [off] [n]");
    batch_set_errorlevel(2);
}

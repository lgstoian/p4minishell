/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_spell_sd.c
 * @brief SD half of the offline spellchecker (wordlists and the user overlay).
 *
 * The pure core (editor_spell.c) owns the wordlist sets, the lookups and the
 * tokenizer. Everything that touches the card lives here: reading a DICTS file
 * through an internal DMA bounce buffer, installing the parsed set, and the
 * two writes `spell learn` / `spell forget` perform on the user overlay.
 *
 * This file is firmware-only: the host unit-test lane compiles the core and
 * provides the two storage-backed operations as unavailable (there is no SD
 * card on that lane).
 *
 * @module components/editor
 * @layer leaf
 * @domain app
 * @provides spell wordlist load and user-overlay learn/forget from SD
 * @status stable
 * @test test/main/test_editor.c
 * @hard-rule SD I/O only - the wordlist logic lives in editor_spell.c
 */

#include "editor_spell.h"
#include "editor_spell_internal.h"
#include "editor.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage.h"
#include "shell.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "bsp/esp-bsp.h"

/* Read a DICTS/<rel> file into a heap pool (NUL-terminated). Shared by the
 * base and user loaders: one SD read path, one DMA rule. Returns NULL when
 * missing/unreadable or on allocation failure. */
static char *spell_read_rel(const char *rel, size_t *got_out)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    char *pool;
    uint8_t *bounce;
    size_t bounce_size = P4_CONFIG_FILE_IO_BUFFER_BYTES;
    size_t got = 0;

    if (got_out != NULL) {
        *got_out = 0;
    }
    if (shell_fs_resolve_path(rel, resolved, sizeof(resolved)) != ESP_OK) {
        return NULL;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return NULL;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "spell");
        return NULL;
    }
    pool = p4heap_alloc_psram(P4_CONFIG_SPELL_MAX_BYTES + 1);
    /* SD/FATFS reads go through DMA: the bounce buffer MUST be internal,
     * DMA-capable memory. PSRAM is NOT MALLOC_CAP_DMA on this P4 build, so
     * reading straight into the PSRAM pool crashes (same rule the editor's
     * SD load/save follow). Read in chunks and copy into the pool. */
    bounce = p4heap_alloc_dma(bounce_size);
    if (bounce == NULL) {
        bounce = editor_io_fallback(&bounce_size);
    }
    if (pool == NULL || bounce == NULL) {
        fclose(file);
        shell_sd_end(&session, "spell");
        heap_caps_free(pool);
        heap_caps_free(bounce);
        return NULL;
    }
    while (got < P4_CONFIG_SPELL_MAX_BYTES) {
        size_t want = P4_CONFIG_SPELL_MAX_BYTES - got;
        size_t r;
        if (want > bounce_size) {
            want = bounce_size;
        }
        r = fread(bounce, 1, want, file);
        if (r == 0) {
            break;
        }
        memcpy(pool + got, bounce, r);
        got += r;
    }
    heap_caps_free(bounce);
    fclose(file);
    pool[got] = '\0';
    shell_sd_end(&session, "spell");
    if (got_out != NULL) {
        *got_out = got;
    }
    return pool;
}

bool editor_spell_load(const char *name)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char *pool;
    char **index;
    size_t got = 0;
    size_t words = 0;

    if (name == NULL || name[0] == '\0') {
        name = P4_CONFIG_SPELL_DICT_NAME;
    }
    snprintf(rel, sizeof(rel), "%s/%s.words",
             P4_CONFIG_SPELL_DICT_DIR_NAME, name);
    pool = spell_read_rel(rel, &got);
    if (pool == NULL) {
        return false;
    }
    if (!editor_spell_build_index(pool, got, P4_CONFIG_SPELL_MAX_WORDS,
                                  &index, &words) || words == 0) {
        heap_caps_free(pool);
        heap_caps_free(index);
        return false;
    }

    /* Swap in (release the previous list only after the new one is ready). */
    heap_caps_free(editor_spell_base_pool);
    heap_caps_free(editor_spell_base_index);
    editor_spell_base_pool = pool;
    editor_spell_base_index = index;
    editor_spell_base_count = words;
    editor_spell_base_ready = true;
    return true;
}

/* Relative SD path of the user overlay file. */
static void spell_user_rel(char *rel, size_t size)
{
    snprintf(rel, size, "%s/%s",
             P4_CONFIG_SPELL_DICT_DIR_NAME, P4_CONFIG_SPELL_USER_FILE);
}

bool spell_user_load(void)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char *pool;
    char **index = NULL;
    size_t got = 0;
    size_t words = 0;

    spell_user_rel(rel, sizeof(rel));
    pool = spell_read_rel(rel, &got);
    if (pool == NULL) {
        /* Missing file is fine: an empty overlay. */
        spell_user_unload();
        editor_spell_user_ready = true;
        return true;
    }
    if (!editor_spell_build_index(pool, got, P4_CONFIG_SPELL_USER_MAX_WORDS,
                                  &index, &words)) {
        /* An all-whitespace overlay is legal: it means every learned word was
         * forgotten. Clear the overlay instead of failing and leaving a stale
         * in-memory index behind (which made `spell forget` of the last word
         * report the word as still present). */
        size_t i;
        bool empty = true;

        for (i = 0; i < got; i++) {
            if (!isspace((unsigned char)pool[i])) {
                empty = false;
                break;
            }
        }
        heap_caps_free(pool);
        if (empty) {
            spell_user_unload();
            editor_spell_user_ready = true;
            return true;
        }
        return false;
    }
    heap_caps_free(editor_spell_user_pool);
    heap_caps_free(editor_spell_user_index);
    editor_spell_user_pool = pool;
    editor_spell_user_index = index;
    editor_spell_user_count = words;
    editor_spell_user_ready = true;
    return true;
}

bool editor_spell_user_append(const char *norm)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;

    spell_user_rel(rel, sizeof(rel));
    if (shell_fs_resolve_path(rel, resolved, sizeof(resolved)) != ESP_OK) {
        return false;
    }
    /* The DICTS directory may not exist yet (a board whose wordlists were
     * never pushed): create it so the first learned word can be written. */
    {
        char dir_rel[P4_CONFIG_SD_PATH_BYTES];
        char dir_resolved[P4_CONFIG_SD_PATH_BYTES];

        snprintf(dir_rel, sizeof(dir_rel), "%s", P4_CONFIG_SPELL_DICT_DIR_NAME);
        if (shell_fs_resolve_path(dir_rel, dir_resolved, sizeof(dir_resolved)) == ESP_OK) {
            (void)storage_mkdir_p(dir_resolved);
        }
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    file = fopen(resolved, "ab");
    if (file == NULL) {
        shell_sd_end(&session, "spell");
        return false;
    }
    if (fwrite(norm, 1, strlen(norm), file) != strlen(norm) ||
        fwrite("\n", 1, 1, file) != 1 || fclose(file) != 0) {
        shell_sd_end(&session, "spell");
        return false;
    }
    shell_sd_end(&session, "spell");
    return spell_user_load();
}

bool editor_spell_user_remove(const char *norm)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char *pool;
    size_t got = 0;
    char *kept = NULL;
    size_t kept_len = 0;
    size_t pos = 0;
    bool removed = false;

    spell_user_rel(rel, sizeof(rel));
    pool = spell_read_rel(rel, &got);
    if (pool == NULL) {
        return false;
    }
    kept = p4heap_alloc_psram(got + 1);
    if (kept == NULL) {
        heap_caps_free(pool);
        return false;
    }
    /* Keep every line except the forgotten word (stored lower-cased, like
     * the loader folds them, so compare folded). */
    while (pos < got) {
        size_t start = pos;
        while (pos < got && pool[pos] != '\n' && pool[pos] != '\r') {
            pos++;
        }
        if (pos > start) {
            size_t k;
            bool same = ((pos - start) == strlen(norm));
            for (k = start; same && k < pos; k++) {
                if (tolower((unsigned char)pool[k]) != (unsigned char)norm[k - start]) {
                    same = false;
                }
            }
            if (same) {
                removed = true;
            } else {
                memcpy(kept + kept_len, pool + start, pos - start);
                kept_len += pos - start;
                kept[kept_len++] = '\n';
            }
        }
        while (pos < got && (pool[pos] == '\n' || pool[pos] == '\r')) {
            pos++;
        }
    }
    kept[kept_len] = '\0';
    heap_caps_free(pool);
    if (!removed) {
        heap_caps_free(kept);
        return false;
    }
    /* Atomic rewrite; the loader tolerates the trailing newline. */
    if (storage_write_text_file(rel, kept) != ESP_OK) {
        heap_caps_free(kept);
        return false;
    }
    heap_caps_free(kept);
    return spell_user_load();
}

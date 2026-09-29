/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_spell.c
 * @brief Offline spellchecker core (see editor_spell.h).
 *
 * Memory model: one PSRAM pool holds the whole wordlist as NUL-separated,
 * lower-cased words; a second PSRAM array holds pointers into the pool.
 * Lookups binary-search the sorted pointer array. Everything is bounded by
 * P4_CONFIG_SPELL_* so a corrupt/huge file cannot exhaust memory.
 */

#include "editor_spell.h"

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

static char *s_pool;
static char **s_index;
static size_t s_count;
static bool s_ready;

/* User overlay: same format, separate small set consulted before the base. */
static char *u_pool;
static char **u_index;
static size_t u_count;
static bool u_ready;

/* Session ignore list: NUL-joined lower-cased words, linear scan. */
static char *ig_pool;
static size_t ig_used;
static size_t ig_count;

/* Forward: defined below, used by editor_spell_ok above. */
static bool spell_ignored_has(const char *word, size_t len);

/* qsort comparator: both arguments point at `char *` elements. */
static int spell_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool editor_spell_ready(void)
{
    return (s_ready && s_count > 0) || (u_ready && u_count > 0);
}

size_t editor_spell_word_count(void)
{
    return s_ready ? s_count : 0;
}

void editor_spell_unload(void)
{
    heap_caps_free(s_pool);
    heap_caps_free(s_index);
    s_pool = NULL;
    s_index = NULL;
    s_count = 0;
    s_ready = false;
    spell_user_unload();
    spell_ignored_clear();
}

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
    bounce = p4heap_alloc_dma(P4_CONFIG_FILE_IO_BUFFER_BYTES);
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
        if (want > P4_CONFIG_FILE_IO_BUFFER_BYTES) {
            want = P4_CONFIG_FILE_IO_BUFFER_BYTES;
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

/* Split lines in place (NUL-terminate, lower-case) and build the sorted
 * pointer index, capped at cap_words. Shared by both loaders. */
static bool spell_build_index(char *pool, size_t got, size_t cap_words,
                              char ***index_out, size_t *count_out)
{
    char **index;
    size_t words = 0;
    size_t i;

    if (index_out != NULL) {
        *index_out = NULL;
    }
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (pool == NULL) {
        return false;
    }
    {
        size_t start = 0;
        size_t k;

        for (k = 0; k <= got; k++) {
            char c = pool[k];
            if (c == '\n' || c == '\r' || c == '\0') {
                pool[k] = '\0';
                if (k > start) {
                    size_t j;
                    for (j = start; j < k; j++) {
                        pool[j] = (char)tolower((unsigned char)pool[j]);
                    }
                    if (words < cap_words) {
                        /* Index rebuilt below; just count here. */
                        words++;
                    }
                }
                start = k + 1;
            }
        }
    }
    if (words == 0) {
        return false;
    }
    index = p4heap_alloc_psram((words + 1) * sizeof(*index));
    if (index == NULL) {
        return false;
    }
    {
        size_t n = 0;
        size_t start = 0;
        for (i = 0; i <= got && n < words; i++) {
            if (pool[i] == '\0') {
                if (i > start) {
                    index[n++] = pool + start;
                }
                start = i + 1;
            }
        }
        words = n;
    }
    qsort(index, words, sizeof(*index), spell_cmp);
    if (index_out != NULL) {
        *index_out = index;
    } else {
        heap_caps_free(index);
    }
    if (count_out != NULL) {
        *count_out = words;
    }
    return true;
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
    if (!spell_build_index(pool, got, P4_CONFIG_SPELL_MAX_WORDS, &index, &words) ||
        words == 0) {
        heap_caps_free(pool);
        heap_caps_free(index);
        return false;
    }

    /* Swap in (release the previous list only after the new one is ready). */
    heap_caps_free(s_pool);
    heap_caps_free(s_index);
    s_pool = pool;
    s_index = index;
    s_count = words;
    s_ready = true;
    return true;
}

/* Length-unlimited lookup key: the token bytes plus their length (the
 * token is not NUL-terminated, so the comparator walks both sides). */
typedef struct {
    const char *s;
    size_t len;
} spell_lookup_t;

/* Compare a length-delimited token (ASCII case-folded on the fly) against
 * one lower-cased index entry. Reads neither side past its end. */
static int spell_cmp_token(const void *keyp, const void *elemp)
{
    const spell_lookup_t *k = (const spell_lookup_t *)keyp;
    const char *e = *(const char *const *)elemp;
    size_t i = 0;

    for (;;) {
        bool k_end = (i >= k->len);
        bool e_end = (e[i] == '\0');
        if (k_end && e_end) {
            return 0;
        }
        if (k_end) {
            return -1;
        }
        if (e_end) {
            return 1;
        }
        {
            int kd = tolower((unsigned char)k->s[i]);
            int ed = (unsigned char)e[i]; /* Entries are stored lower-cased. */
            if (kd != ed) {
                return kd - ed;
            }
        }
        i++;
    }
}

/* Membership in one sorted set (shared by base and overlay lookups). */
static bool spell_set_ok(char **index, size_t count, const char *word, size_t len)
{
    spell_lookup_t key;

    if (index == NULL || count == 0) {
        return false;
    }
    key.s = word;
    key.len = len;
    return bsearch(&key, index, count, sizeof(*index), spell_cmp_token) != NULL;
}

bool editor_spell_ok(const char *word, size_t len)
{
    if (word == NULL || len == 0) {
        return true;
    }
    if (spell_ignored_has(word, len)) {
        return true;
    }
    if (u_ready && u_count > 0 && spell_set_ok(u_index, u_count, word, len)) {
        return true;
    }
    if (!editor_spell_ready()) {
        return true; /* No dictionary: nothing is ever flagged. */
    }
    return spell_set_ok(s_index, s_count, word, len);
}

/* ------------------------------------------------------------------------
 * User dictionary overlay + session ignore list
 * ---------------------------------------------------------------------- */

/* Normalize a candidate word into buf (ASCII-lowercased, NUL-terminated).
 * Rejects empty/overlong/control/whitespace so file lines stay clean. */
static bool spell_normalize(const char *word, size_t len, char *buf, size_t size)
{
    size_t i;

    if (word == NULL || len == 0 || len + 1 > size ||
        len > P4_CONFIG_SPELL_WORD_MAX) {
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)word[i];
        if (c <= ' ' || c == 0x7F) {
            return false;
        }
        buf[i] = (char)tolower(c);
    }
    buf[len] = '\0';
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
        u_ready = true;
        return true;
    }
    if (!spell_build_index(pool, got, P4_CONFIG_SPELL_USER_MAX_WORDS,
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
            u_ready = true;
            return true;
        }
        return false;
    }
    heap_caps_free(u_pool);
    heap_caps_free(u_index);
    u_pool = pool;
    u_index = index;
    u_count = words;
    u_ready = true;
    return true;
}

void spell_user_unload(void)
{
    heap_caps_free(u_pool);
    heap_caps_free(u_index);
    u_pool = NULL;
    u_index = NULL;
    u_count = 0;
    u_ready = false;
}

size_t spell_user_count(void)
{
    return u_ready ? u_count : 0;
}

/** True when the normalized word is in the user overlay. */
static bool spell_user_has(const char *norm)
{
    spell_lookup_t key;

    if (!u_ready || u_count == 0 || norm == NULL) {
        return false;
    }
    key.s = norm;
    key.len = strlen(norm);
    return bsearch(&key, u_index, u_count, sizeof(*u_index),
                   spell_cmp_token) != NULL;
}

bool spell_user_learn(const char *word, size_t len)
{
    char norm[P4_CONFIG_SPELL_WORD_MAX + 1];
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    size_t norm_len;
    FILE *file;

    if (!spell_normalize(word, len, norm, sizeof(norm))) {
        return false;
    }
    /* Skip only when the word is genuinely known. Do not use
     * editor_spell_ok() here: with no base dictionary loaded it reports every
     * word as known, so learning would silently become a no-op. */
    norm_len = strlen(norm);
    if (spell_ignored_has(norm, norm_len) ||
        spell_user_has(norm) ||
        (editor_spell_ready() && spell_set_ok(s_index, s_count, norm, norm_len))) {
        return true; /* Already known (base, overlay, or ignored). */
    }
    if (u_count >= P4_CONFIG_SPELL_USER_MAX_WORDS) {
        return false;
    }
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

bool spell_user_forget(const char *word, size_t len)
{
    char norm[P4_CONFIG_SPELL_WORD_MAX + 1];
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char *pool;
    size_t got = 0;
    char *kept = NULL;
    size_t kept_len = 0;
    size_t pos = 0;
    bool removed = false;

    if (!spell_normalize(word, len, norm, sizeof(norm))) {
        return false;
    }
    if (!spell_user_has(norm)) {
        return false; /* Absent from the overlay (base words stay). */
    }
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

size_t spell_user_list(char *buf, size_t size)
{
    size_t i;
    size_t pos = 0;
    size_t need = 0;

    if (!u_ready) {
        if (buf != NULL && size > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    for (i = 0; i < u_count; i++) {
        size_t n = strlen(u_index[i]);
        need += n + 1;
        if (buf != NULL && pos + n + 2 <= size) {
            memcpy(buf + pos, u_index[i], n);
            pos += n;
            buf[pos++] = '\n';
        }
    }
    if (buf != NULL && size > 0) {
        buf[pos < size ? pos : size - 1] = '\0';
    }
    return need;
}

bool spell_ignore(const char *word, size_t len)
{
    char norm[P4_CONFIG_SPELL_WORD_MAX + 1];
    size_t n;
    size_t pool_cap = (size_t)P4_CONFIG_SPELL_IGNORE_MAX_WORDS *
                      (P4_CONFIG_SPELL_WORD_MAX + 1);

    if (!spell_normalize(word, len, norm, sizeof(norm))) {
        return false;
    }
    n = strlen(norm);
    if (spell_ignored_has(norm, n)) {
        return true;
    }
    if (ig_count >= (size_t)P4_CONFIG_SPELL_IGNORE_MAX_WORDS) {
        return false;
    }
    if (ig_pool == NULL) {
        ig_pool = p4heap_alloc_psram(pool_cap);
        if (ig_pool == NULL) {
            return false;
        }
        ig_used = 0;
    }
    if (ig_used + n + 1 > pool_cap) {
        return false;
    }
    memcpy(ig_pool + ig_used, norm, n + 1);
    ig_used += n + 1;
    ig_count++;
    return true;
}

void spell_ignored_clear(void)
{
    heap_caps_free(ig_pool);
    ig_pool = NULL;
    ig_used = 0;
    ig_count = 0;
}

size_t spell_ignored_count(void)
{
    return ig_count;
}

/** Linear scan of the session ignore pool (small by cap). */
static bool spell_ignored_has(const char *word, size_t len)
{
    char norm[P4_CONFIG_SPELL_WORD_MAX + 1];
    const char *p;
    const char *end;

    if (ig_pool == NULL || ig_count == 0) {
        return false;
    }
    if (!spell_normalize(word, len, norm, sizeof(norm))) {
        return false;
    }
    p = ig_pool;
    end = ig_pool + ig_used;
    while (p < end) {
        size_t n = strlen(p);
        if (n == strlen(norm) && memcmp(p, norm, n) == 0) {
            return true;
        }
        p += n + 1;
    }
    return false;
}

size_t editor_spell_utf8(const char *s, size_t avail, unsigned long *cp_out)
{
    unsigned char lead;
    size_t len;
    unsigned long cp;
    size_t i;

    if (s == NULL || avail == 0 || s[0] == '\0') {
        return 0;
    }
    lead = (unsigned char)s[0];
    if (lead < 0x80) {
        if (cp_out != NULL) {
            *cp_out = lead;
        }
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        len = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        cp = lead & 0x07;
    } else {
        return 0;
    }
    if (len > avail) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        unsigned char cont = (unsigned char)s[i];
        if ((cont & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    if (cp_out != NULL) {
        *cp_out = cp;
    }
    return len;
}

bool editor_spell_is_cjk(unsigned long cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) || /* Hangul Jamo */
           (cp >= 0x2E80 && cp <= 0x303E) || /* CJK roots/punctuation */
           (cp >= 0x3041 && cp <= 0x33FF) || /* Hiragana/Katakana/CJK compat */
           (cp >= 0x3400 && cp <= 0x4DBF) || /* Ext A */
           (cp >= 0x4E00 && cp <= 0x9FFF) || /* Unified ideographs */
           (cp >= 0xA000 && cp <= 0xA4CF) || /* Yi */
           (cp >= 0xAC00 && cp <= 0xD7AF) || /* Hangul syllables */
           (cp >= 0xF900 && cp <= 0xFAFF) || /* Compat ideographs */
           (cp >= 0xFE30 && cp <= 0xFE4F) || /* CJK compat forms */
           (cp >= 0xFF00 && cp <= 0xFF60) || /* Fullwidth forms */
           (cp >= 0x20000 && cp <= 0x3FFFD); /* Ext B and beyond */
}

bool editor_spell_is_letter(unsigned long cp)
{
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
        return true;
    }
    if ((cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xF6) ||
        (cp >= 0xF8 && cp <= 0xFF)) {
        return true; /* Latin-1 supplement letters (excluding x/divide). */
    }
    if ((cp >= 0x100 && cp <= 0x24F) || /* Latin Extended-A/B */
        (cp >= 0x1E00 && cp <= 0x1EFF) || /* Latin Extended Additional */
        (cp >= 0x370 && cp <= 0x3FF) || /* Greek */
        (cp >= 0x400 && cp <= 0x4FF) || /* Cyrillic */
        (cp >= 0x300 && cp <= 0x36F)) { /* Combining marks attach. */
        return true;
    }
    return editor_spell_is_cjk(cp);
}

bool editor_spell_is_joiner(unsigned long cp)
{
    return cp == '\'' || cp == '-' || cp == 0x2019;
}

bool editor_spell_token_ok(const char *word, size_t len)
{
    size_t i = 0;
    bool has_cjk = false;
    bool has_nonen = false;
    bool has_digit = false;
    bool has_score = false;
    bool checked = false;

    if (!editor_spell_ready()) {
        return true; /* No dictionary: nothing is ever flagged. */
    }
    if (word == NULL || len == 0) {
        return true;
    }
    while (i < len) {
        unsigned long cp = 0;
        size_t step = editor_spell_utf8(word + i, len - i, &cp);
        if (step == 0) {
            i++;
            continue;
        }
        if (editor_spell_is_cjk(cp)) {
            has_cjk = true;
        } else if (cp >= 0x80) {
            has_nonen = true;
        } else if (cp >= '0' && cp <= '9') {
            has_digit = true;
        } else if (cp == '_') {
            has_score = true;
        }
        i += step;
    }
    /* The wordlist cannot cover these: skip rather than split or flag. */
    if (has_cjk || has_digit || has_score || has_nonen) {
        return true;
    }
    if (editor_spell_ok(word, len)) {
        return true;
    }
    /* Contractions/hyphenates: every letter part of length >= 2 must hit
     * (shorter parts like the `t` in `don't` carry no meaning alone). */
    i = 0;
    while (i < len) {
        size_t a = i;
        while (i < len && word[i] != '\'' && word[i] != '-') {
            i++;
        }
        if (i > a) {
            if (i - a >= 2) {
                checked = true;
                if (!editor_spell_ok(word + a, i - a)) {
                    return false;
                }
            }
        } else {
            i++;
        }
    }
    return checked;
}

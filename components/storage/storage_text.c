/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file storage_text.c
 * @brief Text utilities (find/more/fc/sort/findstr/comp).
 *
 * Moved verbatim out of storage_commands.c in v0.35.6. Declared in
 * storage_commands.h; the single dispatcher in components/command/
 * calls these, it never implements them.
 */

#include "storage_commands.h"
#include "storage.h"
#include "shell.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

/* ========================================================================
 * TEXT UTILITIES: find, more, fc, sort
 * ======================================================================== */

/**
 * Strip the trailing newline sequence from a line read with fgets().
 * @return New length after stripping.
 */
static size_t shell_text_strip_eol(char *line)
{
    size_t length;

    if (line == NULL) {
        return 0;
    }

    length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }

    return length;
}

/** Case-insensitive substring search used by `find /I`. */
static const char *shell_text_find_ci(const char *haystack, const char *needle)
{
    size_t needle_len;

    if (haystack == NULL || needle == NULL) {
        return NULL;
    }

    needle_len = strlen(needle);
    if (needle_len == 0) {
        return haystack;
    }

    for (; *haystack != '\0'; haystack++) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
    }

    return NULL;
}

/* ========================================================================
 * FIND FILE DISCOVERY MODE
 * ========================================================================
 * `find` in file-discovery mode recursively walks a directory tree and
 * reports entries filtered by filename wildcard, size, and modification date.
 * It is entered automatically when any discovery switch is present
 * (`/NAME:`, `/SIZE:`, `/NEWER:`, `/OLDER:`, `/DIRS`, `/B`, `/S`); with the
 * classic switches only it stays the original text-search command. The
 * walker reuses the same FATFS primitives as `dir /s` and keeps each
 * recursion level's state in a single heap block.
 */

/** Filters and running totals for the file-discovery walk. */
typedef struct {
    char name_pattern[SHELL_LFN_BYTES];
    bool have_name;
    bool have_size;
    uint64_t size_min;          /* inclusive */
    uint64_t size_max;          /* inclusive; UINT64_MAX = unbounded */
    bool have_newer;
    uint16_t newer_fdate;       /* FAT date word, inclusive */
    bool have_older;
    uint16_t older_fdate;       /* FAT date word, inclusive */
    bool include_dirs;
    bool bare;
    unsigned int matches;
    unsigned int total_files;
    unsigned int total_dirs;
    uint64_t total_bytes;
    bool truncated;
} shell_find_ctx_t;

/** Parse a size token ("123", "10K", "2M", "1G") into bytes. */
static bool shell_find_parse_size_value(const char *text, uint64_t *out)
{
    uint64_t value;
    char *end = NULL;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    value = strtoull(text, &end, 10);
    if (end == text) {
        return false;
    }
    if (*end == 'K' || *end == 'k') {
        value *= 1024ULL;
        end++;
    } else if (*end == 'M' || *end == 'm') {
        value *= 1024ULL * 1024ULL;
        end++;
    } else if (*end == 'G' || *end == 'g') {
        value *= 1024ULL * 1024ULL * 1024ULL;
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

/**
 * Parse a `/SIZE:` filter into an inclusive byte range.
 *
 * The primary syntax is redirection-safe (no `>`/`<`, which the shell treats
 * as output/input operators): `N-M` (range), `N-` (at least N), `-M` (at most
 * M), or `N` (exact), each with an optional K/M/G suffix. The comparison
 * forms `>N`, `>=N`, `<N`, `<=N` are also accepted when quoted.
 */
static bool shell_find_parse_size(const char *spec, uint64_t *min_out, uint64_t *max_out)
{
    uint64_t a;
    uint64_t b;

    if (spec[0] == '<') {
        if (spec[1] == '=') {
            if (!shell_find_parse_size_value(spec + 2, &b)) {
                return false;
            }
            *min_out = 0;
            *max_out = b;
        } else {
            if (!shell_find_parse_size_value(spec + 1, &b)) {
                return false;
            }
            *min_out = 0;
            *max_out = (b > 0) ? (b - 1) : 0;
        }
        return true;
    }
    if (spec[0] == '>') {
        if (spec[1] == '=') {
            if (!shell_find_parse_size_value(spec + 2, &a)) {
                return false;
            }
            *min_out = a;
            *max_out = UINT64_MAX;
        } else {
            if (!shell_find_parse_size_value(spec + 1, &a)) {
                return false;
            }
            *min_out = (a < UINT64_MAX) ? (a + 1) : a;
            *max_out = UINT64_MAX;
        }
        return true;
    }

    /* Leading '-' is the "at most M" form: -M */
    if (spec[0] == '-') {
        if (!shell_find_parse_size_value(spec + 1, &b)) {
            return false;
        }
        *min_out = 0;
        *max_out = b;
        return true;
    }

    {
        const char *dash = strchr(spec, '-');
        if (dash != NULL) {
            char left[32];
            size_t n = (size_t)(dash - spec);

            if (n == 0 || n >= sizeof(left)) {
                return false;
            }
            memcpy(left, spec, n);
            left[n] = '\0';
            if (!shell_find_parse_size_value(left, &a)) {
                return false;
            }
            if (dash[1] == '\0') {
                /* Trailing '-' is the "at least N" form: N- */
                *min_out = a;
                *max_out = UINT64_MAX;
                return true;
            }
            if (!shell_find_parse_size_value(dash + 1, &b)) {
                return false;
            }
            if (a > b) {
                return false;
            }
            *min_out = a;
            *max_out = b;
            return true;
        }
        if (!shell_find_parse_size_value(spec, &a)) {
            return false;
        }
        *min_out = a;
        *max_out = a;
        return true;
    }
}

/** Parse "YYYY-MM-DD" into a FAT date word. */
/** Shared with `xcopy /D` (storage_files.c); declared in storage_commands.h. */
bool shell_find_parse_date(const char *text, uint16_t *fdate_out)
{
    unsigned int year;
    unsigned int month;
    unsigned int day;

    if (text == NULL || fdate_out == NULL ||
        sscanf(text, "%u-%u-%u", &year, &month, &day) != 3) {
        return false;
    }
    if (year < 1980 || year > 2107 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    *fdate_out = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    return true;
}

/** Print one matched entry. */
static void shell_find_emit(const char *vfs_dir, const FILINFO *info, bool is_dir,
                            shell_find_ctx_t *ctx)
{
    char stamp[24];
    char size_text[24];
    char full[SHELL_SD_PATH_BYTES + SHELL_LFN_BYTES];

    if (ctx->bare) {
        snprintf(full, sizeof(full), "%s/%s", vfs_dir, info->fname);
        shell_transcript_appendf("%s\n", full);
    } else {
        shell_dir_format_stamp(info->fdate, info->ftime, stamp, sizeof(stamp));
        snprintf(full, sizeof(full), "%s/%s", vfs_dir, info->fname);
        if (is_dir) {
            shell_transcript_appendf_ansi("  " SH_DIR "%-36s" SH_RST " " SH_LBL "<DIR>" SH_RST " "
                                          SH_TIME "%s" SH_RST "\n", full, stamp);
        } else {
            shell_sd_format_size((uint64_t)info->fsize, size_text, sizeof(size_text));
            shell_transcript_appendf_ansi("  " SH_DIR "%-36s" SH_RST " " SH_SIZE "%10s" SH_RST " "
                                          SH_TIME "%s" SH_RST "\n", full, size_text, stamp);
        }
    }
    ctx->matches++;
}

/**
 * Recursively walk one directory, printing entries that pass every filter.
 * Each recursion level keeps its own heap block (FATFS handle + path scratch),
 * matching the `dir /s` walker so the 8 KB worker stack is never at risk.
 */
static void shell_find_walk(const char *vfs_dir, int depth, shell_find_ctx_t *ctx)
{
    struct find_level_scratch {
        char fatfs_path[SHELL_SD_PATH_BYTES];
        char child_vfs[SHELL_SD_PATH_BYTES];
        FF_DIR dir;
        FILINFO info;
    } *scratch = NULL;
    FRESULT result;

    if (depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX || ctx->truncated) {
        return;
    }

    scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL) {
        shell_record_errorf("find", ESP_ERR_NO_MEM, "Out of memory during recursive find");
        ctx->truncated = true;
        return;
    }

    if (shell_sd_vfs_to_fatfs_path(vfs_dir, scratch->fatfs_path, sizeof(scratch->fatfs_path)) != ESP_OK) {
        free(scratch);
        return;
    }

    result = f_opendir(&scratch->dir, scratch->fatfs_path);
    if (result != FR_OK) {
        free(scratch);
        return;
    }

    while (ctx->matches < P4_CONFIG_FIND_MATCH_MAX) {
        bool is_dir;
        bool name_ok;
        bool size_ok;
        bool date_ok;
        bool dir_ok;

        result = f_readdir(&scratch->dir, &scratch->info);
        if (result != FR_OK || scratch->info.fname[0] == '\0') {
            break;
        }
        if (strcmp(scratch->info.fname, ".") == 0 || strcmp(scratch->info.fname, "..") == 0) {
            continue;
        }

        is_dir = (scratch->info.fattrib & AM_DIR) != 0;
        name_ok = !ctx->have_name ||
                  shell_wildcard_match(ctx->name_pattern, scratch->info.fname);
        size_ok = is_dir || !ctx->have_size ||
                  ((uint64_t)scratch->info.fsize >= ctx->size_min &&
                   (uint64_t)scratch->info.fsize <= ctx->size_max);
        date_ok = (!ctx->have_newer || scratch->info.fdate >= ctx->newer_fdate) &&
                  (!ctx->have_older || scratch->info.fdate <= ctx->older_fdate);
        dir_ok = !is_dir || ctx->include_dirs;

        if (name_ok && size_ok && date_ok && dir_ok) {
            shell_find_emit(vfs_dir, &scratch->info, is_dir, ctx);
            if (is_dir) {
                ctx->total_dirs++;
            } else {
                ctx->total_files++;
                ctx->total_bytes += (uint64_t)scratch->info.fsize;
            }
        }

        /* Recurse into every subdirectory regardless of the filters. */
        if (is_dir) {
            int written = snprintf(scratch->child_vfs, sizeof(scratch->child_vfs),
                                   "%s/%s", vfs_dir, scratch->info.fname);
            if (written > 0 && (size_t)written < sizeof(scratch->child_vfs)) {
                shell_find_walk(scratch->child_vfs, depth + 1, ctx);
            }
        }
    }

    if (ctx->matches >= P4_CONFIG_FIND_MATCH_MAX) {
        ctx->truncated = true;
    }

    (void)f_closedir(&scratch->dir);
    free(scratch);
}

/** Return true when a `/`-token selects the file-discovery mode of `find`. */
static bool shell_find_is_discovery_token(const char *token)
{
    if (token[0] != '/') {
        return false;
    }
    if (strncasecmp(token, "/NAME:", 6) == 0 ||
        strncasecmp(token, "/SIZE:", 6) == 0 ||
        strncasecmp(token, "/NEWER:", 7) == 0 ||
        strncasecmp(token, "/OLDER:", 7) == 0 ||
        shell_text_equals_ignore_case(token, "/DIRS") ||
        shell_text_equals_ignore_case(token, "/B") ||
        shell_text_equals_ignore_case(token, "/S")) {
        return true;
    }
    return false;
}

/**
 * File-discovery mode of `find`: recursively list entries matching the
 * filename wildcard, size, and date filters. Returns an ERRORLEVEL.
 */
static int shell_find_files(int argc, char **argv)
{
    shell_find_ctx_t ctx;
    char resolved_path[SHELL_SD_PATH_BYTES];
    char dir_part[SHELL_SD_PATH_BYTES];
    char total_text[24];
    shell_sd_session_t session;
    struct stat path_stat;
    const char *path_arg = NULL;
    esp_err_t error;
    int index;

    memset(&ctx, 0, sizeof(ctx));
    ctx.size_max = UINT64_MAX;

    for (index = 1; index < argc; index++) {
        const char *token = argv[index];

        if (token[0] != '/') {
            if (path_arg == NULL) {
                path_arg = token;
                continue;
            }
            shell_print_usage("Usage: find [path] [/NAME:pat] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B]");
            return 2;
        }

        if (strncasecmp(token, "/NAME:", 6) == 0) {
            snprintf(ctx.name_pattern, sizeof(ctx.name_pattern), "%s", token + 6);
            ctx.have_name = true;
        } else if (strncasecmp(token, "/SIZE:", 6) == 0) {
            if (!shell_find_parse_size(token + 6, &ctx.size_min, &ctx.size_max)) {
                shell_print_error("find: invalid /SIZE filter '%s'", token + 6);
                shell_print_muted("  Use N-M (range), N- (at least), -M (at most), or N (exact), with an optional K/M/G suffix");
                return 2;
            }
            ctx.have_size = true;
        } else if (strncasecmp(token, "/NEWER:", 7) == 0) {
            if (!shell_find_parse_date(token + 7, &ctx.newer_fdate)) {
                shell_print_error("find: invalid /NEWER date '%s' (expected YYYY-MM-DD)", token + 7);
                return 2;
            }
            ctx.have_newer = true;
        } else if (strncasecmp(token, "/OLDER:", 7) == 0) {
            if (!shell_find_parse_date(token + 7, &ctx.older_fdate)) {
                shell_print_error("find: invalid /OLDER date '%s' (expected YYYY-MM-DD)", token + 7);
                return 2;
            }
            ctx.have_older = true;
        } else if (shell_text_equals_ignore_case(token, "/DIRS")) {
            ctx.include_dirs = true;
        } else if (shell_text_equals_ignore_case(token, "/B")) {
            ctx.bare = true;
        } else if (shell_text_equals_ignore_case(token, "/S")) {
            /* Discovery recurses by default; /S is accepted for clarity. */
        } else {
            shell_print_error("find: unknown option %s", token);
            shell_print_usage("Usage: find [path] [/NAME:pat] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B]");
            return 2;
        }
    }

    /* A wildcard in the path splits into a directory plus a name pattern,
     * exactly like `dir`. */
    if (path_arg != NULL && (strchr(path_arg, '*') != NULL || strchr(path_arg, '?') != NULL)) {
        const char *last_sep = strrchr(path_arg, '/');

        if (last_sep == NULL) {
            last_sep = strrchr(path_arg, '\\');
        }
        if (last_sep != NULL) {
            size_t dir_len = (size_t)(last_sep - path_arg);

            if (dir_len >= sizeof(dir_part)) {
                dir_len = sizeof(dir_part) - 1;
            }
            memcpy(dir_part, path_arg, dir_len);
            dir_part[dir_len] = '\0';
            if (!ctx.have_name) {
                snprintf(ctx.name_pattern, sizeof(ctx.name_pattern), "%s", last_sep + 1);
                ctx.have_name = true;
            }
            error = shell_fs_resolve_path(dir_part, resolved_path, sizeof(resolved_path));
        } else {
            if (!ctx.have_name) {
                snprintf(ctx.name_pattern, sizeof(ctx.name_pattern), "%s", path_arg);
                ctx.have_name = true;
            }
            error = shell_fs_resolve_path(".", resolved_path, sizeof(resolved_path));
        }
    } else {
        error = shell_fs_resolve_path(path_arg, resolved_path, sizeof(resolved_path));
    }
    if (error != ESP_OK) {
        shell_print_error("find: invalid path");
        return 2;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("find: SD card not present - insert and retry");
        return 2;
    }

    if (shell_sd_stat_path(resolved_path, &path_stat) != ESP_OK || !S_ISDIR(path_stat.st_mode)) {
        shell_transcript_appendf("find: path not found or not a directory %s\n", resolved_path);
        shell_sd_end(&session, "find");
        return 2;
    }

    shell_find_walk(resolved_path, 0, &ctx);
    shell_sd_end(&session, "find");

    shell_sd_format_size(ctx.total_bytes, total_text, sizeof(total_text));
    shell_transcript_appendf_ansi(SH_MUTE "find:" SH_RST " %u file(s), %u dir(s), %s total%s\n",
                                  ctx.total_files,
                                  ctx.total_dirs,
                                  total_text,
                                  ctx.truncated ? " (truncated)" : "");
    if (ctx.truncated) {
        shell_transcript_appendf_ansi(SH_MUTE "find:" SH_RST " result cap " SH_NUM "%u" SH_RST
                                      " reached; narrow the filters\n",
                                      (unsigned int)P4_CONFIG_FIND_MATCH_MAX);
    }
    return (ctx.matches > 0) ? 0 : 1;
}

/**
 * `find` — search a text file for a literal substring, or recursively list
 * files by name / size / date.
 *
 * Text search (unchanged): find "text" [file] [/I] [/N] [/C] [/V]
 *   /I  Case-insensitive match
 *   /N  Prefix each match with its line number
 *   /C  Print only the match count
 *   /V  Print the lines that do NOT match
 *
 * File discovery (automatic when a discovery switch is present):
 *   find [path] [/NAME:pattern] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B]
 *   /NAME:pattern  filename wildcard (e.g. *.log, *config*)
 *   /SIZE:spec     >N >=N <N <=N N-M or N (bytes, optional K/M/G suffix)
 *   /NEWER:date    only entries modified on/after YYYY-MM-DD
 *   /OLDER:date    only entries modified on/before YYYY-MM-DD
 *   /DIRS          include directories as well as files
 *   /B             bare: full paths only, no colour (redirectable / pipable)
 *
 * With no file argument the pending `<` or pipe input source is used, so
 * `type notes.txt | find "error"` works the way DOS users expect.
 */
int shell_command_find(int argc, char **argv)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char line[SHELL_TEXT_LINE_BYTES];
    shell_sd_session_t session;
    const char *search = NULL;
    const char *file_arg = NULL;
    bool ignore_case = false;
    bool show_numbers = false;
    bool count_only = false;
    bool invert = false;
    FILE *file;
    esp_err_t error;
    int lineno = 0;
    int matches = 0;
    int index;

    /* The presence of any discovery switch selects file-discovery mode;
     * otherwise `find` keeps its classic text-search behaviour. */
    for (index = 1; index < argc; index++) {
        if (shell_find_is_discovery_token(argv[index])) {
            return shell_find_files(argc, argv);
        }
    }

    for (index = 1; index < argc; index++) {
        if (argv[index][0] == '/' && argv[index][1] != '\0' && argv[index][2] == '\0') {
            switch (toupper((unsigned char)argv[index][1])) {
            case 'I': ignore_case = true;  continue;
            case 'N': show_numbers = true; continue;
            case 'C': count_only = true;   continue;
            case 'V': invert = true;       continue;
            default:
                shell_print_error("find: unknown option %s", argv[index]);
                shell_print_usage("Usage: find <text> [file] [/I] [/N] [/C] [/V]");
                return 2;
            }
        }

        if (search == NULL) {
            search = argv[index];
        } else if (file_arg == NULL) {
            file_arg = argv[index];
        } else {
            shell_print_usage("Usage: find <text> [file] [/I] [/N] [/C] [/V]");
            return 2;
        }
    }

    if (search == NULL) {
        shell_print_usage("Usage: find <text> [file] [/I] [/N] [/C] [/V]");
        return 2;
    }

    error = storage_resolve_input_source(file_arg, resolved, sizeof(resolved));
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_append_text("find: no input file given and no input redirection active\n");
        shell_print_usage("Usage: find <text> [file] [/I] [/N] [/C] [/V]");
        return 2;
    }
    if (error != ESP_OK) {
        shell_print_error("find: invalid path");
        return 2;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("find: SD card not present - insert and retry");
        return 2;
    }

    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_print_error("find: cannot open %s (%s)", resolved, strerror(errno));
        shell_sd_end(&session, "find");
        return 2;
    }

    shell_transcript_appendf("---------- %s\n", resolved);
    while (fgets(line, sizeof(line), file) != NULL) {
        bool hit;

        lineno++;
        shell_text_strip_eol(line);

        hit = ignore_case ? (shell_text_find_ci(line, search) != NULL)
                          : (strstr(line, search) != NULL);
        if (invert) {
            hit = !hit;
        }

        if (!hit) {
            continue;
        }

        matches++;
        if (count_only) {
            continue;
        }

        if (show_numbers) {
            shell_transcript_appendf("[%d] %s\n", lineno, line);
        } else {
            shell_transcript_appendf("%s\n", line);
        }
    }

    fclose(file);
    shell_sd_end(&session, "find");

    if (count_only) {
        shell_transcript_appendf("find: %d line(s)\n", matches);
    } else {
        shell_transcript_appendf("find: %d match(es)\n", matches);
    }

    return (matches > 0) ? 0 : 1;
}

/**
 * `more` — page a text file, waiting for a keypress between pages.
 *
 * At the `-- More --` prompt: Enter or Space advances one page, `Q` quits.
 * When no interactive key source is attached the command falls back to the
 * configured page delay so a headless batch run still completes.
 */
int shell_command_more(int argc, char **argv)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char line[SHELL_TEXT_LINE_BYTES];
    shell_sd_session_t session;
    FILE *file;
    esp_err_t error;
    bool interactive;
    bool quit = false;
    int count = 0;

    if (argc > 2) {
        shell_print_usage("Usage: more [file]");
        return 2;
    }

    error = storage_resolve_input_source(argc >= 2 ? argv[1] : NULL, resolved, sizeof(resolved));
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_append_text("more: no input file given and no input redirection active\n");
        shell_print_usage("Usage: more [file]");
        return 2;
    }
    if (error != ESP_OK) {
        shell_print_error("more: invalid path");
        return 2;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("more: SD card not present - insert and retry");
        return 2;
    }

    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_print_error("more: cannot open %s (%s)", resolved, strerror(errno));
        shell_sd_end(&session, "more");
        return 2;
    }

    interactive = shell_key_input_available();

    while (!quit && fgets(line, sizeof(line), file) != NULL) {
        shell_text_strip_eol(line);
        shell_transcript_appendf("%s\n", line);
        count++;

        if (count % SHELL_MORE_PAGE_LINES != 0) {
            continue;
        }

        if (!interactive) {
            shell_transcript_append_text("-- More --\n");
            vTaskDelay(pdMS_TO_TICKS(SHELL_MORE_PAGE_DELAY_MS));
            continue;
        }

        shell_transcript_append_text("-- More -- (Enter/Space = page, Q = quit)\n");
        shell_key_wait_begin();
        {
            char key = '\0';

            if (!shell_wait_for_key(P4_CONFIG_KEY_WAIT_TIMEOUT_MS, &key)) {
                /* Timed out waiting for the user: stop paging rather than
                 * hold the worker task open indefinitely. */
                shell_transcript_append_text("more: timed out waiting for a key\n");
                quit = true;
            } else if (key == 'q' || key == 'Q') {
                quit = true;
            }
        }
        shell_key_wait_end();
    }

    fclose(file);
    shell_sd_end(&session, "more");

    if (quit) {
        shell_transcript_appendf("more: stopped after %d line(s)\n", count);
    }
    return 0;
}

/**
 * `fc` — compare two text files line by line.
 *
 * Reports the first differing line pair in DOS style and keeps reading so a
 * trailing length difference is also reported. Output is bounded by the
 * transcript itself; the comparison is not.
 */
int shell_command_fc(int argc, char **argv)
{
    char resolved1[SHELL_SD_PATH_BYTES];
    char resolved2[SHELL_SD_PATH_BYTES];
    char line1[SHELL_TEXT_LINE_BYTES];
    char line2[SHELL_TEXT_LINE_BYTES];
    shell_sd_session_t session;
    FILE *file1 = NULL;
    FILE *file2 = NULL;
    esp_err_t error;
    int lineno = 0;
    int diffs = 0;

    if (argc != 3) {
        shell_print_usage("Usage: fc <file1> <file2>");
        return 2;
    }

    error = shell_fs_resolve_path(argv[1], resolved1, sizeof(resolved1));
    if (error != ESP_OK) {
        shell_print_error("fc: invalid path for the first file");
        return 2;
    }

    error = shell_fs_resolve_path(argv[2], resolved2, sizeof(resolved2));
    if (error != ESP_OK) {
        shell_print_error("fc: invalid path for the second file");
        return 2;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("fc: SD card not present - insert and retry");
        return 2;
    }

    file1 = fopen(resolved1, "r");
    file2 = fopen(resolved2, "r");
    if (file1 == NULL || file2 == NULL) {
        shell_print_error("fc: cannot open %s", file1 == NULL ? resolved1 : resolved2);
        if (file1 != NULL) fclose(file1);
        if (file2 != NULL) fclose(file2);
        shell_sd_end(&session, "fc");
        return 2;
    }

    shell_print_heading("Comparing files %s and %s", resolved1, resolved2);

    while (true) {
        char *got1 = fgets(line1, sizeof(line1), file1);
        char *got2 = fgets(line2, sizeof(line2), file2);

        if (got1 == NULL && got2 == NULL) {
            break;
        }

        lineno++;

        if (got1 == NULL) {
            shell_text_strip_eol(line2);
            shell_transcript_appendf("***** line %d only in %s\n", lineno, resolved2);
            shell_transcript_appendf("      %s\n", line2);
            diffs++;
            continue;
        }

        if (got2 == NULL) {
            shell_text_strip_eol(line1);
            shell_transcript_appendf("***** line %d only in %s\n", lineno, resolved1);
            shell_transcript_appendf("      %s\n", line1);
            diffs++;
            continue;
        }

        shell_text_strip_eol(line1);
        shell_text_strip_eol(line2);

        if (strcmp(line1, line2) == 0) {
            continue;
        }

        shell_transcript_appendf("***** line %d\n", lineno);
        shell_transcript_appendf("  %s: %s\n", resolved1, line1);
        shell_transcript_appendf("  %s: %s\n", resolved2, line2);
        diffs++;
    }

    fclose(file1);
    fclose(file2);
    shell_sd_end(&session, "fc");

    if (diffs == 0) {
        shell_print_ok("FC: no differences encountered");
        return 0;
    }
    shell_transcript_appendf("fc: %d difference(s)\n", diffs);
    return 1;
}

/** qsort comparator for ascending case-sensitive line order. */
static int shell_sort_cmp_asc(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;

    return strcmp(*a, *b);
}

/** qsort comparator for descending case-sensitive line order (`/R`). */
static int shell_sort_cmp_desc(const void *left, const void *right)
{
    return -shell_sort_cmp_asc(left, right);
}

/** qsort comparator for ascending case-insensitive line order (`/I`). */
static int shell_sort_cmp_asc_ci(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;

    return strcasecmp(*a, *b);
}

/** qsort comparator for descending case-insensitive line order (`/I /R`). */
static int shell_sort_cmp_desc_ci(const void *left, const void *right)
{
    return -shell_sort_cmp_asc_ci(left, right);
}

/**
 * `sort` — print a text file with its lines sorted.
 *
 * Usage: sort [file] [/R] [/I] [/U]
 *   /R  Reverse (descending) order
 *   /I  Case-insensitive comparison
 *   /U  Drop duplicate adjacent lines after sorting
 *
 * Uses qsort() on an array of heap line copies rather than the previous
 * bubble sort, and frees every successful allocation on every exit path
 * including a mid-read allocation failure. With no file argument the pending
 * `<` or pipe input source is used.
 */
int shell_command_sort(int argc, char **argv)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char buffer[SHELL_TEXT_LINE_BYTES];
    shell_sd_session_t session;
    char **lines = NULL;
    const char *file_arg = NULL;
    bool reverse = false;
    bool ignore_case = false;
    bool unique = false;
    FILE *file;
    esp_err_t error;
    int count = 0;
    int index;
    int printed = 0;

    for (index = 1; index < argc; index++) {
        if (argv[index][0] == '/' && argv[index][1] != '\0' && argv[index][2] == '\0') {
            switch (toupper((unsigned char)argv[index][1])) {
            case 'R': reverse = true;     continue;
            case 'I': ignore_case = true; continue;
            case 'U': unique = true;      continue;
            default:
                shell_print_error("sort: unknown option %s", argv[index]);
                shell_print_usage("Usage: sort [file] [/R] [/I] [/U]");
                return 2;
            }
        }

        if (file_arg == NULL) {
            file_arg = argv[index];
        } else {
            shell_print_usage("Usage: sort [file] [/R] [/I] [/U]");
            return 2;
        }
    }

    error = storage_resolve_input_source(file_arg, resolved, sizeof(resolved));
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_append_text("sort: no input file given and no input redirection active\n");
        shell_print_usage("Usage: sort [file] [/R] [/I] [/U]");
        return 2;
    }
    if (error != ESP_OK) {
        shell_print_error("sort: invalid path");
        return 2;
    }

    lines = calloc(SHELL_SORT_LINE_MAX, sizeof(*lines));
    if (lines == NULL) {
        shell_transcript_append_text("sort: out of memory\n");
        shell_record_errorf("sort", ESP_ERR_NO_MEM, "Out of memory allocating the line table");
        return 1;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        free(lines);
        shell_print_error("sort: SD card not present - insert and retry");
        return 2;
    }

    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_print_error("sort: cannot open %s (%s)", resolved, strerror(errno));
        shell_sd_end(&session, "sort");
        free(lines);
        return 2;
    }

    while (count < SHELL_SORT_LINE_MAX && fgets(buffer, sizeof(buffer), file) != NULL) {
        char *copy;

        shell_text_strip_eol(buffer);
        copy = strdup(buffer);
        if (copy == NULL) {
            /* Report and stop reading, but still sort and print what was
             * collected. Every prior allocation is released below. */
            shell_transcript_append_text("sort: out of memory, output is truncated\n");
            shell_record_errorf("sort", ESP_ERR_NO_MEM, "Out of memory copying a line");
            break;
        }

        lines[count++] = copy;
    }

    if (count == SHELL_SORT_LINE_MAX && fgets(buffer, sizeof(buffer), file) != NULL) {
        shell_transcript_appendf("sort: input truncated at %d lines\n", SHELL_SORT_LINE_MAX);
        shell_record_warningf("sort", "Truncated sort input for %s at %d lines", resolved, SHELL_SORT_LINE_MAX);
    }

    fclose(file);
    shell_sd_end(&session, "sort");

    if (count > 1) {
        int (*comparator)(const void *, const void *);

        if (ignore_case) {
            comparator = reverse ? shell_sort_cmp_desc_ci : shell_sort_cmp_asc_ci;
        } else {
            comparator = reverse ? shell_sort_cmp_desc : shell_sort_cmp_asc;
        }

        qsort(lines, (size_t)count, sizeof(*lines), comparator);
    }

    for (index = 0; index < count; index++) {
        if (unique && index > 0) {
            int same = ignore_case ? strcasecmp(lines[index - 1], lines[index])
                                   : strcmp(lines[index - 1], lines[index]);
            if (same == 0) {
                continue;
            }
        }

        shell_transcript_appendf("%s\n", lines[index]);
        printed++;
    }

    /* Single release point: every successful strdup() is freed exactly once,
     * whether the read finished normally, hit the line cap, or ran out of
     * memory partway through. */
    for (index = 0; index < count; index++) {
        free(lines[index]);
    }
    free(lines);

    if (unique && printed != count) {
        shell_transcript_appendf("sort: %d line(s), %d unique\n", count, printed);
    }
    return 0;
}

/* ========================================================================
 * FINDSTR: classic DOS text search (regex-lite)
 * ========================================================================
 * `findstr` searches files for one or more literal strings or small regular
 * expressions, case-sensitive by default (unlike `find`, which matches
 * case-insensitively and only literally). The regex engine deliberately
 * implements only the DOS findstr subset: `.` (any char), `*` (zero or more
 * of the preceding atom), `^` / `$` anchors, `[class]` / `[^class]` / `[a-z]`,
 * `\<` / `\>` word boundaries, and `\c` escapes.
 *
 * The engine is pure (no transcript or file I/O), so the unit tests can
 * exercise it directly.
 */

/** Is a character a "word" character for the `\<` / `\>` boundaries? */
static bool shell_fsre_is_word(unsigned char ch)
{
    return ch == '_' || isalnum(ch);
}

/** Does the bracket class starting at pat[p] (pat[p] == '[') match ch? */
static bool shell_fsre_class_match(const char *pat, int p, char ch, bool icase)
{
    int i = p + 1;
    bool negate = false;
    bool matched = false;

    if (pat[i] == '^') {
        negate = true;
        i++;
    }
    if (pat[i] == ']') {
        /* A ']' right after '[' or '[^' is a literal member. */
        if (ch == ']') {
            matched = true;
        }
        i++;
    }
    while (pat[i] != '\0' && pat[i] != ']') {
        char lo = pat[i];

        i++;
        if (pat[i] == '-' && pat[i + 1] != '\0' && pat[i + 1] != ']') {
            char hi = pat[i + 1];

            i += 2;
            if (icase) {
                int cl = tolower((unsigned char)ch);
                int ll = tolower((unsigned char)lo);
                int hl = tolower((unsigned char)hi);

                if (cl >= ll && cl <= hl) {
                    matched = true;
                }
            } else if (ch >= lo && ch <= hi) {
                matched = true;
            }
        } else if (icase ? (tolower((unsigned char)ch) == tolower((unsigned char)lo))
                         : (ch == lo)) {
            matched = true;
        }
    }
    return negate ? !matched : matched;
}

/** Does the regex atom at pattern index pi match the single char ch? */
static bool shell_fsre_atom_matches(const char *pat, int pi, char ch, bool icase)
{
    char c = pat[pi];

    if (c == '.') {
        return true;
    }
    if (c == '\\') {
        char e = pat[pi + 1];

        return icase ? (tolower((unsigned char)ch) == tolower((unsigned char)e))
                     : (ch == e);
    }
    if (c == '[') {
        return shell_fsre_class_match(pat, pi, ch, icase);
    }
    return icase ? (tolower((unsigned char)ch) == tolower((unsigned char)c))
                 : (ch == c);
}

/** Pattern width of the atom at pat[pi]: 1 for a literal or '.', 2 for an
 *  escape `\x`, and the full bracket-class width for `[...]`. The matcher
 *  must advance past the whole atom, otherwise a class like `[ab]` or an
 *  escape like `\.` would consume only its first character. */
static int shell_fsre_atom_len(const char *pat, int pi)
{
    if (pat[pi] == '\\' && pat[pi + 1] != '\0') {
        return 2;
    }
    if (pat[pi] == '[') {
        int i = pi + 1;

        if (pat[i] == '^') {
            i++;
        }
        if (pat[i] == ']') {
            i++;
        }
        while (pat[i] != '\0' && pat[i] != ']') {
            i++;
        }
        if (pat[i] == ']') {
            i++;
        }
        return i - pi;
    }
    return 1;
}

/**
 * Recursive backtracking matcher: match pat[pi..] against text starting at
 * ti. Returns the text index just past the match, or -1.
 *
 * @p at_start is true only while ti still points at the beginning of the
 * line, which is what the `^` anchor requires.
 */
static int shell_fsre_match_here(const char *pat, int pi, const char *text, int ti,
                                 int tlen, bool icase, bool at_start)
{
    int n;
    int atom_len;

    if (pat[pi] == '\0') {
        return ti;
    }
    if (pat[pi] == '^' && pi == 0) {
        if (!at_start) {
            return -1;
        }
        return shell_fsre_match_here(pat, pi + 1, text, ti, tlen, icase, at_start);
    }
    if (pat[pi] == '$' && pat[pi + 1] == '\0') {
        return (ti == tlen) ? ti : -1;
    }
    if (pat[pi] == '\\' && pat[pi + 1] == '<') {
        /* Word start: the previous character (if any) is not a word char and
         * the current one is. Zero-width. */
        if (ti >= tlen || (ti > 0 && shell_fsre_is_word((unsigned char)text[ti - 1])) ||
            !shell_fsre_is_word((unsigned char)text[ti])) {
            return -1;
        }
        return shell_fsre_match_here(pat, pi + 2, text, ti, tlen, icase, at_start);
    }
    if (pat[pi] == '\\' && pat[pi + 1] == '>') {
        /* Word end: the previous character is a word char and the current one
         * (if any) is not. Zero-width. */
        if (ti == 0 || !shell_fsre_is_word((unsigned char)text[ti - 1]) ||
            (ti < tlen && shell_fsre_is_word((unsigned char)text[ti]))) {
            return -1;
        }
        return shell_fsre_match_here(pat, pi + 2, text, ti, tlen, icase, at_start);
    }

    /* Starred atom: pat[pi .. pi+atom_len-1] followed by '*'. Greedy with
     * backtracking. The quantifier applies to a complete atom, so an escaped
     * `\*` (the star sits inside the 2-char escape) is a literal star, not a
     * quantifier. */
    atom_len = shell_fsre_atom_len(pat, pi);
    if (pat[pi + atom_len] == '*') {
        n = 0;
        while (ti + n <= tlen) {
            int r = shell_fsre_match_here(pat, pi + atom_len + 1, text, ti + n, tlen,
                                          icase, at_start && (ti + n == 0));

            if (r >= 0) {
                return r;
            }
            if (ti + n == tlen || !shell_fsre_atom_matches(pat, pi, text[ti + n], icase)) {
                break;
            }
            n++;
        }
        return -1;
    }

    if (ti >= tlen || !shell_fsre_atom_matches(pat, pi, text[ti], icase)) {
        return -1;
    }
    /* Classes and escapes span several pattern characters; advance by the
     * atom's full width so a `[ab]c` match consumes the whole class. */
    return shell_fsre_match_here(pat, pi + atom_len, text, ti + 1, tlen, icase, false);
}

/**
 * Search for the regex pattern anywhere in text (unless anchored with `^`).
 * Returns the match start index, or -1; @p end_out receives the index just
 * past the match. Exposed (non-static) so the unit tests can drive it.
 */
int shell_fsre_search(const char *pattern, const char *text, bool icase,
                      int *end_out)
{
    int tlen;
    bool anchor_beg;
    int ti;

    if (pattern == NULL || text == NULL || end_out == NULL) {
        return -1;
    }
    tlen = (int)strlen(text);
    anchor_beg = (pattern[0] == '^');

    for (ti = 0; ti <= tlen; ti++) {
        int end;

        if (anchor_beg && ti != 0) {
            break;
        }
        end = shell_fsre_match_here(pattern, 0, text, ti, tlen, icase, ti == 0);
        if (end >= 0) {
            *end_out = end;
            return ti;
        }
    }
    return -1;
}

/**
 * Match one line against one search string, honouring the /X /E /B switches.
 * @p is_regex selects the regex engine; otherwise the string is literal.
 * Exposed (non-static) so the unit tests can drive it.
 */
bool shell_findstr_match_line(const char *pattern, bool is_regex,
                              const char *line, bool icase,
                              bool beg, bool end, bool whole)
{
    int line_len;

    if (pattern == NULL || line == NULL) {
        return false;
    }
    line_len = (int)strlen(line);

    if (is_regex) {
        int start;
        int match_end;

        start = shell_fsre_search(pattern, line, icase, &match_end);
        if (start < 0) {
            return false;
        }
        if (whole) {
            return start == 0 && match_end == line_len;
        }
        if (beg && start != 0) {
            return false;
        }
        if (end && match_end != line_len) {
            return false;
        }
        return true;
    }

    if (whole) {
        return icase ? (strcasecmp(line, pattern) == 0)
                     : (strcmp(line, pattern) == 0);
    }
    if (beg) {
        return icase ? (strncasecmp(line, pattern, strlen(pattern)) == 0)
                     : (strncmp(line, pattern, strlen(pattern)) == 0);
    }
    if (end) {
        size_t plen = strlen(pattern);
        size_t llen = strlen(line);

        if (plen > llen) {
            return false;
        }
        return icase ? (strncasecmp(line + llen - plen, pattern, plen) == 0)
                     : (strcmp(line + llen - plen, pattern) == 0);
    }
    return icase ? (shell_text_find_ci(line, pattern) != NULL)
                 : (strstr(line, pattern) != NULL);
}

/** Parsed `findstr` options and collected search strings. */
typedef struct {
    bool use_regex;
    bool icase;
    bool numbers;
    bool invert;
    bool whole;
    bool beg;
    bool end;
    bool recursive;
    bool files_only;
    const char *filelist_arg;
    const char *strings_arg;
    char strings[P4_CONFIG_FINDSTR_MAX_STRINGS][P4_CONFIG_FINDSTR_PATTERN_BYTES];
    bool regex_strings[P4_CONFIG_FINDSTR_MAX_STRINGS];
    int nstrings;
} findstr_opts_t;

/** Add one search string; /C: strings are literal regardless of /R. */
static bool shell_findstr_add_string(findstr_opts_t *opts, const char *text, bool is_regex)
{
    if (opts->nstrings >= P4_CONFIG_FINDSTR_MAX_STRINGS) {
        shell_print_error("findstr: too many search strings (max %d)",
                          P4_CONFIG_FINDSTR_MAX_STRINGS);
        return false;
    }
    snprintf(opts->strings[opts->nstrings], sizeof(opts->strings[0]), "%s", text);
    opts->regex_strings[opts->nstrings] = is_regex;
    opts->nstrings++;
    return true;
}

/* ------------------------------------------------------------------------
 * SHARED SEARCH SCAN CORE (storage_scan_file_lines + storage_walk_files)
 * ------------------------------------------------------------------------
 * The ONE line-reading loop and the ONE recursive search walker. `findstr`
 * (below) and `gfind /files` both build on these, so no search path grows a
 * second file loop or a second tree walk. See ai-context.md "no duplication".
 */

esp_err_t storage_scan_file_lines(const char *resolved_path,
                                  storage_line_cb_t cb, void *ctx)
{
    char line[SHELL_TEXT_LINE_BYTES];
    FILE *file;
    int lineno = 0;

    if (resolved_path == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    file = fopen(resolved_path, "r");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        lineno++;
        shell_text_strip_eol(line);
        if (!cb(resolved_path, lineno, line, ctx)) {
            break;   /* consumer asked to stop (e.g. match cap reached) */
        }
    }
    fclose(file);
    return ESP_OK;
}

static void storage_walk_dir(const char *vfs_dir, int depth,
                             storage_walk_skip_cb_t skip,
                             storage_file_cb_t on_file, void *ctx)
{
    struct walk_level_scratch {
        char fatfs_path[SHELL_SD_PATH_BYTES];
        char child[SHELL_SD_PATH_BYTES];
        FF_DIR dir;
        FILINFO info;
    } *scratch = NULL;
    FRESULT result;

    if (depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX) {
        return;
    }
    /* The per-level scratch (FATFS handle, FILINFO, path buffers) never needs
     * to be DMA-capable — FATFS reads through its own DMA bounce buffer — so
     * it prefers PSRAM. This keeps the scarce internal heap unfragmented for
     * callers that DO need internal DMA (e.g. the esp-aes path in `crypt`;
     * see bugs.md F23). NULL propagates to the error path below. */
    scratch = p4heap_calloc_psram(1, sizeof(*scratch));
    if (scratch == NULL) {
        shell_record_errorf("storage", ESP_ERR_NO_MEM, "Out of memory during a file walk");
        return;
    }
    if (shell_sd_vfs_to_fatfs_path(vfs_dir, scratch->fatfs_path,
                                   sizeof(scratch->fatfs_path)) != ESP_OK) {
        heap_caps_free(scratch);
        return;
    }
    result = f_opendir(&scratch->dir, scratch->fatfs_path);
    if (result != FR_OK) {
        heap_caps_free(scratch);
        return;
    }

    while (true) {
        bool is_dir;

        result = f_readdir(&scratch->dir, &scratch->info);
        if (result != FR_OK || scratch->info.fname[0] == '\0') {
            break;
        }
        if (strcmp(scratch->info.fname, ".") == 0 ||
            strcmp(scratch->info.fname, "..") == 0) {
            continue;
        }
        if (skip != NULL && skip(vfs_dir, scratch->info.fname,
                                 (scratch->info.fattrib & AM_DIR) != 0, ctx)) {
            continue;
        }
        if (snprintf(scratch->child, sizeof(scratch->child), "%s/%s",
                     vfs_dir, scratch->info.fname) < 0) {
            continue;
        }
        is_dir = (scratch->info.fattrib & AM_DIR) != 0;
        if (is_dir) {
            storage_walk_dir(scratch->child, depth + 1, skip, on_file, ctx);
        } else {
            on_file(scratch->child, ctx);
        }
    }

    (void)f_closedir(&scratch->dir);
    heap_caps_free(scratch);
}

esp_err_t storage_walk_files(const char *vfs_root, storage_walk_skip_cb_t skip,
                             storage_file_cb_t on_file, void *ctx)
{
    struct stat st;

    if (vfs_root == NULL || on_file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(vfs_root, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (S_ISDIR(st.st_mode)) {
        storage_walk_dir(vfs_root, 0, skip, on_file, ctx);
    } else {
        on_file(vfs_root, ctx);
    }
    return ESP_OK;
}

/** Context for one `findstr` file scan. */
typedef struct {
    const findstr_opts_t *opts;
    bool prefix_file;
    int *total_matches;
    int matched_lines;
} findstr_file_ctx_t;

/** Per-line matching + printing for `findstr` (over storage_scan_file_lines). */
static bool shell_findstr_line_cb(const char *path, int lineno, const char *line,
                                  void *ctx)
{
    findstr_file_ctx_t *c = (findstr_file_ctx_t *)ctx;
    bool hit = false;
    int index;

    if (*c->total_matches >= P4_CONFIG_FINDSTR_MATCH_MAX) {
        return false;
    }
    for (index = 0; index < c->opts->nstrings; index++) {
        if (shell_findstr_match_line(c->opts->strings[index],
                                     c->opts->regex_strings[index],
                                     line, c->opts->icase, c->opts->beg,
                                     c->opts->end, c->opts->whole)) {
            hit = true;
            break;
        }
    }
    if (c->opts->invert) {
        hit = !hit;
    }
    if (!hit) {
        return true;
    }

    c->matched_lines++;
    (*c->total_matches)++;

    if (c->opts->files_only) {
        return true;   /* the filename is printed once at the end */
    }
    if (c->prefix_file) {
        shell_transcript_appendf("%s:", path);
    }
    if (c->opts->numbers) {
        shell_transcript_appendf("%d:", lineno);
    }
    shell_transcript_appendf("%s\n", line);
    return true;
}

/**
 * Search one file, printing matching lines. When @p prefix_file is set (more
 * than one source) each line is prefixed with the filename. Stops once the
 * global match cap is reached.
 */
static void shell_findstr_search_file(const char *path, const findstr_opts_t *opts,
                                      bool prefix_file, int *total_matches)
{
    findstr_file_ctx_t ctx = { opts, prefix_file, total_matches, 0 };

    if (storage_scan_file_lines(path, shell_findstr_line_cb, &ctx) != ESP_OK) {
        shell_print_error("findstr: cannot open %s (%s)", path, strerror(errno));
        return;
    }
    if (opts->files_only && ctx.matched_lines > 0) {
        shell_transcript_appendf("%s\n", path);
    }
}

/** `findstr /S` per-file visitor (over storage_walk_files). */
static void shell_findstr_visit_file(const char *path, void *ctx)
{
    findstr_file_ctx_t *fctx = (findstr_file_ctx_t *)ctx;

    if (*fctx->total_matches < P4_CONFIG_FINDSTR_MATCH_MAX) {
        shell_findstr_search_file(path, fctx->opts, true, fctx->total_matches);
    }
}

/**
 * `findstr` command entry point.
 *
 * Usage: findstr [/R] [/C:"string"] [/I] [/N] [/V] [/X] [/E] [/B] [/L]
 *        [/S] [/M] [/F:file] [/G:file] <search> [file...]
 * Reads the pending `<` or pipe source when no file is given.
 * Returns an ERRORLEVEL: 0 match found, 1 no match, 2 usage.
 */
int shell_command_findstr(int argc, char **argv)
{
    findstr_opts_t opts;
    shell_sd_session_t session;
    const char *files[P4_CONFIG_SD_LIST_LIMIT];
    const char *bare_string = NULL;
    int nfiles = 0;
    bool have_filelist = false;   /* /F: entries are strdup'd and owned */
    /* True once /C: or /G: has supplied the search string(s). Then the first
     * bare token is a FILE, not the search string (DOS findstr semantics). */
    bool option_search = false;
    int total_matches = 0;
    int index;
    int rc = 1;
    esp_err_t error;

    memset(&opts, 0, sizeof(opts));

    for (index = 1; index < argc; index++) {
        const char *token = argv[index];

        if (token[0] == '/') {
            if (strncasecmp(token, "/C:", 3) == 0) {
                option_search = true;
                if (!shell_findstr_add_string(&opts, token + 3, false)) {
                    return 2;
                }
                continue;
            }
            if (strncasecmp(token, "/F:", 3) == 0) {
                opts.filelist_arg = token + 3;
                continue;
            }
            if (strncasecmp(token, "/G:", 3) == 0) {
                option_search = true;
                opts.strings_arg = token + 3;
                continue;
            }
            if (token[1] != '\0' && token[2] == '\0') {
                switch (toupper((unsigned char)token[1])) {
                case 'R': opts.use_regex = true;  continue;
                case 'L': opts.use_regex = false; continue;
                case 'S': opts.recursive = true;  continue;
                case 'I': opts.icase = true;      continue;
                case 'N': opts.numbers = true;    continue;
                case 'V': opts.invert = true;     continue;
                case 'X': opts.whole = true;      continue;
                case 'E': opts.end = true;        continue;
                case 'B': opts.beg = true;        continue;
                case 'M': opts.files_only = true; continue;
                default:
                    shell_print_error("findstr: unknown option %s", token);
                    shell_print_usage("Usage: findstr [/R] [/C:string] [/I] [/N] [/V] [/X] [/E] [/B] [/L] [/S] [/M] [/F:file] [/G:file] <search> [file...]");
                    return 2;
                }
            }
            shell_print_error("findstr: unknown option %s", token);
            shell_print_usage("Usage: findstr [/R] [/C:string] [/I] [/N] [/V] [/X] [/E] [/B] [/L] [/S] [/M] [/F:file] [/G:file] <search> [file...]");
            return 2;
        }

        /* The first bare token is the single search string; any further bare
         * tokens are files, matching DOS findstr. When /C: or /G: already
         * supplied the search, EVERY bare token is a file. */
        if (!option_search && bare_string == NULL) {
            bare_string = token;
        } else if (nfiles < (int)(sizeof(files) / sizeof(files[0]))) {
            files[nfiles++] = token;
        } else {
            shell_print_error("findstr: too many files (max %d)",
                              (int)(sizeof(files) / sizeof(files[0])));
            return 2;
        }
    }

    if (bare_string != NULL) {
        if (!shell_findstr_add_string(&opts, bare_string, opts.use_regex)) {
            return 2;
        }
    }

    /* /G:file supplies additional search strings, one per line. */
    if (opts.strings_arg != NULL) {
        char resolved[SHELL_SD_PATH_BYTES];
        char line[SHELL_TEXT_LINE_BYTES];
        FILE *file;
        shell_sd_session_t g_session;

        if (shell_fs_resolve_path(opts.strings_arg, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("findstr: invalid /G file path");
            return 2;
        }
        if (shell_sd_begin(&g_session) != ESP_OK) {
            shell_print_error("findstr: SD card not present - insert and retry");
            return 1;
        }
        file = fopen(resolved, "r");
        if (file == NULL) {
            shell_print_error("findstr: cannot open /G file %s", resolved);
            shell_sd_end(&g_session, "findstr");
            return 1;
        }
        while (fgets(line, sizeof(line), file) != NULL) {
            shell_text_strip_eol(line);
            if (line[0] == '\0') {
                continue;
            }
            if (!shell_findstr_add_string(&opts, line, opts.use_regex)) {
                fclose(file);
                shell_sd_end(&g_session, "findstr");
                return 2;
            }
        }
        fclose(file);
        shell_sd_end(&g_session, "findstr");
    }

    if (opts.nstrings == 0) {
        shell_print_usage("Usage: findstr [/R] [/C:string] [/I] [/N] [/V] [/X] [/E] [/B] [/L] [/S] [/M] [/F:file] [/G:file] <search> [file...]");
        return 2;
    }

    /* /F:file supplies the file list, one path per line. */
    if (opts.filelist_arg != NULL) {
        char resolved[SHELL_SD_PATH_BYTES];
        char line[SHELL_SD_PATH_BYTES];
        FILE *file;
        shell_sd_session_t f_session;

        if (shell_fs_resolve_path(opts.filelist_arg, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("findstr: invalid /F file path");
            return 2;
        }
        if (shell_sd_begin(&f_session) != ESP_OK) {
            shell_print_error("findstr: SD card not present - insert and retry");
            return 1;
        }
        file = fopen(resolved, "r");
        if (file == NULL) {
            shell_print_error("findstr: cannot open /F file %s", resolved);
            shell_sd_end(&f_session, "findstr");
            return 1;
        }
        have_filelist = true;
        while (fgets(line, sizeof(line), file) != NULL) {
            char *copy;

            shell_text_strip_eol(line);
            if (line[0] == '\0') {
                continue;
            }
            if (nfiles >= (int)(sizeof(files) / sizeof(files[0]))) {
                break;
            }
            copy = strdup(line);
            if (copy == NULL) {
                break;
            }
            files[nfiles++] = copy;
        }
        fclose(file);
        shell_sd_end(&f_session, "findstr");
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("findstr: SD card not present - insert and retry");
        if (have_filelist) {
            for (index = 0; index < nfiles; index++) {
                free((void *)files[index]);
            }
        }
        return 1;
    }

    if (nfiles == 0) {
        char resolved[SHELL_SD_PATH_BYTES];

        /* Read the pending `<` or pipe source. */
        error = storage_resolve_input_source(NULL, resolved, sizeof(resolved));
        if (error == ESP_ERR_NOT_FOUND) {
            shell_transcript_append_text("findstr: no input file given and no input redirection active\n");
            shell_sd_end(&session, "findstr");
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("findstr: invalid path");
            shell_sd_end(&session, "findstr");
            return 1;
        }
        shell_findstr_search_file(resolved, &opts, false, &total_matches);
    } else if (opts.recursive) {
        for (index = 0; index < nfiles; index++) {
            char resolved[SHELL_SD_PATH_BYTES];
            struct stat st;

            if (shell_sd_resolve_path(files[index], resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("findstr: invalid path %s", files[index]);
                continue;
            }
            if (stat(resolved, &st) != 0) {
                shell_print_error("findstr: path not found %s", files[index]);
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                findstr_file_ctx_t fctx = { &opts, true, &total_matches, 0 };

                (void)storage_walk_files(resolved, NULL, shell_findstr_visit_file, &fctx);
            } else {
                shell_findstr_search_file(resolved, &opts, nfiles > 1, &total_matches);
            }
        }
    } else {
        bool prefix_file = (nfiles > 1);

        for (index = 0; index < nfiles; index++) {
            char resolved[SHELL_SD_PATH_BYTES];
            struct stat st;

            if (shell_sd_resolve_path(files[index], resolved, sizeof(resolved)) != ESP_OK) {
                shell_print_error("findstr: invalid path %s", files[index]);
                continue;
            }
            if (stat(resolved, &st) != 0) {
                shell_print_error("findstr: path not found %s", files[index]);
                continue;
            }
            if (S_ISDIR(st.st_mode) && !opts.recursive) {
                shell_print_error("findstr: %s is a directory (use /S)", files[index]);
                continue;
            }
            shell_findstr_search_file(resolved, &opts, prefix_file, &total_matches);
        }
    }

    shell_sd_end(&session, "findstr");

    if (have_filelist) {
        for (index = 0; index < nfiles; index++) {
            free((void *)files[index]);
        }
    }

    if (total_matches >= P4_CONFIG_FINDSTR_MATCH_MAX) {
        shell_transcript_appendf("findstr: output truncated at %d match(es)\n",
                                 P4_CONFIG_FINDSTR_MATCH_MAX);
    }
    if (total_matches > 0) {
        rc = 0;
    }
    return rc;
}

/* ========================================================================
 * COMP: classic DOS byte-for-byte file comparison
 * ======================================================================== */

/** Parsed `comp` options. */
typedef struct {
    bool decimal;      /* /D */
    bool ascii;        /* /A */
    bool lines;        /* /L */
    bool icase;        /* /C */
    bool have_n;       /* /N=number */
    int n_lines;
} comp_opts_t;

/**
 * Find the first differing byte between two buffers. Returns true and fills
 * @p pos (0-based), @p va, @p vb when a difference exists, including a length
 * difference (the missing byte reads as 0x00). Case-insensitive when
 * @p icase. Exposed (non-static) so the unit tests can drive it.
 */
bool shell_comp_first_diff(const uint8_t *a, size_t an,
                           const uint8_t *b, size_t bn,
                           bool icase, size_t *pos, uint8_t *va, uint8_t *vb)
{
    size_t i;
    size_t n;

    if (pos == NULL || va == NULL || vb == NULL ||
        (a == NULL && an > 0) || (b == NULL && bn > 0)) {
        return false;
    }
    n = (an < bn) ? an : bn;

    for (i = 0; i < n; i++) {
        unsigned char ca = a[i];
        unsigned char cb = b[i];

        if (icase) {
            ca = (unsigned char)tolower(ca);
            cb = (unsigned char)tolower(cb);
        }
        if (ca != cb) {
            *pos = i;
            *va = a[i];
            *vb = b[i];
            return true;
        }
    }
    if (an != bn) {
        *pos = n;
        *va = (n < an) ? a[n] : 0;
        *vb = (n < bn) ? b[n] : 0;
        return true;
    }
    return false;
}

/** Print one `comp` mismatch line honouring /D /A /L. */
static void shell_comp_report(const comp_opts_t *opts, size_t offset, int line,
                              uint8_t va, uint8_t vb)
{
    if (opts->lines) {
        shell_transcript_appendf("Compare error at LINE %d\n", line);
    } else if (opts->decimal) {
        shell_transcript_appendf("Compare error at OFFSET %u\n", (unsigned)offset);
    } else {
        shell_transcript_appendf("Compare error at OFFSET %X\n", (unsigned)offset);
    }
    if (opts->ascii) {
        shell_transcript_appendf("file1 = %c\n", isprint(va) ? (int)va : '.');
        shell_transcript_appendf("file2 = %c\n", isprint(vb) ? (int)vb : '.');
    } else {
        shell_transcript_appendf("file1 = %02X\n", (unsigned)va);
        shell_transcript_appendf("file2 = %02X\n", (unsigned)vb);
    }
}

/**
 * Compare two open files line by line (used by /L and /N=number), reporting
 * up to P4_CONFIG_COMP_MISMATCH_MAX mismatches. Returns the mismatch count.
 */
static int shell_comp_compare_lines(FILE *file1, FILE *file2, const comp_opts_t *opts)
{
    char line1[SHELL_TEXT_LINE_BYTES];
    char line2[SHELL_TEXT_LINE_BYTES];
    int line = 0;
    int mismatches = 0;

    while (mismatches < P4_CONFIG_COMP_MISMATCH_MAX &&
           (opts->have_n ? line < opts->n_lines : true)) {
        char *got1 = fgets(line1, sizeof(line1), file1);
        char *got2 = fgets(line2, sizeof(line2), file2);
        size_t len1;
        size_t len2;
        size_t pos;
        uint8_t va;
        uint8_t vb;

        line++;
        if (got1 == NULL && got2 == NULL) {
            break;
        }
        len1 = got1 != NULL ? strlen(line1) : 0;
        len2 = got2 != NULL ? strlen(line2) : 0;

        if (shell_comp_first_diff((const uint8_t *)line1, len1,
                                  (const uint8_t *)line2, len2,
                                  opts->icase, &pos, &va, &vb)) {
            shell_comp_report(opts, opts->lines ? 0 : pos, line, va, vb);
            mismatches++;
            if (got1 == NULL || got2 == NULL) {
                break;
            }
        }
    }
    return mismatches;
}

/**
 * Compare two open files byte by byte (the default mode), reporting up to
 * P4_CONFIG_COMP_MISMATCH_MAX mismatches. Returns the mismatch count.
 */
static int shell_comp_compare_bytes(FILE *file1, FILE *file2, const comp_opts_t *opts)
{
    uint8_t buf1[SHELL_SD_IO_BUFFER_BYTES];
    uint8_t buf2[SHELL_SD_IO_BUFFER_BYTES];
    size_t base = 0;
    int mismatches = 0;

    while (mismatches < P4_CONFIG_COMP_MISMATCH_MAX) {
        size_t n1 = fread(buf1, 1, sizeof(buf1), file1);
        size_t n2 = fread(buf2, 1, sizeof(buf2), file2);
        size_t pos;
        uint8_t va;
        uint8_t vb;

        if (n1 == 0 && n2 == 0) {
            break;
        }
        if (shell_comp_first_diff(buf1, n1, buf2, n2, opts->icase, &pos, &va, &vb)) {
            /* Report at the absolute offset, then resume just past the byte. */
            shell_comp_report(opts, base + pos, 0, va, vb);
            mismatches++;

            if (mismatches >= P4_CONFIG_COMP_MISMATCH_MAX) {
                break;
            }
            if (fseek(file1, (long)(base + pos + 1), SEEK_SET) != 0 ||
                fseek(file2, (long)(base + pos + 1), SEEK_SET) != 0) {
                break;
            }
            base = base + pos + 1;
            continue;
        }
        if (n1 != n2) {
            /* Lengths differ: report the extra length and stop. */
            shell_comp_report(opts, base + (n1 < n2 ? n1 : n2), 0, 0, 0);
            mismatches++;
            break;
        }
        if (n1 == 0) {
            break;
        }
        base += n1;
    }
    return mismatches;
}

/**
 * `comp` command entry point.
 *
 * Usage: comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]
 *   /D  decimal offsets      /A  ASCII display
 *   /L  line numbers         /N=n  compare only the first n lines
 *   /C  case-insensitive
 * Returns an ERRORLEVEL: 0 identical, 1 different, 2 usage.
 */
int shell_command_comp(int argc, char **argv)
{
    comp_opts_t opts;
    shell_sd_session_t session;
    char resolved1[SHELL_SD_PATH_BYTES];
    char resolved2[SHELL_SD_PATH_BYTES];
    FILE *file1 = NULL;
    FILE *file2 = NULL;
    esp_err_t error;
    const char *arg1 = NULL;
    const char *arg2 = NULL;
    int mismatches;
    int index;

    memset(&opts, 0, sizeof(opts));

    for (index = 1; index < argc; index++) {
        const char *token = argv[index];

        if (token[0] == '/') {
            if (strncasecmp(token, "/N=", 3) == 0) {
                char *end = NULL;
                long parsed;

                parsed = strtol(token + 3, &end, 10);
                if (end == token + 3 || *end != '\0' || parsed < 1) {
                    shell_print_error("comp: invalid /N value %s (use /N=number)", token + 3);
                    return 2;
                }
                opts.have_n = true;
                opts.n_lines = (int)parsed;
                continue;
            }
            if (token[1] != '\0' && token[2] == '\0') {
                switch (toupper((unsigned char)token[1])) {
                case 'D': opts.decimal = true; continue;
                case 'A': opts.ascii = true;   continue;
                case 'L': opts.lines = true;   continue;
                case 'C': opts.icase = true;   continue;
                default:
                    shell_print_error("comp: unknown option %s", token);
                    shell_print_usage("Usage: comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]");
                    return 2;
                }
            }
            shell_print_error("comp: unknown option %s", token);
            shell_print_usage("Usage: comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]");
            return 2;
        }

        if (arg1 == NULL) {
            arg1 = token;
        } else if (arg2 == NULL) {
            arg2 = token;
        } else {
            shell_print_usage("Usage: comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]");
            return 2;
        }
    }

    if (arg1 == NULL || arg2 == NULL) {
        shell_print_usage("Usage: comp <file1> <file2> [/D] [/A] [/L] [/N=number] [/C]");
        return 2;
    }

    error = shell_fs_resolve_path(arg1, resolved1, sizeof(resolved1));
    if (error != ESP_OK) {
        shell_print_error("comp: invalid path for the first file");
        return 2;
    }
    error = shell_fs_resolve_path(arg2, resolved2, sizeof(resolved2));
    if (error != ESP_OK) {
        shell_print_error("comp: invalid path for the second file");
        return 2;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("comp: SD card not present - insert and retry");
        return 1;
    }

    file1 = fopen(resolved1, "rb");
    file2 = fopen(resolved2, "rb");
    if (file1 == NULL || file2 == NULL) {
        shell_print_error("comp: cannot open %s", file1 == NULL ? resolved1 : resolved2);
        if (file1 != NULL) {
            fclose(file1);
        }
        if (file2 != NULL) {
            fclose(file2);
        }
        shell_sd_end(&session, "comp");
        return 1;
    }

    shell_print_heading("Comparing %s and %s", resolved1, resolved2);

    if (opts.lines || opts.have_n) {
        mismatches = shell_comp_compare_lines(file1, file2, &opts);
    } else {
        mismatches = shell_comp_compare_bytes(file1, file2, &opts);
    }

    fclose(file1);
    fclose(file2);
    shell_sd_end(&session, "comp");

    if (mismatches == 0) {
        shell_print_ok("Files compare OK");
        return 0;
    }
    shell_transcript_appendf("%d mismatch%s - ending compare\n", mismatches,
                             mismatches == 1 ? "" : "es");
    return 1;
}

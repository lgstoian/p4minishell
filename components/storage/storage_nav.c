/**
 * @file storage_nav.c
 * @brief Navigation and listing verbs (cd/dir/tree).
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
 * NAVIGATION AND LISTING
 * ======================================================================== */

void shell_command_cd(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    struct stat path_stat;
    shell_sd_session_t session;
    esp_err_t error;

    if (argc == 1) {
        shell_fs_print_cwd();
        return;
    }

    if (argc != 2) {
        shell_print_usage("Usage: cd <path>");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("cd: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("cd: SD card not present - insert and retry");
        return;
    }

    error = shell_sd_stat_path(resolved_path, &path_stat);
    if (error != ESP_OK || !S_ISDIR(path_stat.st_mode)) {
        shell_transcript_appendf("cd: path not found %s\n", argv[1]);
        shell_sd_end(&session, "cd");
        return;
    }

    shell_sd_end(&session, "cd");

    storage_set_cwd(resolved_path);
    shell_fs_print_cwd();
}

/* ========================================================================
 * DIRECTORY LISTING (dir)
 * ========================================================================
 * Supports the DOS option set: /W wide, /P paged, /S recursive, /B bare,
 * /L lowercase, /A attribute filtering, and /O sort ordering.
 *
 * Each directory level is buffered so entries can be sorted before printing,
 * which is what /O requires. The buffer is heap-allocated and bounded by
 * P4_CONFIG_DIR_SORT_ENTRY_MAX, and the recursion for /S is bounded by
 * P4_CONFIG_DIR_RECURSE_DEPTH_MAX, so neither a wide nor a deep card can
 * exhaust memory or the worker task stack.
 */

/** Sort key selected by `/O`. */
typedef enum {
    DIR_SORT_NONE = 0,   /**< Directory order as FATFS returns it. */
    DIR_SORT_NAME,       /**< /O:N */
    DIR_SORT_SIZE,       /**< /O:S */
    DIR_SORT_EXTENSION,  /**< /O:E */
    DIR_SORT_DATE,       /**< /O:D */
} dir_sort_key_t;

/** One buffered directory entry. */
typedef struct {
    char name[SHELL_LFN_BYTES];
    char altname[16];
    uint64_t size;
    uint16_t fdate;
    uint16_t ftime;
    uint8_t attrib;
    bool is_dir;
} dir_entry_t;

/** Parsed `dir` options plus the running totals for the whole invocation. */
typedef struct {
    bool wide;
    bool paged;
    bool recursive;
    bool bare;
    bool lowercase;
    /* Attribute filter: require_mask entries must be present, exclude_mask
     * entries must be absent. Zero means "no constraint". */
    uint8_t require_mask;
    uint8_t exclude_mask;
    bool have_attr_filter;
    /* Bare `/A` (show everything, including hidden and system). Without any
     * `/A`, hidden and system entries are suppressed like DOS does. */
    bool show_all;
    dir_sort_key_t sort_key;
    bool sort_reverse;
    bool group_dirs_first;
    /* Running totals across every directory visited. */
    unsigned int total_files;
    unsigned int total_dirs;
    uint64_t total_bytes;
    unsigned int lines_since_pause;
    bool aborted;
    bool interactive;
} dir_ctx_t;

/**
 * Print one line, honoring `/P` paging.
 *
 * Returns false when the user asked to stop, which unwinds the whole listing
 * including any pending recursion.
 */
static bool shell_dir_emit(dir_ctx_t *ctx, const char *text)
{
    /* Routed through the ANSI path so the palette macros embedded in the
     * listing rows are interpreted for the transcript and passed through to
     * the serial console. A row with no specifiers is unaffected. */
    shell_transcript_append_ansi(text);

    if (!ctx->paged || ctx->aborted) {
        return !ctx->aborted;
    }

    ctx->lines_since_pause++;
    if (ctx->lines_since_pause < P4_CONFIG_DIR_PAGE_LINES) {
        return true;
    }

    ctx->lines_since_pause = 0;

    if (!ctx->interactive) {
        /* No key source attached: keep going rather than stalling. */
        return true;
    }

    shell_transcript_appendf_ansi(SH_MUTE "-- More -- (Enter/Space = page, Q = quit)" SH_RST "\n");
    shell_key_wait_begin();
    {
        char key = '\0';

        if (!shell_wait_for_key(P4_CONFIG_KEY_WAIT_TIMEOUT_MS, &key)) {
            shell_print_warning("dir: timed out waiting for a key");
            ctx->aborted = true;
        } else if (key == 'q' || key == 'Q') {
            ctx->aborted = true;
        }
    }
    shell_key_wait_end();

    return !ctx->aborted;
}

/** printf-style wrapper around shell_dir_emit(). */
static bool shell_dir_emitf(dir_ctx_t *ctx, const char *format, ...)
{
    char buffer[SHELL_SD_PATH_BYTES + SHELL_LFN_BYTES];
    va_list args;

    va_start(args, format);
    /* Route through ansi_vformat so SH_* palette macros (e.g. @K, @M, @w) in
     * the format string are converted to real SGR escapes — plain vsnprintf
     * would leave the @-specifiers literal on the transcript and UART. */
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);

    return shell_dir_emit(ctx, buffer);
}

/** Lowercase a name in place for `/L`. */
static void shell_dir_apply_case(const dir_ctx_t *ctx, char *name)
{
    if (!ctx->lowercase || name == NULL) {
        return;
    }

    for (; *name != '\0'; name++) {
        *name = (char)tolower((unsigned char)*name);
    }
}

/** Return the extension of a filename, or "" when it has none. */
static const char *shell_dir_extension(const char *name)
{
    const char *dot;

    if (name == NULL) {
        return "";
    }

    dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return "";
    }

    return dot + 1;
}

/* Comparator state. qsort() takes no user pointer, and this runs on a single
 * worker task, so a file-scope pointer is the straightforward way to share
 * the active sort settings with the comparator. */
static const dir_ctx_t *s_dir_sort_ctx;

static int shell_dir_compare(const void *left, const void *right)
{
    const dir_entry_t *a = (const dir_entry_t *)left;
    const dir_entry_t *b = (const dir_entry_t *)right;
    const dir_ctx_t *ctx = s_dir_sort_ctx;
    int result = 0;

    if (ctx == NULL) {
        return 0;
    }

    /* /O:G groups directories ahead of files before any other key applies. */
    if (ctx->group_dirs_first && a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }

    switch (ctx->sort_key) {
    case DIR_SORT_SIZE:
        if (a->size < b->size) result = -1;
        else if (a->size > b->size) result = 1;
        break;
    case DIR_SORT_EXTENSION:
        result = strcasecmp(shell_dir_extension(a->name), shell_dir_extension(b->name));
        break;
    case DIR_SORT_DATE:
        if (a->fdate != b->fdate) {
            result = (a->fdate < b->fdate) ? -1 : 1;
        } else if (a->ftime != b->ftime) {
            result = (a->ftime < b->ftime) ? -1 : 1;
        }
        break;
    case DIR_SORT_NAME:
    case DIR_SORT_NONE:
    default:
        result = 0;
        break;
    }

    /* Name is the tie-breaker for every key, so the order is deterministic. */
    if (result == 0) {
        result = strcasecmp(a->name, b->name);
    }

    return ctx->sort_reverse ? -result : result;
}

/** Test one entry against the `/A` attribute filter. */
static bool shell_dir_attr_matches(const dir_ctx_t *ctx, uint8_t attrib)
{
    if (!ctx->have_attr_filter) {
        /* DOS default: hidden and system entries are suppressed unless a bare
         * `/A` was given, which shows everything. This keeps the hidden
         * `.trash` recycle bin out of ordinary listings. */
        if (ctx->show_all) {
            return true;
        }
        return (attrib & (AM_HID | AM_SYS)) == 0;
    }

    if ((ctx->require_mask != 0) && ((attrib & ctx->require_mask) != ctx->require_mask)) {
        return false;
    }

    if ((ctx->exclude_mask != 0) && ((attrib & ctx->exclude_mask) != 0)) {
        return false;
    }

    return true;
}

/**
 * Map an attribute letter to its FATFS bit.
 * @return The bit, or 0 when the letter is unknown.
 */
static uint8_t shell_dir_attr_bit(char letter)
{
    switch (toupper((unsigned char)letter)) {
    case 'D': return AM_DIR;
    case 'H': return AM_HID;
    case 'S': return AM_SYS;
    case 'R': return AM_RDO;
    case 'A': return AM_ARC;
    default:  return 0;
    }
}

/** Render the FATFS date and time words as `YYYY-MM-DD  HH:MM`. */
/** Shared with find discovery output (storage_text.c). */
void shell_dir_format_stamp(uint16_t fdate, uint16_t ftime, char *output, size_t output_size)
{
    /* FATFS packs the date as yyyyyyym mmmddddd with the year relative to
     * 1980, and the time as hhhhhmmm mmmsssss with two-second resolution. */
    unsigned int year = 1980u + ((fdate >> 9) & 0x7Fu);
    unsigned int month = (fdate >> 5) & 0x0Fu;
    unsigned int day = fdate & 0x1Fu;
    unsigned int hour = (ftime >> 11) & 0x1Fu;
    unsigned int minute = (ftime >> 5) & 0x3Fu;

    if (month == 0 || day == 0) {
        snprintf(output, output_size, "%-16s", "");
        return;
    }

    snprintf(output, output_size, "%04u-%02u-%02u  %02u:%02u", year, month, day, hour, minute);
}

/**
 * List one directory, recursing when `/S` is active.
 *
 * @param dir_path Absolute VFS path.
 * @param pattern  Wildcard filter, or NULL for everything.
 * @param depth    Current recursion depth.
 * @return false when the user aborted a paged listing.
 */
static bool shell_dir_list_one(const char *dir_path, const char *pattern, int depth, dir_ctx_t *ctx)
{
    /* Every sizeable local lives in one heap block. This function recurses
     * once per directory level for /S, and the command worker task stack is
     * shared with the whole dispatch path, so large stack locals here would
     * overflow it well before the depth limit. */
    struct dir_level_scratch {
        char fatfs_path[SHELL_SD_PATH_BYTES];
        /* Sized for every column at full width plus one over-long final cell,
         * which is clipped to the column width but appended before the row is
         * flushed. Without the extra allowance the last append could be
         * truncated mid-row and break the grid. */
        char wide_row[(P4_CONFIG_DIR_WIDE_COLUMNS + 1) * (P4_CONFIG_DIR_WIDE_COLUMN_WIDTH + 4) + 8];
        FF_DIR dir;
        FILINFO info;
        dir_entry_t entries[P4_CONFIG_DIR_SORT_ENTRY_MAX];
        /* Per-level working buffers live here, not on the task stack: /S
         * recurses once per level, so stack copies would multiply by the
         * depth limit on the shared command-worker stack. */
        char display_name[SHELL_LFN_BYTES];
        char cell[SHELL_LFN_BYTES + 4];
        char upper[SHELL_LFN_BYTES];
        char coloured_name[SHELL_LFN_BYTES + 8];
    } *scratch = NULL;
    FRESULT result;
    size_t count = 0;
    size_t index;
    unsigned int local_files = 0;
    unsigned int local_dirs = 0;
    uint64_t local_bytes = 0;
    size_t wide_column = 0;
    bool truncated = false;
    bool keep_going = true;

    if (ctx->aborted || depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX) {
        return !ctx->aborted;
    }

    /* ~40 KB per level: prefer PSRAM so the DMA-capable internal heap (which
     * backs LVGL spans, WiFi/SDIO pools and the USB ring buffers) is left
     * alone. Fall back to internal RAM when PSRAM is unavailable. */
    scratch = heap_caps_calloc(1, sizeof(*scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (scratch == NULL) {
        scratch = calloc(1, sizeof(*scratch));
    }
    if (scratch == NULL) {
        shell_transcript_append_text("dir: out of memory buffering the directory\n");
        shell_record_errorf("dir", ESP_ERR_NO_MEM, "Out of memory buffering a directory level");
        return false;
    }

    if (shell_sd_vfs_to_fatfs_path(dir_path, scratch->fatfs_path, sizeof(scratch->fatfs_path)) != ESP_OK) {
        free(scratch);
        shell_print_error("dir: invalid FAT path for %s", dir_path);
        return true;
    }

    result = f_opendir(&scratch->dir, scratch->fatfs_path);
    if (result != FR_OK) {
        free(scratch);
        shell_print_error("dir: cannot open %s", dir_path);
        return true;
    }

    /* Buffer the level first so it can be sorted before anything prints. */
    while (count < P4_CONFIG_DIR_SORT_ENTRY_MAX) {
        result = f_readdir(&scratch->dir, &scratch->info);
        if (result != FR_OK || scratch->info.fname[0] == '\0') {
            break;
        }

        if (strcmp(scratch->info.fname, ".") == 0 || strcmp(scratch->info.fname, "..") == 0) {
            continue;
        }

        if (pattern != NULL && !shell_wildcard_match(pattern, scratch->info.fname)) {
            continue;
        }

        if (!shell_dir_attr_matches(ctx, scratch->info.fattrib)) {
            continue;
        }

        snprintf(scratch->entries[count].name, sizeof(scratch->entries[count].name), "%s", scratch->info.fname);
        snprintf(scratch->entries[count].altname, sizeof(scratch->entries[count].altname), "%s", scratch->info.altname);
        scratch->entries[count].size = (uint64_t)scratch->info.fsize;
        scratch->entries[count].fdate = scratch->info.fdate;
        scratch->entries[count].ftime = scratch->info.ftime;
        scratch->entries[count].attrib = scratch->info.fattrib;
        scratch->entries[count].is_dir = (scratch->info.fattrib & AM_DIR) != 0;
        count++;
    }

    /* Detect an over-long directory so the user is told rather than misled. */
    if (count == P4_CONFIG_DIR_SORT_ENTRY_MAX) {
        result = f_readdir(&scratch->dir, &scratch->info);
        if (result == FR_OK && scratch->info.fname[0] != '\0') {
            truncated = true;
        }
    }

    (void)f_closedir(&scratch->dir);

    if (ctx->sort_key != DIR_SORT_NONE || ctx->group_dirs_first) {
        s_dir_sort_ctx = ctx;
        qsort(scratch->entries, count, sizeof(scratch->entries[0]), shell_dir_compare);
        s_dir_sort_ctx = NULL;
    }

    if (!ctx->bare) {
        keep_going = shell_dir_emitf(ctx, "\n Directory of %s\n\n", dir_path);
    }

    scratch->wide_row[0] = '\0';

    for (index = 0; index < count && keep_going; index++) {
        dir_entry_t *entry = &scratch->entries[index];

        snprintf(scratch->display_name, sizeof(scratch->display_name), "%s", entry->name);
        shell_dir_apply_case(ctx, scratch->display_name);

        if (entry->is_dir) {
            local_dirs++;
        } else {
            local_files++;
            local_bytes += entry->size;
        }

        if (ctx->bare) {
            /* Bare mode prints a full path under /S so the output can be
             * piped into another command, exactly like DOS. It stays
             * uncoloured: the transcript strips escapes, but a redirected
             * copy must be plain text for the next stage to parse. */
            if (ctx->recursive) {
                keep_going = shell_dir_emitf(ctx, "%s/%s\n", dir_path, scratch->display_name);
            } else {
                keep_going = shell_dir_emitf(ctx, "%s\n", scratch->display_name);
            }
            continue;
        }

        /* Entry colour is chosen once here from the palette: directories,
         * runnable .bat files, and ordinary files each get their own so a
         * listing is scannable at a glance. */
        const char *entry_colour = entry->is_dir
                                       ? SH_DIR
                                       : (shell_path_has_extension(entry->name, ".bat") ? SH_EXE : SH_FILE);

        if (ctx->wide) {
            /* Sized for a full long file name plus the directory brackets, so
             * a long name is clipped deliberately below rather than by an
             * accidental snprintf() truncation. */
            size_t cell_len;

            /* DOS brackets directory names in wide mode. The colour is added
             * after padding is computed, so the escape bytes do not disturb
             * the column arithmetic. */
            if (entry->is_dir) {
                snprintf(scratch->cell, sizeof(scratch->cell), "[%s]", scratch->display_name);
            } else {
                snprintf(scratch->cell, sizeof(scratch->cell), "%s", scratch->display_name);
            }

            /* Clip anything that will not fit the column so the grid stays
             * aligned, marking the cut with a trailing '~' like DOS does. */
            cell_len = strlen(scratch->cell);
            if (cell_len > P4_CONFIG_DIR_WIDE_COLUMN_WIDTH - 1) {
                scratch->cell[P4_CONFIG_DIR_WIDE_COLUMN_WIDTH - 2] = '~';
                scratch->cell[P4_CONFIG_DIR_WIDE_COLUMN_WIDTH - 1] = '\0';
            }

            /* Append the cell and its padding by index rather than with
             * strncat(). The row length is tracked explicitly, so the bound is
             * a compile-time constant and the write is provably in range. */
            {
                size_t row_len = strlen(scratch->wide_row);
                size_t copy_index = 0;

                cell_len = strlen(scratch->cell);
                while (copy_index < cell_len && row_len + 1 < sizeof(scratch->wide_row)) {
                    scratch->wide_row[row_len++] = scratch->cell[copy_index++];
                }

                wide_column++;
                if (wide_column >= P4_CONFIG_DIR_WIDE_COLUMNS) {
                    scratch->wide_row[row_len] = '\0';
                    keep_going = shell_dir_emitf(ctx, "%s\n", scratch->wide_row);
                    scratch->wide_row[0] = '\0';
                    wide_column = 0;
                } else {
                    /* Pad out to the column boundary so the grid stays aligned. */
                    size_t used = cell_len;

                    while (used < (size_t)P4_CONFIG_DIR_WIDE_COLUMN_WIDTH &&
                           row_len + 1 < sizeof(scratch->wide_row)) {
                        scratch->wide_row[row_len++] = ' ';
                        used++;
                    }
                    scratch->wide_row[row_len] = '\0';
                }
            }
            continue;
        }

        /* Detailed listing: timestamp, size or <DIR>, then the name. */
        {
            char stamp[24];
            char size_text[24];
            char sfn[20] = "";

            shell_dir_format_stamp(entry->fdate, entry->ftime, stamp, sizeof(stamp));

            if (entry->is_dir) {
                snprintf(size_text, sizeof(size_text), "%10s", "<DIR>");
            } else {
                char formatted[24];

                shell_sd_format_size(entry->size, formatted, sizeof(formatted));
                snprintf(size_text, sizeof(size_text), "%10s", formatted);
            }

            /* Show the 8.3 alternate name only when it differs from the LFN,
             * which is the existing behavior and stays unchanged. */
            if (entry->altname[0] != '\0') {
                char *cursor;

                snprintf(scratch->upper, sizeof(scratch->upper), "%s", entry->name);
                for (cursor = scratch->upper; *cursor != '\0'; cursor++) {
                    *cursor = (char)toupper((unsigned char)*cursor);
                }
                if (strcmp(entry->altname, scratch->upper) != 0) {
                    snprintf(sfn, sizeof(sfn), " [%s]", entry->altname);
                }
            }

            /* Colour is applied after all width formatting is done, so the
             * escape bytes never affect the column alignment computed above:
             * muted timestamp, magenta size, and an entry colour chosen by
             * kind (directory, runnable .bat, or ordinary file). */
            /* entry_colour holds @-specifiers (e.g. SH_FILE = @w). Build a
             * format string with the colour literal so ansi_format converts
             * it to real SGR escapes; the entry name is substituted as %s. */
            char colour_fmt[16];
            snprintf(colour_fmt, sizeof(colour_fmt), "%s%%s", entry_colour);
            ansi_format(scratch->coloured_name, sizeof(scratch->coloured_name), colour_fmt, scratch->display_name);
            keep_going = shell_dir_emitf(ctx,
                                         SH_TIME "%s" SH_RST "  "
                                         SH_SIZE "%s" SH_RST "  "
                                         "%s" SH_RST
                                         SH_MUTE "%s" SH_RST "\n",
                                         stamp, size_text, scratch->coloured_name, sfn);
        }
    }

    /* Flush a partially filled wide row. */
    if (keep_going && ctx->wide && scratch->wide_row[0] != '\0') {
        keep_going = shell_dir_emitf(ctx, "%s\n", scratch->wide_row);
    }

    if (truncated) {
        shell_transcript_appendf("dir: listing truncated after %u entries\n",
                                 (unsigned int)P4_CONFIG_DIR_SORT_ENTRY_MAX);
        shell_record_warningf("dir", "Truncated directory listing for %s", dir_path);
    }

    if (keep_going && !ctx->bare) {
        char total_text[24];

        shell_sd_format_size(local_bytes, total_text, sizeof(total_text));
        keep_going = shell_dir_emitf(ctx, "%15u file(s)  %s\n", local_files, total_text);
        if (keep_going) {
            keep_going = shell_dir_emitf(ctx, "%15u dir(s)\n", local_dirs);
        }
    }

    ctx->total_files += local_files;
    ctx->total_dirs += local_dirs;
    ctx->total_bytes += local_bytes;

    /* Recurse after printing this level, matching DOS ordering. The whole
     * scratch block is released before descending so nested levels never
     * hold more than one directory buffer at a time. */
    if (ctx->recursive && keep_going && !ctx->aborted) {
        char (*subdirs)[SHELL_LFN_BYTES] = NULL;
        size_t subdir_count = 0;

        /* Up to 128 x 256 B: prefer PSRAM to protect the internal heap. */
        subdirs = heap_caps_calloc(count > 0 ? count : 1, sizeof(*subdirs),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (subdirs == NULL) {
            subdirs = calloc(count > 0 ? count : 1, sizeof(*subdirs));
        }
        if (subdirs != NULL) {
            for (index = 0; index < count; index++) {
                if (scratch->entries[index].is_dir) {
                    snprintf(subdirs[subdir_count], SHELL_LFN_BYTES, "%s", scratch->entries[index].name);
                    subdir_count++;
                }
            }
        }

        free(scratch);
        scratch = NULL;

        if (subdirs != NULL) {
            /* child_path lives on the heap because this function recurses once
             * per directory level: a 320-byte stack copy multiplied by
             * P4_CONFIG_DIR_RECURSE_DEPTH_MAX would overflow the 8 KB command
             * worker task stack (a stack overflow corrupts memory and shows up
             * as an intermittent crash). */
            char *child_path = heap_caps_malloc(SHELL_SD_PATH_BYTES,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (child_path == NULL) {
                child_path = malloc(SHELL_SD_PATH_BYTES);
            }
            if (child_path == NULL) {
                free(subdirs);
                return false;
            }
            for (index = 0; index < subdir_count && keep_going && !ctx->aborted; index++) {
                if (snprintf(child_path, SHELL_SD_PATH_BYTES, "%s/%s", dir_path, subdirs[index]) < SHELL_SD_PATH_BYTES) {
                    keep_going = shell_dir_list_one(child_path, pattern, depth + 1, ctx);
                }
            }
            free(child_path);
            free(subdirs);
        }
    }

    free(scratch);
    return keep_going;
}

void shell_command_dir(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    char dir_part[SHELL_SD_PATH_BYTES];
    char pattern_buf[SHELL_LFN_BYTES];
    const char *pattern = NULL;
    const char *path_arg = NULL;
    shell_sd_session_t session;
    storage_space_info_t space;
    dir_ctx_t ctx;
    struct stat path_stat;
    esp_err_t error;
    int index;

    memset(&ctx, 0, sizeof(ctx));
    ctx.interactive = shell_key_input_available();

    /* Parse the DOS switch set. Options may appear before or after the path
     * and are accepted with or without the ':' separator. */
    for (index = 1; index < argc; index++) {
        const char *token = argv[index];
        char flag;
        const char *value;

        if (token[0] != '/') {
            if (path_arg == NULL) {
                path_arg = token;
                continue;
            }
            shell_print_usage("Usage: dir [path|pattern] [/W] [/P] [/S] [/B] [/L] [/A:attrs] [/O:order]");
            return;
        }

        flag = (char)toupper((unsigned char)token[1]);
        value = token + 2;
        if (*value == ':' || *value == '=') {
            value++;
        }

        switch (flag) {
        case 'W': ctx.wide = true;      continue;
        case 'P': ctx.paged = true;     continue;
        case 'S': ctx.recursive = true; continue;
        case 'B': ctx.bare = true;      continue;
        case 'L': ctx.lowercase = true; continue;

        case 'A': {
            /* /A with no value means "show everything including hidden". */
            if (*value == '\0') {
                ctx.have_attr_filter = false;
                ctx.show_all = true;
                continue;
            }

            ctx.have_attr_filter = true;
            while (*value != '\0') {
                bool negate = false;
                uint8_t bit;

                if (*value == '-') {
                    negate = true;
                    value++;
                    if (*value == '\0') {
                        break;
                    }
                }

                bit = shell_dir_attr_bit(*value);
                if (bit == 0) {
                    shell_print_error("dir: unknown attribute '%c' in %s", *value, token);
                    shell_transcript_append_text("  Use D (dir), H (hidden), S (system), R (read-only), A (archive)\n");
                    return;
                }

                if (negate) {
                    ctx.exclude_mask |= bit;
                } else {
                    ctx.require_mask |= bit;
                }
                value++;
            }
            continue;
        }

        case 'O': {
            /* /O with no value sorts by name ascending. */
            if (*value == '\0') {
                ctx.sort_key = DIR_SORT_NAME;
                continue;
            }

            while (*value != '\0') {
                if (*value == '-') {
                    ctx.sort_reverse = true;
                    value++;
                    continue;
                }

                switch (toupper((unsigned char)*value)) {
                case 'N': ctx.sort_key = DIR_SORT_NAME;      break;
                case 'S': ctx.sort_key = DIR_SORT_SIZE;      break;
                case 'E': ctx.sort_key = DIR_SORT_EXTENSION; break;
                case 'D': ctx.sort_key = DIR_SORT_DATE;      break;
                case 'G': ctx.group_dirs_first = true;       break;
                default:
                    shell_print_error("dir: unknown sort order '%c' in %s", *value, token);
                    shell_transcript_append_text("  Use N (name), S (size), E (extension), D (date), G (dirs first)\n");
                    return;
                }
                value++;
            }
            continue;
        }

        default:
            shell_print_error("dir: unknown option %s", token);
            shell_print_usage("Usage: dir [path|pattern] [/W] [/P] [/S] [/B] [/L] [/A:attrs] [/O:order]");
            return;
        }
    }

    /* A wildcard in the argument splits into a directory plus a pattern. */
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
            snprintf(pattern_buf, sizeof(pattern_buf), "%s", last_sep + 1);
        } else {
            snprintf(dir_part, sizeof(dir_part), ".");
            snprintf(pattern_buf, sizeof(pattern_buf), "%s", path_arg);
        }

        pattern = pattern_buf;
        error = shell_fs_resolve_path(dir_part, resolved_path, sizeof(resolved_path));
    } else {
        error = shell_fs_resolve_path(path_arg, resolved_path, sizeof(resolved_path));
    }

    if (error != ESP_OK) {
        shell_print_error("dir: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("dir: SD card not present - insert and retry");
        return;
    }

    error = shell_sd_stat_path(resolved_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("dir: path not found %s\n", resolved_path);
        shell_sd_end(&session, "dir");
        return;
    }

    /* Naming a single file reports just that file, matching DOS. */
    if (!S_ISDIR(path_stat.st_mode)) {
        char size_text[24];

        shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        shell_transcript_appendf("%s  %s\n", size_text, resolved_path);
        shell_sd_end(&session, "dir");
        return;
    }

    if (!ctx.bare) {
        shell_transcript_appendf(" Volume in drive %s\n", P4_CONFIG_SD_DRIVE_LETTER);
    }

    (void)shell_dir_list_one(resolved_path, pattern, 0, &ctx);

    /* Grand totals and free space, the way DOS closes a listing. Recursive
     * runs get an explicit "total" banner so the per-directory counts above
     * are not mistaken for the whole tree. */
    if (!ctx.bare && !ctx.aborted) {
        char total_text[24];

        shell_sd_format_size(ctx.total_bytes, total_text, sizeof(total_text));

        if (ctx.recursive) {
            shell_transcript_append_text("\n Total files listed:\n");
            shell_transcript_appendf("%15u file(s)  %s\n", ctx.total_files, total_text);
            shell_transcript_appendf("%15u dir(s)\n", ctx.total_dirs);
        }

        if (storage_get_space_info(&space) == ESP_OK) {
            char free_text[24];

            shell_sd_format_size(space.free_bytes, free_text, sizeof(free_text));
            shell_transcript_appendf("%15s   %s free\n", "", free_text);
        }
    }

    shell_sd_end(&session, "dir");
}

/* ========================================================================
 * RECURSIVE DIRECTORY TREE (tree)
 * ========================================================================
 * Walks the directory graph depth-first and draws the DOS box-drawing
 * outline. Recursion is bounded by P4_CONFIG_TREE_DEPTH_MAX and the total
 * entry count by P4_CONFIG_SD_LIST_LIMIT, so a deep or wide card cannot
 * exhaust the worker-task stack or flood the transcript.
 *
 * Usage:
 *   tree [path] [/F] [/A]
 *     /F  Include files as well as directories (DOS default is dirs only)
 *     /A  Use plain ASCII connectors instead of the default `+`/`\` glyphs
 */

/** Running totals and limits shared across one `tree` invocation. */
typedef struct {
    unsigned int dir_count;
    unsigned int file_count;
    unsigned int emitted;
    bool truncated;
    bool show_files;
    bool ascii_mode;
} shell_tree_ctx_t;

/**
 * Print one indentation run for a tree row.
 *
 * @param prefix  Accumulated prefix string built by the parent levels.
 * @param is_last true when this entry is the final child of its parent.
 */
static void shell_tree_print_row(const shell_tree_ctx_t *ctx,
                                 const char *prefix,
                                 const char *name,
                                 bool is_last,
                                 bool is_dir)
{
    const char *branch;

    if (ctx->ascii_mode) {
        branch = is_last ? "\\-- " : "+-- ";
    } else {
        branch = is_last ? "\\---" : "+---";
    }

    shell_transcript_appendf("%s%s%s%s\n",
                             prefix,
                             branch,
                             name,
                             is_dir ? "\\" : "");
}

/**
 * Recursive worker for `tree`.
 *
 * @param dir_path  Absolute VFS path of the directory to walk.
 * @param prefix    Indentation prefix inherited from the parent level.
 * @param depth     Current recursion depth, 0 at the root.
 */
static void shell_tree_walk(const char *dir_path,
                            const char *prefix,
                            int depth,
                            shell_tree_ctx_t *ctx)
{
    DIR *dir;
    struct dirent *entry;
    char (*names)[SHELL_LFN_BYTES];
    bool *is_dir_flags;
    size_t count = 0;
    size_t index;

    if (ctx->truncated || depth >= P4_CONFIG_TREE_DEPTH_MAX) {
        return;
    }

    dir = opendir(dir_path);
    if (dir == NULL) {
        shell_transcript_appendf("%s[cannot open %s]\n", prefix, dir_path);
        return;
    }

    /* Buffer one directory level so the last entry can be drawn with the
     * closing connector. Heap-allocated because a full level can be large
     * and the worker task stack is shared with every other command. */
    names = calloc(SHELL_SD_LIST_LIMIT, sizeof(*names));
    is_dir_flags = calloc(SHELL_SD_LIST_LIMIT, sizeof(*is_dir_flags));
    if (names == NULL || is_dir_flags == NULL) {
        free(names);
        free(is_dir_flags);
        closedir(dir);
        shell_transcript_append_text("tree: out of memory\n");
        shell_record_errorf("tree", ESP_ERR_NO_MEM, "Out of memory buffering a directory level");
        ctx->truncated = true;
        return;
    }

    while ((entry = readdir(dir)) != NULL && count < SHELL_SD_LIST_LIMIT) {
        char child[SHELL_SD_PATH_BYTES + SHELL_LFN_BYTES];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // A truncated child path must never reach stat(): skip the entry
        // deterministically instead of stating a silently-cut path.
        int child_n = snprintf(child, sizeof(child), "%s/%s", dir_path, entry->d_name);
        if (child_n < 0 || (size_t)child_n >= sizeof(child)) {
            continue;
        }
        if (stat(child, &st) != 0) {
            continue;
        }

        if (!S_ISDIR(st.st_mode) && !ctx->show_files) {
            continue;
        }

        snprintf(names[count], sizeof(names[count]), "%s", entry->d_name);
        is_dir_flags[count] = S_ISDIR(st.st_mode);
        count++;
    }
    closedir(dir);

    /* The two path buffers are heap-allocated rather than declared inside the
     * loop: this function recurses once per tree level, so a stack copy of
     * each would multiply by the depth limit on the shared worker stack. */
    {
        char *child_prefix = malloc(SHELL_SD_PATH_BYTES);
        char *child_path = malloc(SHELL_SD_PATH_BYTES + SHELL_LFN_BYTES);

        if (child_prefix == NULL || child_path == NULL) {
            free(child_prefix);
            free(child_path);
            free(names);
            free(is_dir_flags);
            shell_transcript_append_text("tree: out of memory\n");
            shell_record_errorf("tree", ESP_ERR_NO_MEM, "Out of memory building a child path");
            ctx->truncated = true;
            return;
        }

        for (index = 0; index < count; index++) {
            bool is_last = (index + 1 == count);

            if (ctx->emitted >= SHELL_SD_LIST_LIMIT) {
                shell_transcript_appendf("tree: listing truncated after %u entries\n",
                                         (unsigned int)SHELL_SD_LIST_LIMIT);
                shell_record_warningf("tree", "Truncated tree listing at %s", dir_path);
                ctx->truncated = true;
                break;
            }

            shell_tree_print_row(ctx, prefix, names[index], is_last, is_dir_flags[index]);
            ctx->emitted++;

            if (is_dir_flags[index]) {
                ctx->dir_count++;
            } else {
                ctx->file_count++;
                continue;
            }

            /* Children of a last entry get blank spacing; otherwise the
             * vertical bar continues so the outline stays connected. */
            snprintf(child_prefix, SHELL_SD_PATH_BYTES, "%s%s", prefix, is_last ? "    " : "|   ");
            // Never descend into a truncated path: mark truncated and stop.
            int child_n = snprintf(child_path, SHELL_SD_PATH_BYTES + SHELL_LFN_BYTES, "%s/%s", dir_path, names[index]);
            if (child_n < 0 || (size_t)child_n >= SHELL_SD_PATH_BYTES + SHELL_LFN_BYTES) {
                ctx->truncated = true;
                break;
            }
            shell_tree_walk(child_path, child_prefix, depth + 1, ctx);

            if (ctx->truncated) {
                break;
            }
        }

        free(child_prefix);
        free(child_path);
    }

    free(names);
    free(is_dir_flags);
}

void shell_command_tree(int argc, char **argv)
{
    char resolved[SHELL_SD_PATH_BYTES];
    const char *path_arg = NULL;
    shell_sd_session_t session;
    shell_tree_ctx_t ctx = {0};
    struct stat root_stat;
    esp_err_t error;
    int index;

    /* DOS-style switches may appear before or after the path. */
    for (index = 1; index < argc; index++) {
        if (argv[index][0] == '/') {
            char flag = (char)toupper((unsigned char)argv[index][1]);

            if (flag == 'F') {
                ctx.show_files = true;
            } else if (flag == 'A') {
                ctx.ascii_mode = true;
            } else {
                shell_print_error("tree: unknown option %s", argv[index]);
                shell_print_usage("Usage: tree [path] [/F] [/A]");
                return;
            }
            continue;
        }

        if (path_arg == NULL) {
            path_arg = argv[index];
        } else {
            shell_print_usage("Usage: tree [path] [/F] [/A]");
            return;
        }
    }

    error = shell_fs_resolve_path(path_arg, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        shell_print_error("tree: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("tree: SD card not present - insert and retry");
        return;
    }

    error = shell_sd_stat_path(resolved, &root_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("tree: path not found %s\n", resolved);
        shell_sd_end(&session, "tree");
        return;
    }

    if (!S_ISDIR(root_stat.st_mode)) {
        shell_transcript_appendf("tree: %s is not a directory\n", resolved);
        shell_sd_end(&session, "tree");
        return;
    }

    shell_print_heading("Folder PATH listing for volume %s", P4_CONFIG_SD_DRIVE_LETTER);
    shell_transcript_appendf("%s\n", resolved);

    shell_tree_walk(resolved, "", 0, &ctx);

    if (ctx.dir_count == 0 && ctx.file_count == 0) {
        shell_transcript_append_text("No subfolders exist\n");
    } else if (ctx.show_files) {
        shell_transcript_appendf("\n%u director%s, %u file(s)\n",
                                 ctx.dir_count,
                                 ctx.dir_count == 1 ? "y" : "ies",
                                 ctx.file_count);
    } else {
        shell_transcript_appendf("\n%u director%s\n",
                                 ctx.dir_count,
                                 ctx.dir_count == 1 ? "y" : "ies");
    }

    shell_sd_end(&session, "tree");
}

/**
 * @file storage_commands.c
 * @brief DOS-style file command implementations for P4MiniShell.
 *
 * Holds every command that reads or writes the SD filesystem: navigation,
 * listing, the recursive directory tree, file manipulation, the extended DOS
 * tools (`attrib`, `label`, `xcopy`), the text utilities (`find`, `more`,
 * `fc`, `sort`), and the `sd` command family. The primitives these build on —
 * guarded SD sessions, path resolution, FATFS conversion, size formatting,
 * wildcard matching, and input redirection — live in storage.c.
 *
 * Every text-processing command resolves its input through
 * storage_resolve_input_source(), so an explicit filename, a `<` redirection,
 * and a `|` pipe stage all reach the same code path.
 *
 * All transcript output and debug logging go through shell.c.
 */

#include "storage_commands.h"
#include "storage.h"
#include "shell.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
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
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

/* Backward-compatibility aliases */
#define SHELL_SD_FATFS_DRIVE            P4_CONFIG_SD_FATFS_DRIVE
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_SD_LIST_LIMIT             P4_CONFIG_SD_LIST_LIMIT
#define SHELL_SD_CAT_DEFAULT_BYTES      P4_CONFIG_SD_CAT_DEFAULT_BYTES
#define SHELL_SD_CAT_MAX_BYTES          P4_CONFIG_SD_CAT_MAX_BYTES
#define SHELL_SD_IO_BUFFER_BYTES        P4_CONFIG_SD_IO_BUFFER_BYTES
#define SHELL_BATCH_LINE_BYTES          P4_CONFIG_BATCH_LINE_BYTES
#define SHELL_LFN_BYTES                 P4_CONFIG_LFN_BYTES
#define SHELL_TEXT_LINE_BYTES           P4_CONFIG_TEXT_LINE_BYTES
#define SHELL_SORT_LINE_MAX             P4_CONFIG_SORT_LINE_MAX
#define SHELL_MORE_PAGE_LINES           P4_CONFIG_MORE_PAGE_LINES
#define SHELL_MORE_PAGE_DELAY_MS        P4_CONFIG_MORE_PAGE_DELAY_MS

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static void shell_sd_print_usage(void);
static void shell_command_sd_info(void);
static void shell_command_sd_ls(char *command);
static void shell_command_sd_stat(char *command);
static void shell_command_sd_cat(char *command);

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
        return true;
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
static void shell_dir_format_stamp(uint16_t fdate, uint16_t ftime, char *output, size_t output_size)
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

    scratch = calloc(1, sizeof(*scratch));
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
        char display_name[SHELL_LFN_BYTES];

        snprintf(display_name, sizeof(display_name), "%s", entry->name);
        shell_dir_apply_case(ctx, display_name);

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
                keep_going = shell_dir_emitf(ctx, "%s/%s\n", dir_path, display_name);
            } else {
                keep_going = shell_dir_emitf(ctx, "%s\n", display_name);
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
            char cell[SHELL_LFN_BYTES + 4];
            size_t cell_len;

            /* DOS brackets directory names in wide mode. The colour is added
             * after padding is computed, so the escape bytes do not disturb
             * the column arithmetic. */
            if (entry->is_dir) {
                snprintf(cell, sizeof(cell), "[%s]", display_name);
            } else {
                snprintf(cell, sizeof(cell), "%s", display_name);
            }

            /* Clip anything that will not fit the column so the grid stays
             * aligned, marking the cut with a trailing '~' like DOS does. */
            cell_len = strlen(cell);
            if (cell_len > P4_CONFIG_DIR_WIDE_COLUMN_WIDTH - 1) {
                cell[P4_CONFIG_DIR_WIDE_COLUMN_WIDTH - 2] = '~';
                cell[P4_CONFIG_DIR_WIDE_COLUMN_WIDTH - 1] = '\0';
            }

            /* Append the cell and its padding by index rather than with
             * strncat(). The row length is tracked explicitly, so the bound is
             * a compile-time constant and the write is provably in range. */
            {
                size_t row_len = strlen(scratch->wide_row);
                size_t copy_index = 0;

                cell_len = strlen(cell);
                while (copy_index < cell_len && row_len + 1 < sizeof(scratch->wide_row)) {
                    scratch->wide_row[row_len++] = cell[copy_index++];
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
                char upper[SHELL_LFN_BYTES];
                char *cursor;

                snprintf(upper, sizeof(upper), "%s", entry->name);
                for (cursor = upper; *cursor != '\0'; cursor++) {
                    *cursor = (char)toupper((unsigned char)*cursor);
                }
                if (strcmp(entry->altname, upper) != 0) {
                    snprintf(sfn, sizeof(sfn), " [%s]", entry->altname);
                }
            }

            /* Colour is applied after all width formatting is done, so the
             * escape bytes never affect the column alignment computed above:
             * muted timestamp, magenta size, and an entry colour chosen by
             * kind (directory, runnable .bat, or ordinary file). */
            char coloured_name[SHELL_LFN_BYTES + 8];
            /* entry_colour holds @-specifiers (e.g. SH_FILE = @w). Build a
             * format string with the colour literal so ansi_vformat converts
             * it to real SGR escapes; the entry name is substituted as %s. */
            char colour_fmt[16];
            snprintf(colour_fmt, sizeof(colour_fmt), "%s%%s", entry_colour);
            ansi_format(coloured_name, sizeof(coloured_name), colour_fmt, display_name);
            keep_going = shell_dir_emitf(ctx,
                                         SH_TIME "%s" SH_RST "  "
                                         SH_SIZE "%s" SH_RST "  "
                                         "%s" SH_RST
                                         SH_MUTE "%s" SH_RST "\n",
                                         stamp, size_text, coloured_name, sfn);
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

        subdirs = calloc(count > 0 ? count : 1, sizeof(*subdirs));
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
            char *child_path = malloc(SHELL_SD_PATH_BYTES);
            if (child_path == NULL) {
                free(subdirs);
                free(scratch);
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

        snprintf(child, sizeof(child), "%s/%s", dir_path, entry->d_name);
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
            snprintf(child_path, SHELL_SD_PATH_BYTES + SHELL_LFN_BYTES, "%s/%s", dir_path, names[index]);
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

/* ========================================================================
 * FILE MANIPULATION
 * ======================================================================== */

void shell_command_copy(int argc, char **argv)
{
    char source_path[SHELL_SD_PATH_BYTES];
    char dest_path[SHELL_SD_PATH_BYTES];
    char src_dir[SHELL_SD_PATH_BYTES];
    const char *pattern = NULL;
    esp_err_t error;

    if (argc != 3) {
        shell_print_usage("Usage: copy <source|pattern> <destination>");
        return;
    }

    /* Check for wildcard in source */
    if (strchr(argv[1], '*') != NULL || strchr(argv[1], '?') != NULL) {
        const char *last_sep = strrchr(argv[1], '/');
        if (last_sep == NULL) last_sep = strrchr(argv[1], '\\');
        if (last_sep != NULL) {
            size_t dir_len = (size_t)(last_sep - argv[1]);
            if (dir_len >= sizeof(src_dir)) dir_len = sizeof(src_dir) - 1;
            memcpy(src_dir, argv[1], dir_len);
            src_dir[dir_len] = '\0';
            pattern = last_sep + 1;
        } else {
            snprintf(src_dir, sizeof(src_dir), ".");
            pattern = argv[1];
        }
        error = shell_fs_resolve_path(src_dir, source_path, sizeof(source_path));
        if (error != ESP_OK) {
            shell_print_error("copy: invalid source path");
            return;
        }
        error = shell_fs_resolve_path(argv[2], dest_path, sizeof(dest_path));
        if (error != ESP_OK) {
            shell_print_error("copy: invalid destination path");
            return;
        }

        /* Ensure destination is a directory for wildcard copy */
        struct stat dst_st;
        if (stat(dest_path, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
            shell_transcript_append_text("copy: destination must be a directory for wildcard copy\n");
            return;
        }

        DIR *d = opendir(source_path);
        if (d == NULL) {
            shell_print_error("copy: cannot open %s", source_path);
            return;
        }
        struct dirent *entry;
        int copied = 0;
        while ((entry = readdir(d)) != NULL) {
            if (!shell_wildcard_match(pattern, entry->d_name))
                continue;
            char sf[SHELL_SD_PATH_BYTES + 256], df[SHELL_SD_PATH_BYTES + 256];
            snprintf(sf, sizeof(sf), "%s/%s", source_path, entry->d_name);
            snprintf(df, sizeof(df), "%s/%s", dest_path, entry->d_name);
            struct stat cs;
            if (stat(sf, &cs) != 0 || S_ISDIR(cs.st_mode))
                continue;
            if (shell_fs_copy_file(sf, df) == ESP_OK) {
                shell_transcript_appendf("  %s\n", entry->d_name);
                copied++;
            } else {
                shell_transcript_appendf("  %s (failed)\n", entry->d_name);
            }
        }
        closedir(d);
        shell_transcript_appendf("copy: %d file(s) copied\n", copied);
        return;
    }

    /* Single file copy (original behavior) */
    error = shell_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        shell_print_error("copy: invalid source path");
        return;
    }

    error = shell_fs_resolve_path(argv[2], dest_path, sizeof(dest_path));
    if (error != ESP_OK) {
        shell_print_error("copy: invalid destination path");
        return;
    }

    error = shell_fs_copy_file(source_path, dest_path);
    if (error != ESP_OK) {
        shell_print_error("copy: failed (%s)", esp_err_to_name(error));
        return;
    }

    shell_print_ok("1 file(s) copied to %s", dest_path);
}

void shell_command_del(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    char dir_part[SHELL_SD_PATH_BYTES];
    const char *pattern = NULL;
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: del <path|pattern>");
        return;
    }

    /* Check for wildcard pattern */
    if (strchr(argv[1], '*') != NULL || strchr(argv[1], '?') != NULL) {
        const char *last_sep = strrchr(argv[1], '/');
        if (last_sep == NULL) last_sep = strrchr(argv[1], '\\');
        if (last_sep != NULL) {
            size_t dir_len = (size_t)(last_sep - argv[1]);
            if (dir_len >= sizeof(dir_part)) dir_len = sizeof(dir_part) - 1;
            memcpy(dir_part, argv[1], dir_len);
            dir_part[dir_len] = '\0';
            pattern = last_sep + 1;
        } else {
            snprintf(dir_part, sizeof(dir_part), ".");
            pattern = argv[1];
        }
        error = shell_fs_resolve_path(dir_part, resolved_path, sizeof(resolved_path));
        if (error != ESP_OK) {
            shell_print_error("del: invalid path");
            return;
        }

        error = shell_sd_begin(&session);
        if (error != ESP_OK) {
            shell_print_error("del: SD card not present");
            return;
        }

        DIR *d = opendir(resolved_path);
        if (d == NULL) {
            shell_print_error("del: cannot open %s", resolved_path);
            shell_sd_end(&session, "del");
            return;
        }
        struct dirent *entry;
        int deleted = 0;
        while ((entry = readdir(d)) != NULL) {
            if (!shell_wildcard_match(pattern, entry->d_name))
                continue;
            char fp[SHELL_SD_PATH_BYTES + 256];
            snprintf(fp, sizeof(fp), "%s/%s", resolved_path, entry->d_name);
            if (unlink(fp) == 0) {
                shell_transcript_appendf("  Deleted %s\n", entry->d_name);
                deleted++;
            } else {
                shell_transcript_appendf("  Failed: %s (%s)\n", entry->d_name, strerror(errno));
            }
        }
        closedir(d);
        shell_transcript_appendf("del: %d file(s) deleted\n", deleted);
        shell_sd_end(&session, "del");
        return;
    }

    /* Single file deletion (original behavior) */
    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("del: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("del: SD card not present - insert and retry");
        return;
    }

    if (unlink(resolved_path) != 0) {
        shell_print_error("del: failed to delete %s (%s)", resolved_path, strerror(errno));
        shell_sd_end(&session, "del");
        return;
    }

    shell_sd_end(&session, "del");
    shell_print_ok("Deleted %s", resolved_path);
}

void shell_command_rename(int argc, char **argv, const char *verb)
{
    char source_path[SHELL_SD_PATH_BYTES];
    char target_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 3) {
        shell_print_usage("Usage: %s <source> <destination>", verb);
        return;
    }

    error = shell_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: invalid source path\n", verb);
        return;
    }

    error = shell_resolve_target_from_source(source_path, argv[2], target_path, sizeof(target_path));
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: invalid destination path\n", verb);
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: SD card not present - insert and retry\n", verb);
        return;
    }

    if (rename(source_path, target_path) != 0) {
        shell_transcript_appendf("%s: failed (%s)\n", verb, strerror(errno));
        shell_sd_end(&session, verb);
        return;
    }

    shell_sd_end(&session, verb);
    shell_transcript_appendf("%s -> %s\n", source_path, target_path);
}

void shell_command_mkdir(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: mkdir <path>");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("mkdir: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("mkdir: SD card not present - insert and retry");
        return;
    }

    if (mkdir(resolved_path, 0775) != 0) {
        shell_print_error("mkdir: failed to create %s (%s)", resolved_path, strerror(errno));
        shell_sd_end(&session, "mkdir");
        return;
    }

    shell_sd_end(&session, "mkdir");
    shell_print_ok("Created directory %s", resolved_path);
}

void shell_command_rmdir(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: rmdir <path>");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("rmdir: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("rmdir: SD card not present - insert and retry");
        return;
    }

    if (rmdir(resolved_path) != 0) {
        shell_print_error("rmdir: failed to remove %s (%s)", resolved_path, strerror(errno));
        shell_sd_end(&session, "rmdir");
        return;
    }

    shell_sd_end(&session, "rmdir");
    shell_print_ok("Removed directory %s", resolved_path);
}

void shell_command_type_file(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: type <path>");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("type: invalid path");
        return;
    }

    error = shell_print_file_text(resolved_path);
    if (error == ESP_ERR_INVALID_ARG) {
        shell_transcript_appendf("type: %s is not a regular text file\n", resolved_path);
    } else if (error != ESP_OK) {
        shell_print_error("type: failed to read %s (%s)", resolved_path, esp_err_to_name(error));
    }
}

void shell_command_write_file(int argc, char **argv, bool append_mode)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    char text[SHELL_BATCH_LINE_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    if (argc < 3) {
        shell_print_usage("Usage: %s <path> <text>", append_mode ? "append" : "write");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: invalid path\n", append_mode ? "append" : "write");
        return;
    }

    shell_join_args(argv, 2, argc, text, sizeof(text));
    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_transcript_appendf("%s: SD card not present - insert and retry\n", append_mode ? "append" : "write");
        return;
    }

    /* Refuse before opening the file so an overwrite cannot destroy the
     * existing contents and then fail for lack of room. A truncating write
     * reclaims whatever the file currently holds. */
    {
        uint64_t needed = (uint64_t)strlen(text) + 1u;
        uint64_t reclaim = append_mode ? 0u : storage_get_file_size(resolved_path);

        if (!storage_check_free_space(needed, reclaim, append_mode ? "append" : "write")) {
            shell_sd_end(&session, append_mode ? "append" : "write");
            return;
        }
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        shell_transcript_appendf("%s: failed to open %s (%s)\n",
                                 append_mode ? "append" : "write",
                                 resolved_path,
                                 strerror(errno));
        shell_sd_end(&session, append_mode ? "append" : "write");
        return;
    }

    if (fwrite(text, 1, strlen(text), file) != strlen(text) || fwrite("\n", 1, 1, file) != 1) {
        shell_transcript_appendf("%s: failed while writing %s\n", append_mode ? "append" : "write", resolved_path);
        fclose(file);
        shell_sd_end(&session, append_mode ? "append" : "write");
        return;
    }

    fclose(file);
    shell_sd_end(&session, append_mode ? "append" : "write");
    shell_transcript_appendf("%s: %s\n", append_mode ? "Appended" : "Wrote", resolved_path);
}

void shell_command_touch(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: touch <path>");
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("touch: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("touch: SD card not present - insert and retry");
        return;
    }

    file = fopen(resolved_path, "ab");
    if (file == NULL) {
        shell_print_error("touch: failed to open %s (%s)", resolved_path, strerror(errno));
        shell_sd_end(&session, "touch");
        return;
    }

    fclose(file);
    (void)utime(resolved_path, NULL);
    shell_sd_end(&session, "touch");
    shell_print_ok("Touched %s", resolved_path);
}

void shell_command_move(int argc, char **argv)
{
    char source_path[SHELL_SD_PATH_BYTES];
    char target_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;

    if (argc != 3) {
        shell_print_usage("Usage: move <source> <destination>");
        return;
    }

    error = shell_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        shell_print_error("move: invalid source path");
        return;
    }

    error = shell_resolve_target_from_source(source_path, argv[2], target_path, sizeof(target_path));
    if (error != ESP_OK) {
        shell_print_error("move: invalid destination path");
        return;
    }

    /* Moving a file onto itself is a no-op in DOS, and the copy fallback
     * below would otherwise truncate the source. */
    if (storage_paths_are_same(source_path, target_path)) {
        shell_transcript_appendf("move: %s and the destination are the same file\n", source_path);
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("move: SD card not present - insert and retry");
        return;
    }

    if (rename(source_path, target_path) == 0) {
        shell_sd_end(&session, "move");
        shell_print_ok("Moved %s -> %s", source_path, target_path);
        return;
    }

    shell_sd_end(&session, "move");
    error = shell_fs_copy_file(source_path, target_path);
    if (error != ESP_OK) {
        shell_print_error("move: failed to copy %s (%s)", source_path, esp_err_to_name(error));
        return;
    }

    error = shell_sd_begin(&session);
    if (error == ESP_OK) {
        if (unlink(source_path) != 0) {
            shell_transcript_appendf("move: warning, copied but could not remove %s (%s)\n",
                                     source_path,
                                     strerror(errno));
        }
        shell_sd_end(&session, "move");
    }
    shell_print_ok("Moved %s -> %s", source_path, target_path);
}

/* ========================================================================
 * DOS ATTRIBUTE COMMAND (attrib)
 * ========================================================================
 * Reads and sets FATFS file attributes: R (read-only), H (hidden),
 * S (system), A (archive). Uses f_stat() and f_chmod() from FATFS.
 *
 * Usage:
 *   attrib [path]              Show attributes
 *   attrib +R <path>           Set read-only
 *   attrib -R <path>           Clear read-only
 *   attrib +H <path>           Set hidden
 *   attrib -H <path>           Clear hidden
 *   attrib +S <path>           Set system
 *   attrib -S <path>           Clear system
 *   attrib +A <path>           Set archive
 *   attrib -A <path>           Clear archive
 */

void shell_command_attrib(int argc, char **argv)
{
    shell_sd_session_t session;
    esp_err_t error;
    char resolved[SHELL_SD_PATH_BYTES];
    char fatfs_path[SHELL_SD_PATH_BYTES];
    FILINFO info;
    FRESULT fr;
    BYTE attr = 0;
    bool set_attr = false;
    char attr_char = 0;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("attrib: SD card not present");
        return;
    }

    /* Parse attribute operation: +R, -R, +H, -H, +S, -S, +A, -A */
    if (argc >= 2 && (argv[1][0] == '+' || argv[1][0] == '-') &&
        argv[1][1] != '\0' && argv[1][2] == '\0') {
        set_attr = (argv[1][0] == '+');
        attr_char = (char)toupper((unsigned char)argv[1][1]);

        switch (attr_char) {
        case 'R': attr = AM_RDO; break;
        case 'H': attr = AM_HID; break;
        case 'S': attr = AM_SYS; break;
        case 'A': attr = AM_ARC; break;
        default:
            shell_print_error("attrib: invalid attribute. Use R, H, S, or A");
            shell_sd_end(&session, "attrib");
            return;
        }

        if (argc < 3) {
            shell_transcript_append_text("attrib: path required when setting/clearing attributes\n");
            shell_sd_end(&session, "attrib");
            return;
        }

        error = shell_sd_resolve_path(argv[2], resolved, sizeof(resolved));
        if (error == ESP_OK) {
            error = shell_sd_vfs_to_fatfs_path(resolved, fatfs_path, sizeof(fatfs_path));
        }
        if (error != ESP_OK) {
            shell_print_error("attrib: invalid path");
            shell_sd_end(&session, "attrib");
            return;
        }

        fr = f_stat(fatfs_path, &info);
        if (fr != FR_OK) {
            shell_print_error("attrib: cannot access %s", argv[2]);
            shell_sd_end(&session, "attrib");
            return;
        }

        if (set_attr) info.fattrib |= attr;
        else           info.fattrib &= ~attr;

        fr = f_chmod(fatfs_path, info.fattrib, AM_RDO | AM_HID | AM_SYS | AM_ARC);
        if (fr != FR_OK) {
            shell_transcript_appendf("attrib: failed to change attributes\n");
        } else {
            shell_transcript_appendf("attrib: %c%c set for %s\n",
                                     set_attr ? '+' : '-', attr_char, argv[2]);
        }
        shell_sd_end(&session, "attrib");
        return;
    }

    /* Show attributes for a file or directory listing */
    if (argc < 2) {
        shell_print_usage("Usage: attrib [path] | attrib [+-RHSA] <file>");
        shell_sd_end(&session, "attrib");
        return;
    }
    const char *path = argv[1];
    error = shell_sd_resolve_path(path, resolved, sizeof(resolved));
    if (error == ESP_OK) {
        error = shell_sd_vfs_to_fatfs_path(resolved, fatfs_path, sizeof(fatfs_path));
    }
    if (error != ESP_OK) {
        shell_print_error("attrib: invalid path");
        shell_sd_end(&session, "attrib");
        return;
    }

    fr = f_stat(fatfs_path, &info);
    if (fr == FR_OK && !(info.fattrib & AM_DIR)) {
        char a[5] = {
            (info.fattrib & AM_RDO) ? 'R' : '-',
            (info.fattrib & AM_HID) ? 'H' : '-',
            (info.fattrib & AM_SYS) ? 'S' : '-',
            (info.fattrib & AM_ARC) ? 'A' : '-', 0
        };
        shell_transcript_appendf("  %s  %s\n", a, info.fname);
    } else {
        DIR *d = opendir(resolved);
        if (d == NULL) {
            shell_print_error("attrib: cannot access %s", path);
            shell_sd_end(&session, "attrib");
            return;
        }
        struct dirent *entry;
        int count = 0;
        while ((entry = readdir(d)) != NULL && count < SHELL_SD_LIST_LIMIT) {
            char fp[SHELL_SD_PATH_BYTES + 256];
            snprintf(fp, sizeof(fp), "%s/%s", fatfs_path, entry->d_name);
            if (f_stat(fp, &info) == FR_OK) {
                char a[5] = {
                    (info.fattrib & AM_RDO) ? 'R' : '-',
                    (info.fattrib & AM_HID) ? 'H' : '-',
                    (info.fattrib & AM_SYS) ? 'S' : '-',
                    (info.fattrib & AM_ARC) ? 'A' : '-', 0
                };
                shell_transcript_appendf("  %s  %s  %s\n", a,
                    (info.fattrib & AM_DIR) ? "<DIR>" : "     ",
                    entry->d_name);
                count++;
            }
        }
        closedir(d);
    }
    shell_sd_end(&session, "attrib");
}

/* ========================================================================
 * VOLUME LABEL COMMAND (label)
 * ========================================================================
 * Reads and sets the FATFS volume label using f_getlabel()/f_setlabel().
 *
 * Usage:
 *   label                Show current volume label
 *   label <name>         Set volume label (max 11 chars, FAT 8.3 convention)
 */

void shell_command_label(int argc, char **argv)
{
    shell_sd_session_t session;
    esp_err_t error;
    char label[12];
    FRESULT fr;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("label: SD card not present");
        return;
    }

    if (argc >= 2) {
        char drive_label[16];
        if (strlen(argv[1]) > 11) {
            shell_transcript_append_text("label: volume name must be 11 characters or fewer\n");
            shell_sd_end(&session, "label");
            return;
        }
        memset(label, ' ', 11);
        label[11] = '\0';
        size_t len = strlen(argv[1]);
        for (size_t i = 0; i < len && i < 11; i++)
            label[i] = (char)toupper((unsigned char)argv[1][i]);
        snprintf(drive_label, sizeof(drive_label), "%s%s", SHELL_SD_FATFS_DRIVE, label);
        fr = f_setlabel(drive_label);
        if (fr == FR_OK)
            shell_transcript_appendf("label: volume label set to \"%s\"\n", argv[1]);
        else
            shell_transcript_appendf("label: failed to set volume label\n");
    } else {
        fr = f_getlabel(SHELL_SD_FATFS_DRIVE, label, NULL);
        if (fr == FR_OK) {
            int end = 10;
            while (end >= 0 && label[end] == ' ') end--;
            label[end + 1] = '\0';
            shell_transcript_appendf("label: %s\n",
                                     label[0] ? label : "(no volume label)");
        } else {
            shell_transcript_appendf("label: failed to read volume label\n");
        }
    }
    shell_sd_end(&session, "label");
}

/* ========================================================================
 * RECURSIVE COPY (xcopy)
 * ========================================================================
 * Copies files and directory trees recursively.
 * Usage: xcopy <source> <destination> [/S]
 *   /S  Copy directories and subdirectories (except empty ones)
 */

void shell_command_xcopy(int argc, char **argv)
{
    shell_sd_session_t session;
    esp_err_t error;
    char src_resolved[SHELL_SD_PATH_BYTES];
    char dst_resolved[SHELL_SD_PATH_BYTES];
    bool recursive = false;
    const char *src_arg = NULL;
    const char *dst_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && toupper((unsigned char)argv[i][1]) == 'S')
            recursive = true;
        else if (src_arg == NULL) src_arg = argv[i];
        else if (dst_arg == NULL) dst_arg = argv[i];
    }
    if (src_arg == NULL || dst_arg == NULL) {
        shell_print_usage("Usage: xcopy <source> <destination> [/S]");
        shell_transcript_append_text("  /S  Copy directories and subdirectories\n");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("xcopy: SD card not present");
        return;
    }

    error = shell_sd_resolve_path(src_arg, src_resolved, sizeof(src_resolved));
    if (error != ESP_OK) {
        shell_print_error("xcopy: invalid source path");
        shell_sd_end(&session, "xcopy");
        return;
    }
    error = shell_sd_resolve_path(dst_arg, dst_resolved, sizeof(dst_resolved));
    if (error != ESP_OK) {
        shell_print_error("xcopy: invalid destination path");
        shell_sd_end(&session, "xcopy");
        return;
    }

    struct stat src_st;
    if (stat(src_resolved, &src_st) != 0) {
        shell_transcript_appendf("xcopy: source not found: %s\n", src_arg);
        shell_sd_end(&session, "xcopy");
        return;
    }

    if (S_ISDIR(src_st.st_mode)) {
        if (!recursive) {
            shell_transcript_append_text("xcopy: use /S to copy directories\n");
            shell_sd_end(&session, "xcopy");
            return;
        }
        struct stat dst_st;
        if (stat(dst_resolved, &dst_st) != 0) {
            mkdir(dst_resolved, 0755);

            /* Carry the source directory's attributes onto the one just
             * created, so a hidden or system folder stays that way. Done
             * before descending, because a read-only directory still accepts
             * new children on FAT. */
            (void)storage_copy_attributes(src_resolved, dst_resolved);
        }

        DIR *d = opendir(src_resolved);
        if (d == NULL) {
            shell_print_error("xcopy: cannot open source directory");
            shell_sd_end(&session, "xcopy");
            return;
        }
        struct dirent *entry;
        int copied = 0;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char sc[SHELL_SD_PATH_BYTES + 256], dc[SHELL_SD_PATH_BYTES + 256];
            snprintf(sc, sizeof(sc), "%s/%s", src_resolved, entry->d_name);
            snprintf(dc, sizeof(dc), "%s/%s", dst_resolved, entry->d_name);
            struct stat cs;
            if (stat(sc, &cs) != 0) continue;
            if (S_ISDIR(cs.st_mode)) {
                char *xa[4] = { "xcopy", sc, dc, "/S" };
                shell_command_xcopy(4, xa);
            } else {
                if (shell_fs_copy_file(sc, dc) == ESP_OK) {
                    shell_transcript_appendf("  %s\n", entry->d_name);
                    copied++;
                } else {
                    shell_transcript_appendf("  %s (failed)\n", entry->d_name);
                }
            }
        }
        closedir(d);
        shell_transcript_appendf("xcopy: %d file(s) copied\n", copied);
    } else {
        error = shell_fs_copy_file(src_resolved, dst_resolved);
        if (error == ESP_OK)
            shell_transcript_append_text("xcopy: 1 file copied\n");
        else
            shell_transcript_appendf("xcopy: copy failed (%s)\n", esp_err_to_name(error));
    }
    shell_sd_end(&session, "xcopy");
}

/* ========================================================================
 * VOLUME MANAGEMENT (chkdsk, format)
 * ======================================================================== */

/** Running totals for the optional `chkdsk /F` directory walk. */
typedef struct {
    unsigned int dirs;
    unsigned int files;
    uint64_t bytes;
    unsigned int unreadable;
    bool depth_limited;
} chkdsk_scan_t;

/**
 * Walk one directory verifying that every entry can be stat'ed.
 *
 * This is a read-only integrity check. The firmware deliberately never
 * rewrites FAT structures: a card with real corruption should be imaged and
 * repaired on a host, not modified in place by an embedded shell.
 */
static void shell_chkdsk_walk(const char *dir_path, int depth, chkdsk_scan_t *scan)
{
    /* Heap-allocated for the same reason as the `dir` walker: this recurses
     * once per directory level on the shared command worker task stack. */
    struct chkdsk_level_scratch {
        char fatfs_path[SHELL_SD_PATH_BYTES];
        FF_DIR dir;
        FILINFO info;
    } *scratch = NULL;
    char (*subdirs)[SHELL_LFN_BYTES] = NULL;
    size_t subdir_count = 0;
    FRESULT result;

    if (depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX) {
        scan->depth_limited = true;
        return;
    }

    scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL) {
        shell_print_error("chkdsk: out of memory scanning the directory");
        scan->unreadable++;
        return;
    }

    if (shell_sd_vfs_to_fatfs_path(dir_path, scratch->fatfs_path, sizeof(scratch->fatfs_path)) != ESP_OK) {
        free(scratch);
        scan->unreadable++;
        return;
    }

    result = f_opendir(&scratch->dir, scratch->fatfs_path);
    if (result != FR_OK) {
        free(scratch);
        shell_print_error("chkdsk: cannot open %s (FatFs=%u)", dir_path, (unsigned int)result);
        scan->unreadable++;
        return;
    }

    subdirs = calloc(SHELL_SD_LIST_LIMIT, sizeof(*subdirs));

    while (true) {
        result = f_readdir(&scratch->dir, &scratch->info);
        if (result != FR_OK) {
            shell_print_error("chkdsk: read error in %s (FatFs=%u)", dir_path, (unsigned int)result);
            scan->unreadable++;
            break;
        }

        if (scratch->info.fname[0] == '\0') {
            break;
        }

        if (strcmp(scratch->info.fname, ".") == 0 || strcmp(scratch->info.fname, "..") == 0) {
            continue;
        }

        if (scratch->info.fattrib & AM_DIR) {
            scan->dirs++;
            if (subdirs != NULL && subdir_count < SHELL_SD_LIST_LIMIT) {
                snprintf(subdirs[subdir_count], SHELL_LFN_BYTES, "%s", scratch->info.fname);
                subdir_count++;
            }
        } else {
            scan->files++;
            scan->bytes += (uint64_t)scratch->info.fsize;
        }
    }

    (void)f_closedir(&scratch->dir);
    free(scratch);

    /* Recurse after closing this handle and releasing the buffer, so only
     * one directory handle and one buffer exist at a time regardless of
     * how deep the tree goes. */
    if (subdirs != NULL) {
        size_t index;

        /* child lives on the heap because this function recurses once per
         * directory level: a 320-byte stack copy multiplied by the recursion
         * depth would overflow the 8 KB command worker task stack and corrupt
         * memory (seen as an intermittent crash). */
        char *child = malloc(SHELL_SD_PATH_BYTES);
        if (child == NULL) {
            free(subdirs);
            return;
        }
        for (index = 0; index < subdir_count; index++) {
            if (snprintf(child, SHELL_SD_PATH_BYTES, "%s/%s", dir_path, subdirs[index]) < SHELL_SD_PATH_BYTES) {
                shell_chkdsk_walk(child, depth + 1, scan);
            }
        }
        free(child);
        free(subdirs);
    }
}

void shell_command_chkdsk(int argc, char **argv)
{
    char resolved[SHELL_SD_PATH_BYTES];
    char label[16];
    char text[24];
    shell_sd_session_t session;
    storage_space_info_t space;
    const char *path_arg = NULL;
    bool full_scan = false;
    esp_err_t error;
    int index;

    for (index = 1; index < argc; index++) {
        if (argv[index][0] == '/') {
            if (toupper((unsigned char)argv[index][1]) == 'F' && argv[index][2] == '\0') {
                full_scan = true;
                continue;
            }
            shell_print_error("chkdsk: unknown option %s", argv[index]);
            shell_print_usage("Usage: chkdsk [path] [/F]");
            return;
        }

        if (path_arg == NULL) {
            path_arg = argv[index];
            continue;
        }

        shell_print_usage("Usage: chkdsk [path] [/F]");
        return;
    }

    error = shell_fs_resolve_path(path_arg, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        shell_print_error("chkdsk: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("chkdsk: SD card not present - insert and retry");
        return;
    }

    /* Volume identity first, matching the DOS report layout. */
    if (f_getlabel(SHELL_SD_FATFS_DRIVE, label, NULL) == FR_OK && label[0] != '\0') {
        shell_print_heading("Volume %s created on this card", label);
    } else {
        shell_print_muted("Volume has no label");
    }

    error = storage_get_space_info(&space);
    if (error != ESP_OK) {
        shell_print_error("chkdsk: cannot read the allocation table (%s)", esp_err_to_name(error));
        shell_record_errorf("chkdsk", error, "f_getfree failed");
        shell_sd_end(&session, "chkdsk");
        return;
    }

    /* Capacity figures are pre-formatted to a fixed width before the colour
     * is applied, so the escape bytes never disturb the right alignment. */
    shell_transcript_append_text("\n");
    shell_sd_format_size(space.total_bytes, text, sizeof(text));
    shell_transcript_appendf_ansi(SH_NUM "%16s" SH_RST " " SH_LBL "total disk space" SH_RST "\n", text);
    shell_sd_format_size(space.used_bytes, text, sizeof(text));
    shell_transcript_appendf_ansi(SH_NUM "%16s" SH_RST " " SH_LBL "in use" SH_RST "\n", text);
    shell_sd_format_size(space.free_bytes, text, sizeof(text));
    shell_transcript_appendf_ansi(SH_NUM "%16s" SH_RST " " SH_LBL "available" SH_RST "\n", text);

    shell_transcript_append_text("\n");
    shell_transcript_appendf_ansi(SH_NUM "%16u" SH_RST " " SH_LBL "bytes in each allocation unit" SH_RST "\n",
                                  (unsigned int)space.cluster_bytes);
    shell_transcript_appendf_ansi(SH_NUM "%16u" SH_RST " " SH_LBL "total allocation units" SH_RST "\n",
                                  (unsigned int)space.total_clusters);
    shell_transcript_appendf_ansi(SH_NUM "%16u" SH_RST " " SH_LBL "available allocation units" SH_RST "\n",
                                  (unsigned int)space.free_clusters);

    if (full_scan) {
        chkdsk_scan_t scan = {0};

        shell_transcript_appendf("\nScanning %s ...\n", resolved);
        shell_chkdsk_walk(resolved, 0, &scan);

        shell_transcript_append_text("\n");
        shell_transcript_appendf("%16u director%s scanned\n", scan.dirs, scan.dirs == 1 ? "y" : "ies");
        shell_sd_format_size(scan.bytes, text, sizeof(text));
        shell_transcript_appendf("%16u file(s), %s\n", scan.files, text);

        if (scan.depth_limited) {
            shell_transcript_appendf("chkdsk: stopped descending past %d levels\n",
                                     P4_CONFIG_DIR_RECURSE_DEPTH_MAX);
        }

        if (scan.unreadable > 0) {
            shell_transcript_appendf("chkdsk: %u director%s could not be read\n",
                                     scan.unreadable,
                                     scan.unreadable == 1 ? "y" : "ies");
            shell_record_warningf("chkdsk", "%u unreadable directories under %s", scan.unreadable, resolved);
        } else {
            shell_print_ok("chkdsk: no problems found");
        }
    } else {
        shell_transcript_append_text("\nRun chkdsk /F to also verify every directory is readable\n");
    }

    shell_print_muted("chkdsk: this check is read-only and never rewrites FAT structures");
    shell_sd_end(&session, "chkdsk");
}

/* ========================================================================
 * DESTRUCTIVE-OPERATION CONFIRMATION
 * ========================================================================
 * Shared by `format`, `disk clean`, and `disk delete partition`. Requires the
 * exact confirmation word through the shell key queue and refuses to run when
 * no interactive input source is attached, so a batch file can never wipe the
 * card unattended.
 */

/**
 * Collect the exact confirmation word for a destructive operation.
 *
 * @param operation    Command name used in messages and the debug log.
 * @param warning      Bright-red warning line printed before the prompt.
 * @param detail       Optional detail lines (filesystem, label, size), or "".
 * @return true when the user typed the exact confirmation word.
 */
static bool shell_confirm_destructive(const char *operation, const char *warning, const char *detail)
{
    char confirm[32];

    shell_transcript_appendf_ansi(SH_ERR "%s" SH_RST "\n", warning);
    if (detail != NULL && detail[0] != '\0') {
        shell_transcript_append_text(detail);
    }
    shell_transcript_appendf("Type %s to continue: ", P4_CONFIG_FORMAT_CONFIRM_WORD);

    if (!shell_key_input_available()) {
        shell_transcript_appendf("\n%s: refused, no interactive input is available to confirm\n", operation);
        shell_record_warningf(operation, "Refused destructive operation with no interactive confirmation source");
        return false;
    }

    {
        size_t length = 0;

        confirm[0] = '\0';
        shell_key_wait_begin();
        while (length + 1 < sizeof(confirm)) {
            char key = '\0';

            if (!shell_wait_for_key(P4_CONFIG_KEY_WAIT_TIMEOUT_MS, &key)) {
                break;
            }
            if (key == '\r' || key == '\n') {
                break;
            }
            if (key == '\b' || key == 0x7F) {
                if (length > 0) {
                    confirm[--length] = '\0';
                }
                continue;
            }
            confirm[length++] = key;
            confirm[length] = '\0';
        }
        shell_key_wait_end();
    }

    shell_transcript_appendf("%s\n", confirm);
    return strcmp(confirm, P4_CONFIG_FORMAT_CONFIRM_WORD) == 0;
}

/**
 * Parse an allocation-unit token with an optional K/M suffix (DOS `/A:4K`).
 * @return true on success.
 */
static bool shell_parse_alloc_unit(const char *text, uint32_t *bytes_out)
{
    char *end;
    unsigned long value;
    uint64_t multiplier = 1;

    if (text == NULL || bytes_out == NULL || *text == '\0') {
        return false;
    }

    value = strtoul(text, &end, 10);
    if (end == text) {
        return false;
    }

    if (*end == 'k' || *end == 'K') {
        multiplier = 1024ULL;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1024ULL * 1024ULL;
        end++;
    }

    if (*end != '\0' || value == 0) {
        return false;
    }

    *bytes_out = (uint32_t)((uint64_t)value * multiplier);
    return true;
}

/**
 * Shared FORMAT.COM / diskpart `format` core: mounts the card if needed,
 * requires the exact confirmation word, formats via storage_format_volume(),
 * and reports the resulting geometry with the semantic palette.
 *
 * @param operation   Command name used in messages ("format" or "disk format").
 * @param fs_type     Requested filesystem ("FAT", "FAT32", "EXFAT", or NULL).
 * @param new_label   Requested volume label, or NULL.
 * @param alloc_unit  Allocation-unit size in bytes (ignored unless @p alloc_set).
 * @param alloc_set   True when the user supplied /A: or au=.
 */
static void shell_format_execute(const char *operation,
                                 const char *fs_type,
                                 const char *new_label,
                                 uint32_t alloc_unit,
                                 bool alloc_set)
{
    storage_format_opts_t opts;
    esp_err_t error;

    memset(&opts, 0, sizeof(opts));
    opts.alloc_unit_bytes = P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES;

    if (!storage_sd_is_mounted()) {
        shell_sd_session_t session;

        if (shell_sd_begin(&session) != ESP_OK) {
            shell_print_error("%s: SD card not present - insert and retry", operation);
            return;
        }
        shell_sd_end(&session, operation);
    }

    if (bsp_sdcard == NULL) {
        shell_print_error("%s: no SD card handle is available", operation);
        shell_record_errorf(operation, ESP_ERR_INVALID_STATE, "bsp_sdcard is NULL");
        return;
    }

    /* Destructive operation: require the exact confirmation word. */
    {
        char detail[SHELL_SD_PATH_BYTES];
        size_t pos = 0;

        detail[0] = '\0';
        if (fs_type != NULL) {
            pos += (size_t)snprintf(detail + pos, sizeof(detail) - pos, "Filesystem: %s\n", fs_type);
        }
        if (new_label != NULL) {
            pos += (size_t)snprintf(detail + pos, sizeof(detail) - pos, "Volume label: %s\n", new_label);
        }
        if (alloc_set) {
            pos += (size_t)snprintf(detail + pos, sizeof(detail) - pos, "Allocation unit: %u bytes\n",
                                    (unsigned)alloc_unit);
        }

        if (!shell_confirm_destructive(operation,
                                       "WARNING: This will erase ALL data on the SD card.",
                                       detail)) {
            shell_print_warning("%s: cancelled, the card was not modified", operation);
            shell_record_warningf(operation, "Cancelled %s by user", operation);
            return;
        }
    }

    shell_print_warning("%s: formatting, do not remove the card ...", operation);

    if (alloc_set) {
        opts.alloc_unit_bytes = alloc_unit;
    }
    if (new_label != NULL) {
        snprintf(opts.label, sizeof(opts.label), "%s", new_label);
    }

    error = storage_format_volume(STORAGE_VOLUME_SD, &opts);
    if (error != ESP_OK) {
        shell_print_error("%s: failed (%s)", operation, esp_err_to_name(error));
        shell_record_errorf(operation, error, "SD card format failed");
        return;
    }

    shell_print_ok("%s: complete", operation);
    shell_record_infof(operation, "SD card reformatted");

    /* Report the resulting geometry so the user sees the outcome. Colours
     * match chkdsk: magenta numbers, cyan labels. */
    {
        storage_space_info_t space;
        const char *fat_type = storage_get_fat_type();

        shell_transcript_appendf_ansi(SH_HEAD "Volume" SH_RST " %s (%s)\n",
                                      new_label != NULL ? new_label : SHELL_SD_FATFS_DRIVE,
                                      fat_type);
        if (storage_get_space_info(&space) == ESP_OK) {
            char text[24];

            shell_transcript_append_text("\n");
            shell_sd_format_size(space.total_bytes, text, sizeof(text));
            shell_transcript_appendf_ansi(SH_NUM "%16s" SH_RST " " SH_LBL "total disk space" SH_RST "\n", text);
            shell_sd_format_size(space.free_bytes, text, sizeof(text));
            shell_transcript_appendf_ansi(SH_NUM "%16s" SH_RST " " SH_LBL "available" SH_RST "\n", text);
            shell_transcript_appendf_ansi(SH_NUM "%16u" SH_RST " " SH_LBL "bytes in each allocation unit" SH_RST "\n",
                                          (unsigned int)space.cluster_bytes);
        }
    }
}

void shell_command_format(int argc, char **argv)
{
    const char *fs_type = NULL;
    const char *new_label = NULL;
    bool alloc_set = false;
    uint32_t alloc_unit = P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES;
    int index;

    for (index = 1; index < argc; index++) {
        const char *token = argv[index];
        char flag;
        const char *value;

        if (token[0] != '/') {
            shell_print_error("format: unexpected argument %s", token);
            shell_print_usage("Usage: format [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q]");
            return;
        }

        /* /FS: needs two letters to disambiguate from a future /F. */
        if (toupper((unsigned char)token[1]) == 'F' && toupper((unsigned char)token[2]) == 'S') {
            value = token + 3;
            if (*value == ':' || *value == '=') {
                value++;
            }
            if (*value == '\0') {
                shell_print_error("format: /FS needs a type, for example /FS:FAT32");
                return;
            }
            fs_type = value;
            continue;
        }

        flag = (char)toupper((unsigned char)token[1]);
        value = token + 2;
        if (*value == ':' || *value == '=') {
            value++;
        }

        switch (flag) {
        case 'V':
            if (*value == '\0') {
                shell_print_error("format: /V needs a label, for example /V:DATA");
                return;
            }
            if (strlen(value) > 11) {
                shell_print_error("format: volume label must be 11 characters or fewer");
                return;
            }
            new_label = value;
            continue;
        case 'A':
            if (*value == '\0') {
                shell_print_error("format: /A needs a size, for example /A:32K");
                return;
            }
            if (!shell_parse_alloc_unit(value, &alloc_unit) ||
                alloc_unit < P4_CONFIG_FORMAT_ALLOC_UNIT_MIN ||
                alloc_unit > P4_CONFIG_FORMAT_ALLOC_UNIT_MAX) {
                shell_print_error("format: invalid /A allocation unit size %s", value);
                shell_print_usage("Usage: format [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q]");
                return;
            }
            alloc_set = true;
            continue;
        case 'Q':
            /* Accepted for DOS familiarity. The ESP-IDF helper always does
             * the equivalent of a quick format, so this is a no-op rather
             * than a silent lie about doing a surface scan. */
            continue;
        default:
            shell_print_error("format: unknown option %s", token);
            shell_print_usage("Usage: format [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q]");
            return;
        }
    }

    /* Validate the filesystem type before warning the user, so a typo does
     * not get as far as the confirmation prompt. FAT/FAT32 use the standard
     * IDF helper's size-appropriate selection (FAT12/16 for small volumes,
     * FAT32 for modern SD cards). exFAT cannot be created in this firmware
     * build (FF_FS_EXFAT is off) - warn honestly and format as FAT32 rather
     * than pretending. */
    if (fs_type != NULL) {
        if (shell_text_equals_ignore_case(fs_type, "EXFAT")) {
            shell_print_warning("format: exFAT is not supported in this firmware build");
            shell_transcript_append_text("  Formatting as FAT32 instead.\n");
        } else if (!shell_text_equals_ignore_case(fs_type, "FAT") &&
                   !shell_text_equals_ignore_case(fs_type, "FAT32")) {
            shell_print_error("format: unsupported filesystem type %s", fs_type);
            shell_transcript_append_text("  Supported: FAT, FAT32\n");
            return;
        }
    }

    shell_format_execute("format", fs_type, new_label, alloc_unit, alloc_set);
}

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

/**
 * `find` — search a text file for a literal substring.
 *
 * Usage: find "text" [file] [/I] [/N] [/C] [/V]
 *   /I  Case-insensitive match
 *   /N  Prefix each match with its line number
 *   /C  Print only the match count
 *   /V  Print the lines that do NOT match
 *
 * With no file argument the pending `<` or pipe input source is used, so
 * `type notes.txt | find "error"` works the way DOS users expect.
 */
void shell_command_find(int argc, char **argv)
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
                return;
            }
        }

        if (search == NULL) {
            search = argv[index];
        } else if (file_arg == NULL) {
            file_arg = argv[index];
        } else {
            shell_print_usage("Usage: find <text> [file] [/I] [/N] [/C] [/V]");
            return;
        }
    }

    if (search == NULL) {
        shell_print_usage("Usage: find <text> [file] [/I] [/N] [/C] [/V]");
        return;
    }

    error = storage_resolve_input_source(file_arg, resolved, sizeof(resolved));
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_append_text("find: no input file given and no input redirection active\n");
        shell_print_usage("Usage: find <text> [file] [/I] [/N] [/C] [/V]");
        return;
    }
    if (error != ESP_OK) {
        shell_print_error("find: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("find: SD card not present - insert and retry");
        return;
    }

    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_print_error("find: cannot open %s (%s)", resolved, strerror(errno));
        shell_sd_end(&session, "find");
        return;
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
}

/**
 * `more` — page a text file, waiting for a keypress between pages.
 *
 * At the `-- More --` prompt: Enter or Space advances one page, `Q` quits.
 * When no interactive key source is attached the command falls back to the
 * configured page delay so a headless batch run still completes.
 */
void shell_command_more(int argc, char **argv)
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
        return;
    }

    error = storage_resolve_input_source(argc >= 2 ? argv[1] : NULL, resolved, sizeof(resolved));
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_append_text("more: no input file given and no input redirection active\n");
        shell_print_usage("Usage: more [file]");
        return;
    }
    if (error != ESP_OK) {
        shell_print_error("more: invalid path");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("more: SD card not present - insert and retry");
        return;
    }

    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_print_error("more: cannot open %s (%s)", resolved, strerror(errno));
        shell_sd_end(&session, "more");
        return;
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
}

/**
 * `fc` — compare two text files line by line.
 *
 * Reports the first differing line pair in DOS style and keeps reading so a
 * trailing length difference is also reported. Output is bounded by the
 * transcript itself; the comparison is not.
 */
void shell_command_fc(int argc, char **argv)
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
        return;
    }

    error = shell_fs_resolve_path(argv[1], resolved1, sizeof(resolved1));
    if (error != ESP_OK) {
        shell_print_error("fc: invalid path for the first file");
        return;
    }

    error = shell_fs_resolve_path(argv[2], resolved2, sizeof(resolved2));
    if (error != ESP_OK) {
        shell_print_error("fc: invalid path for the second file");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("fc: SD card not present - insert and retry");
        return;
    }

    file1 = fopen(resolved1, "r");
    file2 = fopen(resolved2, "r");
    if (file1 == NULL || file2 == NULL) {
        shell_print_error("fc: cannot open %s", file1 == NULL ? resolved1 : resolved2);
        if (file1 != NULL) fclose(file1);
        if (file2 != NULL) fclose(file2);
        shell_sd_end(&session, "fc");
        return;
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
    } else {
        shell_transcript_appendf("fc: %d difference(s)\n", diffs);
    }
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
void shell_command_sort(int argc, char **argv)
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
                return;
            }
        }

        if (file_arg == NULL) {
            file_arg = argv[index];
        } else {
            shell_print_usage("Usage: sort [file] [/R] [/I] [/U]");
            return;
        }
    }

    error = storage_resolve_input_source(file_arg, resolved, sizeof(resolved));
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_append_text("sort: no input file given and no input redirection active\n");
        shell_print_usage("Usage: sort [file] [/R] [/I] [/U]");
        return;
    }
    if (error != ESP_OK) {
        shell_print_error("sort: invalid path");
        return;
    }

    lines = calloc(SHELL_SORT_LINE_MAX, sizeof(*lines));
    if (lines == NULL) {
        shell_transcript_append_text("sort: out of memory\n");
        shell_record_errorf("sort", ESP_ERR_NO_MEM, "Out of memory allocating the line table");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        free(lines);
        shell_print_error("sort: SD card not present - insert and retry");
        return;
    }

    file = fopen(resolved, "r");
    if (file == NULL) {
        shell_print_error("sort: cannot open %s (%s)", resolved, strerror(errno));
        shell_sd_end(&session, "sort");
        free(lines);
        return;
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
}

/* ========================================================================
 * SD COMMAND FAMILY
 * ======================================================================== */

static void shell_sd_print_usage(void)
{
    shell_print_usage("Usage:");
    shell_transcript_append_text("  sd info\n");
    shell_transcript_append_text("  sd ls [path]\n");
    shell_transcript_append_text("  sd stat <path>\n");
    shell_transcript_append_text("  sd cat <path> [max_bytes]\n");
    shell_transcript_append_text("  sd eject          (safe unmount before card removal)\n");
    shell_transcript_append_text("  sdeject           (alias for sd eject)\n");
    shell_transcript_append_text("Paths: sd:/file.txt, /sdcard/file.txt, or relative-to-sd-root\n");
}

static void shell_command_sd_info(void)
{
    shell_sd_session_t session;
    esp_err_t error;
    struct stat root_stat;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD info mount failed");
        return;
    }

    shell_print_field("sd.mount_point:", "%s", BSP_SD_MOUNT_POINT);
    if (bsp_sdcard != NULL) {
        char capacity_text[32];
        uint64_t capacity_bytes = (uint64_t)bsp_sdcard->csd.capacity * (uint64_t)bsp_sdcard->csd.sector_size;

        shell_sd_format_size(capacity_bytes, capacity_text, sizeof(capacity_text));
        shell_print_field("sd.card_name:", "%s", bsp_sdcard->cid.name);
        shell_print_field_num("sd.sector_size:", (long)bsp_sdcard->csd.sector_size);
        shell_print_field("sd.capacity:", "%s", capacity_text);
        shell_print_field_num("sd.max_freq_khz:", (long)bsp_sdcard->max_freq_khz);
    } else {
        shell_print_muted("sd.card_name: unavailable");
    }

    error = shell_sd_stat_path(BSP_SD_MOUNT_POINT, &root_stat);
    if (error == ESP_OK) {
        shell_print_field("sd.root:", "%s", shell_sd_entry_type(&root_stat));
    } else {
        shell_transcript_appendf("sd.root: unavailable (%s)\n", esp_err_to_name(error));
    }

    /* Filesystem capacity, which is what users actually want before a copy.
     * The card capacity printed above is the raw device size. */
    {
        storage_space_info_t space;

        if (storage_get_space_info(&space) == ESP_OK) {
            char text[24];

            shell_sd_format_size(space.total_bytes, text, sizeof(text));
            shell_print_field("sd.fs_total:", "%s", text);
            shell_sd_format_size(space.used_bytes, text, sizeof(text));
            shell_print_field("sd.fs_used:", "%s", text);
            shell_sd_format_size(space.free_bytes, text, sizeof(text));
            shell_print_field("sd.fs_free:", "%s", text);
            shell_print_field_num("sd.cluster_bytes:", (long)space.cluster_bytes);
        }
    }

    shell_sd_end(&session, "sd info");
}

static void shell_command_sd_ls(char *command)
{
    char *argv[4];
    int argc = shell_split_args(command, argv, 4);
    const char *input_path = argc >= 3 ? argv[2] : BSP_SD_MOUNT_POINT;
    char normalized_path[SHELL_SD_PATH_BYTES];
    char fatfs_path[SHELL_SD_PATH_BYTES];
    char lfn[SHELL_LFN_BYTES];
    char size_text[32];
    struct stat path_stat;
    FF_DIR dir;
    FILINFO entry_info;
    FRESULT result;
    esp_err_t error;
    size_t listed_entries = 0;
    shell_sd_session_t session;
    bool dir_open = false;

    if (argc > 3) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd ls command");
        return;
    }

    error = shell_sd_resolve_path(input_path, normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd ls");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD card not present or mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        shell_transcript_appendf("sd: %s type=%s size=%s\n",
                                 normalized_path,
                                 shell_sd_entry_type(&path_stat),
                                 size_text);
        goto cleanup;
    }

    error = shell_sd_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        shell_print_error("sd: invalid FAT path for %s", normalized_path);
        shell_record_errorf("sd", error, "Could not convert directory path %s to FatFs path", normalized_path);
        goto cleanup;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        error = shell_sd_fresult_to_esp_err(result);
        shell_print_error("sd: could not open %s (FatFs=%u)",
                                 normalized_path,
                                 (unsigned int)result);
        shell_record_errorf("sd", error, "Could not open directory %s (FatFs=%u)", normalized_path, (unsigned int)result);
        goto cleanup;
    }
    dir_open = true;

    shell_transcript_appendf("sd: listing %s\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = shell_sd_fresult_to_esp_err(result);
            shell_transcript_appendf("sd: directory read failed for %s (FatFs=%u)\n",
                                     normalized_path,
                                     (unsigned int)result);
            shell_record_errorf("sd", error, "Directory read failed for %s (FatFs=%u)", normalized_path, (unsigned int)result);
            break;
        }

        if (entry_info.fname[0] == '\0') {
            break;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        if (strcmp(lfn, ".") == 0 || strcmp(lfn, "..") == 0) {
            continue;
        }

        if (listed_entries == SHELL_SD_LIST_LIMIT) {
            shell_transcript_appendf("sd: listing truncated after %u entries\n", (unsigned int)SHELL_SD_LIST_LIMIT);
            shell_record_warningf("sd", "Truncated directory listing for %s", normalized_path);
            break;
        }

        shell_sd_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
        shell_transcript_appendf("sd: %-4s %s (%s)\n",
                                 (entry_info.fattrib & AM_DIR) ? "DIR" : "FILE",
                                 lfn,
                                 (entry_info.fattrib & AM_DIR) ? "-" : size_text);
        listed_entries++;
    }

cleanup:
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    shell_sd_end(&session, "sd ls");
}

static void shell_command_sd_stat(char *command)
{
    char *argv[4];
    int argc = shell_split_args(command, argv, 4);
    char normalized_path[SHELL_SD_PATH_BYTES];
    char size_text[32];
    struct stat path_stat;
    esp_err_t error;
    shell_sd_session_t session;

    if (argc != 3) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd stat command");
        return;
    }

    error = shell_sd_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd stat");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD stat mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        shell_sd_end(&session, "sd stat");
        return;
    }

    shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
    shell_print_field("sd.path:", "%s", normalized_path);
    shell_print_field("sd.type:", "%s", shell_sd_entry_type(&path_stat));
    shell_print_field("sd.size:", "%s", size_text);
    shell_transcript_appendf("sd.mode: 0%o\n", (unsigned int)(path_stat.st_mode & 0777));

    shell_sd_end(&session, "sd stat");
}

static void shell_command_sd_cat(char *command)
{
    char *argv[5];
    int argc = shell_split_args(command, argv, 5);
    char normalized_path[SHELL_SD_PATH_BYTES];
    struct stat path_stat;
    esp_err_t error;
    shell_sd_session_t session;
    FILE *file = NULL;
    size_t max_bytes = SHELL_SD_CAT_DEFAULT_BYTES;
    size_t displayed_bytes = 0;
    bool truncated = false;
    bool ended_with_newline = false;
    unsigned char buffer[SHELL_SD_IO_BUFFER_BYTES + 1];

    if (argc < 3 || argc > 4) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd cat command");
        return;
    }

    if (argc == 4 && !shell_parse_size_arg(argv[3], 1, SHELL_SD_CAT_MAX_BYTES, &max_bytes)) {
        shell_transcript_appendf("sd: max_bytes must be between 1 and %u\n", (unsigned int)SHELL_SD_CAT_MAX_BYTES);
        shell_record_warningf("sd", "Rejected invalid max_bytes for sd cat");
        return;
    }

    error = shell_sd_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd cat");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD cat mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        shell_transcript_appendf("sd: %s is not a regular file\n", normalized_path);
        shell_record_warningf("sd", "Rejected non-file path for sd cat: %s", normalized_path);
        goto cleanup;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        shell_print_error("sd: could not open %s (%s)", normalized_path, strerror(errno));
        shell_record_errorf("sd", ESP_FAIL, "Could not open file %s", normalized_path);
        goto cleanup;
    }

    shell_transcript_appendf("sd: preview %s (%u bytes max)\n", normalized_path, (unsigned int)max_bytes);
    while (displayed_bytes < max_bytes) {
        size_t index;
        size_t bytes_to_read = MIN(sizeof(buffer) - 1, max_bytes - displayed_bytes);
        size_t bytes_read = fread(buffer, 1, bytes_to_read, file);

        if (bytes_read == 0) {
            break;
        }

        for (index = 0; index < bytes_read; index++) {
            if (buffer[index] == '\n' || buffer[index] == '\r' || buffer[index] == '\t') {
                continue;
            }
            if (!isprint(buffer[index])) {
                buffer[index] = '.';
            }
        }
        buffer[bytes_read] = '\0';
        ended_with_newline = bytes_read > 0 && buffer[bytes_read - 1] == '\n';

        shell_transcript_appendf("%s", (char *)buffer);
        displayed_bytes += bytes_read;
        if (bytes_read < bytes_to_read) {
            break;
        }
    }

    if (displayed_bytes == 0) {
        shell_transcript_append_text("sd: file is empty\n");
    } else if (!feof(file)) {
        truncated = true;
    }

    if (displayed_bytes > 0 && !ended_with_newline) {
        shell_transcript_append_text("\n");
    }

    if (truncated) {
        shell_transcript_appendf("sd: preview truncated at %u bytes\n", (unsigned int)max_bytes);
    }

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    shell_sd_end(&session, "sd cat");
}

/**
 * `sd` command family entry point.
 *
 * Grouping the storage operations behind one verb lets them share validated
 * argument parsing, bounded output, and a consistent cleanup path. Receives
 * the original unsplit command text because the subcommands re-tokenize it.
 */
void shell_command_sd(char *command)
{
    char *argv[5];
    int argc;
    char *cmd_copy = (command != NULL) ? strdup(command) : NULL;

    /* shell_split_args() writes token terminators into the buffer in place,
     * so `command` would be truncated to just "sd" for the sub-handlers.
     * Hand them a preserved heap copy instead. */
    argc = shell_split_args(command, argv, 5);

    if (argc <= 1) {
        shell_command_sd_info();
        shell_sd_print_usage();
    } else if (strcmp(argv[1], "help") == 0) {
        shell_sd_print_usage();
    } else if (strcmp(argv[1], "info") == 0) {
        if (argc != 2) {
            shell_sd_print_usage();
            shell_record_warningf("sd", "Usage error for sd info command");
        } else {
            shell_command_sd_info();
        }
    } else if (strcmp(argv[1], "ls") == 0) {
        shell_command_sd_ls(cmd_copy);
    } else if (strcmp(argv[1], "stat") == 0) {
        shell_command_sd_stat(cmd_copy);
    } else if (strcmp(argv[1], "cat") == 0) {
        shell_command_sd_cat(cmd_copy);
    } else if (strcmp(argv[1], "eject") == 0) {
        shell_command_sd_eject();
    } else {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Unknown sd subcommand: %s", argv[1]);
    }

    free(cmd_copy);
}

/* ========================================================================
 * DISK COMMAND FAMILY (diskpart-style disk and partition management)
 * ========================================================================
 * Physical-disk operations on the SD card: geometry, MBR partition-table
 * inspection and editing, and a diskpart-style format alias. The storage
 * module owns the low-level mechanics; these handlers own parsing and the
 * destructive-confirmation contract. USB OTG MSC is a future target - the
 * storage_volume_t plumbing is already in place for it.
 */

static void shell_disk_print_usage(void)
{
    shell_print_usage("Usage:");
    shell_transcript_append_text("  disk list                     show the physical disk(s)\n");
    shell_transcript_append_text("  disk detail                   show disk geometry and the MBR partition table\n");
    shell_transcript_append_text("  disk clean                    remove the partition table (destructive)\n");
    shell_transcript_append_text("  disk create partition primary [size=N]  create a primary partition (N in MB)\n");
    shell_transcript_append_text("  disk delete partition N       delete MBR partition N (1-4)\n");
    shell_transcript_append_text("  disk format [fs=FAT32] [label=X] [au=size] [quick]   format the volume (diskpart style)\n");
    shell_transcript_append_text("After clean or create, run 'format' (or 'disk format') to create the filesystem.\n");
}

/** Friendly name for an MBR partition-type byte. */
static const char *shell_disk_partition_type_name(uint8_t type)
{
    switch (type) {
    case 0x01:
    case 0x04:
    case 0x06:
    case 0x0E:
        return "FAT12/FAT16";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x05:
    case 0x0F:
        return "Extended";
    case 0x07:
        return "NTFS/exFAT";
    default:
        return "Unknown";
    }
}

static void shell_disk_report_geometry(const storage_disk_info_t *info)
{
    char capacity_text[32];

    shell_sd_format_size(info->capacity_bytes, capacity_text, sizeof(capacity_text));
    shell_print_field("disk.number:", "%u", 0);
    shell_print_field("disk.name:", "%s", info->card_name);
    shell_print_field("disk.capacity:", "%s", capacity_text);
    shell_print_field_num("disk.sector_size:", (long)info->sector_size);
    shell_print_field_num("disk.sector_count:", (long)info->sector_count);
    shell_print_field_num("disk.max_freq_khz:", (long)info->max_freq_khz);
}

static void shell_command_disk_list(void)
{
    storage_disk_info_t info;
    esp_err_t error;

    error = storage_disk_get_info(STORAGE_VOLUME_SD, &info);
    if (error != ESP_OK) {
        shell_print_error("disk: no SD card available (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk list could not read the card");
        return;
    }

    shell_print_heading("Physical Disks");
    shell_disk_report_geometry(&info);
}

static void shell_command_disk_detail(void)
{
    storage_disk_info_t info;
    storage_mbr_t mbr;
    esp_err_t error;
    int index;

    error = storage_disk_get_info(STORAGE_VOLUME_SD, &info);
    if (error != ESP_OK) {
        shell_print_error("disk: no SD card available (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk detail could not read the card");
        return;
    }

    shell_print_heading("Disk 0 - %s", info.card_name);
    shell_disk_report_geometry(&info);

    error = storage_disk_read_mbr(STORAGE_VOLUME_SD, &mbr);
    if (error != ESP_OK) {
        shell_print_error("disk: could not read the partition table (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk detail MBR read failed");
        return;
    }

    shell_print_heading("MBR Partition Table");
    if (!mbr.valid) {
        shell_print_warning("disk: no valid MBR partition table (0x55AA signature missing)");
        shell_transcript_append_text("  Run 'disk create partition primary' then 'format' to create one.\n");
        return;
    }

    for (index = 0; index < 4; index++) {
        const storage_partition_t *part = &mbr.partitions[index];
        char size_text[24];

        if (part->type == 0 && part->size_lba == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "  Partition %d: <unused>" SH_RST "\n", index + 1);
            continue;
        }

        shell_sd_format_size((uint64_t)part->size_lba * (uint64_t)info.sector_size,
                             size_text, sizeof(size_text));
        shell_transcript_appendf_ansi("  " SH_LBL "Partition %d:" SH_RST
                                      " " SH_USAGE "type=0x%02X %s" SH_RST
                                      " " SH_LBL "start" SH_RST "=" SH_NUM "%" PRIu32 SH_RST
                                      " " SH_LBL "size" SH_RST "=" SH_NUM "%s" SH_RST "%s\n",
                                      index + 1,
                                      part->type, shell_disk_partition_type_name(part->type),
                                      part->start_lba, size_text,
                                      part->bootable ? " (boot)" : "");
    }
}

static void shell_command_disk_clean(void)
{
    esp_err_t error;

    if (!shell_confirm_destructive("disk clean",
                                   "WARNING: This will remove ALL partitions on the SD card.",
                                   "  Run 'disk create partition primary' then 'format' to recreate a filesystem.\n")) {
        shell_print_warning("disk: clean cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk clean by user");
        return;
    }

    error = storage_disk_clean(STORAGE_VOLUME_SD);
    if (error != ESP_OK) {
        shell_print_error("disk: clean failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk clean failed");
        return;
    }

    shell_print_ok("disk: partition table removed");
    shell_transcript_append_text("  Run 'disk create partition primary' then 'format' to recreate a filesystem.\n");
}

/**
 * Parse a partition-size token. diskpart uses megabytes by default; an
 * optional K/M/G suffix overrides that. @p bytes_out receives bytes.
 */
static bool shell_disk_parse_partition_size(const char *text, uint64_t *bytes_out)
{
    char *end;
    unsigned long value;
    uint64_t multiplier = 1024ULL * 1024ULL;

    if (text == NULL || bytes_out == NULL || *text == '\0') {
        return false;
    }

    value = strtoul(text, &end, 10);
    if (end == text) {
        return false;
    }

    if (*end == 'k' || *end == 'K') {
        multiplier = 1024ULL;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1024ULL * 1024ULL;
        end++;
    } else if (*end == 'g' || *end == 'G') {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        end++;
    }

    if (*end != '\0' || value == 0) {
        return false;
    }

    *bytes_out = (uint64_t)value * multiplier;
    return true;
}

static void shell_command_disk_create(int argc, char **argv)
{
    uint64_t size_bytes = 0;
    esp_err_t error;
    int index;

    /* disk create partition primary [size=N] */
    if (argc < 4 || strcmp(argv[2], "partition") != 0) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk create command");
        return;
    }
    if (strcmp(argv[3], "primary") != 0) {
        shell_print_error("disk: only 'primary' partitions are supported");
        shell_record_warningf("disk", "Unsupported partition type: %s", argv[3]);
        return;
    }

    for (index = 4; index < argc; index++) {
        if (strncmp(argv[index], "size=", 5) == 0) {
            if (!shell_disk_parse_partition_size(argv[index] + 5, &size_bytes)) {
                shell_print_error("disk: invalid partition size %s", argv[index] + 5);
                return;
            }
        } else {
            shell_print_error("disk: unexpected argument %s", argv[index]);
            shell_disk_print_usage();
            return;
        }
    }

    error = storage_disk_create_primary_partition(STORAGE_VOLUME_SD, size_bytes);
    if (error == ESP_ERR_INVALID_STATE) {
        shell_print_error("disk: no free partition slot (all 4 MBR entries are used)");
        return;
    }
    if (error != ESP_OK) {
        shell_print_error("disk: create partition failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk create partition failed");
        return;
    }

    shell_print_ok("disk: primary partition created");
    shell_transcript_append_text("  Run 'format' (or 'disk format') to create the filesystem.\n");
}

static void shell_command_disk_delete(int argc, char **argv)
{
    char *end;
    unsigned long partition_index;
    esp_err_t error;

    /* disk delete partition N (1-4) */
    if (argc < 4 || strcmp(argv[2], "partition") != 0) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk delete command");
        return;
    }

    partition_index = strtoul(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' || partition_index < 1 || partition_index > 4) {
        shell_print_error("disk: partition number must be 1-4");
        return;
    }

    if (!shell_confirm_destructive("disk delete",
                                   "WARNING: This will remove partition information from the SD card.",
                                   "")) {
        shell_print_warning("disk: delete cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk delete by user");
        return;
    }

    error = storage_disk_delete_partition(STORAGE_VOLUME_SD, (unsigned)(partition_index - 1));
    if (error != ESP_OK) {
        shell_print_error("disk: delete partition failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk delete partition failed");
        return;
    }

    shell_print_ok("disk: partition %lu removed", partition_index);
    shell_transcript_append_text("  Run 'format' (or 'disk format') to recreate a filesystem.\n");
}

static void shell_command_disk_format(int argc, char **argv)
{
    const char *fs_type = NULL;
    const char *label = NULL;
    uint32_t alloc_unit = P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES;
    bool alloc_set = false;
    int index;

    /* disk format [fs=FAT32] [label=X] [au=size] [quick] */
    for (index = 2; index < argc; index++) {
        const char *token = argv[index];

        if (strncmp(token, "fs=", 3) == 0) {
            fs_type = token + 3;
        } else if (strncmp(token, "label=", 6) == 0) {
            label = token + 6;
            if (strlen(label) > 11) {
                shell_print_error("disk format: volume label must be 11 characters or fewer");
                return;
            }
        } else if (strncmp(token, "au=", 3) == 0) {
            if (!shell_parse_alloc_unit(token + 3, &alloc_unit) ||
                alloc_unit < P4_CONFIG_FORMAT_ALLOC_UNIT_MIN ||
                alloc_unit > P4_CONFIG_FORMAT_ALLOC_UNIT_MAX) {
                shell_print_error("disk format: invalid allocation unit size %s", token + 3);
                return;
            }
            alloc_set = true;
        } else if (strcmp(token, "quick") == 0) {
            /* Accepted for diskpart familiarity; the format is always quick. */
        } else {
            shell_print_error("disk format: unexpected argument %s", token);
            shell_disk_print_usage();
            return;
        }
    }

    /* Same filesystem validation as FORMAT.COM. */
    if (fs_type != NULL) {
        if (shell_text_equals_ignore_case(fs_type, "EXFAT")) {
            shell_print_warning("disk format: exFAT is not supported in this firmware build");
            shell_transcript_append_text("  Formatting as FAT32 instead.\n");
        } else if (!shell_text_equals_ignore_case(fs_type, "FAT") &&
                   !shell_text_equals_ignore_case(fs_type, "FAT32")) {
            shell_print_error("disk format: unsupported filesystem type %s", fs_type);
            shell_transcript_append_text("  Supported: FAT, FAT32\n");
            return;
        }
    }

    shell_format_execute("disk format", fs_type, label, alloc_unit, alloc_set);
}

/**
 * `disk` command family entry point.
 */
void shell_command_disk(char *command)
{
    char *argv[8];
    int argc;

    argc = shell_split_args(command, argv, 8);

    if (argc <= 1) {
        shell_command_disk_list();
        shell_disk_print_usage();
    } else if (strcmp(argv[1], "help") == 0) {
        shell_disk_print_usage();
    } else if (strcmp(argv[1], "list") == 0) {
        shell_command_disk_list();
    } else if (strcmp(argv[1], "detail") == 0) {
        shell_command_disk_detail();
    } else if (strcmp(argv[1], "clean") == 0) {
        shell_command_disk_clean();
    } else if (strcmp(argv[1], "create") == 0) {
        shell_command_disk_create(argc, argv);
    } else if (strcmp(argv[1], "delete") == 0) {
        shell_command_disk_delete(argc, argv);
    } else if (strcmp(argv[1], "format") == 0) {
        shell_command_disk_format(argc, argv);
    } else {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Unknown disk subcommand: %s", argv[1]);
    }
}

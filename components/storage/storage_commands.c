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
#include <strings.h>
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
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
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
static bool shell_find_parse_date(const char *text, uint16_t *fdate_out);

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

/**
 * If @p dest_path is an existing directory, append @p source_path's basename
 * so `copy file dir` / `move file dir` copy INTO the directory, matching DOS.
 * Otherwise leaves @p dest_path untouched (a plain file destination).
 */
static void shell_join_dest_into_dir(char *dest_path, size_t dest_size,
                                     const char *source_path)
{
    struct stat dst_st;
    char joined[SHELL_SD_PATH_BYTES];
    const char *base;
    size_t len = strlen(dest_path);

    while (len > 1 && dest_path[len - 1] == '/') {
        dest_path[--len] = '\0';
    }
    if (stat(dest_path, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
        return;
    }
    base = strrchr(source_path, '/');
    base = (base != NULL) ? base + 1 : source_path;
    if (snprintf(joined, sizeof(joined), "%s/%s", dest_path, base) < (int)sizeof(joined)) {
        snprintf(dest_path, dest_size, "%s", joined);
    }
}

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

    shell_join_dest_into_dir(dest_path, sizeof(dest_path), source_path);

    error = shell_fs_copy_file(source_path, dest_path);
    if (error != ESP_OK) {
        shell_print_error("copy: failed (%s)", esp_err_to_name(error));
        return;
    }

    shell_print_ok("1 file(s) copied to %s", dest_path);
}

int shell_command_del(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    char dir_part[SHELL_SD_PATH_BYTES];
    char pattern_buf[P4_CONFIG_LFN_BYTES];
    const char *pattern = NULL;
    bool recursive = false;
    bool permanent = false;
    int path_index = -1;
    int index;
    shell_sd_session_t session;
    esp_err_t error;
    int deleted = 0;

    /* Parse switches and the path argument. */
    for (index = 1; index < argc; index++) {
        if (argv[index][0] == '/') {
            switch (toupper((unsigned char)argv[index][1])) {
            case 'S': recursive = true; break;
            case 'P':
            case 'F': permanent = true; break;
            default:
                shell_print_usage("Usage: del [/s] [/p|/f|/permanent] <path|pattern>");
                return 2;
            }
        } else if (path_index < 0) {
            path_index = index;
        } else {
            shell_print_usage("Usage: del [/s] [/p|/f|/permanent] <path|pattern>");
            return 2;
        }
    }
    if (path_index < 0) {
        shell_print_usage("Usage: del [/s] [/p|/f|/permanent] <path|pattern>");
        return 2;
    }

    /* Recursive delete destroys data across a tree: require confirmation. */
    if (recursive) {
        char warning[392];
        snprintf(warning, sizeof(warning),
                 "WARNING: Recursive delete of \"%s\"%s.",
                 argv[path_index],
                 permanent ? " (permanent, bypasses .trash)" : " (moves matches to .trash)");
        if (!shell_confirm_destructive("del", warning, "")) {
            shell_print_warning("del: cancelled, nothing was deleted");
            return 1;
        }
    }

    if (!storage_trash_enabled() && !permanent) {
        shell_print_warning("del: recycle bin is disabled; use /p or /permanent to delete");
        return 1;
    }

    /* Resolve a wildcard argument into its directory and filename pattern. */
    if (strchr(argv[path_index], '*') != NULL || strchr(argv[path_index], '?') != NULL) {
        const char *last_sep = strrchr(argv[path_index], '/');

        if (last_sep == NULL) {
            last_sep = strrchr(argv[path_index], '\\');
        }
        if (last_sep != NULL) {
            size_t dir_len = (size_t)(last_sep - argv[path_index]);

            if (dir_len >= sizeof(dir_part)) {
                dir_len = sizeof(dir_part) - 1;
            }
            memcpy(dir_part, argv[path_index], dir_len);
            dir_part[dir_len] = '\0';
            pattern = last_sep + 1;
        } else {
            snprintf(dir_part, sizeof(dir_part), ".");
            pattern = argv[path_index];
        }
        error = shell_fs_resolve_path(dir_part, resolved_path, sizeof(resolved_path));
        if (error != ESP_OK) {
            shell_print_error("del: invalid path");
            return 1;
        }
        snprintf(pattern_buf, sizeof(pattern_buf), "%s", pattern);

        error = storage_trash_delete_pattern(resolved_path, pattern_buf,
                                             recursive, permanent, &deleted);
        if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
            shell_print_error("del: failed (%s)", esp_err_to_name(error));
            shell_record_errorf("del", error, "del pattern failed");
            return 1;
        }
        if (storage_trash_enabled()) {
            storage_trash_enforce_limits();
        }
        if (permanent) {
            shell_print_ok("del: %d file(s) deleted permanently", deleted);
        } else {
            shell_print_ok("del: %d file(s) moved to %s", deleted, storage_trash_path());
        }
        return 0;
    }

    /* Single path. */
    error = shell_fs_resolve_path(argv[path_index], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("del: invalid path");
        return 1;
    }
    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("del: SD card not present - insert and retry");
        return 1;
    }
    {
        struct stat st;

        if (stat(resolved_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            shell_sd_end(&session, "del");
            shell_print_error("del: %s is a directory (use rd)", resolved_path);
            return 1;
        }
    }
    shell_sd_end(&session, "del");

    if (recursive && !permanent) {
        /* A plain file with /s deletes same-named files in subdirectories. */
        const char *base = strrchr(resolved_path, '/');
        char parent[SHELL_SD_PATH_BYTES];

        base = (base != NULL) ? base + 1 : resolved_path;
        if (base == resolved_path) {
            snprintf(parent, sizeof(parent), ".");
        } else {
            size_t len = (size_t)((strrchr(resolved_path, '/')) - resolved_path);
            memcpy(parent, resolved_path, len);
            parent[len] = '\0';
        }
        error = storage_trash_delete_pattern(parent, base, true, permanent, &deleted);
        if (storage_trash_enabled()) {
            storage_trash_enforce_limits();
        }
        if (error != ESP_OK) {
            shell_print_error("del: failed (%s)", esp_err_to_name(error));
            return 1;
        }
        shell_print_ok("del: %d file(s) %s", deleted, permanent ? "deleted permanently" : "moved to .trash");
        return 0;
    }

    error = storage_trash_delete_file(resolved_path, permanent);
    if (error == ESP_ERR_NOT_SUPPORTED) {
        /* Trash disabled: fall back to a plain unlink. */
        error = shell_sd_begin(&session);
        if (error != ESP_OK) {
            return 1;
        }
        if (unlink(resolved_path) != 0) {
            shell_print_error("del: failed to delete %s (%s)", resolved_path, strerror(errno));
            shell_sd_end(&session, "del");
            return 1;
        }
        shell_sd_end(&session, "del");
        shell_print_ok("Deleted %s", resolved_path);
        return 0;
    }
    if (error != ESP_OK) {
        shell_print_error("del: failed to delete %s (%s)", resolved_path, esp_err_to_name(error));
        shell_record_errorf("del", error, "del failed for %s", resolved_path);
        return 1;
    }
    if (storage_trash_enabled()) {
        storage_trash_enforce_limits();
    }
    if (permanent) {
        shell_print_ok("del: deleted %s permanently", resolved_path);
    } else {
        shell_print_ok("del: %s moved to %s", resolved_path, storage_trash_path());
    }
    return 0;
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

int shell_command_rmdir(int argc, char **argv)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    esp_err_t error;
    bool recursive = false;
    bool permanent = false;
    int path_index = -1;
    int index;

    for (index = 1; index < argc; index++) {
        if (argv[index][0] == '/') {
            switch (toupper((unsigned char)argv[index][1])) {
            case 'S': recursive = true; break;
            case 'P':
            case 'F': permanent = true; break;
            default:
                shell_print_usage("Usage: rd [/s] [/p|/f|/permanent] <path>");
                return 2;
            }
        } else if (path_index < 0) {
            path_index = index;
        } else {
            shell_print_usage("Usage: rd [/s] [/p|/f|/permanent] <path>");
            return 2;
        }
    }
    if (path_index < 0) {
        shell_print_usage("Usage: rd [/s] [/p|/f|/permanent] <path>");
        return 2;
    }

    error = shell_fs_resolve_path(argv[path_index], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        shell_print_error("rd: invalid path");
        return 1;
    }

    if (recursive) {
        char warning[392];

        snprintf(warning, sizeof(warning),
                 "WARNING: Recursive removal of directory \"%s\"%s.",
                 resolved_path,
                 permanent ? " (permanent, bypasses .trash)" : " (moves to .trash)");
        if (!shell_confirm_destructive("rd", warning, "")) {
            shell_print_warning("rd: cancelled, nothing was removed");
            return 1;
        }
        if (!storage_trash_enabled() && !permanent) {
            shell_print_warning("rd: recycle bin is disabled; use /p or /permanent to delete");
            return 1;
        }
        error = storage_trash_remove_tree(resolved_path, permanent);
        if (error != ESP_OK) {
            shell_print_error("rd: failed (%s)", esp_err_to_name(error));
            shell_record_errorf("rd", error, "rd /s failed for %s", resolved_path);
            return 1;
        }
        if (storage_trash_enabled()) {
            storage_trash_enforce_limits();
        }
        if (permanent) {
            shell_print_ok("rd: removed %s permanently", resolved_path);
        } else {
            shell_print_ok("rd: %s moved to %s", resolved_path, storage_trash_path());
        }
        return 0;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("rd: SD card not present - insert and retry");
        return 1;
    }

    if (rmdir(resolved_path) != 0) {
        shell_print_error("rd: failed to remove %s (%s)", resolved_path, strerror(errno));
        shell_sd_end(&session, "rd");
        return 1;
    }

    shell_sd_end(&session, "rd");
    shell_print_ok("Removed directory %s", resolved_path);
    return 0;
}

int shell_command_undelete(int argc, char **argv)
{
    esp_err_t error;

    if (argc != 2) {
        shell_print_usage("Usage: undelete <name|index>");
        return 2;
    }
    if (!storage_trash_enabled()) {
        shell_print_error("undelete: recycle bin is disabled");
        return 1;
    }
    error = storage_trash_restore(argv[1]);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("undelete: no trash entry \"%s\"", argv[1]);
        shell_record_warningf("undelete", "Unknown trash entry %s", argv[1]);
        return 1;
    }
    if (error == ESP_ERR_INVALID_STATE) {
        shell_print_error("undelete: the original location of \"%s\" is occupied", argv[1]);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("undelete: restore failed (%s)", esp_err_to_name(error));
        shell_record_errorf("undelete", error, "Restore failed for %s", argv[1]);
        return 1;
    }
    shell_print_ok("undelete: restored %s", argv[1]);
    storage_trash_enforce_limits();
    return 0;
}

int shell_command_trash(int argc, char **argv)
{
    esp_err_t error;

    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "list"))) {
        return (storage_trash_list() == ESP_OK) ? 0 : 1;
    }
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "info")) {
        return (storage_trash_info() == ESP_OK) ? 0 : 1;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "restore")) {
        if (!storage_trash_enabled()) {
            shell_print_error("trash: recycle bin is disabled");
            return 1;
        }
        error = storage_trash_restore(argv[2]);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("trash: no entry \"%s\"", argv[2]);
            return 1;
        }
        if (error == ESP_ERR_INVALID_STATE) {
            shell_print_error("trash: the original location of \"%s\" is occupied", argv[2]);
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("trash: restore failed (%s)", esp_err_to_name(error));
            return 1;
        }
        shell_print_ok("trash: restored %s", argv[2]);
        storage_trash_enforce_limits();
        return 0;
    }
    if (argc == 3 && shell_text_equals_ignore_case(argv[1], "purge")) {
        char warning[160];

        if (!storage_trash_enabled()) {
            shell_print_error("trash: recycle bin is disabled");
            return 1;
        }
        snprintf(warning, sizeof(warning),
                 "WARNING: This will permanently delete trash entry \"%s\".",
                 argv[2]);
        if (!shell_confirm_destructive("trash", warning, "")) {
            shell_print_warning("trash: purge cancelled");
            return 1;
        }
        error = storage_trash_purge(argv[2]);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("trash: no entry \"%s\"", argv[2]);
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("trash: purge failed (%s)", esp_err_to_name(error));
            return 1;
        }
        shell_print_ok("trash: purged %s", argv[2]);
        storage_trash_enforce_limits();
        return 0;
    }
    if (argc == 2 && shell_text_equals_ignore_case(argv[1], "empty")) {
        if (!storage_trash_enabled()) {
            shell_print_error("trash: recycle bin is disabled");
            return 1;
        }
        if (!shell_confirm_destructive("trash",
                                       "WARNING: This will permanently delete ALL items in the recycle bin.", "")) {
            shell_print_warning("trash: empty cancelled");
            return 1;
        }
        error = storage_trash_empty();
        if (error != ESP_OK) {
            shell_print_error("trash: empty failed (%s)", esp_err_to_name(error));
            return 1;
        }
        shell_print_ok("trash: emptied");
        storage_trash_enforce_limits();
        return 0;
    }

    shell_print_usage("Usage: trash [list] | trash info | trash restore <name|index> | trash purge <name|index> | trash empty");
    shell_record_warningf("trash", "Usage error for trash command");
    return 2;
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
    /* The joined text can be a full interactive command line (up to
     * P4_CONFIG_COMMAND_BYTES); a batch-line-sized stack buffer would both
     * overflow the worker path if it ran from a batch file and silently
     * truncate long `write`/`append` text. Heap-allocated, freed on every path. */
    char *text = NULL;
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

    text = malloc(SHELL_COMMAND_BYTES);
    if (text == NULL) {
        shell_transcript_appendf("%s: out of memory\n", append_mode ? "append" : "write");
        return;
    }
    shell_join_args(argv, 2, argc, text, SHELL_COMMAND_BYTES);

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        free(text);
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
            free(text);
            shell_sd_end(&session, append_mode ? "append" : "write");
            return;
        }
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        free(text);
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
        free(text);
        shell_sd_end(&session, append_mode ? "append" : "write");
        return;
    }

    fclose(file);
    free(text);
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

    /* The destination is resolved against the current directory (like copy),
     * not the source's directory: DOS `move dir\file.txt name.txt` lands in
     * the cwd. `ren` keeps the source-relative resolution. */
    error = shell_fs_resolve_path(argv[2], target_path, sizeof(target_path));
    if (error != ESP_OK) {
        shell_print_error("move: invalid destination path");
        return;
    }

    /* A directory destination keeps the source basename (DOS move file dir). */
    shell_join_dest_into_dir(target_path, sizeof(target_path), source_path);

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

/* ========================================================================
 * XCOPY: recursive directory copy with the classic DOS 6.x / WinXP switch set
 * ========================================================================
 * `xcopy` copies files and (with /S /E) whole directory trees. The walker
 * keeps each recursion level's state in one heap block (the shared command
 * worker stack must never hold a path-sized or FATFS buffer while recursing,
 * exactly like `dir /s` and the `find` discovery walker).
 *
 * Switches (DOS 6.x / WinXP semantics):
 *   /S  copy directories and subdirectories (empty ones only with /E)
 *   /E  copy subdirectories including empty ones (implies /S)
 *   /I  assume the destination is a directory (create it if needed)
 *   /Y  suppress the overwrite prompt; /-Y force it
 *   /P  prompt before copying each file
 *   /W  wait for a key before copying begins
 *   /V  verify each copied file (size post-check)
 *   /C  continue copying after an error
 *   /Q  quiet: no per-file listing
 *   /T  create the directory tree only, do not copy files
 *   /F  display full source and destination names while copying
 *   /L  list the files that would be copied, do not copy
 *   /H  include hidden and system files (skipped by default)
 *   /R  overwrite a read-only destination
 *   /K  keep attributes (this shell always carries attributes, so /K is the
 *       effective default; accepted for DOS compatibility)
 *   /D[:mm-dd-yyyy]  copy only files newer than the date, or (no date) newer
 *                    than the same-named destination file
 *   /A  copy only files with the archive attribute set
 *   /M  copy only archive files and clear the archive attribute on the source
 *   /U  copy only files that already exist at the destination
 *   /N  generate short (8.3) names for the destination
 *   /V  verify (accepted; each copy is already written and closed by the
 *       shared copy path)
 *
 * ERRORLEVEL: 0 success, 1 nothing copied / a copy failed, 2 usage.
 */

/** Parsed `xcopy` options. */
typedef struct {
    bool recursive;
    bool include_empty;
    bool assume_dir;
    bool overwrite_silent;   /* /Y */
    bool overwrite_prompt;   /* /-Y */
    bool prompt_each;        /* /P */
    bool wait_key;           /* /W */
    bool verify;             /* /V */
    bool continue_on_error;  /* /C */
    bool quiet;              /* /Q */
    bool tree_only;          /* /T */
    bool full_names;         /* /F */
    bool list_only;          /* /L */
    bool include_hidden;     /* /H */
    bool overwrite_readonly; /* /R */
    bool keep_attrs;         /* /K */
    bool have_date;          /* /D[:date] */
    bool compare_dest;       /* /D with no date: newer than the destination */
    uint16_t date_fdate;     /* FAT date word from /D:mm-dd-yyyy */
    bool archive_only;       /* /A */
    bool archive_clear;      /* /M */
    bool update_only;        /* /U */
    bool short_names;        /* /N */
} xcopy_opts_t;

/** Running totals for a copy run. */
typedef struct {
    int copied;
    int errors;
    bool stopped;
} xcopy_stats_t;

/** Read the FAT attribute byte for a VFS path via f_stat. */
static bool shell_xcopy_get_attrib(const char *vfs_path, uint8_t *attrib_out)
{    char fatfs_path[SHELL_SD_PATH_BYTES];
    FILINFO info;

    memset(&info, 0, sizeof(info));
    if (shell_sd_vfs_to_fatfs_path(vfs_path, fatfs_path, sizeof(fatfs_path)) != ESP_OK) {
        return false;
    }
    if (f_stat(fatfs_path, &info) != FR_OK) {
        return false;
    }
    *attrib_out = info.fattrib;
    return true;
}

/** Read the FAT date word for a VFS path (used by /D with no date). */
static bool shell_xcopy_get_fat_date(const char *vfs_path, uint16_t *fdate_out)
{
    char fatfs_path[SHELL_SD_PATH_BYTES];
    FILINFO info;

    memset(&info, 0, sizeof(info));
    if (shell_sd_vfs_to_fatfs_path(vfs_path, fatfs_path, sizeof(fatfs_path)) != ESP_OK) {
        return false;
    }
    if (f_stat(fatfs_path, &info) != FR_OK) {
        return false;
    }
    *fdate_out = info.fdate;
    return true;
}

/** Set or clear the archive attribute on a VFS path (used by /M). */
static void shell_xcopy_set_archive(const char *vfs_path, bool set)
{
    char fatfs_path[SHELL_SD_PATH_BYTES];
    FILINFO info;
    uint8_t new_attrib;

    if (shell_sd_vfs_to_fatfs_path(vfs_path, fatfs_path, sizeof(fatfs_path)) != ESP_OK) {
        return;
    }
    memset(&info, 0, sizeof(info));
    if (f_stat(fatfs_path, &info) != FR_OK) {
        return;
    }
    new_attrib = set ? (uint8_t)(info.fattrib | AM_ARC) : (uint8_t)(info.fattrib & ~AM_ARC);
    (void)f_chmod(fatfs_path, new_attrib, AM_ARC);
}

/** Clear the read-only attribute on a destination so /R can overwrite it. */
static void shell_xcopy_clear_readonly(const char *vfs_path)
{
    char fatfs_path[SHELL_SD_PATH_BYTES];
    FILINFO info;

    if (shell_sd_vfs_to_fatfs_path(vfs_path, fatfs_path, sizeof(fatfs_path)) != ESP_OK) {
        return;
    }
    memset(&info, 0, sizeof(info));
    if (f_stat(fatfs_path, &info) != FR_OK) {
        return;
    }
    (void)f_chmod(fatfs_path, (uint8_t)(info.fattrib & ~AM_RDO), AM_RDO);
}

/**
 * Decide whether to overwrite an existing destination file.
 *
 * /Y overwrites silently. /-Y always asks. With neither switch the command
 * asks when an interactive key source is attached and otherwise overwrites
 * silently, so a headless or batch run never stalls and never regresses the
 * previous always-copy behaviour. The key wait is bounded; on timeout the
 * file is skipped (the conservative choice).
 */
static bool shell_xcopy_allow_overwrite(const char *dst_vfs, const xcopy_opts_t *opts)
{
    bool want_prompt;

    if (opts->overwrite_silent) {
        return true;
    }
    want_prompt = opts->overwrite_prompt || shell_key_input_available();
    if (!want_prompt) {
        return true;
    }

    shell_transcript_appendf_ansi(SH_WARN "Overwrite " SH_RST "%s? (Y/N) ",
                                  dst_vfs);
    shell_key_wait_begin();
    {
        char key = '\0';

        if (shell_wait_for_key(P4_CONFIG_KEY_WAIT_TIMEOUT_MS, &key)) {
            shell_key_wait_end();
            shell_transcript_append_text("\n");
            return (key == 'y' || key == 'Y');
        }
        shell_key_wait_end();
        shell_transcript_append_text("xcopy: overwrite prompt timed out, skipping\n");
    }
    return false;
}

/** Ask once before copying begins when /W is given (interactive only). */
static void shell_xcopy_wait_key(const xcopy_opts_t *opts)
{
    char key = '\0';

    if (!opts->wait_key || !shell_key_input_available()) {
        return;
    }
    shell_transcript_append_text("Press any key to begin copying...\n");
    shell_key_wait_begin();
    (void)shell_wait_for_key(P4_CONFIG_KEY_WAIT_TIMEOUT_MS, &key);
    shell_key_wait_end();
}

/** Ask before copying one file when /P is given. */
static bool shell_xcopy_confirm_file(const char *name, const xcopy_opts_t *opts)
{
    char key = '\0';

    if (!opts->prompt_each || !shell_key_input_available()) {
        return true;
    }
    shell_transcript_appendf("Copy %s? (Y/N) ", name);
    shell_key_wait_begin();
    if (shell_wait_for_key(P4_CONFIG_KEY_WAIT_TIMEOUT_MS, &key)) {
        shell_key_wait_end();
        shell_transcript_append_text("\n");
        return (key == 'y' || key == 'Y');
    }
    shell_key_wait_end();
    shell_transcript_append_text("xcopy: prompt timed out, skipping\n");
    return false;
}

/** Copy one file through the shared copy path and apply the /M /V switches. */
static void shell_xcopy_copy_file(const char *src_vfs, const char *dst_vfs,
                                  const xcopy_opts_t *opts, xcopy_stats_t *stats)
{
    esp_err_t error;

    error = shell_fs_copy_file(src_vfs, dst_vfs);
    if (error != ESP_OK) {
        shell_print_error("xcopy: copy failed %s -> %s (%s)",
                          src_vfs, dst_vfs, esp_err_to_name(error));
        stats->errors++;
        if (!opts->continue_on_error) {
            stats->stopped = true;
        }
        return;
    }

    if (opts->archive_clear) {
        shell_xcopy_set_archive(src_vfs, false);
    }
    if (opts->verify) {
        uint64_t src_size = storage_get_file_size(src_vfs);
        uint64_t dst_size = storage_get_file_size(dst_vfs);

        if (src_size != dst_size) {
            shell_print_warning("xcopy: size mismatch after copying %s", dst_vfs);
            stats->errors++;
        }
    }
    stats->copied++;
}

/**
 * Recursively walk one source directory copying into the matching destination.
 * Each recursion level keeps its own heap block, matching the `dir /s` and
 * `find` walkers so the 8 KB command-worker stack is never at risk.
 */
static void shell_xcopy_walk(const char *src_vfs, const char *dst_vfs, int depth,
                             const xcopy_opts_t *opts, xcopy_stats_t *stats)
{
    struct xcopy_level_scratch {
        char src_fatfs[SHELL_SD_PATH_BYTES];
        char child_src[SHELL_SD_PATH_BYTES];
        char child_dst[SHELL_SD_PATH_BYTES];
        FF_DIR dir;
        FILINFO info;
    } *scratch = NULL;
    FRESULT result;

    if (depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX || stats->stopped) {
        return;
    }

    scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL) {
        shell_record_errorf("xcopy", ESP_ERR_NO_MEM, "Out of memory during recursive xcopy");
        stats->stopped = true;
        return;
    }
    if (shell_sd_vfs_to_fatfs_path(src_vfs, scratch->src_fatfs, sizeof(scratch->src_fatfs)) != ESP_OK) {
        free(scratch);
        return;
    }
    result = f_opendir(&scratch->dir, scratch->src_fatfs);
    if (result != FR_OK) {
        free(scratch);
        return;
    }

    while (!stats->stopped) {
        bool is_dir;
        const char *dest_name;

        result = f_readdir(&scratch->dir, &scratch->info);
        if (result != FR_OK || scratch->info.fname[0] == '\0') {
            break;
        }
        if (strcmp(scratch->info.fname, ".") == 0 || strcmp(scratch->info.fname, "..") == 0) {
            continue;
        }
        /* Hidden and system entries are skipped unless /H is given. */
        if ((scratch->info.fattrib & (AM_HID | AM_SYS)) != 0 && !opts->include_hidden) {
            continue;
        }

        is_dir = (scratch->info.fattrib & AM_DIR) != 0;
        dest_name = (opts->short_names && scratch->info.altname[0] != '\0')
                        ? scratch->info.altname
                        : scratch->info.fname;

        if (snprintf(scratch->child_src, sizeof(scratch->child_src), "%s/%s",
                     src_vfs, scratch->info.fname) < 0 ||
            snprintf(scratch->child_dst, sizeof(scratch->child_dst), "%s/%s",
                     dst_vfs, dest_name) < 0) {
            continue;
        }

        if (is_dir) {
            if (opts->recursive) {
                /* Create the matching destination directory before descending.
                 * Without /E an empty directory is removed again afterwards. */
                (void)mkdir(scratch->child_dst, 0775);
                (void)storage_copy_attributes(scratch->child_src, scratch->child_dst);
                shell_xcopy_walk(scratch->child_src, scratch->child_dst, depth + 1,
                                 opts, stats);
                if (!opts->include_empty) {
                    (void)rmdir(scratch->child_dst);
                }
            }
            continue;
        }

        /* File entry. */
        if (opts->tree_only) {
            continue;
        }
        if ((opts->archive_only || opts->archive_clear) &&
            (scratch->info.fattrib & AM_ARC) == 0) {
            continue;
        }
        if (opts->have_date && scratch->info.fdate < opts->date_fdate) {
            continue;
        }
        if (opts->compare_dest || opts->update_only) {
            struct stat dst_st;
            uint16_t dst_fdate;

            if (stat(scratch->child_dst, &dst_st) != 0) {
                if (opts->update_only) {
                    continue;   /* /U: only files already at the destination */
                }
            } else {
                if (opts->update_only && opts->have_date) {
                    if (scratch->info.fdate < opts->date_fdate) {
                        continue;
                    }
                }
                if (opts->compare_dest &&
                    shell_xcopy_get_fat_date(scratch->child_dst, &dst_fdate) &&
                    scratch->info.fdate <= dst_fdate) {
                    continue;   /* /D (no date): only newer than the destination */
                }
            }
        }

        if (!shell_xcopy_confirm_file(scratch->info.fname, opts)) {
            continue;
        }

        if (opts->list_only) {
            if (opts->full_names) {
                shell_transcript_appendf("%s -> %s\n", scratch->child_src, scratch->child_dst);
            } else {
                shell_transcript_appendf("  %s\n", scratch->info.fname);
            }
            stats->copied++;
            continue;
        }

        /* Destination exists: decide whether to overwrite. */
        {
            struct stat dst_st;
            uint8_t dst_attrib;

            if (stat(scratch->child_dst, &dst_st) == 0) {
                if (!shell_xcopy_allow_overwrite(scratch->child_dst, opts)) {
                    continue;
                }
                if ((dst_st.st_mode & S_IWUSR) == 0 &&
                    shell_xcopy_get_attrib(scratch->child_dst, &dst_attrib) &&
                    (dst_attrib & AM_RDO) != 0) {
                    if (!opts->overwrite_readonly) {
                        shell_print_error("xcopy: %s is read-only (use attrib -R or /R)",
                                          scratch->child_dst);
                        stats->errors++;
                        if (!opts->continue_on_error) {
                            stats->stopped = true;
                        }
                        continue;
                    }
                    shell_xcopy_clear_readonly(scratch->child_dst);
                }
            }
        }

        shell_xcopy_copy_file(scratch->child_src, scratch->child_dst, opts, stats);
        if (opts->quiet) {
            continue;
        }
        if (opts->full_names) {
            shell_transcript_appendf("  %s -> %s\n", scratch->child_src, scratch->child_dst);
        } else {
            shell_transcript_appendf("  %s\n", scratch->info.fname);
        }
    }

    (void)f_closedir(&scratch->dir);
    free(scratch);
}

/**
 * Basename of a resolved path, accepting both '/' and '\' separators (the
 * resolver keeps DOS-style backslashes in the filename part).
 */
static const char *shell_xcopy_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');

    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    return (slash != NULL) ? slash + 1 : path;
}

/**
 * `xcopy` command entry point.
 *
 * Usage: xcopy <source> <destination> [switches]
 * Returns an ERRORLEVEL: 0 success, 1 nothing copied / copy failed, 2 usage.
 */
int shell_command_xcopy(int argc, char **argv)
{
    xcopy_opts_t opts;
    xcopy_stats_t stats;
    shell_sd_session_t session;
    esp_err_t error;
    char src_resolved[SHELL_SD_PATH_BYTES];
    char dst_resolved[SHELL_SD_PATH_BYTES];
    /* A directory destination plus a source basename can exceed one path
     * buffer; size this for the joined form so -Werror=format-truncation
     * cannot fire. */
    char dst_file[SHELL_SD_PATH_BYTES * 2 + 16];
    const char *src_arg = NULL;
    const char *dst_arg = NULL;
    struct stat src_st;
    int index;

    memset(&opts, 0, sizeof(opts));
    memset(&stats, 0, sizeof(stats));

    for (index = 1; index < argc; index++) {
        const char *token = argv[index];

        if (token[0] != '/') {
            if (src_arg == NULL) {
                src_arg = token;
            } else if (dst_arg == NULL) {
                dst_arg = token;
            } else {
                shell_print_usage("Usage: xcopy <source> <destination> [/S] [/E] [/I] [/Y|/-Y] [/D[:date]] [/H] [/R] [/K] [/C] [/Q] [/T] [/F] [/L] [/A] [/M] [/U] [/P] [/W] [/N]");
                return 2;
            }
            continue;
        }

        if (token[1] == '-' && token[2] == 'Y' && token[3] == '\0') {
            opts.overwrite_prompt = true;
        } else if (toupper((unsigned char)token[1]) == 'Y' && token[2] == '\0') {
            opts.overwrite_silent = true;
        } else if (toupper((unsigned char)token[1]) == 'D') {
            opts.have_date = true;
            if (token[2] == ':' && token[3] != '\0') {
                if (!shell_find_parse_date(token + 3, &opts.date_fdate)) {
                    shell_print_error("xcopy: invalid /D date %s (use mm-dd-yyyy)", token + 3);
                    return 2;
                }
            } else {
                opts.compare_dest = true;
            }
        } else {
            switch (toupper((unsigned char)token[1])) {
            case 'S': opts.recursive = true;          break;
            case 'E': opts.recursive = true; opts.include_empty = true; break;
            case 'I': opts.assume_dir = true;         break;
            case 'P': opts.prompt_each = true;        break;
            case 'W': opts.wait_key = true;           break;
            case 'V': opts.verify = true;             break;
            case 'C': opts.continue_on_error = true;  break;
            case 'Q': opts.quiet = true;              break;
            case 'T': opts.tree_only = true;          break;
            case 'F': opts.full_names = true;         break;
            case 'L': opts.list_only = true;          break;
            case 'H': opts.include_hidden = true;     break;
            case 'R': opts.overwrite_readonly = true; break;
            case 'K': opts.keep_attrs = true;         break;
            case 'A': opts.archive_only = true;       break;
            case 'M': opts.archive_clear = true;      break;
            case 'U': opts.update_only = true;        break;
            case 'N': opts.short_names = true;        break;
            default:
                shell_print_error("xcopy: unknown option %s", token);
                shell_print_usage("Usage: xcopy <source> <destination> [/S] [/E] [/I] [/Y|/-Y] [/D[:date]] [/H] [/R] [/K] [/C] [/Q] [/T] [/F] [/L] [/A] [/M] [/U] [/P] [/W] [/N]");
                return 2;
            }
        }
    }

    if (src_arg == NULL || dst_arg == NULL) {
        shell_print_usage("Usage: xcopy <source> <destination> [/S] [/E] [/I] [/Y|/-Y] [/D[:date]] [/H] [/R] [/K] [/C] [/Q] [/T] [/F] [/L] [/A] [/M] [/U] [/P] [/W] [/N]");
        return 2;
    }
    /* /A and /M are mutually exclusive in DOS. */
    if (opts.archive_only && opts.archive_clear) {
        shell_print_error("xcopy: /A and /M are mutually exclusive");
        return 2;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("xcopy: SD card not present");
        return 1;
    }

    error = shell_sd_resolve_path(src_arg, src_resolved, sizeof(src_resolved));
    if (error != ESP_OK) {
        shell_print_error("xcopy: invalid source path");
        shell_sd_end(&session, "xcopy");
        return 1;
    }
    error = shell_sd_resolve_path(dst_arg, dst_resolved, sizeof(dst_resolved));
    if (error != ESP_OK) {
        shell_print_error("xcopy: invalid destination path");
        shell_sd_end(&session, "xcopy");
        return 1;
    }

    if (stat(src_resolved, &src_st) != 0) {
        shell_transcript_appendf("xcopy: source not found: %s\n", src_arg);
        shell_sd_end(&session, "xcopy");
        return 1;
    }

    shell_xcopy_wait_key(&opts);

    if (S_ISDIR(src_st.st_mode)) {
        struct stat dst_st;

        if (!opts.recursive) {
            shell_transcript_append_text("xcopy: use /S (or /E) to copy directories\n");
            shell_sd_end(&session, "xcopy");
            return 1;
        }
        if (stat(dst_resolved, &dst_st) == 0 && !S_ISDIR(dst_st.st_mode)) {
            shell_print_error("xcopy: destination %s exists and is not a directory", dst_resolved);
            shell_sd_end(&session, "xcopy");
            return 1;
        }
        if (stat(dst_resolved, &dst_st) != 0) {
            if (mkdir(dst_resolved, 0775) != 0) {
                shell_print_error("xcopy: cannot create destination directory %s", dst_resolved);
                shell_sd_end(&session, "xcopy");
                return 1;
            }
            /* Carry the source directory's attributes onto the one just
             * created, so a hidden or system folder stays that way. */
            (void)storage_copy_attributes(src_resolved, dst_resolved);
        }

        shell_xcopy_walk(src_resolved, dst_resolved, 0, &opts, &stats);
    } else {
        /* Single-file source. */
        struct stat dst_st;

        if (opts.tree_only) {
            shell_transcript_append_text("xcopy: /T copies directories only, nothing to do\n");
            shell_sd_end(&session, "xcopy");
            return 0;
        }
        if ((opts.archive_only || opts.archive_clear) && (src_st.st_mode & S_IFREG) != 0) {
            uint8_t attrib;

            if (shell_xcopy_get_attrib(src_resolved, &attrib) && (attrib & AM_ARC) == 0) {
                shell_transcript_append_text("xcopy: 0 file(s) copied (source has no archive attribute)\n");
                shell_sd_end(&session, "xcopy");
                return 1;
            }
        }

        /* Destination path: a directory, or /I makes it one. */
        if (stat(dst_resolved, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
            snprintf(dst_file, sizeof(dst_file), "%s/%s",
                     dst_resolved, shell_xcopy_basename(src_resolved));
        } else if (opts.assume_dir) {
            if (mkdir(dst_resolved, 0775) != 0 && errno != EEXIST) {
                shell_print_error("xcopy: cannot create destination directory %s", dst_resolved);
                shell_sd_end(&session, "xcopy");
                return 1;
            }
            snprintf(dst_file, sizeof(dst_file), "%s/%s",
                     dst_resolved, shell_xcopy_basename(src_resolved));
        } else {
            snprintf(dst_file, sizeof(dst_file), "%s", dst_resolved);
        }

        /* /D and /U filters for a single-file source (the walker applies the
         * same rules to every file it enumerates). */
        if (opts.have_date || opts.compare_dest || opts.update_only) {
            uint16_t src_fdate;

            if (opts.have_date &&
                shell_xcopy_get_fat_date(src_resolved, &src_fdate) &&
                src_fdate < opts.date_fdate) {
                shell_transcript_append_text("xcopy: 0 file(s) copied (source older than /D date)\n");
                shell_sd_end(&session, "xcopy");
                return 1;
            }
            if (opts.compare_dest || opts.update_only) {
                if (stat(dst_file, &dst_st) != 0) {
                    if (opts.update_only) {
                        shell_transcript_append_text("xcopy: 0 file(s) copied (/U: destination does not exist)\n");
                        shell_sd_end(&session, "xcopy");
                        return 1;
                    }
                } else if (opts.compare_dest &&
                           shell_xcopy_get_fat_date(src_resolved, &src_fdate)) {
                    uint16_t dst_fdate;

                    if (shell_xcopy_get_fat_date(dst_file, &dst_fdate) &&
                        src_fdate <= dst_fdate) {
                        shell_transcript_append_text("xcopy: 0 file(s) copied (source not newer than destination)\n");
                        shell_sd_end(&session, "xcopy");
                        return 1;
                    }
                }
            }
        }

        if (opts.list_only) {
            if (opts.full_names) {
                shell_transcript_appendf("%s -> %s\n", src_resolved, dst_file);
            } else {
                shell_transcript_appendf("  %s\n", shell_xcopy_basename(src_resolved));
            }
            stats.copied++;
        } else {
            if (stat(dst_file, &dst_st) == 0) {
                uint8_t dst_attrib;

                if (!shell_xcopy_allow_overwrite(dst_file, &opts)) {
                    shell_sd_end(&session, "xcopy");
                    shell_transcript_append_text("xcopy: 0 file(s) copied\n");
                    return 1;
                }
                if ((dst_st.st_mode & S_IWUSR) == 0 &&
                    shell_xcopy_get_attrib(dst_file, &dst_attrib) &&
                    (dst_attrib & AM_RDO) != 0) {
                    if (!opts.overwrite_readonly) {
                        shell_print_error("xcopy: %s is read-only (use attrib -R or /R)", dst_file);
                        shell_sd_end(&session, "xcopy");
                        return 1;
                    }
                    shell_xcopy_clear_readonly(dst_file);
                }
            }
            if (!shell_xcopy_confirm_file(dst_file, &opts)) {
                shell_sd_end(&session, "xcopy");
                shell_transcript_append_text("xcopy: 0 file(s) copied\n");
                return 1;
            }
            shell_xcopy_copy_file(src_resolved, dst_file, &opts, &stats);
            if (!opts.quiet) {
                if (opts.full_names) {
                    shell_transcript_appendf("  %s -> %s\n", src_resolved, dst_file);
                } else {
                    shell_transcript_appendf("  %s\n", shell_xcopy_basename(dst_file));
                }
            }
        }
    }

    shell_sd_end(&session, "xcopy");

    if (opts.list_only) {
        shell_transcript_appendf("xcopy: %d file(s) would be copied\n", stats.copied);
    } else if (stats.errors > 0) {
        shell_transcript_appendf("xcopy: %d file(s) copied, %d error(s)\n",
                                 stats.copied, stats.errors);
    } else {
        shell_transcript_appendf("xcopy: %d file(s) copied\n", stats.copied);
    }

    return (stats.copied > 0 && stats.errors == 0) ? 0 : 1;
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
bool shell_confirm_destructive(const char *operation, const char *warning, const char *detail)
{
    char confirm[32];

    shell_transcript_appendf_ansi(SH_ERR "%s" SH_RST "\n", warning);
    if (detail != NULL && detail[0] != '\0') {
        shell_transcript_append_text(detail);
    }

    /* A batch file must never be able to drive a destructive operation, even
     * when a serial console is attached that could answer the prompt. Refuse
     * outright so an unattended script cannot wipe the card. */
    if (shell_is_batch_active()) {
        shell_transcript_appendf("\n%s: refused, cannot be confirmed from a batch file\n", operation);
        shell_record_warningf(operation, "Refused destructive operation requested by a batch file");
        return false;
    }

    shell_transcript_appendf("Type %s to continue: ", P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD);

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
    return strcmp(confirm, P4_CONFIG_DESTRUCTIVE_CONFIRM_WORD) == 0;
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
static int shell_format_execute(const char *operation,
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
            return 1;
        }
        shell_sd_end(&session, operation);
    }

    if (bsp_sdcard == NULL) {
        shell_print_error("%s: no SD card handle is available", operation);
        shell_record_errorf(operation, ESP_ERR_INVALID_STATE, "bsp_sdcard is NULL");
        return 1;
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
            return 1;
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
        return 1;
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

    return 0;
}

int shell_command_format(int argc, char **argv)
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
            return 2;
        }

        /* /FS: needs two letters to disambiguate from a future /F. */
        if (toupper((unsigned char)token[1]) == 'F' && toupper((unsigned char)token[2]) == 'S') {
            value = token + 3;
            if (*value == ':' || *value == '=') {
                value++;
            }
            if (*value == '\0') {
                shell_print_error("format: /FS needs a type, for example /FS:FAT32");
                return 2;
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
                return 2;
            }
            if (strlen(value) > 11) {
                shell_print_error("format: volume label must be 11 characters or fewer");
                return 2;
            }
            new_label = value;
            continue;
        case 'A':
            if (*value == '\0') {
                shell_print_error("format: /A needs a size, for example /A:32K");
                return 2;
            }
            if (!shell_parse_alloc_unit(value, &alloc_unit) ||
                alloc_unit < P4_CONFIG_FORMAT_ALLOC_UNIT_MIN ||
                alloc_unit > P4_CONFIG_FORMAT_ALLOC_UNIT_MAX) {
                shell_print_error("format: invalid /A allocation unit size %s", value);
                shell_print_usage("Usage: format [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q]");
                return 2;
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
            return 2;
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
            return 2;
        }
    }

    return shell_format_execute("format", fs_type, new_label, alloc_unit, alloc_set);
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
static bool shell_find_parse_date(const char *text, uint16_t *fdate_out)
{
    unsigned int year;
    unsigned int month;
    unsigned int day;

    if (text == NULL || sscanf(text, "%u-%u-%u", &year, &month, &day) != 3) {
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
    int tlen = (int)strlen(text);
    bool anchor_beg = (pattern[0] == '^');
    int ti;

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
    int line_len = (int)strlen(line);

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

/**
 * Search one file, printing matching lines. When @p prefix_file is set (more
 * than one source) each line is prefixed with the filename. Stops once the
 * global match cap is reached.
 */
static void shell_findstr_search_file(const char *path, const findstr_opts_t *opts,
                                      bool prefix_file, int *total_matches)
{
    char line[SHELL_TEXT_LINE_BYTES];
    FILE *file;
    int lineno = 0;
    int matched_lines = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        shell_print_error("findstr: cannot open %s (%s)", path, strerror(errno));
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL &&
           *total_matches < P4_CONFIG_FINDSTR_MATCH_MAX) {
        bool hit = false;
        int index;

        lineno++;
        shell_text_strip_eol(line);

        for (index = 0; index < opts->nstrings; index++) {
            if (shell_findstr_match_line(opts->strings[index], opts->regex_strings[index],
                                         line, opts->icase, opts->beg, opts->end,
                                         opts->whole)) {
                hit = true;
                break;
            }
        }
        if (opts->invert) {
            hit = !hit;
        }
        if (!hit) {
            continue;
        }

        matched_lines++;
        (*total_matches)++;

        if (opts->files_only) {
            continue;   /* the filename is printed once at the end */
        }
        if (prefix_file) {
            shell_transcript_appendf("%s:", path);
        }
        if (opts->numbers) {
            shell_transcript_appendf("%d:", lineno);
        }
        shell_transcript_appendf("%s\n", line);
    }

    fclose(file);

    if (opts->files_only && matched_lines > 0) {
        shell_transcript_appendf("%s\n", path);
    }
}

/**
 * Recursively search every file under @p vfs_dir (`findstr /S`). Each level
 * keeps its state in one heap block, matching the `find` discovery walker.
 */
static void shell_findstr_walk(const char *vfs_dir, int depth, const findstr_opts_t *opts,
                               int *total_matches)
{
    struct findstr_level_scratch {
        char fatfs_path[SHELL_SD_PATH_BYTES];
        char child[SHELL_SD_PATH_BYTES];
        FF_DIR dir;
        FILINFO info;
    } *scratch = NULL;
    FRESULT result;

    if (depth > P4_CONFIG_DIR_RECURSE_DEPTH_MAX ||
        *total_matches >= P4_CONFIG_FINDSTR_MATCH_MAX) {
        return;
    }

    scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL) {
        shell_record_errorf("findstr", ESP_ERR_NO_MEM, "Out of memory during findstr /S");
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

    while (true) {
        result = f_readdir(&scratch->dir, &scratch->info);
        if (result != FR_OK || scratch->info.fname[0] == '\0' ||
            *total_matches >= P4_CONFIG_FINDSTR_MATCH_MAX) {
            break;
        }
        if (strcmp(scratch->info.fname, ".") == 0 || strcmp(scratch->info.fname, "..") == 0) {
            continue;
        }
        if (snprintf(scratch->child, sizeof(scratch->child), "%s/%s",
                     vfs_dir, scratch->info.fname) < 0) {
            continue;
        }
        if ((scratch->info.fattrib & AM_DIR) != 0) {
            shell_findstr_walk(scratch->child, depth + 1, opts, total_matches);
        } else {
            shell_findstr_search_file(scratch->child, opts, true, total_matches);
        }
    }

    (void)f_closedir(&scratch->dir);
    free(scratch);
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
    int total_matches = 0;
    int index;
    int rc = 1;
    esp_err_t error;

    memset(&opts, 0, sizeof(opts));

    for (index = 1; index < argc; index++) {
        const char *token = argv[index];

        if (token[0] == '/') {
            if (strncasecmp(token, "/C:", 3) == 0) {
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
         * tokens are files, matching DOS findstr. */
        if (bare_string == NULL) {
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

        if (shell_fs_resolve_path(opts.strings_arg, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("findstr: invalid /G file path");
            return 2;
        }
        file = fopen(resolved, "r");
        if (file == NULL) {
            shell_print_error("findstr: cannot open /G file %s", resolved);
            return 1;
        }
        while (fgets(line, sizeof(line), file) != NULL) {
            shell_text_strip_eol(line);
            if (line[0] == '\0') {
                continue;
            }
            if (!shell_findstr_add_string(&opts, line, opts.use_regex)) {
                fclose(file);
                return 2;
            }
        }
        fclose(file);
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

        if (shell_fs_resolve_path(opts.filelist_arg, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("findstr: invalid /F file path");
            return 2;
        }
        file = fopen(resolved, "r");
        if (file == NULL) {
            shell_print_error("findstr: cannot open /F file %s", resolved);
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
                shell_findstr_walk(resolved, 0, &opts, &total_matches);
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
    size_t n = (an < bn) ? an : bn;

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
    shell_transcript_append_text("  sd mount          (mount / re-mount after eject)\n");
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
    } else if (strcmp(argv[1], "mount") == 0) {
        shell_command_sd_mount();
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

static int shell_command_disk_clean(void)
{
    esp_err_t error;

    if (!shell_confirm_destructive("disk clean",
                                   "WARNING: This will remove ALL partitions on the SD card.",
                                   "  Run 'disk create partition primary' then 'format' to recreate a filesystem.\n")) {
        shell_print_warning("disk: clean cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk clean by user");
        return 1;
    }

    error = storage_disk_clean(STORAGE_VOLUME_SD);
    if (error != ESP_OK) {
        shell_print_error("disk: clean failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk clean failed");
        return 1;
    }

    shell_print_ok("disk: partition table removed");
    shell_transcript_append_text("  Run 'disk create partition primary' then 'format' to recreate a filesystem.\n");
    return 0;
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

static int shell_command_disk_create(int argc, char **argv)
{
    uint64_t size_bytes = 0;
    esp_err_t error;
    int index;

    /* disk create partition primary [size=N] */
    if (argc < 4 || strcmp(argv[2], "partition") != 0) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk create command");
        return 2;
    }
    if (strcmp(argv[3], "primary") != 0) {
        shell_print_error("disk: only 'primary' partitions are supported");
        shell_record_warningf("disk", "Unsupported partition type: %s", argv[3]);
        return 2;
    }

    for (index = 4; index < argc; index++) {
        if (strncmp(argv[index], "size=", 5) == 0) {
            if (!shell_disk_parse_partition_size(argv[index] + 5, &size_bytes)) {
                shell_print_error("disk: invalid partition size %s", argv[index] + 5);
                return 2;
            }
        } else {
            shell_print_error("disk: unexpected argument %s", argv[index]);
            shell_disk_print_usage();
            return 2;
        }
    }

    error = storage_disk_create_primary_partition(STORAGE_VOLUME_SD, size_bytes);
    if (error == ESP_ERR_INVALID_STATE) {
        shell_print_error("disk: no free partition slot (all 4 MBR entries are used)");
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("disk: create partition failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk create partition failed");
        return 1;
    }

    shell_print_ok("disk: primary partition created");
    shell_transcript_append_text("  Run 'format' (or 'disk format') to create the filesystem.\n");
    return 0;
}

static int shell_command_disk_delete(int argc, char **argv)
{
    char *end;
    unsigned long partition_index;
    esp_err_t error;

    /* disk delete partition N (1-4) */
    if (argc < 4 || strcmp(argv[2], "partition") != 0) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk delete command");
        return 2;
    }

    partition_index = strtoul(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' || partition_index < 1 || partition_index > 4) {
        shell_print_error("disk: partition number must be 1-4");
        return 2;
    }

    if (!shell_confirm_destructive("disk delete",
                                   "WARNING: This will remove partition information from the SD card.",
                                   "")) {
        shell_print_warning("disk: delete cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk delete by user");
        return 1;
    }

    error = storage_disk_delete_partition(STORAGE_VOLUME_SD, (unsigned)(partition_index - 1));
    if (error != ESP_OK) {
        shell_print_error("disk: delete partition failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk delete partition failed");
        return 1;
    }

    shell_print_ok("disk: partition %lu removed", partition_index);
    shell_transcript_append_text("  Run 'format' (or 'disk format') to recreate a filesystem.\n");
    return 0;
}

static int shell_command_disk_format(int argc, char **argv)
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
                return 2;
            }
        } else if (strncmp(token, "au=", 3) == 0) {
            if (!shell_parse_alloc_unit(token + 3, &alloc_unit) ||
                alloc_unit < P4_CONFIG_FORMAT_ALLOC_UNIT_MIN ||
                alloc_unit > P4_CONFIG_FORMAT_ALLOC_UNIT_MAX) {
                shell_print_error("disk format: invalid allocation unit size %s", token + 3);
                return 2;
            }
            alloc_set = true;
        } else if (strcmp(token, "quick") == 0) {
            /* Accepted for diskpart familiarity; the format is always quick. */
        } else {
            shell_print_error("disk format: unexpected argument %s", token);
            shell_disk_print_usage();
            return 2;
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
            return 2;
        }
    }

    return shell_format_execute("disk format", fs_type, label, alloc_unit, alloc_set);
}

/**
 * `disk` command family entry point.
 */
int shell_command_disk(char *command)
{
    char *argv[8];
    int argc;
    int rc = 2;

    argc = shell_split_args(command, argv, 8);

    if (argc <= 1) {
        shell_command_disk_list();
        shell_disk_print_usage();
        rc = 2;
    } else if (strcmp(argv[1], "help") == 0) {
        shell_disk_print_usage();
        rc = 0;
    } else if (strcmp(argv[1], "list") == 0) {
        shell_command_disk_list();
        rc = 0;
    } else if (strcmp(argv[1], "detail") == 0) {
        shell_command_disk_detail();
        rc = 0;
    } else if (strcmp(argv[1], "clean") == 0) {
        rc = shell_command_disk_clean();
    } else if (strcmp(argv[1], "create") == 0) {
        rc = shell_command_disk_create(argc, argv);
    } else if (strcmp(argv[1], "delete") == 0) {
        rc = shell_command_disk_delete(argc, argv);
    } else if (strcmp(argv[1], "format") == 0) {
        rc = shell_command_disk_format(argc, argv);
    } else {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Unknown disk subcommand: %s", argv[1]);
        rc = 2;
    }

    return rc;
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file storage_disk.c
 * @brief Volume verbs (chkdsk/format) + destructive confirm helper.
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
    if (subdirs == NULL) {
        // Directories are still counted below, but without the buffer this
        // level cannot be descended: say so instead of silently skipping.
        shell_record_warningf("chkdsk", "Out of memory buffering subdirectories at %s", dir_path);
        scan->unreadable++;
    }

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
/** Shared with the disk family (storage_fam.c). Unit-tested via format paths. */
bool shell_parse_alloc_unit(const char *text, uint32_t *bytes_out)
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
/** Shared with the disk family (storage_fam.c): validated format run. */
int shell_format_execute(const char *operation,
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

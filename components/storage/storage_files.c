/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file storage_files.c
 * @brief File manipulation verbs + attrib/label/xcopy.
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

    if (recursive) {
        /* A plain file with /s deletes same-named files in subdirectories
         * (permanent deletes bypass the recycle bin). */
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

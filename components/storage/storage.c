/**
 * @file storage.c
 * @brief SD session management and filesystem services for P4MiniShell.
 *
 * Owns the guarded SD mount session, all path resolution, VFS-to-FATFS
 * conversion, size formatting, DOS wildcard matching, the RAM-only current
 * working directory, and the shared file helpers used by the DOS file
 * commands. All transcript output and debug logging go through shell.c.
 *
 * State owned here:
 *   - Current working directory (RAM-only, rooted at the SD mount point)
 *   - SD mount tracking and the explicit eject latch
 */

#include "storage.h"
#include "shell.h"
#include "ansi_palette.h"
#include "header.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

/* Backward-compatibility aliases */
#define STORAGE_TAG                     P4_CONFIG_SHELL_TAG
#define SHELL_SD_FATFS_DRIVE            P4_CONFIG_SD_FATFS_DRIVE
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_SD_LIST_LIMIT             P4_CONFIG_SD_LIST_LIMIT
#define SHELL_SD_IO_BUFFER_BYTES        P4_CONFIG_SD_IO_BUFFER_BYTES
#define SHELL_FILE_IO_BUFFER_BYTES      P4_CONFIG_FILE_IO_BUFFER_BYTES
#define SHELL_LFN_BYTES                 P4_CONFIG_LFN_BYTES

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/* Shell working directory (RAM-only, not persisted across boots) */
static char s_shell_cwd[SHELL_SD_PATH_BYTES];

/* SD mount tracking */
static bool s_sd_persistent_mounted;  /* true while the card is mounted */
static bool s_sd_ejected;             /* set by sdeject to block auto-remount */

/* Pending `<` or pipe input source for the text-processing commands */
static char s_input_redirect[SHELL_SD_PATH_BYTES];
static bool s_input_redirect_active;

/* ========================================================================
 * SD SESSION
 * ======================================================================== */

/**
 * Begin a guarded SD access session.
 *
 * Every SD-touching command goes through this single mount path so a failure
 * can never leak mounted state or dereference missing card metadata. The
 * header SD icon is updated on mount so the status bar stays in sync with the
 * real card state.
 *
 * The mount is persistent: shell_sd_end() deliberately does not unmount.
 * Only the explicit `sdeject` command tears the mount down.
 */
esp_err_t shell_sd_begin(shell_sd_session_t *session)
{
    esp_err_t error;

    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    session->mounted_here = false;

    /* If already mounted, just return success (persistent mount) */
    if (s_sd_persistent_mounted) {
        return ESP_OK;
    }

    /* If ejected, refuse to mount unless user runs an explicit command */
    if (s_sd_ejected) {
        return ESP_ERR_INVALID_STATE;
    }

    error = bsp_sdcard_mount();
    if (error == ESP_OK) {
        session->mounted_here = true;
        s_sd_persistent_mounted = true;
        header_update_sd(HEADER_SD_MOUNTED);
        return ESP_OK;
    }

    if (error == ESP_ERR_INVALID_STATE) {
        /* Already mounted (BSP internal state) */
        s_sd_persistent_mounted = true;
        header_update_sd(HEADER_SD_MOUNTED);
        return ESP_OK;
    }

    return error;
}

void shell_sd_end(shell_sd_session_t *session, const char *operation)
{
    /* Persistent mount: do NOT unmount after each command.
     * Only unmount on explicit sdeject command.
     * This keeps the SD card accessible and the header icon accurate. */
    (void)session;
    (void)operation;
}

bool storage_sd_is_mounted(void)
{
    /* Persistent mount tracking rather than stat(), which only succeeds
     * while the card is already mounted. */
    return s_sd_persistent_mounted;
}

void shell_command_sd_eject(void)
{
    esp_err_t error;

    /* Force unmount the SD card unconditionally (not session-guarded).
     * This is the safe-removal path: unmounts even if mounted by another session. */
    error = bsp_sdcard_unmount();
    if (error == ESP_OK) {
        s_sd_persistent_mounted = false;
        s_sd_ejected = true;
        shell_print_ok("SD card unmounted safely. You may now remove the card.");
        shell_header_notify("SD card ejected", P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS);
        header_update_sd(HEADER_SD_NONE);
    } else if (error == ESP_ERR_INVALID_STATE) {
        shell_print_muted("SD card is not currently mounted.");
    } else {
        shell_print_warning("SD eject warning: %s (0x%x)", esp_err_to_name(error), (unsigned int)error);
        shell_record_warningf("sd", "Eject unmount warning: %s", esp_err_to_name(error));
    }
}

/* ========================================================================
 * PATH RESOLUTION AND CONVERSION
 * ======================================================================== */

esp_err_t shell_sd_resolve_path(const char *input, char *output, size_t output_size)
{
    int written;
    const char *source = input;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source == NULL || source[0] == '\0') {
        source = BSP_SD_MOUNT_POINT;
    }

    if (strncmp(source, "sd:/", 4) == 0) {
        written = snprintf(output, output_size, "%s%s", BSP_SD_MOUNT_POINT, source + 3);
    } else if (strncmp(source, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0 ||
               strcmp(source, BSP_SD_MOUNT_POINT) == 0) {
        written = snprintf(output, output_size, "%s", source);
    } else if (source[0] == '/') {
        written = snprintf(output, output_size, "%s", source);
    } else {
        written = snprintf(output, output_size, "%s/%s", BSP_SD_MOUNT_POINT, source);
    }

    if (written < 0) {
        output[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t shell_sd_stat_path(const char *path, struct stat *st)
{
    if (path == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, st) == 0) {
        return ESP_OK;
    }

    if (errno == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_FAIL;
}

esp_err_t shell_sd_fresult_to_esp_err(FRESULT result)
{
    switch (result) {
    case FR_OK:
        return ESP_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return ESP_ERR_NOT_FOUND;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
    case FR_INVALID_DRIVE:
        return ESP_ERR_INVALID_ARG;
    case FR_NOT_READY:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return ESP_ERR_INVALID_STATE;
    case FR_NOT_ENOUGH_CORE:
        return ESP_ERR_NO_MEM;
    case FR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

/**
 * Convert a VFS path under the SD mount point to a raw FATFS path.
 *
 * The direct FATFS API is used for directory listings because it exposes long
 * file names (CONFIG_FATFS_LFN_HEAP with MAX_LFN=255) and the 8.3 alternate
 * name, which the POSIX dirent path does not surface.
 */
esp_err_t shell_sd_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t fatfs_path_size)
{
    const char *relative_path;
    int written;

    if (vfs_path == NULL || fatfs_path == NULL || fatfs_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(vfs_path, BSP_SD_MOUNT_POINT) == 0) {
        relative_path = "/";
    } else if (strncmp(vfs_path, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        relative_path = vfs_path + strlen(BSP_SD_MOUNT_POINT);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(fatfs_path, fatfs_path_size, "%s%s", SHELL_SD_FATFS_DRIVE, relative_path);
    if (written < 0) {
        fatfs_path[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= fatfs_path_size) {
        fatfs_path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t shell_fs_resolve_path(const char *input, char *output, size_t output_size)
{
    char combined[SHELL_SD_PATH_BYTES];
    char scratch[SHELL_SD_PATH_BYTES];
    char *segments[32];
    size_t segment_count = 0;
    char *token;
    char *context = NULL;
    int written;
    size_t index;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (input == NULL || input[0] == '\0') {
        snprintf(output, output_size, "%s", s_shell_cwd[0] != '\0' ? s_shell_cwd : BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    if (strncmp(input, "sd:/", 4) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input + 4);
    } else if (strcmp(input, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/");
    } else if (strncmp(input, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        snprintf(combined, sizeof(combined), "%s", input + strlen(BSP_SD_MOUNT_POINT));
    } else if (input[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", input);
    } else if (strcmp(s_shell_cwd, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input);
    } else {
        snprintf(combined,
                 sizeof(combined),
                 "%s/%s",
                 s_shell_cwd + strlen(BSP_SD_MOUNT_POINT),
                 input);
    }

    snprintf(scratch, sizeof(scratch), "%s", combined);
    token = strtok_r(scratch, "/\\", &context);
    while (token != NULL && segment_count < (sizeof(segments) / sizeof(segments[0]))) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok_r(NULL, "/\\", &context);
            continue;
        }

        if (strcmp(token, "..") == 0) {
            if (segment_count > 0) {
                segment_count--;
            }
            token = strtok_r(NULL, "/\\", &context);
            continue;
        }

        segments[segment_count++] = token;
        token = strtok_r(NULL, "/\\", &context);
    }

    written = snprintf(output, output_size, "%s", BSP_SD_MOUNT_POINT);
    if (written < 0 || (size_t)written >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    for (index = 0; index < segment_count; index++) {
        size_t used = strlen(output);
        written = snprintf(output + used, output_size - used, "/%s", segments[index]);
        if (written < 0 || (size_t)written >= output_size - used) {
            output[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return ESP_OK;
}

bool shell_path_has_directory_component(const char *path)
{
    return path != NULL && (strchr(path, '/') != NULL || strchr(path, '\\') != NULL);
}

bool shell_path_has_extension(const char *path, const char *extension)
{
    size_t path_len;
    size_t ext_len;

    if (path == NULL || extension == NULL) {
        return false;
    }

    path_len = strlen(path);
    ext_len = strlen(extension);
    if (path_len < ext_len) {
        return false;
    }

    return shell_text_equals_ignore_case(path + path_len - ext_len, extension);
}

esp_err_t shell_resolve_target_from_source(const char *source_path,
                                           const char *target_input,
                                           char *target_path,
                                           size_t target_path_size)
{
    char source_dir[SHELL_SD_PATH_BYTES];
    char *last_sep;

    if (source_path == NULL || target_input == NULL || target_path == NULL || target_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (shell_path_has_directory_component(target_input) ||
        strncmp(target_input, "sd:/", 4) == 0 ||
        target_input[0] == '/') {
        return shell_fs_resolve_path(target_input, target_path, target_path_size);
    }

    snprintf(source_dir, sizeof(source_dir), "%s", source_path);
    last_sep = strrchr(source_dir, '/');
    if (last_sep == NULL || strcmp(source_dir, BSP_SD_MOUNT_POINT) == 0) {
        return shell_fs_resolve_path(target_input, target_path, target_path_size);
    }

    *last_sep = '\0';
    if (source_dir[0] == '\0') {
        snprintf(source_dir, sizeof(source_dir), "%s", BSP_SD_MOUNT_POINT);
    }

    if (snprintf(target_path, target_path_size, "%s/%s", source_dir, target_input) >= (int)target_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* ========================================================================
 * FORMATTING AND MATCHING
 * ======================================================================== */

void shell_sd_format_size(uint64_t size_bytes, char *output, size_t output_size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = (double)size_bytes;
    size_t unit_index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    while (value >= 1024.0 && unit_index < (sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(output, output_size, "%llu %s", (unsigned long long)size_bytes, units[unit_index]);
    } else {
        snprintf(output, output_size, "%.1f %s", value, units[unit_index]);
    }
}

const char *shell_sd_entry_type(const struct stat *st)
{
    if (st == NULL) {
        return "unknown";
    }

    if (S_ISDIR(st->st_mode)) {
        return "dir";
    }

    if (S_ISREG(st->st_mode)) {
        return "file";
    }

    return "other";
}

/* DOS-style wildcard matching: `*` spans any run of characters, `?` matches
 * exactly one, and comparisons are case-insensitive like FAT itself. */
bool shell_wildcard_match(const char *pattern, const char *name)
{
    if (pattern == NULL || name == NULL) return false;

    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0') return true;
            while (*name) {
                if (shell_wildcard_match(pattern, name)) return true;
                name++;
            }
            return false;
        } else if (*pattern == '?') {
            if (*name == '\0') return false;
            pattern++;
            name++;
        } else {
            if (toupper((unsigned char)*pattern) != toupper((unsigned char)*name))
                return false;
            pattern++;
            name++;
        }
    }
    return *name == '\0';
}

/* ========================================================================
 * CURRENT WORKING DIRECTORY
 * ======================================================================== */

const char *shell_get_cwd(void)
{
    return s_shell_cwd;
}

void storage_set_cwd(const char *absolute_path)
{
    if (absolute_path == NULL || absolute_path[0] == '\0') {
        return;
    }

    snprintf(s_shell_cwd, sizeof(s_shell_cwd), "%s", absolute_path);
}

void shell_fs_print_cwd(void)
{
    if (strcmp(s_shell_cwd, BSP_SD_MOUNT_POINT) == 0) {
        shell_transcript_append_text("\\\n");
        return;
    }

    shell_transcript_appendf("%s\n", s_shell_cwd + strlen(BSP_SD_MOUNT_POINT));
}

/* ========================================================================
 * SHARED FILE OPERATIONS
 * ======================================================================== */

/**
 * Copy one regular file with guardrails.
 *
 * Before transferring a byte this refuses to copy a file onto itself, and
 * checks that the destination volume has room for the whole source plus the
 * configured safety margin. Without the precheck a large copy would fill the
 * card and leave a truncated destination behind.
 *
 * Files above P4_CONFIG_COPY_PROGRESS_THRESHOLD report percentage progress,
 * so a multi-megabyte transfer does not look like a hang.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG when source and destination are the
 *         same file, ESP_ERR_NOT_FOUND when the source is missing,
 *         ESP_ERR_NO_MEM when free space is insufficient, or ESP_FAIL.
 */
esp_err_t shell_fs_copy_file(const char *source_path, const char *dest_path)
{
    shell_sd_session_t session;
    FILE *source = NULL;
    FILE *dest = NULL;
    uint8_t buffer[SHELL_FILE_IO_BUFFER_BYTES];
    uint64_t source_bytes;
    uint64_t dest_bytes;
    uint64_t copied = 0;
    int last_progress_pct = 0;
    bool report_progress;
    esp_err_t error;

    if (source_path == NULL || dest_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Copying a file onto itself would open the destination with "wb",
     * truncating the source to zero before the first read. */
    if (storage_paths_are_same(source_path, dest_path)) {
        shell_print_error("copy: %s and the destination are the same file", source_path);
        return ESP_ERR_INVALID_ARG;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    source_bytes = storage_get_file_size(source_path);
    dest_bytes = storage_get_file_size(dest_path);

    /* An existing destination is truncated, so its blocks come back to the
     * free pool and only the difference is actually needed. */
    if (!storage_check_free_space(source_bytes, dest_bytes, "copy")) {
        shell_sd_end(&session, "copy");
        return ESP_ERR_NO_MEM;
    }

    source = fopen(source_path, "rb");
    if (source == NULL) {
        shell_sd_end(&session, "copy");
        return ESP_ERR_NOT_FOUND;
    }

    dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        char dest_fatfs[SHELL_SD_PATH_BYTES];
        FILINFO dest_info;

        fclose(source);

        /* Now that attributes are carried across copies, a read-only
         * destination is a realistic reason for this failure. Say so instead
         * of reporting a bare error, and point at the fix. */
        memset(&dest_info, 0, sizeof(dest_info));
        if (shell_sd_vfs_to_fatfs_path(dest_path, dest_fatfs, sizeof(dest_fatfs)) == ESP_OK &&
            f_stat(dest_fatfs, &dest_info) == FR_OK &&
            (dest_info.fattrib & AM_RDO) != 0) {
            shell_print_error("copy: %s is read-only (use attrib -R to clear it)", dest_path);
        }

        shell_sd_end(&session, "copy");
        return ESP_FAIL;
    }

    report_progress = (source_bytes >= P4_CONFIG_COPY_PROGRESS_THRESHOLD);

    while (!feof(source)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), source);
        if (bytes_read == 0) {
            break;
        }
        if (fwrite(buffer, 1, bytes_read, dest) != bytes_read) {
            fclose(dest);
            fclose(source);
            /* Remove the partial destination so a failed copy never leaves a
             * truncated file that looks complete. */
            (void)unlink(dest_path);
            shell_print_error("copy: write failed, removed the partial %s", dest_path);
            shell_sd_end(&session, "copy");
            return ESP_FAIL;
        }

        copied += bytes_read;

        if (report_progress && source_bytes > 0) {
            int pct = (int)((copied * 100ULL) / source_bytes);

            if (pct >= last_progress_pct + P4_CONFIG_COPY_PROGRESS_STEP_PCT) {
                last_progress_pct = pct - (pct % P4_CONFIG_COPY_PROGRESS_STEP_PCT);
                shell_transcript_appendf_ansi(SH_LBL "copy:" SH_RST " " SH_NUM "%d%%" SH_RST "\n", last_progress_pct);
            }
        }
    }

    fclose(dest);
    fclose(source);

    /* Carry the source's R/H/S/A attributes across, after the data is written
     * because a read-only destination could not have been opened for writing.
     * A failure here is reported but does not fail the copy: the file content
     * is already correct, and losing an archive bit is not worth discarding
     * a completed transfer. */
    {
        esp_err_t attr_error = storage_copy_attributes(source_path, dest_path);

        if (attr_error != ESP_OK) {
            shell_record_warningf("copy",
                                  "Copied %s but could not carry its attributes (%s)",
                                  source_path,
                                  esp_err_to_name(attr_error));
        }
    }

    shell_sd_end(&session, "copy");
    return ESP_OK;
}

esp_err_t storage_copy_attributes(const char *source_path, const char *dest_path)
{
    char source_fatfs[SHELL_SD_PATH_BYTES];
    char dest_fatfs[SHELL_SD_PATH_BYTES];
    FILINFO info;
    FRESULT result;
    esp_err_t error;

    if (source_path == NULL || dest_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    error = shell_sd_vfs_to_fatfs_path(source_path, source_fatfs, sizeof(source_fatfs));
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_vfs_to_fatfs_path(dest_path, dest_fatfs, sizeof(dest_fatfs));
    if (error != ESP_OK) {
        return error;
    }

    memset(&info, 0, sizeof(info));
    result = f_stat(source_fatfs, &info);
    if (result != FR_OK) {
        return shell_sd_fresult_to_esp_err(result);
    }

    /* Carry only the user-visible attribute bits. AM_DIR is structural and
     * must never be forced onto a destination. */
    result = f_chmod(dest_fatfs,
                     info.fattrib & (AM_RDO | AM_HID | AM_SYS | AM_ARC),
                     AM_RDO | AM_HID | AM_SYS | AM_ARC);

    return shell_sd_fresult_to_esp_err(result);
}

esp_err_t shell_list_directory_path(const char *normalized_path)
{
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
    unsigned int dir_count = 0;
    unsigned int file_count = 0;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_sd_end(&session, "dir");
        return error;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        shell_transcript_appendf("%s  %s\n", size_text, normalized_path);
        shell_sd_end(&session, "dir");
        return ESP_OK;
    }

    error = shell_sd_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        shell_sd_end(&session, "dir");
        return error;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        shell_sd_end(&session, "dir");
        return shell_sd_fresult_to_esp_err(result);
    }
    dir_open = true;

    shell_transcript_appendf_ansi(SH_HEAD " Directory of %s" SH_RST "\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = shell_sd_fresult_to_esp_err(result);
            break;
        }

        if (entry_info.fname[0] == '\0') {
            error = ESP_OK;
            break;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        if (strcmp(lfn, ".") == 0 || strcmp(lfn, "..") == 0) {
            continue;
        }

        if (listed_entries == SHELL_SD_LIST_LIMIT) {
            shell_transcript_appendf("dir: listing truncated after %u entries\n", (unsigned int)SHELL_SD_LIST_LIMIT);
            shell_record_warningf("dir", "Truncated directory listing for %s", normalized_path);
            error = ESP_OK;
            break;
        }

        if (entry_info.fattrib & AM_DIR) {
            shell_transcript_appendf_ansi(SH_SIZE "<DIR>" SH_RST "      " SH_DIR "%s" SH_RST "\n", lfn);
            dir_count++;
        } else {
            shell_sd_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
            shell_transcript_appendf_ansi(SH_SIZE "%10s" SH_RST " " SH_FILE "%s" SH_RST "\n", size_text, lfn);
            file_count++;
        }
        listed_entries++;
    }

    shell_transcript_appendf_ansi(SH_NUM "%u" SH_RST " " SH_LBL "file(s)" SH_RST "  " SH_NUM "%u" SH_RST " " SH_LBL "dir(s)" SH_RST "\n", file_count, dir_count);
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    shell_sd_end(&session, "dir");
    return error;
}

esp_err_t shell_print_file_text(const char *normalized_path)
{
    shell_sd_session_t session;
    FILE *file = NULL;
    struct stat path_stat;
    esp_err_t error;
    unsigned char buffer[SHELL_SD_IO_BUFFER_BYTES + 1];

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_sd_end(&session, "type");
        return error;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        shell_sd_end(&session, "type");
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "type");
        return ESP_FAIL;
    }

    while (!feof(file)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
        size_t index;

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
        shell_transcript_appendf("%s", (char *)buffer);
    }

    fclose(file);
    shell_transcript_append_text("\n");
    shell_sd_end(&session, "type");
    return ESP_OK;
}

esp_err_t shell_write_redirect_output(const char *path, const char *text, bool append_mode)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    error = shell_fs_resolve_path(path, resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        shell_sd_end(&session, "redirection");
        return ESP_FAIL;
    }

    if (text != NULL && text[0] != '\0') {
        size_t text_len = strlen(text);
        if (fwrite(text, 1, text_len, file) != text_len) {
            fclose(file);
            shell_sd_end(&session, "redirection");
            return ESP_FAIL;
        }
    }

    fclose(file);
    shell_sd_end(&session, "redirection");
    return ESP_OK;
}

/* ========================================================================
 * VOLUME CAPACITY AND GUARDRAILS
 * ======================================================================== */

esp_err_t storage_get_space_info(storage_space_info_t *info_out)
{
    shell_sd_session_t session;
    FATFS *fatfs = NULL;
    DWORD free_clusters = 0;
    FRESULT result;
    esp_err_t error;

    if (info_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info_out, 0, sizeof(*info_out));

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    /* f_getfree() also hands back the FATFS object, which carries the
     * cluster geometry needed to turn cluster counts into bytes. */
    result = f_getfree(SHELL_SD_FATFS_DRIVE, &free_clusters, &fatfs);
    if (result != FR_OK || fatfs == NULL) {
        shell_sd_end(&session, "space");
        return shell_sd_fresult_to_esp_err(result);
    }

    /* n_fatent counts the FAT entries; the first two are reserved, so the
     * usable cluster count is n_fatent - 2. */
    info_out->total_clusters = (fatfs->n_fatent > 2) ? (uint32_t)(fatfs->n_fatent - 2) : 0;
    info_out->free_clusters = (uint32_t)free_clusters;

    /* FATFS only carries a per-volume `ssize` when the build supports more
     * than one sector size; otherwise every volume uses FF_MAX_SS. */
#if FF_MAX_SS != FF_MIN_SS
    info_out->cluster_bytes = (uint32_t)fatfs->csize * (uint32_t)fatfs->ssize;
#else
    info_out->cluster_bytes = (uint32_t)fatfs->csize * (uint32_t)FF_MAX_SS;
#endif
    info_out->total_bytes = (uint64_t)info_out->total_clusters * (uint64_t)info_out->cluster_bytes;
    info_out->free_bytes = (uint64_t)info_out->free_clusters * (uint64_t)info_out->cluster_bytes;
    info_out->used_bytes = (info_out->total_bytes >= info_out->free_bytes)
                               ? (info_out->total_bytes - info_out->free_bytes)
                               : 0;

    shell_sd_end(&session, "space");
    return ESP_OK;
}

uint64_t storage_get_file_size(const char *resolved_path)
{
    struct stat st;

    if (resolved_path == NULL || resolved_path[0] == '\0') {
        return 0;
    }

    if (shell_sd_stat_path(resolved_path, &st) != ESP_OK) {
        return 0;
    }

    if (!S_ISREG(st.st_mode)) {
        return 0;
    }

    return (uint64_t)st.st_size;
}

bool storage_paths_are_same(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    /* Both sides have already been through shell_fs_resolve_path(), which
     * collapses `.` and `..` and produces one canonical spelling, so a plain
     * comparison is exact. FAT is case-insensitive, so compare that way. */
    return strcasecmp(left, right) == 0;
}

bool storage_check_free_space(uint64_t needed_bytes, uint64_t reclaim_bytes, const char *operation)
{
    storage_space_info_t info;
    char needed_text[32];
    char free_text[32];
    uint64_t effective_free;
    esp_err_t error;

    if (needed_bytes == 0) {
        return true;
    }

    error = storage_get_space_info(&info);
    if (error != ESP_OK) {
        /* Capacity is unknown. Allow the operation rather than blocking a
         * legitimate write on a query failure; the write itself will still
         * report an honest error if the card really is full. */
        shell_record_warningf(operation != NULL ? operation : "storage",
                              "Could not read free space (%s); proceeding without the check",
                              esp_err_to_name(error));
        return true;
    }

    /* Space the destination currently occupies is released by the write, so
     * an in-place overwrite only needs the difference. */
    effective_free = info.free_bytes + reclaim_bytes;

    if (effective_free >= needed_bytes &&
        (effective_free - needed_bytes) >= P4_CONFIG_STORAGE_FREE_MARGIN_BYTES) {
        return true;
    }

    shell_sd_format_size(needed_bytes, needed_text, sizeof(needed_text));
    shell_sd_format_size(effective_free, free_text, sizeof(free_text));

    shell_print_error("%s: not enough free space (need %s, available %s)",
                             operation != NULL ? operation : "storage",
                             needed_text,
                             free_text);
    shell_record_warningf(operation != NULL ? operation : "storage",
                          "Refused a write needing %llu bytes with %llu available",
                          (unsigned long long)needed_bytes,
                          (unsigned long long)effective_free);
    return false;
}

/* ========================================================================
 * INPUT REDIRECTION
 * ======================================================================== */

void storage_set_input_redirect(const char *resolved_path)
{
    if (resolved_path == NULL || resolved_path[0] == '\0') {
        storage_clear_input_redirect();
        return;
    }

    snprintf(s_input_redirect, sizeof(s_input_redirect), "%s", resolved_path);
    s_input_redirect_active = true;
}

const char *storage_get_input_redirect(void)
{
    return s_input_redirect_active ? s_input_redirect : NULL;
}

void storage_clear_input_redirect(void)
{
    s_input_redirect[0] = '\0';
    s_input_redirect_active = false;
}

esp_err_t storage_resolve_input_source(const char *argument, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    output[0] = '\0';

    /* An explicit filename always wins, matching DOS behavior where
     * `sort file.txt < other.txt` reads file.txt. */
    if (argument != NULL && argument[0] != '\0') {
        return shell_fs_resolve_path(argument, output, output_size);
    }

    if (!s_input_redirect_active) {
        return ESP_ERR_NOT_FOUND;
    }

    if (snprintf(output, output_size, "%s", s_input_redirect) >= (int)output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void storage_init(void)
{
    if (s_initialized) {
        return;
    }

    snprintf(s_shell_cwd, sizeof(s_shell_cwd), "%s", BSP_SD_MOUNT_POINT);
    s_sd_persistent_mounted = false;
    s_sd_ejected = false;
    storage_clear_input_redirect();

    s_initialized = true;
    ESP_LOGI(STORAGE_TAG, "Storage module initialized");
}

bool storage_is_initialized(void)
{
    return s_initialized;
}

/**
 * @file trash.c
 * @brief Recycle bin (`.trash`) for P4MiniShell.
 *
 * `del`/`erase` move matching files and `rd /s` moves whole directory trees
 * into a hidden `.trash` folder instead of deleting them; `undelete` /
 * `trash restore` bring them back, and `trash`/`recycle` manage the bin.
 *
 * Design:
 *   - Every entry is stored as `<epoch>_<basename>` under the trash folder
 *     with a side-car `<entry>.meta` text file recording the original path,
 *     type, size, and move time. A move is an atomic FATFS `rename`, so a
 *     failed move always leaves the original intact.
 *   - The meta file is written before the rename; if the rename fails the
 *     meta is removed again. Restore reads the meta and renames back,
 *     recreating missing parent directories.
 *   - Size / age / entry-count limits are enforced on every trash operation,
 *     purging the oldest entries first (FIFO by move time).
 *   - All I/O goes through guarded SD sessions; enumeration uses heap
 *     buffers so nothing path-sized lives on the worker stack.
 *
 * The trash folder is created with the FATFS hidden attribute and the `dir`
 * command hides hidden/system entries by default (DOS behaviour), so `.trash`
 * stays out of ordinary listings unless the user asks for hidden files.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "ff.h"

#include "ansi.h"
#include "ansi_palette.h"
#include "shell.h"
#include "storage.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"

#define TRASH_TAG                "trash"

#define TRASH_ENABLE             P4_CONFIG_TRASH_ENABLE
#define TRASH_MAX_BYTES          P4_CONFIG_TRASH_MAX_BYTES
#define TRASH_MAX_AGE_SEC        P4_CONFIG_TRASH_MAX_AGE_SEC
#define TRASH_MAX_ENTRIES        P4_CONFIG_TRASH_MAX_ENTRIES
#define TRASH_OP_MAX             P4_CONFIG_TRASH_OPERATION_MAX
#define TRASH_PATH_BYTES         P4_CONFIG_SD_PATH_BYTES

/* Trash root (<= TRASH_PATH_BYTES) plus "/" plus an entry name (<= LFN) plus
 * the ".meta" suffix; sized so the compiler can prove the joins fit. */
#define TRASH_FULL_PATH_BYTES    (TRASH_PATH_BYTES * 2 + 16)

/** One trash entry snapshot. */
typedef struct {
    char entry_name[TRASH_PATH_BYTES];  /* unique name inside the trash */
    char original[TRASH_PATH_BYTES];    /* original absolute path */
    bool is_dir;
    uint64_t size;
    uint64_t time_sec;
} trash_entry_t;

/** Resolved trash folder path (lazy, absolute). */
static const char *trash_root_path(void)
{
    static char path[TRASH_PATH_BYTES];

    if (path[0] == '\0') {
        if (shell_sd_resolve_path(P4_CONFIG_TRASH_PATH, path, sizeof(path)) != ESP_OK) {
            snprintf(path, sizeof(path), "%s/.trash", BSP_SD_MOUNT_POINT);
        }
    }
    return path;
}

static bool trash_path_is_inside(const char *resolved_path)
{
    size_t root_len = strlen(trash_root_path());

    return strncmp(resolved_path, trash_root_path(), root_len) == 0 &&
           (resolved_path[root_len] == '\0' || resolved_path[root_len] == '/');
}

static const char *trash_basename(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

/** Create the trash folder and mark it hidden. */
static esp_err_t trash_ensure_folder(void)
{
    struct stat st;
    char fatfs_path[TRASH_PATH_BYTES];
    FILINFO info;

    if (stat(trash_root_path(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    if (mkdir(trash_root_path(), 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (shell_sd_vfs_to_fatfs_path(trash_root_path(), fatfs_path, sizeof(fatfs_path)) == ESP_OK &&
        f_stat(fatfs_path, &info) == FR_OK) {
        (void)f_chmod(fatfs_path, info.fattrib | AM_HID, AM_HID);
    }
    return ESP_OK;
}

/** Write the `.meta` side-car for an entry. */
static int trash_meta_write(const char *entry_name, const char *original,
                            bool is_dir, uint64_t size)
{
    char meta_path[TRASH_FULL_PATH_BYTES];
    FILE *file;

    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", trash_root_path(), entry_name);
    file = fopen(meta_path, "w");
    if (file == NULL) {
        return -1;
    }
    fprintf(file, "%s\ntype=%s\nsize=%llu\ntime=%lu\n",
            original != NULL ? original : "",
            is_dir ? "dir" : "file",
            (unsigned long long)size,
            (unsigned long)time(NULL));
    fclose(file);
    return 0;
}

/** Read the `.meta` side-car for an entry. */
static int trash_meta_read(const char *entry_name, trash_entry_t *entry)
{
    char meta_path[TRASH_FULL_PATH_BYTES];
    char line[TRASH_PATH_BYTES + 64];
    int line_no = 0;
    FILE *file;

    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", trash_root_path(), entry_name);
    file = fopen(meta_path, "r");
    if (file == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line_no == 0) {
            strncpy(entry->original, line, sizeof(entry->original) - 1);
            entry->original[sizeof(entry->original) - 1] = '\0';
        } else if (strncmp(line, "type=", 5) == 0) {
            entry->is_dir = (strcmp(line + 5, "dir") == 0);
        } else if (strncmp(line, "size=", 5) == 0) {
            entry->size = strtoull(line + 5, NULL, 10);
        } else if (strncmp(line, "time=", 5) == 0) {
            entry->time_sec = strtoull(line + 5, NULL, 10);
        }
        line_no++;
    }
    fclose(file);
    return 0;
}

/** Produce a unique trash entry name for a basename (retries on collision). */
static esp_err_t trash_unique_name(const char *basename, char *out, size_t out_size)
{
    unsigned long stamp = (unsigned long)time(NULL);
    int attempt;

    for (attempt = 0; attempt < 1000; attempt++) {
        char full[TRASH_FULL_PATH_BYTES];
        int n;

        if (attempt == 0) {
            n = snprintf(out, out_size, "%lu_%s", stamp, basename);
        } else {
            n = snprintf(out, out_size, "%lu_%s_%d", stamp, basename, attempt);
        }
        // A truncated candidate must never be tested for uniqueness:
        // it could collide with a live entry. Try the next attempt.
        if (n < 0 || (size_t)n >= out_size) {
            continue;
        }
        n = snprintf(full, sizeof(full), "%s/%s", trash_root_path(), out);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            continue;
        }
        if (access(full, F_OK) != 0) {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/** Delete a directory tree bottom-up (bounded by the recursion depth cap). */
/** Delete a directory tree bottom-up (bounded by the recursion depth cap).
 * @return 0 on success, -1 if any entry could not be removed. */
static int trash_remove_tree_recursive(const char *dir_path, int depth)
{
    DIR *dir;
    struct dirent *entry;
    int ok = 0;

    if (depth >= P4_CONFIG_DIR_RECURSE_DEPTH_MAX) {
        return -1;
    }
    dir = opendir(dir_path);
    if (dir == NULL) {
        if (rmdir(dir_path) == 0) {
            return 0;
        }
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *full;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        full = malloc(strlen(dir_path) + strlen(entry->d_name) + 2);
        if (full == NULL) {
            ok = -1;
            break;
        }
        snprintf(full, strlen(dir_path) + strlen(entry->d_name) + 2, "%s/%s",
                 dir_path, entry->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (trash_remove_tree_recursive(full, depth + 1) != 0) {
                ok = -1;
            }
        } else if (unlink(full) != 0) {
            ok = -1;
        }
        free(full);
    }
    closedir(dir);
    if (rmdir(dir_path) != 0) {
        ok = -1;
    }
    return ok;
}

/** Delete one trash entry (file or whole tree) plus its `.meta`. */
static void trash_delete_entry(const trash_entry_t *entry)
{
    char full[TRASH_FULL_PATH_BYTES];
    char meta_path[TRASH_FULL_PATH_BYTES];
    struct stat st;

    snprintf(full, sizeof(full), "%s/%s", trash_root_path(), entry->entry_name);
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", trash_root_path(), entry->entry_name);
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        (void)trash_remove_tree_recursive(full, 0);
    } else {
        (void)unlink(full);
    }
    (void)remove(meta_path);
}

/**
 * Walk the trash folder calling @p cb for every entry (skipping `.meta`
 * files and dot entries), bounded by P4_CONFIG_TRASH_MAX_ENTRIES.
 */
typedef int (*trash_visit_cb)(const trash_entry_t *entry, void *arg);

static esp_err_t trash_walk(trash_visit_cb cb, void *arg)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    dir = opendir(trash_root_path());
    if (dir == NULL) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    while ((entry = readdir(dir)) != NULL) {
        size_t len;
        trash_entry_t e;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        len = strlen(entry->d_name);
        if (len > 5 && strcmp(entry->d_name + len - 5, ".meta") == 0) {
            continue;
        }
        if (count >= TRASH_MAX_ENTRIES) {
            break;
        }
        memset(&e, 0, sizeof(e));
        snprintf(e.entry_name, sizeof(e.entry_name), "%s", entry->d_name);
        (void)trash_meta_read(entry->d_name, &e);
        if (cb != NULL && cb(&e, arg) != 0) {
            break;
        }
        count++;
    }
    closedir(dir);
    return ESP_OK;
}

/** Move a resolved file or directory tree into the trash (or delete it). */
static esp_err_t trash_move_entry(const char *resolved_path, bool is_dir, bool permanent)
{
    char entry_name[TRASH_PATH_BYTES];
    char dest[TRASH_FULL_PATH_BYTES];
    char meta_path[TRASH_FULL_PATH_BYTES];
    struct stat st;
    uint64_t size = 0;

    /* Fail loudly on a missing path instead of reporting a successful
     * "permanent" unlink of something that was never there. */
    if (stat(resolved_path, &st) != 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (permanent) {
        if (is_dir || S_ISDIR(st.st_mode)) {
            return trash_remove_tree_recursive(resolved_path, 0) == 0 ? ESP_OK : ESP_FAIL;
        }
        if (unlink(resolved_path) != 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (trash_ensure_folder() != ESP_OK) {
        return ESP_FAIL;
    }
    if (trash_unique_name(trash_basename(resolved_path), entry_name, sizeof(entry_name)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        size = (uint64_t)st.st_size;
    }

    /* Write the meta before the rename; the item is only "in trash" once
     * both the entry and its meta exist. */
    if (!storage_check_free_space(512, 0, "trash")) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (trash_meta_write(entry_name, resolved_path, is_dir, size) != 0) {
        return ESP_FAIL;
    }

    snprintf(dest, sizeof(dest), "%s/%s", trash_root_path(), entry_name);
    if (rename(resolved_path, dest) != 0) {
        snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", trash_root_path(), entry_name);
        (void)remove(meta_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- Public API ---- */

bool storage_trash_enabled(void)
{
    return TRASH_ENABLE != 0;
}

const char *storage_trash_path(void)
{
    return trash_root_path();
}

esp_err_t storage_trash_delete_file(const char *resolved_path, bool permanent)
{
    esp_err_t err;
    shell_sd_session_t session;

    if (!storage_trash_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (resolved_path == NULL || trash_path_is_inside(resolved_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }
    err = trash_move_entry(resolved_path, false, permanent);
    shell_sd_end(&session, TRASH_TAG);
    return err;
}

/* Recursive wildcard matcher state (heap paths only). */
typedef struct {
    const char *pattern;
    bool permanent;
    int count;
    bool stopped;
} pattern_arg_t;

static void pattern_walk_recursive(const char *dir_path, const char *pattern,
                                   bool permanent, pattern_arg_t *arg, int depth)
{
    DIR *dir;
    struct dirent *entry;

    if (arg->stopped || depth >= P4_CONFIG_DIR_RECURSE_DEPTH_MAX ||
        trash_path_is_inside(dir_path)) {
        /* The recycle bin is never traversed: `del *.* /s` must not pull
         * entries already stored there back into the bin. */
        return;
    }
    dir = opendir(dir_path);
    if (dir == NULL) {
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *full;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (arg->count >= TRASH_OP_MAX) {
            arg->stopped = true;
            break;
        }
        full = malloc(strlen(dir_path) + strlen(entry->d_name) + 2);
        if (full == NULL) {
            break;
        }
        snprintf(full, strlen(dir_path) + strlen(entry->d_name) + 2, "%s/%s",
                 dir_path, entry->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            pattern_walk_recursive(full, pattern, permanent, arg, depth + 1);
        } else if (shell_wildcard_match(pattern, entry->d_name)) {
            (void)trash_move_entry(full, false, permanent);
            arg->count++;
        }
        free(full);
        if (arg->stopped) {
            break;
        }
    }
    closedir(dir);
}

esp_err_t storage_trash_delete_pattern(const char *resolved_dir, const char *pattern,
                                       bool recursive, bool permanent, int *deleted_out)
{
    esp_err_t err;
    shell_sd_session_t session;
    int deleted = 0;

    if (!storage_trash_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (resolved_dir == NULL || pattern == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }

    if (recursive) {
        pattern_arg_t arg = {
            .pattern = pattern,
            .permanent = permanent,
            .count = 0,
            .stopped = false,
        };
        pattern_walk_recursive(resolved_dir, pattern, permanent, &arg, 0);
        deleted = arg.count;
    } else {
        DIR *dir = opendir(resolved_dir);
        struct dirent *entry;

        if (dir != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                struct stat st;
                char full[TRASH_FULL_PATH_BYTES];

                if (!shell_wildcard_match(pattern, entry->d_name)) {
                    continue;
                }
                snprintf(full, sizeof(full), "%s/%s", resolved_dir, entry->d_name);
                if (stat(full, &st) == 0 && !S_ISDIR(st.st_mode)) {
                    if (trash_move_entry(full, false, permanent) == ESP_OK) {
                        deleted++;
                    }
                }
            }
            closedir(dir);
        }
    }

    shell_sd_end(&session, TRASH_TAG);
    if (deleted_out != NULL) {
        *deleted_out = deleted;
    }
    return ESP_OK;
}

esp_err_t storage_trash_remove_tree(const char *resolved_path, bool permanent)
{
    esp_err_t err;
    shell_sd_session_t session;

    if (!storage_trash_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (resolved_path == NULL || trash_path_is_inside(resolved_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }
    err = trash_move_entry(resolved_path, true, permanent);
    shell_sd_end(&session, TRASH_TAG);
    return err;
}

/* Resolve an entry by name or 1-based index (walk order). */
typedef struct {
    const char *want;
    int index;
    trash_entry_t found;
    bool match;
} resolve_arg_t;

static int trash_resolve_cb(const trash_entry_t *entry, void *arg)
{
    resolve_arg_t *ra = arg;
    const char *base;

    ra->index++;
    if (ra->match) {
        return 0;
    }
    if (isdigit((unsigned char)ra->want[0])) {
        if (ra->index == atoi(ra->want)) {
            ra->found = *entry;
            ra->match = true;
        }
        return 0;
    }
    if (strcmp(entry->entry_name, ra->want) == 0) {
        ra->found = *entry;
        ra->match = true;
        return 0;
    }
    if (strcmp(entry->original, ra->want) == 0) {
        ra->found = *entry;
        ra->match = true;
        return 0;
    }
    base = trash_basename(entry->original);
    if (base[0] != '\0' && strcmp(base, ra->want) == 0) {
        ra->found = *entry;
        ra->match = true;
    }
    return 0;
}

/** Recursively create missing parent directories of an absolute path. */
static esp_err_t trash_mkdir_parents(const char *path)
{
    char copy[TRASH_PATH_BYTES];
    char *cursor;

    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    /* Trim trailing slashes. */
    cursor = copy + strlen(copy);    while (cursor > copy && *(cursor - 1) == '/') {
        *--cursor = '\0';
    }
    cursor = copy;
    while (*cursor != '\0') {
        if (*cursor == '/') {
            *cursor = '\0';
            if (cursor != copy) {
                (void)mkdir(copy, 0775);
            }
            *cursor = '/';
        }
        cursor++;
    }
    return ESP_OK;
}

esp_err_t storage_trash_restore(const char *name_or_index)
{
    resolve_arg_t ra;
    esp_err_t err;
    shell_sd_session_t session;
    char src[TRASH_FULL_PATH_BYTES];
    char dest[TRASH_FULL_PATH_BYTES];
    char meta_path[TRASH_FULL_PATH_BYTES];
    struct stat st;

    if (!storage_trash_enabled() || name_or_index == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(&ra, 0, sizeof(ra));
    ra.want = name_or_index;
    err = trash_walk(trash_resolve_cb, &ra);
    if (err != ESP_OK || !ra.match) {
        return ESP_ERR_NOT_FOUND;
    }

    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(src, sizeof(src), "%s/%s", trash_root_path(), ra.found.entry_name);
    if (ra.found.original[0] != '\0') {
        snprintf(dest, sizeof(dest), "%s", ra.found.original);
    } else {
        snprintf(dest, sizeof(dest), "%s/%s", shell_get_cwd(), trash_basename(ra.found.entry_name));
    }

    if (stat(dest, &st) == 0) {
        shell_sd_end(&session, TRASH_TAG);
        return ESP_ERR_INVALID_STATE;   /* destination occupied; do not overwrite */
    }
    (void)trash_mkdir_parents(dest);
    if (rename(src, dest) != 0) {
        shell_sd_end(&session, TRASH_TAG);
        return ESP_FAIL;
    }
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", trash_root_path(), ra.found.entry_name);
    (void)remove(meta_path);
    shell_sd_end(&session, TRASH_TAG);
    return ESP_OK;
}

esp_err_t storage_trash_purge(const char *name_or_index)
{
    resolve_arg_t ra;
    esp_err_t err;
    shell_sd_session_t session;
    trash_entry_t entry;

    if (!storage_trash_enabled() || name_or_index == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(&ra, 0, sizeof(ra));
    ra.want = name_or_index;
    err = trash_walk(trash_resolve_cb, &ra);
    if (err != ESP_OK || !ra.match) {
        return ESP_ERR_NOT_FOUND;
    }
    entry = ra.found;

    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }
    trash_delete_entry(&entry);
    shell_sd_end(&session, TRASH_TAG);
    return ESP_OK;
}

static int trash_delete_all_cb(const trash_entry_t *entry, void *arg)
{
    (void)arg;
    trash_delete_entry(entry);
    return 0;
}

esp_err_t storage_trash_empty(void)
{
    esp_err_t err;
    shell_sd_session_t session;

    if (!storage_trash_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }
    err = trash_walk(trash_delete_all_cb, NULL);
    shell_sd_end(&session, TRASH_TAG);
    return err;
}

static int trash_list_cb(const trash_entry_t *entry, void *arg)
{
    int *index = arg;
    char size_text[24];
    unsigned long now = (unsigned long)time(NULL);
    unsigned long age = (entry->time_sec != 0 && now > entry->time_sec) ? now - (unsigned long)entry->time_sec : 0;

    (*index)++;
    shell_sd_format_size(entry->size, size_text, sizeof(size_text));
    shell_transcript_appendf_ansi("  " SH_NUM "%d." SH_RST " " SH_VAL "%-28s" SH_RST " " SH_VAL "%s" SH_RST "%s %s, %lu s\n",
                                  *index,
                                  trash_basename(entry->entry_name),
                                  entry->original[0] != '\0' ? entry->original : "(unknown)",
                                  entry->is_dir ? " <DIR> " : " ",
                                  size_text,
                                  age);
    return 0;
}

esp_err_t storage_trash_list(void)
{
    int index = 0;
    esp_err_t err;
    shell_sd_session_t session;

    if (!storage_trash_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }
    shell_transcript_appendf_ansi(SH_HEAD "Trash" SH_RST " (" SH_PATH "%s" SH_RST ")\n", trash_root_path());
    err = trash_walk(trash_list_cb, &index);
    if (index == 0) {
        shell_transcript_appendf_ansi(SH_MUTE "trash: empty\n" SH_RST);
    }
    shell_sd_end(&session, TRASH_TAG);
    return err;
}

typedef struct {
    size_t count;
    uint64_t bytes;
    uint64_t oldest_sec;
} info_arg_t;

static int trash_info_cb(const trash_entry_t *entry, void *arg)
{
    info_arg_t *info = arg;

    info->count++;
    info->bytes += entry->size;
    if (entry->time_sec != 0 && (info->oldest_sec == 0 || entry->time_sec < info->oldest_sec)) {
        info->oldest_sec = entry->time_sec;
    }
    return 0;
}

esp_err_t storage_trash_info(void)
{
    info_arg_t info;
    esp_err_t err;
    shell_sd_session_t session;
    unsigned long now = (unsigned long)time(NULL);

    if (!storage_trash_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(&info, 0, sizeof(info));
    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }
    err = trash_walk(trash_info_cb, &info);
    shell_sd_end(&session, TRASH_TAG);

    shell_transcript_appendf_ansi(SH_HEAD "Trash" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "entries:" SH_RST " " SH_NUM "%u" SH_RST "/%u\n",
                                  (unsigned int)info.count, (unsigned int)TRASH_MAX_ENTRIES);
    {
        char used[24];
        char cap[24];
        shell_sd_format_size(info.bytes, used, sizeof(used));
        shell_sd_format_size(TRASH_MAX_BYTES, cap, sizeof(cap));
        shell_transcript_appendf_ansi("  " SH_LBL "size:" SH_RST " " SH_NUM "%s" SH_RST " / %s\n", used, cap);
    }
    if (info.oldest_sec != 0 && now > info.oldest_sec) {
        shell_transcript_appendf_ansi("  " SH_LBL "oldest:" SH_RST " " SH_NUM "%lu s" SH_RST "\n",
                                      (unsigned long)(now - (unsigned long)info.oldest_sec));
    } else {
        shell_transcript_appendf_ansi("  " SH_LBL "oldest:" SH_RST " " SH_MUTE "n/a" SH_RST "\n");
    }
    return err;
}

typedef struct {
    info_arg_t info;
    trash_entry_t oldest;
    bool have_oldest;
} limit_arg_t;

static int trash_limit_cb(const trash_entry_t *entry, void *arg)
{
    limit_arg_t *la = arg;

    la->info.count++;
    la->info.bytes += entry->size;
    if (entry->time_sec != 0 &&
        (!la->have_oldest || entry->time_sec < la->oldest.time_sec)) {
        la->oldest = *entry;
        la->have_oldest = true;
    }
    return 0;
}

void storage_trash_enforce_limits(void)
{
    limit_arg_t la;
    unsigned long now;
    shell_sd_session_t session;
    int guard;

    if (!storage_trash_enabled()) {
        return;
    }
    now = (unsigned long)time(NULL);

    for (guard = 0; guard < TRASH_MAX_ENTRIES + 8; guard++) {
        esp_err_t err;

        memset(&la, 0, sizeof(la));
        err = shell_sd_begin(&session);
        if (err != ESP_OK) {
            return;
        }
        (void)trash_walk(trash_limit_cb, &la);
        shell_sd_end(&session, TRASH_TAG);

        if (la.info.count == 0) {
            break;
        }
        if (la.info.count > (size_t)TRASH_MAX_ENTRIES ||
            la.info.bytes > TRASH_MAX_BYTES ||
            (la.have_oldest && la.oldest.time_sec != 0 &&
             now > (unsigned long)la.oldest.time_sec + (unsigned long)TRASH_MAX_AGE_SEC)) {
            shell_sd_begin(&session);
            trash_delete_entry(&la.oldest);
            shell_sd_end(&session, TRASH_TAG);
            continue;
        }
        break;
    }
}

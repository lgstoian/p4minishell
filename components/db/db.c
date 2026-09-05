/**
 * @file db.c
 * @brief Palm-OS-style SD-backed record store (see db.h for the layout).
 *
 * All database data lives on the SD card. Every write:
 *   - opens a guarded SD session
 *   - pre-checks free space (reclaiming the old file's size on overwrite)
 *   - writes a temp file and renames it into place (atomic)
 *   - removes the partial file on any failure
 * Command/record-sized buffers are heap-allocated (this code also runs on the
 * recursive batch path through the `db` command).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include "db.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "storage.h"
#include "bsp/esp-bsp.h"

#define DB_TAG              "db"
#define SHELL_SD_PATH_BYTES P4_CONFIG_SD_PATH_BYTES

#define DB_DIR_NAME         "DBS"
#define DB_HEADER           "HEADER.INI"
#define DB_CATEGORIES       "CATEGORIES.INI"
#define DB_INDEX            "INDEX.TXT"
#define DB_RECORDS_DIR      "RECORDS"

/* ------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/** True when the name is a legal single-path-component database name. */
bool db_name_valid(const char *name)
{
    size_t len;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    len = strlen(name);
    if (len >= P4_CONFIG_DB_NAME_BYTES) {
        return false;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        strchr(name, ':') != NULL || strchr(name, '.') != NULL) {
        return false;
    }
    return true;
}

/** Build the resolved directory path for a database into @p out. */
static void db_dir_path(const char *name, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s/%s.DB", BSP_SD_MOUNT_POINT, DB_DIR_NAME, name);
}

/** Build a resolved file path inside a database. */
static void db_file_path(const char *name, const char *file, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s/%s.DB/%s", BSP_SD_MOUNT_POINT, DB_DIR_NAME, name, file);
}

/** Build the resolved path of a record's .DAT file. */
static void db_record_path(const char *name, uint32_t id, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s/%s.DB/%s/R%08lX.DAT",
             BSP_SD_MOUNT_POINT, DB_DIR_NAME, name, DB_RECORDS_DIR, (unsigned long)id);
}

/** Current unix-ish timestamp (seconds since boot epoch is fine for stamps). */
static uint32_t db_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

/** Stat a path under a guarded session; returns true when it exists. */
static bool db_exists_path(const char *resolved)
{
    shell_sd_session_t session;
    struct stat st;
    bool exists = false;

    if (shell_sd_begin(&session) == ESP_OK) {
        exists = (shell_sd_stat_path(resolved, &st) == ESP_OK);
        shell_sd_end(&session, DB_TAG);
    }
    return exists;
}

/** True when the named database directory exists on the card. */
static bool db_exists(const char *name)
{
    char dir[SHELL_SD_PATH_BYTES];

    db_dir_path(name, dir, sizeof(dir));
    return db_exists_path(dir);
}

/** mkdir -p a path (resolved, absolute) with a guarded session. */
static esp_err_t db_mkdir_p(const char *resolved)
{
    shell_sd_session_t session;
    char *copy;
    char *p;
    esp_err_t error = ESP_OK;

    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    copy = strdup(resolved);
    if (copy == NULL) {
        shell_sd_end(&session, DB_TAG);
        return ESP_ERR_NO_MEM;
    }
    /* Skip the leading "/sdcard" mount point (already exists). */
    p = copy;
    while (*p == '/') {
        p++;
    }
    for (; *p != '\0'; p++) {
        if (*p == '/') {
            char save = *p;
            *p = '\0';
            if (copy[0] != '\0' && mkdir(copy, 0755) != 0 && errno != EEXIST) {
                error = ESP_FAIL;
                *p = save;
                break;
            }
            *p = save;
        }
    }
    if (error == ESP_OK && copy[0] != '\0' && mkdir(copy, 0755) != 0 && errno != EEXIST) {
        error = ESP_FAIL;
    }
    free(copy);
    shell_sd_end(&session, DB_TAG);
    return error;
}

/** Atomic-write a whole text file (temp + rename, free-space precheck). */
static esp_err_t db_write_text_file(const char *resolved, const char *text)
{
    char tmp[SHELL_SD_PATH_BYTES + 16];
    shell_sd_session_t session;
    FILE *file = NULL;
    size_t text_len = strlen(text);

    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", resolved);

    {
        uint64_t needed = (uint64_t)text_len + 512;
        uint64_t reclaim = 0;

        if (db_exists_path(resolved)) {
            /* best-effort reclaim of the old size */
            struct stat st;
            if (shell_sd_begin(&session) == ESP_OK) {
                if (shell_sd_stat_path(resolved, &st) == ESP_OK) {
                    reclaim = (uint64_t)st.st_size;
                }
                shell_sd_end(&session, DB_TAG);
            }
        }
        if (!storage_check_free_space(needed, reclaim, DB_TAG)) {
            shell_sd_end(&session, DB_TAG);
            return ESP_ERR_NO_MEM;
        }
    }

    file = fopen(tmp, "w");
    if (file == NULL) {
        shell_sd_end(&session, DB_TAG);
        return ESP_FAIL;
    }
    if (fwrite(text, 1, text_len, file) != text_len || fflush(file) != 0) {
        fclose(file);
        remove(tmp);
        shell_sd_end(&session, DB_TAG);
        return ESP_FAIL;
    }
    if (fclose(file) != 0) {
        remove(tmp);
        shell_sd_end(&session, DB_TAG);
        return ESP_FAIL;
    }
    if (rename(tmp, resolved) != 0) {
        remove(resolved);
        if (rename(tmp, resolved) != 0) {
            remove(tmp);
            shell_sd_end(&session, DB_TAG);
            return ESP_FAIL;
        }
    }
    shell_sd_end(&session, DB_TAG);
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * HEADER.INI accessors (via the shared INI file editor)
 * ---------------------------------------------------------------------- */

/** Read a HEADER.INI numeric field; returns 0 when absent. */
static uint32_t db_header_get_u32(const char *name, const char *key)
{
    char path[SHELL_SD_PATH_BYTES];
    char value[24];
    char *end;

    db_file_path(name, DB_HEADER, path, sizeof(path));
    if (storage_ini_file_get(path, key, value, sizeof(value)) != ESP_OK) {
        return 0;
    }
    return (uint32_t)strtoul(value, &end, 10);
}

static void db_header_set_u32(const char *name, const char *key, uint32_t v)
{
    char path[SHELL_SD_PATH_BYTES];
    char value[24];

    snprintf(value, sizeof(value), "%lu", (unsigned long)v);
    db_file_path(name, DB_HEADER, path, sizeof(path));
    (void)storage_ini_file_set(path, key, value);
}

/* ------------------------------------------------------------------------
 * INDEX.TXT read/write
 * ---------------------------------------------------------------------- */

/** One parsed index line. */
typedef struct {
    uint32_t id;
    uint8_t cat;
    uint8_t flags;
    char key[P4_CONFIG_DB_KEY_BYTES];
    uint32_t size;
    bool valid;
} db_index_line_t;

/** Parse "id cat flags key size". Returns false on a malformed line.
 *  An empty key is written as `-` so the key field is never ambiguous. */
static bool db_index_parse(const char *line, db_index_line_t *out)
{
    char *end;
    unsigned long id, cat, flags, size;

    memset(out, 0, sizeof(*out));
    id = strtoul(line, &end, 10);
    if (end == line) {
        return false;
    }
    line = end;
    cat = (unsigned long)strtoul(line, &end, 10);
    if (end == line) {
        return false;
    }
    line = end;
    flags = (unsigned long)strtoul(line, &end, 10);
    if (end == line) {
        return false;
    }
    line = end;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    {
        size_t k = 0;
        while (*line != '\0' && *line != ' ' && *line != '\t' && k + 1 < P4_CONFIG_DB_KEY_BYTES) {
            out->key[k++] = *line++;
        }
        out->key[k] = '\0';
    }
    if (strcmp(out->key, "-") == 0) {
        out->key[0] = '\0';
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size = strtoul(line, &end, 10);
    if (end == line) {
        return false;
    }

    out->id = (uint32_t)id;
    out->cat = (uint8_t)cat;
    out->flags = (uint8_t)flags;
    out->size = (uint32_t)size;
    out->valid = true;
    return true;
}

/** Format an index line into @p out (bounded). Empty keys use a `-` sentinel. */
static void db_index_format(const db_index_line_t *line, char *out, size_t out_size)
{
    const char *key = (line->key[0] != '\0') ? line->key : "-";

    snprintf(out, out_size, "%lu %u %u %s %lu\n",
             (unsigned long)line->id, (unsigned)line->cat, (unsigned)line->flags,
             key, (unsigned long)line->size);
}

/**
 * Rebuild INDEX.TXT from a heap array of lines, then update HEADER counts.
 * @p lines is a contiguous region of `db_index_line_t`, @p count entries.
 */
static esp_err_t db_rewrite_index(const char *name,
                                  const db_index_line_t *lines, int count,
                                  uint32_t live_count, uint32_t deleted_count)
{
    char idx_path[SHELL_SD_PATH_BYTES];
    size_t capacity = (size_t)count * (P4_CONFIG_DB_INDEX_LINE_BYTES + 1) + 64;
    char *text;
    size_t used = 0;
    int i;
    esp_err_t error;

    text = malloc(capacity);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    text[0] = '\0';
    for (i = 0; i < count; i++) {
        char one[P4_CONFIG_DB_INDEX_LINE_BYTES + 1];
        size_t len;

        db_index_format(&lines[i], one, sizeof(one));
        len = strlen(one);
        if (used + len + 1 >= capacity) {
            break;
        }
        memcpy(text + used, one, len);
        used += len;
    }
    text[used] = '\0';

    db_file_path(name, DB_INDEX, idx_path, sizeof(idx_path));
    error = db_write_text_file(idx_path, text);
    free(text);
    if (error != ESP_OK) {
        return error;
    }

    db_header_set_u32(name, "record_count", live_count);
    db_header_set_u32(name, "deleted_count", deleted_count);
    db_header_set_u32(name, "modified", db_now());
    return ESP_OK;
}

/** Read every index line into a heap array; returns the count (-1 on error). */
static int db_read_index(const char *name, db_index_line_t **out_lines)
{
    char idx_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    db_index_line_t *lines = NULL;
    int count = 0;
    int capacity = 16;
    char raw[P4_CONFIG_DB_INDEX_LINE_BYTES + 2];

    db_file_path(name, DB_INDEX, idx_path, sizeof(idx_path));

    if (shell_sd_begin(&session) != ESP_OK) {
        return -1;
    }
    file = fopen(idx_path, "r");
    if (file == NULL) {
        shell_sd_end(&session, DB_TAG);
        return 0;   /* missing index = empty db */
    }

    lines = malloc((size_t)capacity * sizeof(db_index_line_t));
    if (lines == NULL) {
        fclose(file);
        shell_sd_end(&session, DB_TAG);
        return -1;
    }

    while (fgets(raw, sizeof(raw), file) != NULL) {
        db_index_line_t line;
        db_index_line_t *grown;

        if (!db_index_parse(raw, &line) || !line.valid) {
            continue;
        }
        if (count >= P4_CONFIG_DB_MAX_RECORDS) {
            break;
        }
        if (count == capacity) {
            capacity *= 2;
            if (capacity > P4_CONFIG_DB_MAX_RECORDS * 2) {
                capacity = P4_CONFIG_DB_MAX_RECORDS * 2;
            }
            grown = realloc(lines, (size_t)capacity * sizeof(db_index_line_t));
            if (grown == NULL) {
                fclose(file);
                free(lines);
                shell_sd_end(&session, DB_TAG);
                return -1;
            }
            lines = grown;
        }
        lines[count++] = line;
    }

    fclose(file);
    shell_sd_end(&session, DB_TAG);
    *out_lines = lines;
    return count;
}

/** Find an index line by id; returns index or -1. */
static int db_index_find_id(const db_index_line_t *lines, int count, uint32_t id)
{
    int i;
    for (i = 0; i < count; i++) {
        if (lines[i].id == id) {
            return i;
        }
    }
    return -1;
}

/** Convert an index line into the public record-info form. */
static void db_index_to_info(const db_index_line_t *line, db_record_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = line->id;
    out->category = line->cat;
    out->flags = line->flags;
    snprintf(out->key, sizeof(out->key), "%s", line->key);
    out->size = line->size;
}

/* ------------------------------------------------------------------------
 * Payload read/write (RECORDS/R<id>.DAT)
 * ---------------------------------------------------------------------- */

/** Read a record's payload into a heap buffer (max DB_RECORD_MAX_BYTES). */
static char *db_read_payload(const char *name, uint32_t id, size_t *out_len)
{
    char path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    char *buf = NULL;
    long size;

    db_record_path(name, id, path, sizeof(path));

    if (shell_sd_begin(&session) != ESP_OK) {
        return NULL;
    }

    /* Determine the payload size from stat rather than fseek/ftell: the FATFS
     * VFS does not report a correct size via fseek(SEEK_END)+ftell. */
    {
        struct stat st;
        if (shell_sd_stat_path(path, &st) != ESP_OK) {
            shell_sd_end(&session, DB_TAG);
            return NULL;
        }
        size = (long)st.st_size;
    }
    if (size < 0 || size > P4_CONFIG_DB_RECORD_MAX_BYTES) {
        shell_sd_end(&session, DB_TAG);
        return NULL;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        shell_sd_end(&session, DB_TAG);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(file);
        shell_sd_end(&session, DB_TAG);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, file) != (size_t)size) {
        ESP_LOGE(DB_TAG, "payload read %s: want %ld got short", path, size);
        free(buf);
        fclose(file);
        shell_sd_end(&session, DB_TAG);
        return NULL;
    }
    buf[size] = '\0';
    fclose(file);
    shell_sd_end(&session, DB_TAG);
    if (out_len != NULL) {
        *out_len = (size_t)size;
    }
    return buf;
}

/** Write a record payload atomically (temp + rename). */
static esp_err_t db_write_payload(const char *name, uint32_t id,
                                  const void *payload, size_t len)
{
    char path[SHELL_SD_PATH_BYTES];
    char tmp[SHELL_SD_PATH_BYTES + 16];
    shell_sd_session_t session;
    FILE *file = NULL;

    if (len > P4_CONFIG_DB_RECORD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    db_record_path(name, id, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    {
        uint64_t reclaim = 0;
        struct stat st;
        if (shell_sd_stat_path(path, &st) == ESP_OK) {
            reclaim = (uint64_t)st.st_size;
        }
        if (!storage_check_free_space((uint64_t)len + 512, reclaim, DB_TAG)) {
            shell_sd_end(&session, DB_TAG);
            return ESP_ERR_NO_MEM;
        }
    }

    file = fopen(tmp, "wb");
    if (file == NULL) {
        shell_sd_end(&session, DB_TAG);
        return ESP_FAIL;
    }
    if ((len == 0 || fwrite(payload, 1, len, file) == len) && fflush(file) == 0) {
        if (fclose(file) == 0) {
            file = NULL;
            if (rename(tmp, path) == 0) {
                shell_sd_end(&session, DB_TAG);
                return ESP_OK;
            }
            remove(path);
            if (rename(tmp, path) == 0) {
                shell_sd_end(&session, DB_TAG);
                return ESP_OK;
            }
            remove(tmp);
            shell_sd_end(&session, DB_TAG);
            return ESP_FAIL;
        }
        file = NULL;
    }
    if (file != NULL) {
        fclose(file);
    }
    remove(tmp);
    shell_sd_end(&session, DB_TAG);
    return ESP_FAIL;
}

/** Physically delete a record's .DAT file (ignores a missing file). */
static void db_remove_payload(const char *name, uint32_t id)
{
    char path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;

    db_record_path(name, id, path, sizeof(path));
    if (shell_sd_begin(&session) == ESP_OK) {
        remove(path);
        shell_sd_end(&session, DB_TAG);
    }
}

/* ------------------------------------------------------------------------
 * Database lifecycle
 * ---------------------------------------------------------------------- */

esp_err_t db_create(const char *name, const char *creator, const char *type,
                    uint32_t version)
{
    char dir[SHELL_SD_PATH_BYTES];
    char head[SHELL_SD_PATH_BYTES];
    char cat[SHELL_SD_PATH_BYTES];
    char header[256];
    char categories[256];
    uint32_t now;
    esp_err_t error;

    if (!db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    db_dir_path(name, dir, sizeof(dir));
    if (db_exists_path(dir)) {
        return ESP_ERR_INVALID_STATE;   /* already exists */
    }

    now = db_now();

    error = db_mkdir_p(dir);
    if (error != ESP_OK) {
        return error;
    }

    db_file_path(name, DB_HEADER, head, sizeof(head));
    snprintf(header, sizeof(header),
             "name=%s\n"
             "creator=%s\n"
             "type=%s\n"
             "version=%lu\n"
             "next_id=1\n"
             "record_count=0\n"
             "deleted_count=0\n"
             "created=%lu\n"
             "modified=%lu\n",
             name, creator ? creator : "P4SH", type ? type : "DATA",
             (unsigned long)version, (unsigned long)now, (unsigned long)now);
    error = db_write_text_file(head, header);
    if (error != ESP_OK) {
        return error;
    }

    db_file_path(name, DB_CATEGORIES, cat, sizeof(cat));
    snprintf(categories, sizeof(categories),
             "0=Unfiled\n"
             "1=Business\n"
             "2=Personal\n"
             "3=Notes\n"
             "4=Reference\n"
             "5=Archive\n");
    error = db_write_text_file(cat, categories);
    if (error != ESP_OK) {
        return error;
    }

    /* RECORDS/ directory. */
    {
        char rec[SHELL_SD_PATH_BYTES];
        db_file_path(name, DB_RECORDS_DIR, rec, sizeof(rec));
        error = db_mkdir_p(rec);
        if (error != ESP_OK) {
            return error;
        }
    }

    return ESP_OK;
}

esp_err_t db_info(const char *name, db_info_t *out)
{
    char dir[SHELL_SD_PATH_BYTES];

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    db_dir_path(name, dir, sizeof(dir));
    if (!db_exists_path(dir)) {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(out->name, sizeof(out->name), "%s", name);
    {
        char path[SHELL_SD_PATH_BYTES];
        char value[P4_CONFIG_DB_ID_BYTES + 1];

        db_file_path(name, DB_HEADER, path, sizeof(path));
        if (storage_ini_file_get(path, "creator", value, sizeof(value)) == ESP_OK) {
            snprintf(out->creator, sizeof(out->creator), "%.*s",
                     (int)(sizeof(out->creator) - 1), value);
        }
        if (storage_ini_file_get(path, "type", value, sizeof(value)) == ESP_OK) {
            snprintf(out->type, sizeof(out->type), "%.*s",
                     (int)(sizeof(out->type) - 1), value);
        }
    }
    out->version = db_header_get_u32(name, "version");
    out->next_id = db_header_get_u32(name, "next_id");
    out->record_count = db_header_get_u32(name, "record_count");
    out->deleted_count = db_header_get_u32(name, "deleted_count");
    out->created_sec = db_header_get_u32(name, "created");
    out->modified_sec = db_header_get_u32(name, "modified");
    return ESP_OK;
}

/** Recursively delete a directory tree (bounded). */
static void db_remove_tree(const char *resolved)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(resolved);
    if (dir == NULL) {
        remove(resolved);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char child[SHELL_SD_PATH_BYTES + P4_CONFIG_LFN_BYTES];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", resolved, entry->d_name);
        if (shell_sd_stat_path(child, &st) == ESP_OK && S_ISDIR(st.st_mode)) {
            db_remove_tree(child);
        } else {
            remove(child);
        }
    }
    closedir(dir);
    remove(resolved);
}

esp_err_t db_drop(const char *name)
{
    char dir[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;

    if (!db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    db_dir_path(name, dir, sizeof(dir));
    if (!db_exists_path(dir)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    db_remove_tree(dir);
    shell_sd_end(&session, DB_TAG);
    return ESP_OK;
}

esp_err_t db_list(db_list_cb_t cb, void *ctx)
{
    char root[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    DIR *dir = NULL;
    struct dirent *entry;
    int count = 0;

    snprintf(root, sizeof(root), "%s/%s", BSP_SD_MOUNT_POINT, DB_DIR_NAME);
    if (shell_sd_begin(&session) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    dir = opendir(root);
    if (dir == NULL) {
        shell_sd_end(&session, DB_TAG);
        return ESP_OK;   /* no DBS dir yet = no databases */
    }
    while ((entry = readdir(dir)) != NULL && count < P4_CONFIG_DB_MAX_DATABASES) {
        size_t len = strlen(entry->d_name);
        char name[P4_CONFIG_DB_NAME_BYTES];

        if (len > 3 && strcmp(entry->d_name + len - 3, ".DB") == 0) {
            len -= 3;
            if (len < P4_CONFIG_DB_NAME_BYTES) {
                memcpy(name, entry->d_name, len);
                name[len] = '\0';
                count++;
                if (cb != NULL && !cb(name, ctx)) {
                    break;
                }
            }
        }
    }
    closedir(dir);
    shell_sd_end(&session, DB_TAG);
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Categories
 * ---------------------------------------------------------------------- */

esp_err_t db_category_set(const char *name, uint8_t cat, const char *label)
{
    char path[SHELL_SD_PATH_BYTES];
    char key[8];

    if (!db_name_valid(name) || cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!db_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(key, sizeof(key), "%u", (unsigned)cat);
    db_file_path(name, DB_CATEGORIES, path, sizeof(path));
    if (label == NULL || label[0] == '\0') {
        return storage_ini_file_delete(path, key);
    }
    return storage_ini_file_set(path, key, label);
}

esp_err_t db_category_get(const char *name, uint8_t cat,
                          char *label, size_t label_size)
{
    char path[SHELL_SD_PATH_BYTES];
    char key[8];
    esp_err_t error;

    if (label == NULL || label_size == 0 || cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!db_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(key, sizeof(key), "%u", (unsigned)cat);
    db_file_path(name, DB_CATEGORIES, path, sizeof(path));
    error = storage_ini_file_get(path, key, label, label_size);
    if (error != ESP_OK) {
        label[0] = '\0';
    }
    return error;
}

/* Adapter: storage_ini passes (const char* key, const char* value); convert
 * the numeric key to a uint8_t and forward to the caller's callback. */
typedef struct {
    bool (*cb)(uint8_t, const char *, void *);
    void *ctx;
    bool stop;
} db_cat_adapter_t;

static bool db_cat_foreach_adapter(const char *key, const char *value, void *arg)
{
    db_cat_adapter_t *a = (db_cat_adapter_t *)arg;
    unsigned long cat = strtoul(key, NULL, 10);

    if (cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
        return true;
    }
    if (!a->cb((uint8_t)cat, value, a->ctx)) {
        a->stop = true;
        return false;
    }
    return true;
}

esp_err_t db_categories_foreach(const char *name,
                                bool (*cb)(uint8_t, const char *, void *), void *ctx)
{
    char path[SHELL_SD_PATH_BYTES];
    db_cat_adapter_t adapter;
    esp_err_t error;

    if (cb == NULL || !db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!db_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    adapter.cb = cb;
    adapter.ctx = ctx;
    adapter.stop = false;
    db_file_path(name, DB_CATEGORIES, path, sizeof(path));
    error = storage_ini_file_foreach(path, db_cat_foreach_adapter, &adapter);
    return error;
}

/* ------------------------------------------------------------------------
 * Records
 * ---------------------------------------------------------------------- */

esp_err_t db_add(const char *name, uint8_t cat, const char *key, bool secret,
                 const void *payload, size_t len, uint32_t *out_id)
{
    db_index_line_t *lines = NULL;
    int count;
    db_index_line_t new_line;
    uint32_t id;
    esp_err_t error;

    if (!db_name_valid(name) || cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > P4_CONFIG_DB_RECORD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!db_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }

    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }
    if (count >= P4_CONFIG_DB_MAX_RECORDS) {
        free(lines);
        return ESP_ERR_NO_MEM;
    }

    id = db_header_get_u32(name, "next_id");
    if (id == 0) {
        id = 1;
    }

    memset(&new_line, 0, sizeof(new_line));
    new_line.id = id;
    new_line.cat = cat;
    new_line.flags = secret ? DB_FLAG_SECRET : 0;
    if (key != NULL) {
        snprintf(new_line.key, sizeof(new_line.key), "%s", key);
    }
    new_line.size = (uint32_t)len;

    /* Crash-safety order: write the payload first, then the index. */
    if (len > 0 || payload != NULL) {
        error = db_write_payload(name, id, payload ? payload : "", len);
        if (error != ESP_OK) {
            free(lines);
            return error;
        }
    } else {
        db_remove_payload(name, id);
    }

    /* Re-read the index (a payload write may have re-mounted / changed state)
     * and append. Simpler: re-read, append, rewrite. */
    free(lines);
    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }
    if (count >= P4_CONFIG_DB_MAX_RECORDS) {
        free(lines);
        return ESP_ERR_NO_MEM;
    }

    {
        db_index_line_t *grown = realloc(lines, (size_t)(count + 2) * sizeof(db_index_line_t));
        if (grown == NULL) {
            free(lines);
            return ESP_ERR_NO_MEM;
        }
        lines = grown;
        lines[count++] = new_line;
    }

    error = db_rewrite_index(name, lines, count,
                             db_header_get_u32(name, "record_count") + 1,
                             db_header_get_u32(name, "deleted_count"));
    free(lines);
    if (error != ESP_OK) {
        return error;
    }
    db_header_set_u32(name, "next_id", id + 1);
    if (out_id != NULL) {
        *out_id = id;
    }
    return ESP_OK;
}

esp_err_t db_get(const char *name, uint32_t id, void *buf, size_t *inout_len,
                 bool reveal_secret, db_record_info_t *out)
{
    db_index_line_t *lines = NULL;
    int count;
    int idx;
    esp_err_t result = ESP_OK;
    char *payload = NULL;
    size_t payload_len = 0;

    if (!db_name_valid(name) || inout_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }
    idx = db_index_find_id(lines, count, id);
    if (idx < 0) {
        free(lines);
        return ESP_ERR_NOT_FOUND;
    }
    if (lines[idx].flags & DB_FLAG_DELETED) {
        free(lines);
        return ESP_ERR_NOT_FOUND;
    }

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->id = lines[idx].id;
        out->category = lines[idx].cat;
        out->flags = lines[idx].flags;
        snprintf(out->key, sizeof(out->key), "%s", lines[idx].key);
        out->size = lines[idx].size;
    }

    if (lines[idx].flags & DB_FLAG_SECRET) {
        if (!reveal_secret) {
            /* Secret payload not revealed: report size only. */
            if (out != NULL) {
                out->flags |= DB_FLAG_SECRET;
            }
            *inout_len = 0;
            free(lines);
            return ESP_OK;
        }
    }

    if (lines[idx].size > 0 && buf != NULL) {
        size_t capacity = *inout_len;

        payload = db_read_payload(name, id, &payload_len);
        if (payload == NULL) {
            free(lines);
            return ESP_ERR_NOT_FOUND;
        }
        if (capacity < payload_len) {
            memcpy(buf, payload, capacity);
            *inout_len = payload_len;   /* report the full size */
        } else {
            memcpy(buf, payload, payload_len);
            /* NUL-terminate so a text payload can be printed as a string. */
            ((char *)buf)[payload_len] = '\0';
            *inout_len = payload_len;
        }
        free(payload);
    } else {
        *inout_len = 0;
    }
    free(lines);
    return result;
}

esp_err_t db_set(const char *name, uint32_t id, uint8_t cat, const char *key,
                 bool secret, const void *payload, size_t len)
{
    db_index_line_t *lines = NULL;
    int count;
    int idx;
    esp_err_t error;

    if (!db_name_valid(name) || cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > P4_CONFIG_DB_RECORD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }
    idx = db_index_find_id(lines, count, id);
    if (idx < 0) {
        free(lines);
        return ESP_ERR_NOT_FOUND;
    }
    if (lines[idx].flags & DB_FLAG_DELETED) {
        free(lines);
        return ESP_ERR_NOT_FOUND;
    }

    error = db_write_payload(name, id, payload ? payload : "", len);
    if (error != ESP_OK) {
        free(lines);
        return error;
    }

    lines[idx].cat = cat;
    lines[idx].flags = secret ? (lines[idx].flags & ~DB_FLAG_SECRET) | DB_FLAG_SECRET
                              : (lines[idx].flags & ~DB_FLAG_SECRET);
    if (key != NULL) {
        snprintf(lines[idx].key, sizeof(lines[idx].key), "%s", key);
    }
    lines[idx].size = (uint32_t)len;

    error = db_rewrite_index(name, lines, count,
                             db_header_get_u32(name, "record_count"),
                             db_header_get_u32(name, "deleted_count"));
    free(lines);
    return error;
}

esp_err_t db_del(const char *name, uint32_t id, bool permanent)
{
    db_index_line_t *lines = NULL;
    int count;
    int idx;
    esp_err_t error;
    uint32_t live, deleted;

    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }
    idx = db_index_find_id(lines, count, id);
    if (idx < 0) {
        free(lines);
        return ESP_ERR_NOT_FOUND;
    }

    live = db_header_get_u32(name, "record_count");
    deleted = db_header_get_u32(name, "deleted_count");

    if (permanent) {
        db_remove_payload(name, id);
        if (!(lines[idx].flags & DB_FLAG_DELETED) && live > 0) {
            live--;
        }
        /* shift left */
        if (idx + 1 < count) {
            memmove(&lines[idx], &lines[idx + 1], (size_t)(count - idx - 1) * sizeof(db_index_line_t));
        }
        count--;
        error = db_rewrite_index(name, lines, count, live, deleted);
        free(lines);
        return error;
    }

    if (!(lines[idx].flags & DB_FLAG_DELETED)) {
        lines[idx].flags |= DB_FLAG_DELETED;
        if (live > 0) {
            live--;
        }
        deleted++;
    }
    error = db_rewrite_index(name, lines, count, live, deleted);
    free(lines);
    return error;
}

esp_err_t db_purge(const char *name)
{
    db_index_line_t *lines = NULL;
    int count;
    int i, w = 0;
    uint32_t live, deleted;

    if (!db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }
    live = 0;
    deleted = 0;
    for (i = 0; i < count; i++) {
        if (lines[i].flags & DB_FLAG_DELETED) {
            db_remove_payload(name, lines[i].id);
        } else {
            lines[w++] = lines[i];
            live++;
        }
    }
    {
        esp_err_t error = db_rewrite_index(name, lines, w, live, deleted);
        free(lines);
        return error;
    }
}

esp_err_t db_count(const char *name, uint8_t cat_filter, int *out_count)
{
    db_index_line_t *lines = NULL;
    int count, i, n = 0;

    if (out_count == NULL || !db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }
    for (i = 0; i < count; i++) {
        if (lines[i].flags & DB_FLAG_DELETED) {
            continue;
        }
        if (cat_filter != 0xFF && lines[i].cat != cat_filter) {
            continue;
        }
        n++;
    }
    *out_count = n;
    free(lines);
    return ESP_OK;
}

static bool db_key_match(const char *key_filter, const char *key, bool ignore_case)
{
    if (key_filter == NULL || key_filter[0] == '\0') {
        return true;
    }
    if (ignore_case) {
        return strcasestr(key, key_filter) != NULL;
    }
    return strstr(key, key_filter) != NULL;
}

esp_err_t db_find(const char *name, uint8_t cat_filter, const char *key_filter,
                  const char *text_filter, bool ignore_case, bool reveal_secret,
                  db_find_cb_t cb, void *ctx, int *out_count)
{
    db_index_line_t *lines = NULL;
    int count, i, n = 0;

    if (out_count == NULL || !db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }

    if (text_filter != NULL && text_filter[0] != '\0') {
        /* Text scan needs each payload; resolve per candidate. A record
         * matches when EITHER its key or its payload contains the text, so a
         * `find` over a database (and Global Find) hits on both. */
        for (i = 0; i < count && n < P4_CONFIG_DB_FIND_MAX; i++) {
            char *payload;
            size_t plen = 0;
            bool key_hit;

            if (lines[i].flags & DB_FLAG_DELETED) {
                continue;
            }
            if (cat_filter != 0xFF && lines[i].cat != cat_filter) {
                continue;
            }
            if (lines[i].flags & DB_FLAG_SECRET && !reveal_secret) {
                continue;
            }
            key_hit = db_key_match(key_filter, lines[i].key, ignore_case);
            if (key_hit) {
                n++;
                {
                    db_record_info_t ri;
                    db_index_to_info(&lines[i], &ri);
                    if (cb != NULL && !cb(&ri, ctx)) {
                        break;
                    }
                }
                continue;
            }
            payload = db_read_payload(name, lines[i].id, &plen);
            if (payload != NULL) {
                bool hit = ignore_case ? (strcasestr(payload, text_filter) != NULL)
                                       : (strstr(payload, text_filter) != NULL);
                free(payload);
                if (!hit) {
                    continue;
                }
            } else if (plen != 0) {
                continue;
            }
            n++;
            {
                db_record_info_t ri;
                db_index_to_info(&lines[i], &ri);
                if (cb != NULL && !cb(&ri, ctx)) {
                    break;
                }
            }
        }
    } else {
        for (i = 0; i < count && n < P4_CONFIG_DB_FIND_MAX; i++) {
            if (lines[i].flags & DB_FLAG_DELETED) {
                continue;
            }
            if (cat_filter != 0xFF && lines[i].cat != cat_filter) {
                continue;
            }
            if (!db_key_match(key_filter, lines[i].key, ignore_case)) {
                continue;
            }
            if (lines[i].flags & DB_FLAG_SECRET && !reveal_secret) {
                continue;
            }
            n++;
            {
                db_record_info_t ri;
                db_index_to_info(&lines[i], &ri);
                if (cb != NULL && !cb(&ri, ctx)) {
                    break;
                }
            }
        }
    }

    *out_count = n;
    free(lines);
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Export / import
 * ---------------------------------------------------------------------- */

/** Hex-encode a payload so export is binary-safe regardless of '|' in data. */
static void db_hex_encode(const void *data, size_t len, char *out, size_t out_size)
{
    const unsigned char *p = data;
    size_t i;

    if (len * 2 + 1 > out_size) {
        return;
    }
    for (i = 0; i < len; i++) {
        snprintf(out + i * 2, 3, "%02X", p[i]);
    }
}

static int db_hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static size_t db_hex_decode(const char *hex, void *out, size_t out_size)
{
    unsigned char *p = out;
    size_t len = 0;
    size_t i;

    for (i = 0; hex[i] && hex[i + 1] && len < out_size; i += 2) {
        int hi = db_hex_val(hex[i]);
        int lo = db_hex_val(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            break;
        }
        p[len++] = (unsigned char)((hi << 4) | lo);
    }
    return len;
}

esp_err_t db_export(const char *name, const char *file)
{
    db_index_line_t *lines = NULL;
    int count, i;
    char out_path[SHELL_SD_PATH_BYTES];
    char export_path[SHELL_SD_PATH_BYTES + 16];
    shell_sd_session_t session;
    FILE *f = NULL;
    size_t total = 0;
    char *hex = NULL;
    char *line = NULL;

    if (!db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    count = db_read_index(name, &lines);
    if (count < 0) {
        return ESP_FAIL;
    }

    /* The hex/line buffers are record-sized; heap-allocate once (this runs on
     * the command worker / batch path where large stack locals overflow). */
    hex = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES * 2 + 1);
    line = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES * 2 + P4_CONFIG_DB_INDEX_LINE_BYTES * 2 + 32);
    if (hex == NULL || line == NULL) {
        free(hex);
        free(line);
        free(lines);
        return ESP_ERR_NO_MEM;
    }

    if (file != NULL && file[0] != '\0') {
        if (shell_fs_resolve_path(file, out_path, sizeof(out_path)) != ESP_OK) {
            free(hex);
            free(line);
            free(lines);
            return ESP_FAIL;
        }
    } else {
        snprintf(out_path, sizeof(out_path), "%s/%s/%s.EXPORT", BSP_SD_MOUNT_POINT, DB_DIR_NAME, name);
    }
    snprintf(export_path, sizeof(export_path), "%s.tmp", out_path);

    if (shell_sd_begin(&session) != ESP_OK) {
        free(hex);
        free(line);
        free(lines);
        return ESP_ERR_INVALID_STATE;
    }

    f = fopen(export_path, "w");
    if (f == NULL) {
        free(hex);
        free(line);
        free(lines);
        shell_sd_end(&session, DB_TAG);
        return ESP_FAIL;
    }

    for (i = 0; i < count; i++) {
        char *payload = NULL;
        size_t plen = 0;

        if (lines[i].flags & DB_FLAG_DELETED) {
            continue;
        }
        if (lines[i].size > 0) {
            payload = db_read_payload(name, lines[i].id, &plen);
        }
        db_hex_encode(payload ? payload : "", payload ? plen : 0, hex,
                      P4_CONFIG_DB_RECORD_MAX_BYTES * 2 + 1);
        if (payload) {
            free(payload);
        }
        snprintf(line, P4_CONFIG_DB_RECORD_MAX_BYTES * 2 + P4_CONFIG_DB_INDEX_LINE_BYTES * 2 + 32,
                 "%lu|%u|%u|%s|%s\n",
                 (unsigned long)lines[i].id, (unsigned)lines[i].cat,
                 (unsigned)lines[i].flags, lines[i].key, hex);
        if (fputs(line, f) == EOF) {
            fclose(f);
            remove(export_path);
            free(hex);
            free(line);
            free(lines);
            shell_sd_end(&session, DB_TAG);
            return ESP_FAIL;
        }
        total += strlen(line);
        if (total > P4_CONFIG_DB_EXPORT_MAX_BYTES) {
            fclose(f);
            remove(export_path);
            free(hex);
            free(line);
            free(lines);
            shell_sd_end(&session, DB_TAG);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        remove(export_path);
        free(hex);
        free(line);
        free(lines);
        shell_sd_end(&session, DB_TAG);
        return ESP_FAIL;
    }
    if (rename(export_path, out_path) != 0) {
        remove(out_path);
        if (rename(export_path, out_path) != 0) {
            remove(export_path);
            free(hex);
            free(line);
            free(lines);
            shell_sd_end(&session, DB_TAG);
            return ESP_FAIL;
        }
    }
    shell_sd_end(&session, DB_TAG);
    free(hex);
    free(line);
    free(lines);
    return ESP_OK;
}

esp_err_t db_import(const char *name, const char *file)
{
    char in_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *f = NULL;
    char *raw = NULL;
    char *hexbuf = NULL;
    char *payload = NULL;
    char *cursor;
    int imported = 0;

    if (!db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!db_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* The import buffer and per-record decode buffers are large; heap-allocate
     * so this never overflows the command worker stack. */
    raw = malloc(P4_CONFIG_DB_EXPORT_MAX_BYTES);
    hexbuf = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES * 2 + 1);
    payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (raw == NULL || hexbuf == NULL || payload == NULL) {
        free(raw);
        free(hexbuf);
        free(payload);
        return ESP_ERR_NO_MEM;
    }

    if (file != NULL && file[0] != '\0') {
        if (shell_fs_resolve_path(file, in_path, sizeof(in_path)) != ESP_OK) {
            free(raw);
            free(hexbuf);
            free(payload);
            return ESP_FAIL;
        }
    } else {
        snprintf(in_path, sizeof(in_path), "%s/%s/%s.EXPORT", BSP_SD_MOUNT_POINT, DB_DIR_NAME, name);
    }

    if (shell_sd_begin(&session) != ESP_OK) {
        free(raw);
        free(hexbuf);
        free(payload);
        return ESP_ERR_INVALID_STATE;
    }
    f = fopen(in_path, "r");
    if (f == NULL) {
        shell_sd_end(&session, DB_TAG);
        free(raw);
        free(hexbuf);
        free(payload);
        return ESP_ERR_NOT_FOUND;
    }
    {
        size_t got = fread(raw, 1, P4_CONFIG_DB_EXPORT_MAX_BYTES - 1, f);
        raw[got] = '\0';
    }
    fclose(f);
    shell_sd_end(&session, DB_TAG);

    cursor = raw;
    while (*cursor != '\0' && imported < P4_CONFIG_DB_EXPORT_MAX_RECORDS) {
        char *nl = strchr(cursor, '\n');
        char *p1, *p2, *p3;
        char *id_str, *cat_str, *flags_str, *key_str, *hex_str;
        unsigned long cat, flags;
        size_t plen;
        uint32_t new_id;
        esp_err_t error;

        if (nl != NULL) {
            *nl = '\0';
        }
        if (cursor[0] == '\0' || cursor[0] == '\n' || cursor[0] == '#') {
            if (nl == NULL) break;
            cursor = nl + 1;
            continue;
        }

        id_str = cursor;
        p1 = strchr(id_str, '|');
        if (p1 == NULL) { if (nl == NULL) break; cursor = nl + 1; continue; }
        *p1 = '\0';
        cat_str = p1 + 1;
        p2 = strchr(cat_str, '|');
        if (p2 == NULL) { if (nl == NULL) break; cursor = nl + 1; continue; }
        *p2 = '\0';
        flags_str = p2 + 1;
        p3 = strchr(flags_str, '|');
        if (p3 == NULL) { if (nl == NULL) break; cursor = nl + 1; continue; }
        *p3 = '\0';
        key_str = p3 + 1;
        /* key and hex are the last two fields. The key cannot contain '|', so
         * split on the FIRST '|' — this also handles an empty key (`||hex`). */
        {
            char *sep = strchr(key_str, '|');
            if (sep == NULL) { if (nl == NULL) break; cursor = nl + 1; continue; }
            *sep = '\0';
            hex_str = sep + 1;
        }

        cat = strtoul(cat_str, NULL, 10);
        flags = strtoul(flags_str, NULL, 10);
        snprintf(hexbuf, P4_CONFIG_DB_RECORD_MAX_BYTES * 2 + 1, "%s", hex_str);
        plen = db_hex_decode(hexbuf, payload, P4_CONFIG_DB_RECORD_MAX_BYTES);
        payload[plen] = '\0';

        error = db_add(name, (uint8_t)cat, key_str, (flags & DB_FLAG_SECRET) != 0,
                       payload, plen, &new_id);
        if (error != ESP_OK) {
            if (nl == NULL) break;
            cursor = nl + 1;
            continue;
        }
        imported++;

        if (nl == NULL) break;
        cursor = nl + 1;
    }
    free(raw);
    free(hexbuf);
    free(payload);
    return imported > 0 ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------------
 * Current-database pointer (RAM-only)
 * ---------------------------------------------------------------------- */

static char s_current_db[P4_CONFIG_DB_NAME_BYTES];

esp_err_t db_current_set(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        s_current_db[0] = '\0';
        return ESP_OK;
    }
    if (!db_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!db_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(s_current_db, sizeof(s_current_db), "%s", name);
    return ESP_OK;
}

bool db_current_get(char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return false;
    }
    if (s_current_db[0] == '\0') {
        buf[0] = '\0';
        return false;
    }
    snprintf(buf, size, "%s", s_current_db);
    return true;
}

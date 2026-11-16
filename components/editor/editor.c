/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor.c
 * @brief SD and worker-session half of the DOS-style inline text editor.
 *
 * Holds everything that needs the board: the PSRAM/DMA bounce allocation for
 * SD transfers, file load/save/reload, the template seeder, the autosave crash
 * writer and the modal `edit` session that drives editor_view.c from the
 * command worker task. All document mutations run on the LVGL task; only
 * editor_doc_load()/save()/reload() touch the filesystem (worker task).
 *
 * The byte-preserving document model itself - lines, cursor, selection,
 * undo/redo, find/replace, the lexers and the key map - lives in editor_doc.c,
 * which is pure C and also runs on the host unit-test lane. The two halves
 * share the line-store primitives through editor_doc_internal.h.
 */

#include "editor.h"
#include "editor_doc_internal.h"
#include "editor_view.h"
#include "editor_spell.h"
#include "p4minishell_config.h"
#include "storage.h"
#include "filetype.h"
#include "shell.h"
#include "ansi_palette.h"
#include "modal.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Bounce buffer size for SD/FATFS transfers. */
#define EDITOR_SD_CHUNK_BYTES 4096

/**
 * Allocate a small internal, DMA-capable buffer for FATFS/SD I/O. The
 * document lives in PSRAM, and on this P4 build PSRAM is not
 * `MALLOC_CAP_DMA`, so PSRAM buffers must never be handed to `fread`/`fwrite`
 * directly (the SDMMC DMA cannot reach them). Data is bounced through this
 * buffer. Returns NULL on failure so callers fall back to a safe path
 * (never a PSRAM pointer: PSRAM is not DMA-capable on this P4 build).
 */
static void *editor_dma_alloc(size_t size)
{
    return p4heap_alloc_dma(size);
}

/* Last-resort bounce buffer: 32 bytes, allocated only when the normal 4 KB
 * chunk (and every smaller halving) fails. It is NOT reserved at boot: a
 * permanent reserve stole DMA from I2S and made `beep` abort on Tab5. .bss is
 * not guaranteed DMA-capable on this SoC, so this must be a real heap block.
 * 32 bytes is one cache line and still moves a small file; a screenshot or
 * display buffer being freed can otherwise leave only tiny DMA blocks. The
 * caller frees it. */
#define EDITOR_IO_FALLBACK_BYTES 32

void *editor_io_fallback(size_t *size_out)
{
    void *ptr = p4heap_alloc_dma(EDITOR_IO_FALLBACK_BYTES + 1);

    if (size_out != NULL) {
        *size_out = (ptr != NULL) ? EDITOR_IO_FALLBACK_BYTES : 0;
    }
    return ptr;
}

static void editor_dma_free(void *ptr)
{
    if (ptr != NULL) {
        editor_mem_free(ptr);
    }
}

/**
 * Queue an LVGL async callback from a worker/serial task under the port lock.
 * lv_async_call() mutates LVGL's timer list; calling it concurrently with
 * lv_timer_handler corrupted the list and made lv_timer_exec jump through a
 * freed callback (0xcececece, F53). The port lock is recursive, so calls
 * already on the LVGL task are safe too.
 */
static bool editor_async_call(void (*cb)(void *), void *user_data)
{
    lv_result_t res;

    if (!lvgl_port_lock(0)) {
        return false;
    }
    res = lv_async_call(cb, user_data);
    lvgl_port_unlock();
    return res == LV_RESULT_OK;
}

/**
 * Allocate the SD bounce buffer, halving the chunk when the DMA pool is
 * fragmented. Heavy UI suites (image/font/theme churn) can leave the largest
 * DMA block below 4 KB; the editor must still open and save (F53: `edit`
 * reported an I/O error after s08 because this allocation failed). A 128-byte
 * dynamic block is the final fallback (freed by the caller).
 * @p size_out receives the usable chunk size (0 only if nothing is available).
 */
static void *editor_dma_alloc_chunk(size_t *size_out)
{
    size_t size = EDITOR_SD_CHUNK_BYTES;
    void *ptr = NULL;

    while (size >= 32) {
        /* One spare byte lets the loader prepend a carried '\r'. */
        ptr = editor_dma_alloc(size + 1);
        if (ptr != NULL) {
            break;
        }
        size /= 2;
    }
    if (ptr == NULL) {
        ptr = editor_io_fallback(&size);
    }
    if (size_out != NULL) {
        *size_out = (ptr != NULL) ? size : 0;
    }
    return ptr;
}
/* ========================================================================
 * LOAD
 * ======================================================================== */

editor_doc_t *editor_doc_load_ex(const char *path, esp_err_t *reason_out)
{
    editor_doc_t *doc;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    struct stat st;
    char *chunk = NULL;
    size_t chunk_size = 0;
    size_t file_size;
    size_t read_total = 0;
    esp_err_t err;
    esp_err_t reason = ESP_FAIL;
    bool ok = false;

    if (reason_out != NULL) {
        *reason_out = ESP_OK;
    }
    if (path == NULL || path[0] == '\0') {
        if (reason_out != NULL) {
            *reason_out = ESP_ERR_INVALID_ARG;
        }
        return NULL;
    }

    err = shell_fs_resolve_path(path, resolved, sizeof(resolved));
    if (err != ESP_OK) {
        if (reason_out != NULL) {
            *reason_out = ESP_ERR_INVALID_ARG;
        }
        return NULL;
    }

    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        if (reason_out != NULL) {
            *reason_out = ESP_ERR_INVALID_STATE;
        }
        return NULL;
    }

    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        shell_sd_end(&session, "edit");
        if (reason_out != NULL) {
            *reason_out = ESP_ERR_NOT_FOUND;
        }
        return NULL;
    }

    file_size = (size_t)st.st_size;
    if (file_size > P4_CONFIG_EDITOR_MAX_BYTES) {
        shell_sd_end(&session, "edit");
        if (reason_out != NULL) {
            *reason_out = ESP_ERR_INVALID_SIZE;
        }
        return NULL;
    }

    doc = calloc(1, sizeof(editor_doc_t));
    file = fopen(resolved, "rb");
    if (doc == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        shell_sd_end(&session, "edit");
        if (reason_out != NULL) {
            *reason_out = ESP_ERR_NO_MEM;
        }
        return NULL;
    }
    snprintf(doc->path, sizeof(doc->path), "%s", path);
    doc->crlf = false; /* default; CRLF only when a \r\n is observed */
    doc->trailing_newline = false;
    if (editor_doc_insert_line(doc, 0) != NULL) {
        /* Stream through an internal DMA-capable buffer: the document is
         * PSRAM, which the SDMMC DMA cannot read into directly. A trailing
         * '\r' is carried into the next chunk so a CRLF split across the
         * boundary is still recognized. */
        bool pending_cr = false;

        chunk = editor_dma_alloc_chunk(&chunk_size);
        if (chunk == NULL) {
            /* A screenshot/display buffer may be mid-free; one short retry
             * recovers the small fallback chunk without a visible delay. */
            vTaskDelay(pdMS_TO_TICKS(20));
            chunk = editor_dma_alloc_chunk(&chunk_size);
        }
        ok = (file != NULL && chunk != NULL);
        if (file == NULL) {
            reason = ESP_FAIL;
        } else if (chunk == NULL) {
            reason = ESP_ERR_NO_MEM;
        }
        while (ok) {
            size_t off = 0;
            size_t got;
            size_t total;

            if (pending_cr) {
                chunk[0] = '\r';
                off = 1;
                pending_cr = false;
            }
            got = fread(chunk + off, 1, chunk_size, file);
            if (got == 0) {
                if (off > 0) {
                    if (!editor_doc_append_bytes(doc, chunk, 1)) {
                        ok = false;
                        reason = ESP_ERR_INVALID_SIZE;
                    } else {
                        read_total += 1;
                    }
                }
                break;
            }
            total = off + got;
            if (chunk[total - 1] == '\r') {
                total--;
                pending_cr = true;
            }
            if (total > 0) {
                if (!editor_doc_append_bytes(doc, chunk, total)) {
                    ok = false;
                    reason = ESP_ERR_INVALID_SIZE;
                    break;
                }
                read_total += total;
            }
        }
    } else {
        reason = ESP_ERR_NO_MEM;
    }
    editor_dma_free(chunk);
    chunk = NULL;
    if (file != NULL) {
        fclose(file);
    }
    editor_doc_pick_syntax(doc);
    doc->content_bytes = read_total;
    /* Read-only detection while the SD session is still held: a probe open
     * for update fails on FATFS read-only files without touching content. */
    if (doc != NULL) {
        char probe_path[P4_CONFIG_SD_PATH_BYTES];
        FILE *probe = NULL;
        doc->readonly = false;
        if (shell_fs_resolve_path(doc->path, probe_path,
                                  sizeof(probe_path)) == ESP_OK) {
            probe = fopen(probe_path, "r+b");
            if (probe != NULL) {
                fclose(probe);
            } else {
                doc->readonly = true;
            }
        }
    }
    shell_sd_end(&session, "edit");

    if (!ok || doc == NULL) {
        if (doc != NULL) {
            editor_doc_free(doc);
        }
        if (reason_out != NULL) {
            *reason_out = reason;
        }
        return NULL;
    }

    /* A trailing newline leaves a phantom empty line after the last real
     * line; drop it. The cursor can still land on a fresh empty line by
     * pressing Enter at the end of the final line. */
    if (doc->trailing_newline && doc->line_count > 1 &&
        doc->lines[doc->line_count - 1].length == 0) {
        editor_doc_remove_line(doc, doc->line_count - 1);
    }

    return doc;
}

editor_doc_t *editor_doc_load(const char *path)
{
    return editor_doc_load_ex(path, NULL);
}

bool editor_file_missing(const char *resolved_path)
{
    shell_sd_session_t session;
    struct stat st;

    if (resolved_path == NULL || resolved_path[0] == '\0') {
        return true;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return true; /* No SD session: treat as missing (fresh buffer). */
    }
    bool missing = (stat(resolved_path, &st) != 0);
    shell_sd_end(&session, "edit");
    return missing;
}

/* ========================================================================
 * SAVE
 * ======================================================================== */

/** Stream the document lines with the document EOL style (plus the trailing
 *  newline when the original file had one) into @p file, bouncing through
 *  @p chunk (internal DMA-capable, @p chunk_size bytes). Binary-safe
 *  (length-counted, never strlen). Shared by the atomic save path and the
 *  autosave crash writer so both emit byte-identical content. */
static esp_err_t editor_doc_write_stream(editor_doc_t *doc, FILE *file,
                                         char *chunk, size_t chunk_size)
{
    const char crlf[] = "\r\n";
    const char lf[] = "\n";
    const char *eol;
    size_t eol_len;
    size_t i;

    if (doc == NULL || file == NULL || chunk == NULL || chunk_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    eol = doc->crlf ? crlf : lf;
    eol_len = doc->crlf ? 2 : 1;
    for (i = 0; i < doc->line_count; i++) {
        const char *text = doc->lines[i].text != NULL ? doc->lines[i].text : "";
        size_t len = doc->lines[i].length;
        size_t off = 0;

        while (off < len) {
            size_t n = len - off;
            if (n > chunk_size) {
                n = chunk_size;
            }
            memcpy(chunk, text + off, n);
            if (fwrite(chunk, 1, n, file) != n) {
                return ESP_FAIL;
            }
            off += n;
        }
        if (i + 1 < doc->line_count) {
            /* CRLF when the file uses CRLF, otherwise a bare LF. The EOL is
             * selected explicitly: writing only the first byte of "\r\n" when
             * crlf is false would emit a lone CR. */
            if (fwrite(eol, 1, eol_len, file) != eol_len) {
                return ESP_FAIL;
            }
        }
    }

    /* Preserve a trailing newline when the original file had one. */
    if (doc->trailing_newline && doc->line_count > 0) {
        if (fwrite(eol, 1, eol_len, file) != eol_len) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t editor_doc_save(editor_doc_t *doc, const char *path)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    char *chunk = NULL;
    size_t chunk_size = 0;
    esp_err_t err;

    if (doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = shell_fs_resolve_path(path, resolved, sizeof(resolved));
    if (err != ESP_OK) {
        return err;
    }

    err = shell_sd_begin(&session);
    if (err != ESP_OK) {
        return err;
    }

    /* All FATFS transfers bounce through this internal DMA-capable buffer. */
    chunk = editor_dma_alloc_chunk(&chunk_size);
    if (chunk == NULL) {
        shell_sd_end(&session, "edit");
        return ESP_ERR_NO_MEM;
    }

    /* Backup: copy any existing destination to "<file>.bak" before the
     * truncating write, so a power loss mid-save cannot lose both copies.
     * Best-effort (chunked copy): a failed backup still saves, and the
     * caller reports it. Over-long paths skip the backup rather than
     * truncating into the wrong file. */
    {
        char bak[P4_CONFIG_SD_PATH_BYTES];
        struct stat bak_st;
        if (snprintf(bak, sizeof(bak), "%s.bak", resolved) < (int)sizeof(bak) &&
            shell_sd_stat_path(resolved, &bak_st) == ESP_OK &&
            S_ISREG(bak_st.st_mode)) {
            FILE *src = fopen(resolved, "rb");
            FILE *dst = NULL;
            if (src != NULL) {
                dst = fopen(bak, "wb");
            }
            if (src != NULL && dst != NULL) {
                size_t got;
                bool bak_ok = true;
                while ((got = fread(chunk, 1, chunk_size, src)) > 0) {
                    if (fwrite(chunk, 1, got, dst) != got) {
                        bak_ok = false;
                        break;
                    }
                }
                fclose(src);
                if (fclose(dst) != 0) {
                    bak_ok = false;
                }
                if (!bak_ok) {
                    remove(bak);
                }
            } else {
                if (src != NULL) {
                    fclose(src);
                }
                if (dst != NULL) {
                    fclose(dst);
                    remove(bak);
                }
            }
            /* Best-effort: save proceeds regardless (outcome unreported
             * here; a full card fails the write below with its own error). */
        }
    }

    /* Atomic write: stream into "<resolved>.tmp", then swap it over the
     * destination with the shared replace primitive. Unlike the old direct
     * truncating write, a power loss can never leave a truncated destination
     * behind. The .bak copy above still protects the previous version, and on
     * failure the original file is left untouched (there is no partial to
     * remove). */
    {
        char tmp[P4_CONFIG_SD_PATH_BYTES + 5];
        FILE *file = NULL;

        if (snprintf(tmp, sizeof(tmp), "%s.tmp", resolved) >= (int)sizeof(tmp)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            file = fopen(tmp, "wb");
            if (file == NULL) {
                err = ESP_FAIL;
            } else {
                err = editor_doc_write_stream(doc, file, chunk, chunk_size);
                if (fclose(file) != 0 && err == ESP_OK) {
                    err = ESP_FAIL;
                }
                file = NULL;
                if (err == ESP_OK) {
                    err = storage_replace_file(tmp, resolved);
                } else {
                    remove(tmp);
                }
            }
        }
    }

    editor_dma_free(chunk);
    shell_sd_end(&session, "edit");
    return err;
}

/* Re-read the document from its bound path, discarding unsaved changes.
 * Cursor is preserved clamped into the reloaded text; undo history resets
 * to the fresh baseline and modified clears. Unnamed buffers cannot reload.
 */
esp_err_t editor_doc_reload(editor_doc_t *doc)
{
    editor_doc_t *fresh;
    size_t i;

    if (doc == NULL || doc->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    fresh = editor_doc_load(doc->path);
    if (fresh == NULL) {
        return ESP_FAIL;
    }
    for (i = 0; i < doc->line_count; i++) {
        editor_line_free(&doc->lines[i]);
    }
    editor_mem_free(doc->lines);
    doc->lines = fresh->lines;
    doc->line_count = fresh->line_count;
    doc->line_capacity = fresh->line_capacity;
    doc->crlf = fresh->crlf;
    doc->trailing_newline = fresh->trailing_newline;
    doc->syntax = fresh->syntax;
    doc->readonly = fresh->readonly;
    doc->content_bytes = fresh->content_bytes;
    fresh->lines = NULL;
    fresh->line_count = 0;
    fresh->line_capacity = 0;
    editor_doc_free(fresh);

    if (doc->line_count == 0) {
        doc->cursor_row = 0;
        doc->cursor_col = 0;
    } else {
        if (doc->cursor_row >= doc->line_count) {
            doc->cursor_row = doc->line_count - 1;
        }
        if (doc->cursor_col > doc->lines[doc->cursor_row].length) {
            doc->cursor_col = doc->lines[doc->cursor_row].length;
        }
    }
    doc->selection_active = false;
    doc->modified = false;
    editor_doc_undo_reset(doc);
    return ESP_OK;
}

/* ========================================================================
 * MODAL SURFACE (worker task)
 * ======================================================================== */

/* Session event bits live once in editor.h (shared with the LVGL view). */

struct editor_session {
    editor_control_t control;
    EventGroupHandle_t event_group;
    bool exited;
    bool open_failed;     /**< The LVGL view could not be created */
};

/* Set while a modal editor session is active (from session start until the
 * view is torn down). Lets the console reader route serial input to the
 * editor even during the async view-open window. */
static volatile bool s_session_active;

/* Last autosave tick (worker task only; reset at session open). */
static TickType_t s_autosave_last;

/** Per-modal-run context handed to the shared modal runtime. */
typedef struct {
    const char *path_arg;
    int *errorlevel_out;
    esp_err_t result;
    editor_session_t *session;
    bool focus;
    const char *template_name;
} editor_modal_ctx_t;

/** Report whether a modal editor session is running (view open or opening). */
bool editor_session_is_active(void)
{
    return s_session_active;
}

/* Forward: view open/close must run on the LVGL task. */
static void editor_session_open_cb(void *user_data);
static void editor_session_close_cb(void *user_data);

static void editor_session_open_cb(void *user_data)
{
    editor_session_t *session = (editor_session_t *)user_data;
    if (session == NULL) {
        return;
    }
    if (!editor_view_open(session->control.doc, &session->control)) {
        /* The view could not be created; wake the worker so the session does
         * not block forever waiting for a quit that will never come. */
        session->open_failed = true;
        xEventGroupSetBits(session->event_group, MODAL_EVENT_CLOSE_REQUEST);
    }
}

static void editor_session_close_cb(void *user_data)
{
    editor_session_t *session = (editor_session_t *)user_data;
    if (session != NULL) {
        editor_view_close();
        xEventGroupSetBits(session->event_group, MODAL_EVENT_CLOSED);
    }
}

/** Validate a template name: lowercase/uppercase/digit/`_`/`-`, 1..
 *  P4_CONFIG_TEMPLATE_NAME_BYTES-1 chars, no path separators or dots. Keeps
 *  `..`, extension games, and overlong names from ever reaching a path. */
static bool editor_template_name_ok(const char *name)
{
    size_t i = 0;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    while (name[i] != '\0') {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
        i++;
    }
    if (i == 0 || i >= P4_CONFIG_TEMPLATE_NAME_BYTES) {
        return false;
    }
    return true;
}

/** Seed a new buffer with sd:/TEMPLATES/<name>.MD (writerdeck). Runs on the
 *  command worker (guarded SD session); the document keeps its own path.
 *  Reports (through @p err) why it failed so the caller can be specific. */
typedef enum {
    EDITOR_TEMPLATE_OK = 0,
    EDITOR_TEMPLATE_BAD_NAME,
    EDITOR_TEMPLATE_NO_SD,
    EDITOR_TEMPLATE_MISSING,
    EDITOR_TEMPLATE_TOO_BIG,
    EDITOR_TEMPLATE_EMPTY,
    EDITOR_TEMPLATE_NO_MEM,
} editor_template_result_t;

static editor_template_result_t editor_seed_template(editor_doc_t *doc,
                                                     const char *name)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    char *buf;
    bool oversize = false;
    size_t got;

    if (doc == NULL || !editor_template_name_ok(name)) {
        return EDITOR_TEMPLATE_BAD_NAME;
    }
    snprintf(rel, sizeof(rel), "%s/%s.MD", P4_CONFIG_TEMPLATES_DIR_NAME, name);
    if (shell_fs_resolve_path(rel, resolved, sizeof(resolved)) != ESP_OK) {
        return EDITOR_TEMPLATE_NO_SD;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return EDITOR_TEMPLATE_NO_SD;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "edit");
        return EDITOR_TEMPLATE_MISSING;
    }
    buf = malloc(P4_CONFIG_TEMPLATE_MAX_BYTES + 1);
    if (buf == NULL) {
        fclose(file);
        shell_sd_end(&session, "edit");
        return EDITOR_TEMPLATE_NO_MEM;
    }
    got = fread(buf, 1, P4_CONFIG_TEMPLATE_MAX_BYTES, file);
    if (got == P4_CONFIG_TEMPLATE_MAX_BYTES && fgetc(file) != EOF) {
        oversize = true;
    }
    fclose(file);
    shell_sd_end(&session, "edit");
    if (got > 0) {
        editor_doc_insert_bytes(doc, buf, got);
    }
    free(buf);
    if (got == 0) {
        return EDITOR_TEMPLATE_EMPTY;
    }
    if (oversize) {
        return EDITOR_TEMPLATE_TOO_BIG;
    }
    /* A seeded buffer is a starting point, not a saved file. */
    doc->modified = true;
    return EDITOR_TEMPLATE_OK;
}

static bool editor_surface_open(void *ctx, EventGroupHandle_t event_group)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    const char *path = mctx->path_arg;
    bool unnamed = (path == NULL || path[0] == '\0');
    char resolved[P4_CONFIG_SD_PATH_BYTES] = "";
    editor_session_t *session = calloc(1, sizeof(*session));

    if (session == NULL) {
        mctx->result = ESP_ERR_NO_MEM;
        return false;
    }

    /* Resolve the save path now so save operations don't re-resolve. */
    if (!unnamed) {
        if (shell_fs_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
            free(session);
            mctx->result = ESP_ERR_INVALID_ARG;
            return false;
        }
    }

    if (unnamed) {
        session->control.doc = editor_doc_new(NULL);
    } else if (!editor_file_missing(resolved)) {
        /* The file exists (or SD is present): a load failure now is a
         * real error. Never fall through to a fresh buffer here - the
         * user would save an empty buffer over the original file. */
        {
            esp_err_t load_err = ESP_OK;

            session->control.doc = editor_doc_load_ex(resolved, &load_err);
            if (session->control.doc == NULL) {
                free(session);
                switch (load_err) {
                case ESP_ERR_NOT_FOUND:
                    shell_print_error("edit: cannot load '%s' (not found or unreadable)",
                                      path);
                    break;
                case ESP_ERR_INVALID_SIZE:
                    shell_print_error("edit: cannot load '%s' (over the %u KB / %u line limit)",
                                      path,
                                      (unsigned)(P4_CONFIG_EDITOR_MAX_BYTES / 1024),
                                      (unsigned)P4_CONFIG_EDITOR_MAX_LINES);
                    break;
                case ESP_ERR_NO_MEM:
                    shell_print_error("edit: cannot load '%s' (out of memory: SD/DMA buffer)",
                                      path);
                    break;
                case ESP_ERR_INVALID_STATE:
                    shell_print_error("edit: cannot load '%s' (SD card not mounted)", path);
                    break;
                case ESP_ERR_INVALID_ARG:
                    shell_print_error("edit: cannot load '%s' (invalid path)", path);
                    break;
                default:
                    shell_print_error("edit: cannot load '%s' (I/O error)", path);
                    break;
                }
                mctx->result = ESP_ERR_NOT_SUPPORTED;
                return false;
            }
        }
    } else {
        /* Missing file: start a new buffer bound to the path. */
        session->control.doc = editor_doc_new(resolved);
    }

    if (session->control.doc == NULL) {
        free(session);
        mctx->result = ESP_ERR_NO_MEM;
        return false;
    }

    /* Seed a brand-new buffer from sd:/TEMPLATES/<name>.MD (writerdeck). Only
     * for new/unnamed buffers: an existing file is never overwritten. */
    if (mctx->template_name != NULL && mctx->template_name[0] != '\0' &&
        (unnamed || editor_file_missing(resolved))) {
        switch (editor_seed_template(session->control.doc, mctx->template_name)) {
        case EDITOR_TEMPLATE_OK:
            shell_transcript_appendf_ansi(SH_MUTE "edit: started from template '%s'\n" SH_RST,
                                          mctx->template_name);
            break;
        case EDITOR_TEMPLATE_TOO_BIG:
            shell_print_warning("edit: template '%s' exceeds %u KB and was truncated",
                                mctx->template_name,
                                (unsigned)(P4_CONFIG_TEMPLATE_MAX_BYTES / 1024));
            break;
        case EDITOR_TEMPLATE_EMPTY:
            shell_print_warning("edit: template '%s' is empty; opened an empty buffer",
                                mctx->template_name);
            break;
        case EDITOR_TEMPLATE_BAD_NAME:
            shell_print_warning("edit: invalid template name '%s' (letters, digits, _-, max %u)",
                                mctx->template_name,
                                (unsigned)(P4_CONFIG_TEMPLATE_NAME_BYTES - 1));
            break;
        case EDITOR_TEMPLATE_NO_SD:
            shell_print_warning("edit: template '%s' unavailable (SD card not ready)",
                                mctx->template_name);
            break;
        default:
            shell_print_warning("edit: template '%s' not found in sd:/%s/",
                                mctx->template_name, P4_CONFIG_TEMPLATES_DIR_NAME);
            break;
        }
    }

    /* Load the spellcheck wordlists on the worker (SD I/O must not run on
     * the LVGL task). Best-effort: absent/broken dictionaries leave spell
     * off. The user overlay loads alongside the base list and a fresh
     * session starts with an empty ignore list. */
#if P4_CONFIG_SPELL_ENABLE
    (void)editor_spell_load(P4_CONFIG_SPELL_DICT_NAME);
    (void)spell_user_load();
    spell_ignored_clear();
#endif
    s_autosave_last = 0;

    session->event_group = event_group;
    session->control.event_group = event_group;
    session->control.focus = mctx->focus;

    /* Mark the session active BEFORE the async view-open so serial input
     * arriving while the view is opening routes to the editor, not the shell. */
    s_session_active = true;
    mctx->session = session;

    /* Open the view on the LVGL task via an async call. */
    if (editor_async_call(editor_session_open_cb, session) != LV_RESULT_OK) {
        s_session_active = false;
        editor_doc_free(session->control.doc);
        free(session);
        mctx->session = NULL;
        mctx->result = ESP_FAIL;
        return false;
    }

    return true;
}

/** Crash-file base name for a document path: alphanumerics kept, everything
 *  else folded to '_', truncated, plus ".autosave". Deterministic per
 *  document so one crash file exists per open file. */
static void editor_autosave_name(const char *docpath, char *out, size_t size)
{
    size_t pos = 0;
    size_t i;

    if (out == NULL || size == 0) {
        return;
    }
    if (docpath == NULL || docpath[0] == '\0') {
        docpath = "untitled";
    }
    for (i = 0; docpath[i] != '\0' && pos + 10 < size; i++) {
        char c = docpath[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            out[pos++] = c;
        } else {
            out[pos++] = '_';
        }
    }
    if (pos == 0 && size > 1) {
        out[pos++] = '_';
    }
    {
        const char *suffix = ".autosave";
        size_t k = 0;
        while (suffix[k] != '\0' && pos + 1 < size) {
            out[pos++] = suffix[k++];
        }
    }
    out[pos] = '\0';
}

/** Write the crash file for @p doc: a `#P4AUTOSAVE <path>` header plus the
 *  exact save bytes, streamed to "<crash>.tmp" and swapped over. Silent on
 *  any failure: autosave must never interrupt editing. */
static void editor_autosave_write(editor_doc_t *doc)
{
    char dir[P4_CONFIG_SD_PATH_BYTES];
    char base[96];
    char crash[P4_CONFIG_SD_PATH_BYTES];
    char tmp[P4_CONFIG_SD_PATH_BYTES + 5];
    shell_sd_session_t session;
    FILE *file = NULL;
    char *chunk = NULL;
    size_t chunk_size = 0;
    const char *orig;
    char head[P4_CONFIG_SD_PATH_BYTES + 16];
    int n;

    if (doc == NULL) {
        return;
    }
    if (storage_recovery_dir(dir, sizeof(dir)) != ESP_OK) {
        return;
    }
    editor_autosave_name(doc->path, base, sizeof(base));
    n = snprintf(crash, sizeof(crash), "%s/%s", dir, base);
    if (n < 0 || (size_t)n >= sizeof(crash)) {
        return;
    }
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", crash);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return;
    }
    chunk = editor_dma_alloc_chunk(&chunk_size);
    if (chunk == NULL) {
        shell_sd_end(&session, "edit");
        return;
    }
    file = fopen(tmp, "wb");
    if (file == NULL) {
        editor_dma_free(chunk);
        shell_sd_end(&session, "edit");
        return;
    }
    /* Header carries the original path for `recover` ("(unnamed)" buffers
     * restore-refuse later). */
    {
        bool ok = true;

        orig = (doc->path[0] != '\0') ? doc->path : "(unnamed)";
        n = snprintf(head, sizeof(head), "#P4AUTOSAVE %s\n", orig);
        if (n < 0 || (size_t)n >= sizeof(head)) {
            ok = false;
        } else if (fwrite(head, 1, (size_t)n, file) != (size_t)n) {
            ok = false;
        } else if (editor_doc_write_stream(doc, file, chunk, chunk_size) != ESP_OK) {
            ok = false;
        }
        if (fclose(file) != 0) {
            ok = false;
        }
        file = NULL;
        if (ok) {
            (void)storage_replace_file(tmp, crash);
        } else {
            remove(tmp);
        }
    }
    editor_dma_free(chunk);
    shell_sd_end(&session, "edit");
}

/** Remove the crash file for @p doc (called after a successful save). */
static void editor_autosave_clear(editor_doc_t *doc)
{
    char dir[P4_CONFIG_SD_PATH_BYTES];
    char base[96];
    char crash[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    int n;

    if (doc == NULL) {
        return;
    }
    if (storage_recovery_dir(dir, sizeof(dir)) != ESP_OK) {
        return;
    }
    editor_autosave_name(doc->path[0] != '\0' ? doc->path : "untitled",
                         base, sizeof(base));
    n = snprintf(crash, sizeof(crash), "%s/%s", dir, base);
    if (n < 0 || (size_t)n >= sizeof(crash)) {
        return;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return;
    }
    remove(crash);
    shell_sd_end(&session, "edit");
}

/** Autosave tick: spill a dirty buffer to its crash file when due. Runs on
 *  the worker inside editor_surface_service (100 ms ticks); the interval and
 *  dirty check keep SD writes sparse. */
static void editor_autosave_tick(editor_control_t *control)
{
    editor_doc_t *doc;
    TickType_t now;

    if (control == NULL || (doc = control->doc) == NULL) {
        return;
    }
    if (P4_CONFIG_EDITOR_AUTOSAVE_SECS <= 0) {
        return;
    }
    if (!editor_doc_is_modified(doc)) {
        return;
    }
    now = xTaskGetTickCount();
    if (s_autosave_last != 0 &&
        (now - s_autosave_last) <
            pdMS_TO_TICKS((TickType_t)P4_CONFIG_EDITOR_AUTOSAVE_SECS * 1000)) {
        return;
    }
    s_autosave_last = now;
    editor_autosave_write(doc);
}

static void editor_surface_service(void *ctx, EventBits_t bits)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    editor_session_t *session = mctx->session;
    editor_control_t *control;
    esp_err_t err;

    if (session == NULL) {
        return;
    }
    control = &session->control;

    /* Autosave runs on every service tick (100 ms), independent of event
     * bits; the dirty + interval check inside keeps it sparse. */
    editor_autosave_tick(control);

    if (((bits & (EDITOR_EVENT_SAVE | EDITOR_EVENT_RELOAD | EDITOR_EVENT_OPEN |
                  EDITOR_EVENT_SPELL)) == 0)) {
        return;
    }
    err = ESP_ERR_INVALID_ARG;

    /* Reload runs first when both bits arrive together: it replaces the
     * document a concurrent save would write. */
    if ((bits & EDITOR_EVENT_RELOAD) != 0) {
        control->reload_requested = false;
        if (control->doc != NULL) {
            err = editor_doc_reload(control->doc);
        }
        control->reload_ok = (err == ESP_OK);
        if (err != ESP_OK) {
            mctx->result = err;
        }
        editor_async_call(editor_view_notify_reloaded_cb,
                      (void *)(intptr_t)(err == ESP_OK));
    }

    /* Save and Open are independent: an Open alone must not be dropped by the
     * save guard (and a Save+Open burst still lands the save first). */
    if ((bits & EDITOR_EVENT_SAVE) != 0) {
        /* A Save-As request carries an explicit target path; otherwise
         * save to the document's source path, and fall back to a
         * generated name only for an unnamed buffer. */
        if (control->save_as_path[0] != '\0') {
            char save_as_resolved[P4_CONFIG_SD_PATH_BYTES] = "";
            if (shell_fs_resolve_path(control->save_as_path,
                                      save_as_resolved,
                                      sizeof(save_as_resolved)) == ESP_OK) {
                err = editor_doc_save(control->doc, save_as_resolved);
                if (err == ESP_OK) {
                    editor_doc_set_path(control->doc, save_as_resolved);
                }
            } else {
                err = ESP_ERR_INVALID_ARG;
            }
            control->save_as_path[0] = '\0';
        } else if (control->doc != NULL && control->doc->path[0] != '\0') {
            err = editor_doc_save(control->doc, control->doc->path);
        } else if (control->doc != NULL) {
            /* Unnamed buffer: fall back to a generated name. */
            err = editor_doc_save(control->doc, "EDIT.NEW");
        }
        control->save_ok = (err == ESP_OK);
        if (err == ESP_OK) {
            /* A saved buffer needs no crash file. */
            editor_autosave_clear(control->doc);
        } else {
            mctx->result = err;
        }

        /* Refresh the status bar on the LVGL task. */
        editor_async_call(editor_view_notify_saved_cb, (void *)(intptr_t)(err == ESP_OK));
    }

    /* Spell learn/forget runs here on the worker: the SD file append and
     * rewrite must stay off the LVGL/UART tasks that raise the request. */
    if ((bits & EDITOR_EVENT_SPELL) != 0) {
        bool ok = false;

        control->spell_op_requested = false;
        if (control->spell_word[0] != '\0') {
            if (control->spell_op == 1) {
                ok = spell_user_forget(control->spell_word, strlen(control->spell_word));
            } else {
                ok = spell_user_learn(control->spell_word, strlen(control->spell_word));
            }
        }
        control->spell_op_ok = ok;
        editor_async_call(editor_view_notify_spell_cb, (void *)(intptr_t)ok);
    }

    if ((bits & EDITOR_EVENT_OPEN) == 0) {
        return;
    }

    /* An explicit save lands first (above) so a Save+Open burst never loses
     * the saved target. Swapping the contents keeps the same document object,
     * so the view only rebuilds; a stale Save-As target must not follow into
     * the new file. */
    control->open_requested = false;
    if (control->doc != NULL && control->open_path[0] != '\0') {
        char open_resolved[P4_CONFIG_SD_PATH_BYTES] = "";
        editor_doc_t *fresh = NULL;

        if (shell_fs_resolve_path(control->open_path, open_resolved,
                                  sizeof(open_resolved)) == ESP_OK) {
            if (editor_file_missing(open_resolved)) {
                fresh = editor_doc_new(open_resolved);
            } else {
                fresh = editor_doc_load(open_resolved);
            }
        }
        if (fresh != NULL) {
            editor_doc_replace_contents(control->doc, fresh);
            editor_doc_set_path(control->doc, open_resolved);
            editor_doc_free(fresh);
            control->save_as_path[0] = '\0';
            err = ESP_OK;
        } else {
            err = ESP_FAIL;
        }
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    control->open_ok = (err == ESP_OK);
    if (err != ESP_OK) {
        mctx->result = err;
    }
    editor_async_call(editor_view_notify_opened_cb, (void *)(intptr_t)(err == ESP_OK));
}

static void editor_surface_close(void *ctx)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    editor_session_t *session = mctx->session;

    if (session != NULL) {
        if (editor_async_call(editor_session_close_cb, session) != LV_RESULT_OK) {
            /* Async queue full: tear the view down under the port lock and
             * signal CLOSED directly, or the worker waits out its 5 s timeout
             * and then frees the document while the view still references it. */
            if (lvgl_port_lock(0)) {
                editor_view_close();
                lvgl_port_unlock();
            }
            xEventGroupSetBits(session->event_group, MODAL_EVENT_CLOSED);
        }
    }
}

static bool editor_surface_handle_usb_key(void *ctx, uint8_t key_code, uint8_t modifiers, char ascii)
{
    (void)ctx;
    if (!editor_view_is_open()) {
        return false;
    }
    editor_view_handle_usb_key(key_code, modifiers, ascii);
    return true;
}

/* Serial console control verbs accepted while the editor is open. */
static bool editor_serial_is_verb(const char *line, const char *verb)
{
    return line[0] == '\\' && strcasecmp(line + 1, verb) == 0;
}

/* Match `\verb` or `\verb <arg>`; copies the argument (possibly empty) into
 * @p out. Used by verbs that take a word. */
static bool editor_serial_verb_arg(const char *line, const char *verb,
                                   char *out, size_t size)
{
    size_t n;

    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';
    if (line == NULL || line[0] != '\\') {
        return false;
    }
    n = strlen(verb);
    if (strncasecmp(line + 1, verb, n) != 0) {
        return false;
    }
    {
        const char *rest = line + 1 + n;
        if (*rest == '\0') {
            return true;
        }
        if (*rest != ' ' && *rest != '\t') {
            return false;
        }
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }
        snprintf(out, size, "%s", rest);
        return true;
    }
}

/* Per-call context for async serial-line dispatch to the LVGL task. */
typedef struct {
    char *line;
    editor_session_t *session;
} editor_serial_line_ctx_t;

/* Runs on the LVGL task: feeds one serial line's characters into the editor. */
static void editor_serial_line_cb(void *user_data)
{
    editor_serial_line_ctx_t *ctx = (editor_serial_line_ctx_t *)user_data;
    const char *line;
    char word[P4_CONFIG_SPELL_WORD_MAX + 1];
    size_t i;
    size_t len;

    if (ctx == NULL || ctx->line == NULL) {
        free(ctx);
        return;
    }

    line = ctx->line;
    len = strlen(line);

    if (editor_serial_is_verb(line, "q") || editor_serial_is_verb(line, "quit")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0x29, 0, 0); /* Esc -> quit */
        } else {
            editor_view_set_quit_requested();
        }
    } else if (editor_serial_is_verb(line, "s") || editor_serial_is_verb(line, "save")) {
        if (editor_view_is_open()) {
            editor_view_handle_usb_key(0, 0x01, 's'); /* Ctrl+S */
        } else {
            editor_view_set_save_requested();
        }
    } else if (editor_serial_is_verb(line, "u") || editor_serial_is_verb(line, "undo")) {
        editor_view_handle_usb_key(0, 0x01, 'z');
    } else if (editor_serial_is_verb(line, "f") || editor_serial_is_verb(line, "find")) {
        editor_view_handle_usb_key(0, 0x01, 'f'); /* Ctrl+F */
    } else if (editor_serial_is_verb(line, "g") || editor_serial_is_verb(line, "goto")) {
        editor_view_handle_usb_key(0, 0x01, 'g'); /* Ctrl+G -> Go to line */
    } else if (editor_serial_is_verb(line, "o") || editor_serial_is_verb(line, "saveas")) {
        editor_view_handle_usb_key(0, 0x01, 'o'); /* Ctrl+O -> Save As */
    } else if (editor_serial_is_verb(line, "open")) {
        editor_view_handle_usb_key(0x3D, 0, 0); /* F4 -> open another file */
    } else if (editor_serial_is_verb(line, "p") || editor_serial_is_verb(line, "preview")) {
        editor_view_handle_usb_key(0, 0x01, 'p'); /* Ctrl+P -> preview toggle */
    } else if (editor_serial_is_verb(line, "r") || editor_serial_is_verb(line, "redo")) {
        editor_view_handle_usb_key(0, 0x03, 'z'); /* Ctrl+Shift+Z */
    } else if (editor_serial_is_verb(line, "all") || editor_serial_is_verb(line, "replaceall")) {
        editor_view_handle_usb_key(0, 0x01, 'r'); /* Ctrl+R -> replace all */
    } else if (editor_serial_is_verb(line, "c") || editor_serial_is_verb(line, "case")) {
        editor_view_handle_usb_key(0, 0x01, 't'); /* Ctrl+T -> case toggle */
    } else if (editor_serial_is_verb(line, "b") || editor_serial_is_verb(line, "match")) {
        editor_view_handle_usb_key(0, 0x01, 'b'); /* Ctrl+B -> match jump */
    } else if (editor_serial_is_verb(line, "co") || editor_serial_is_verb(line, "comment")) {
        editor_view_handle_usb_key(0x38, 0x01, '/'); /* Ctrl+/ -> comment */
    } else if (editor_serial_is_verb(line, "w") || editor_serial_is_verb(line, "wrap")) {
        editor_view_handle_usb_key(0, 0x01, 'w'); /* Ctrl+W -> wrap toggle */
    } else if (editor_serial_is_verb(line, "focus")) {
        editor_view_handle_usb_key(0, 0x03, 'f'); /* Ctrl+Shift+F -> focus toggle */
    } else if (editor_serial_is_verb(line, "spell")) {
        editor_view_handle_usb_key(0, 0x03, 's'); /* Ctrl+Shift+S -> spell toggle */
    } else if (editor_serial_verb_arg(line, "spell-add", word, sizeof(word)) ||
               editor_serial_verb_arg(line, "spelladd", word, sizeof(word))) {
        /* Bare form learns the word under the cursor; `\spell-add WORD` learns
         * the explicit word. Select on verb_arg (not is_verb) or a form with an
         * argument never enters this branch and is typed into the document. */
        editor_view_spell_learn(word[0] != '\0' ? word : NULL);
    } else if (editor_serial_verb_arg(line, "spell-forget", word, sizeof(word)) ||
               editor_serial_verb_arg(line, "spellforget", word, sizeof(word))) {
        editor_view_spell_forget(word);
    } else if (editor_serial_verb_arg(line, "spell-ignore", word, sizeof(word)) ||
               editor_serial_verb_arg(line, "spellignore", word, sizeof(word))) {
        editor_view_spell_ignore(word[0] != '\0' ? word : NULL);
    } else if (editor_serial_is_verb(line, "spell-list") ||
               editor_serial_is_verb(line, "spelllist")) {
        editor_view_spell_list();
    } else if (editor_serial_is_verb(line, "l") || editor_serial_is_verb(line, "reload")) {
        editor_view_handle_usb_key(0, 0x01, 'l'); /* Ctrl+L -> reload */
    } else if (editor_serial_is_verb(line, "a") || editor_serial_is_verb(line, "selectall")) {
        editor_view_handle_usb_key(0, 0x01, 'a');
    } else {
        if (!editor_view_is_open()) {
            free(ctx->line);
            free(ctx);
            return;
        }
        /* Preview is read-only: verbs (quit/toggle) still work, but typed
         * text is discarded so the document can never be dirtied behind
         * the rendered view. */
        if (editor_view_is_preview()) {
            free(ctx->line);
            free(ctx);
            return;
        }
        /* A line of typed text: insert each character, then a newline. */
        for (i = 0; i < len; i++) {
            char ch = line[i];
            if (ch == '\\' && i == 0 && len > 1) {
                /* "\foo" that was not a known verb inserts a literal backslash
                 * and the rest of the line as text. */
                editor_view_handle_usb_key(0, 0, '\\');
                continue;
            }
            if (ch >= 0x20) {
                editor_view_handle_usb_key(0, 0, ch);
            }
        }
        editor_view_handle_usb_key(0x28, 0, '\n'); /* Enter -> newline */
    }

    free(ctx->line);
    free(ctx);
}

static bool editor_surface_handle_serial_line(void *ctx, const char *line)
{
    editor_modal_ctx_t *mctx = (editor_modal_ctx_t *)ctx;
    editor_serial_line_ctx_t *sl_ctx;

    /* Accept lines whenever a session is active (even while the view is
     * still opening), so a quick '\q' after 'edit' is never misrouted as a
     * shell command and lost behind the blocked worker. */
    if (mctx == NULL || mctx->session == NULL || line == NULL) {
        return false;
    }

    /* Defer the whole line to the LVGL task: the editor's document and widget
     * state live there, and rebuilding rows on the UART console task would
     * block it past the watchdog and race the render cycle. */
    sl_ctx = malloc(sizeof(*sl_ctx));
    if (sl_ctx == NULL) {
        return true; /* Consumed (drop) rather than misrouted. */
    }
    sl_ctx->line = strdup(line);
    if (sl_ctx->line == NULL) {
        free(sl_ctx);
        return true;
    }
    sl_ctx->session = mctx->session;

    if (editor_async_call(editor_serial_line_cb, sl_ctx) != LV_RESULT_OK) {
        free(sl_ctx->line);
        free(sl_ctx);
    }
    return true;
}

static const modal_surface_t editor_surface = {
    .name = "edit",
    .open = editor_surface_open,
    .service = editor_surface_service,
    .close = editor_surface_close,
    .handle_usb_key = editor_surface_handle_usb_key,
    .handle_serial_line = editor_surface_handle_serial_line,
};

esp_err_t editor_session_run_opts(const char *path,
                                  const editor_session_opts_t *opts,
                                  int *errorlevel)
{
    editor_modal_ctx_t mctx = {0};
    esp_err_t run_err;

    mctx.path_arg = path;
    mctx.errorlevel_out = errorlevel;
    mctx.result = ESP_OK;
    if (opts != NULL) {
        mctx.focus = opts->focus;
        mctx.template_name = opts->template_name;
    }

    if (errorlevel != NULL) {
        *errorlevel = 0;
    }

    run_err = modal_surface_run(&editor_surface, &mctx, errorlevel);
    if (run_err != ESP_OK && mctx.result == ESP_OK) {
        mctx.result = run_err;
    }

    /* Tear down the session now that the LVGL view is closed. */
    if (mctx.session != NULL) {
        if (mctx.session->open_failed && mctx.result == ESP_OK) {
            mctx.result = ESP_FAIL;
        }
        if (mctx.session->control.doc != NULL) {
            editor_doc_free(mctx.session->control.doc);
        }
        free(mctx.session);
    }

    s_session_active = false;

    /* Release the spellcheck wordlist with the session (returns PSRAM). */
    editor_spell_unload();

    if (errorlevel != NULL && mctx.result != ESP_OK) {
        *errorlevel = 1;
    }

    return mctx.result;
}

esp_err_t editor_session_run(const char *path, int *errorlevel)
{
    return editor_session_run_opts(path, NULL, errorlevel);
}

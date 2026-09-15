/**
 * @file archive_commands.c
 * @brief `archive` / `backup` / `restore` verbs over components/archive.
 *
 * Store-only USTAR (.p4a) backups with a CRC manifest trailer:
 *
 *   archive create <file> <path> [paths...]
 *   archive extract <file> [dest]      (dest defaults to the cwd)
 *   archive list <file> [/b]
 *   archive verify <file>
 *   backup <file> [paths...]          (no paths: sd:/DBS + the alarm store)
 *
 * (`restore` is intentionally NOT aliased: it already means trash-undelete;
 * extraction is `archive extract` + `archive verify`.)
 *
 * Transfers reuse the existing verbs (no new network code): an archive on
 * the SD card downloads over Wi-Fi via `httpd`, pulls via `httpget`, and
 * moves over USB-serial via `send`/`receive`. ERRORLEVEL: 0 ok, 1 nothing
 * archived/imported or a CRC mismatch, 2 usage/I-O.
 */

#include "command.h"
#include "storage.h"
#include "shell.h"
#include "batch.h"
#include "archive.h"
#include "alarm.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef P4_CONFIG_ALARM_PATH
#define P4_CONFIG_ALARM_PATH "sd:/ALARMS"
#endif

#ifndef P4_CONFIG_SD_PATH_BYTES
#define P4_CONFIG_SD_PATH_BYTES 320
#endif

/** Default backup set: the structured stores (skipped when absent). */
static const char *const archive_default_sources[] = {
    "sd:/DBS",
    P4_CONFIG_ALARM_PATH,
};

static void shell_command_archive_usage(const char *verb)
{
    if (verb != NULL && (strcasecmp(verb, "backup") == 0)) {
        shell_print_usage("Usage: backup <file> [paths...]  (no paths: DBS + alarms)");
        return;
    }
    shell_print_usage("Usage: archive create <file> <path> [paths...]");
    shell_print_usage("Usage: archive extract <file> [dest]");
    shell_print_usage("Usage: archive list <file> [/b]");
    shell_print_usage("Usage: archive verify <file>");
}

/** Resolve @p raw into @p out (explicit path; no redirect sourcing here). */
static bool archive_resolve(const char *raw, char *out, size_t out_size)
{
    if (shell_fs_resolve_path(raw, out, out_size) != ESP_OK) {
        shell_print_error("archive: invalid path %s", raw != NULL ? raw : "?");
        return false;
    }
    return true;
}

static void archive_print_stats(const char *done, const archive_stats_t *stats)
{
    shell_print_ok("%s: %d file(s), %d dir(s), %llu byte(s) (%d skipped)",
                   done, stats->files, stats->dirs,
                   (unsigned long long)stats->bytes, stats->skipped);
}

typedef struct {
    bool bare;
    int shown;
} archive_list_ctx_t;

static bool archive_list_cb(const archive_entry_t *e, void *ctx)
{
    archive_list_ctx_t *c = (archive_list_ctx_t *)ctx;

    if (c->bare) {
        shell_transcript_append_text(e->path);
        shell_transcript_append_text("\n");
    } else if (e->is_dir) {
        shell_transcript_appendf_ansi(SH_LBL "%s" SH_RST "\n", e->path);
    } else {
        shell_transcript_appendf_ansi(SH_VAL "%s" SH_RST "  " SH_NUM "%llu" SH_RST "\n",
                                      e->path, (unsigned long long)e->size);
    }
    c->shown++;
    return true;
}

static int archive_cmd_create(const char *verb, const char *file_arg, char **paths,
                              int npaths, const char *const *defaults, int ndefaults)
{
    char file[P4_CONFIG_SD_PATH_BYTES];
    const char **sources;
    char (*resolved)[P4_CONFIG_SD_PATH_BYTES];
    archive_stats_t stats;
    esp_err_t error;
    int i;

    if (file_arg == NULL) {
        shell_command_archive_usage(verb);
        return 2;
    }
    if (!archive_resolve(file_arg, file, sizeof(file))) {
        return 2;
    }
    if (npaths == 0 && ndefaults > 0) {
        /* `backup <file>` with no paths: the structured stores. */
        const char **all = malloc(sizeof(*all) * (size_t)ndefaults);
        if (all == NULL) {
            shell_print_error("archive: out of memory");
            return 2;
        }
        for (i = 0; i < ndefaults; i++) {
            all[i] = defaults[i];
        }
        error = archive_create(file, all, ndefaults, &stats);
        free((void *)all);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("archive: nothing to back up");
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("archive: cannot create %s", file);
            return 2;
        }
        archive_print_stats("backup", &stats);
        return 0;
    }
    if (npaths == 0) {
        shell_command_archive_usage(verb);
        return 2;
    }
    resolved = malloc(sizeof(*resolved) * (size_t)npaths);
    sources = malloc(sizeof(*sources) * (size_t)npaths);
    if (resolved == NULL || sources == NULL) {
        free(resolved);
        free((void *)sources);
        shell_print_error("archive: out of memory");
        return 2;
    }
    for (i = 0; i < npaths; i++) {
        if (!archive_resolve(paths[i], resolved[i], sizeof(resolved[i]))) {
            free(resolved);
            free((void *)sources);
            return 2;
        }
        sources[i] = resolved[i];
    }
    error = archive_create(file, sources, npaths, &stats);
    free(resolved);
    free((void *)sources);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("archive: no such file or directory");
        return 1;
    }
    if (error == ESP_ERR_NO_MEM) {
        shell_print_error("archive: not enough free space");
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("archive: cannot create %s", file);
        return 2;
    }
    archive_print_stats(verb != NULL ? verb : "archive", &stats);
    return 0;
}

static int archive_cmd_extract(const char *file_arg, const char *dest_arg)
{
    char file[P4_CONFIG_SD_PATH_BYTES];
    char dest[P4_CONFIG_SD_PATH_BYTES];
    archive_stats_t stats;
    esp_err_t error;

    if (file_arg == NULL) {
        shell_command_archive_usage(NULL);
        return 2;
    }
    if (!archive_resolve(file_arg, file, sizeof(file))) {
        return 2;
    }
    if (dest_arg != NULL) {
        if (!archive_resolve(dest_arg, dest, sizeof(dest))) {
            return 2;
        }
    } else {
        snprintf(dest, sizeof(dest), "%s", shell_get_cwd());
    }
    error = archive_extract(file, dest, &stats);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("archive: %s not found", file);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("archive: extract failed (%d file(s) landed)", stats.files);
        return 2;
    }
    archive_print_stats("extract", &stats);
    return 0;
}

static int archive_cmd_list(const char *file_arg, char **flags, int nflags)
{
    char file[P4_CONFIG_SD_PATH_BYTES];
    archive_list_ctx_t ctx;
    esp_err_t error;
    int i;
    bool bare = false;

    if (file_arg == NULL) {
        shell_command_archive_usage(NULL);
        return 2;
    }
    for (i = 0; i < nflags; i++) {
        if (strcasecmp(flags[i], "/b") == 0) {
            bare = true;
        } else {
            shell_command_archive_usage(NULL);
            return 2;
        }
    }
    if (!archive_resolve(file_arg, file, sizeof(file))) {
        return 2;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.bare = bare;
    error = archive_list(file, archive_list_cb, &ctx);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("archive: %s not found", file);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("archive: cannot list %s", file);
        return 2;
    }
    if (ctx.shown == 0) {
        shell_print_muted("archive: empty");
        return 1;
    }
    return 0;
}

static int archive_cmd_verify(const char *file_arg)
{
    char file[P4_CONFIG_SD_PATH_BYTES];
    archive_stats_t stats;
    esp_err_t error;

    if (file_arg == NULL) {
        shell_command_archive_usage(NULL);
        return 2;
    }
    if (!archive_resolve(file_arg, file, sizeof(file))) {
        return 2;
    }
    memset(&stats, 0, sizeof(stats));
    error = archive_verify(file, &stats);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("archive: %s not found", file);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("archive: CRC mismatch in %s (%d file(s) checked)",
                          file, stats.files);
        return 1;
    }
    shell_print_ok("archive: %d file(s) verified, %llu byte(s) OK",
                   stats.files, (unsigned long long)stats.bytes);
    return 0;
}

int shell_command_archive(int argc, char **argv)
{
    const char *verb;

    if (argc < 1 || argv[0] == NULL) {
        shell_command_archive_usage(NULL);
        return 2;
    }
    verb = argv[0];
    if (strcasecmp(verb, "backup") == 0) {
        /* backup <file> [paths...]: file is argv[1], paths follow. */
        if (argc < 2) {
            shell_command_archive_usage(verb);
            return 2;
        }
        return archive_cmd_create(verb, argv[1], argv + 2, argc - 2,
                                  archive_default_sources,
                                  (int)(sizeof(archive_default_sources) /
                                        sizeof(archive_default_sources[0])));
    }
    /* `archive` family: subcommand is argv[1]. */
    if (argc < 2) {
        shell_command_archive_usage(NULL);
        return 2;
    }
    if (strcasecmp(argv[1], "create") == 0) {
        /* archive create <file> [paths...]. */
        if (argc < 3) {
            shell_command_archive_usage(NULL);
            return 2;
        }
        return archive_cmd_create("archive", argv[2], argv + 3, argc - 3, NULL, 0);
    }
    if (strcasecmp(argv[1], "extract") == 0) {
        /* archive extract <file> [dest]. */
        if (argc < 3 || argc > 4) {
            shell_command_archive_usage(NULL);
            return 2;
        }
        return archive_cmd_extract(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (strcasecmp(argv[1], "list") == 0) {
        /* archive list <file> [/b]. */
        if (argc < 3) {
            shell_command_archive_usage(NULL);
            return 2;
        }
        return archive_cmd_list(argv[2], argv + 3, argc - 3);
    }
    if (strcasecmp(argv[1], "verify") == 0) {
        /* archive verify <file>. */
        if (argc != 3) {
            shell_command_archive_usage(NULL);
            return 2;
        }
        return archive_cmd_verify(argv[2]);
    }
    shell_command_archive_usage(NULL);
    return 2;
}

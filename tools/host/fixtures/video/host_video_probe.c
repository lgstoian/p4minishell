/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file host_video_probe.c
 * @brief Host-only harness for tools/host_video_gate.py (pre-gate, not firmware).
 *
 * Compiles the SHIPPED pure cores and ONE BMP decoder (components/gfx), the
 * ONE image-to-cell sampler adapter (imagefile_cells.h -> tui_frame.h), the
 * real Expat TOC parsers (components/epub) and the real USTAR machine/bounds
 * core (components/archive). The only host-only pieces are tiny allocator /
 * logging shims in ./stubs; no decoder, parser or palette is reimplemented.
 *
 * PNG/JPEG fixtures are intentionally absent: imgdec rides LVGL's vendored
 * decoders and cannot be linked standalone. The gate checks that wiring
 * statically instead of faking a decode here.
 */
#define _POSIX_C_SOURCE 200809L  /* dirent/strdup on a strict -std=c11 host */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ansi.h"
#include "archive.h"
#include "epub.h"
#include "epub_nav_core.h"
#include "gfx.h"
#include "imagefile_cells.h"
#include "video.h"

/* Host libc compatibility (glibc < 2.38 has no strlcpy); the declaration is
 * supplied to epub_nav.c via -include stubs/strlcpy.h at compile time. */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size > 0) {
        size_t n = (len < size - 1) ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

/* tui_frame.h calls these two; tui.c implements them as direct delegates to
 * the ONE ANSI CGA palette. These wrappers keep that exact delegation. */
uint32_t tui_dos_color_rgb(uint8_t index) { return ansi_cga_color(index); }
uint8_t tui_rgb_to_dos(uint32_t rgb) { return ansi_cga_nearest(rgb); }

/** Decode one fixture BMP through the same gfx core imagefile_decode_mem uses. */
static int probe_frame(const char *path, char *name_out, size_t name_size,
                       int *native_out, int *fit_out, unsigned *cell_out)
{
    FILE *file = fopen(path, "rb");
    static uint8_t data[1 << 16];
    size_t len;
    gfx_bmp_info_t info;
    gfx_surface_t surface;
    int w = 0;
    int h = 0;
    uint8_t cells[8 * 8];
    imagefile_cell_grid_t grid;
    const char *slash;

    if (file == NULL) {
        return 0;
    }
    len = fread(data, 1, sizeof(data), file);
    fclose(file);
    if (!gfx_bmp_parse_header_ex(data, len, &info, GFX_IMAGE_MAX_W,
                                 GFX_IMAGE_MAX_H)) {
        return 0;
    }
    gfx_bmp_fit(info.w, info.h, 64, 32, &w, &h);
    if (!gfx_bmp_decode_scaled_565(data, len, &info, w, h, &surface)) {
        return 0;
    }
    memset(cells, 0, sizeof(cells));
    grid.pixels = cells;
    grid.grid_w = 8;
    grid.grid_h = 8;
    grid.origin_col = 1;
    grid.origin_row = 1;
    imagefile_sample_rgb565_cells(1, 1, 8, 8, &surface,
                                  imagefile_grid_cell_sink, &grid);
    gfx_surface_free(&surface);

    slash = strrchr(path, '/');
    snprintf(name_out, name_size, "%s", slash != NULL ? slash + 1 : path);
    *native_out = info.w * 10000 + info.h;
    *fit_out = w * 10000 + h;
    *cell_out = cells[0];
    return 1;
}

static void probe_frames(const char *dir)
{
    char *paths[16];
    int count = 0;
    DIR *handle = opendir(dir);
    struct dirent *entry;

    if (handle == NULL) {
        return;
    }
    while ((entry = readdir(handle)) != NULL && count < 16) {
        char path[512];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        char *copy;

        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        copy = strdup(path);
        if (copy == NULL || !video_frame_add(paths, &count, 16, copy)) {
            free(copy);
            continue;
        }
    }
    closedir(handle);
    video_frames_sort(paths, count);
    printf("video.plan count=%d order=", count);
    for (int i = 0; i < count; i++) {
        const char *slash = strrchr(paths[i], '/');
        printf("%s%s", i ? "," : "", slash != NULL ? slash + 1 : paths[i]);
    }
    printf("\n");
    for (int i = 0; i < count; i++) {
        char name[128];
        int native = 0;
        int fit = 0;
        unsigned cell = 0;

        if (!probe_frame(paths[i], name, sizeof(name), &native, &fit, &cell)) {
            printf("video.frame name=%s state=decode_failed\n", name);
            continue;
        }
        printf("video.frame name=%s state=ok native=%d.%d fit=%d.%d cell=%02X\n",
               name, native / 10000, native % 10000,
               fit / 10000, fit % 10000, cell);
    }
}

static void probe_timing(void)
{
    video_cursor_t cursor;
    char status[160];
    int fps_values[] = {1, 10, 30};

    for (size_t i = 0; i < sizeof(fps_values) / sizeof(fps_values[0]); i++) {
        printf("video.fps fps=%d interval=%u\n", fps_values[i],
               video_frame_interval_ms(fps_values[i]));
    }
    video_cursor_begin(&cursor, 3, 10);
    for (int step = 0; step < 5; step++) {
        bool advanced = video_cursor_next(&cursor, true);

        printf("video.tick step=%d index=%d played=%d loops=%d advanced=%d budget=%d\n",
               step, cursor.index, cursor.frames_played, cursor.loops_done,
               advanced ? 1 : 0, video_budget_ok(&cursor) ? 1 : 0);
    }
    video_cursor_format(&cursor, status, sizeof(status));
    printf("video.status %s\n", status);
}

static void probe_epub(const char *dir)
{
    char path[512];
    FILE *file;
    static char xml[16 * 1024];
    size_t len;
    epub_toc_entry_t toc[16];
    epub_chapter_t spine[3];
    int count = 0;
    int linked;
    int linear;

    memset(spine, 0, sizeof(spine));
    for (int i = 0; i < 3; i++) {
        snprintf(spine[i].id, sizeof(spine[i].id), "c%d", i + 1);
        snprintf(spine[i].href, sizeof(spine[i].href), "Text/c%d.xhtml", i + 1);
        spine[i].linear = (i != 2);
    }

    snprintf(path, sizeof(path), "%s/toc.ncx", dir);
    file = fopen(path, "rb");
    if (file == NULL) return;
    len = fread(xml, 1, sizeof(xml) - 1, file);
    fclose(file);
    xml[len] = '\0';
    if (!epub_parse_ncx(xml, len, toc, 16, &count)) return;
    linked = epub_toc_link_spine(toc, count, spine, 3);
    linear = epub_spine_linear_count(spine, 3);
    printf("video.epub source=ncx count=%d linked=%d linear=%d "
           "target0=%d target1=%d stale=%d\n",
           count, linked, linear,
           epub_toc_target(toc, count, spine, 3, 0),
           epub_toc_target(toc, count, spine, 3, 1),
           epub_toc_target(toc, count, spine, 3, 99));

    snprintf(path, sizeof(path), "%s/nav.xhtml", dir);
    file = fopen(path, "rb");
    if (file == NULL) return;
    len = fread(xml, 1, sizeof(xml) - 1, file);
    fclose(file);
    xml[len] = '\0';
    count = 0;
    if (!epub_parse_nav(xml, len, toc, 16, &count)) return;
    linked = epub_toc_link_spine(toc, count, spine, 3);
    printf("video.epub source=nav count=%d linked=%d linear=%d "
           "target1=%d\n",
           count, linked, linear,
           epub_toc_target(toc, count, spine, 3, 1));
}

typedef struct {
    int members;
    int safe;
    int unsafe_rejected;
} archive_probe_t;

static bool archive_row(const archive_entry_t *entry, void *ctx)
{
    archive_probe_t *probe = (archive_probe_t *)ctx;
    archive_entry_t unsafe = *entry;
    char row[512];

    if (archive_format_machine_line(entry, row, sizeof(row)) > 0) {
        printf("video.archive row=%s\n", row);
    }
    if (archive_entry_bounds_ok(entry, 1024 * 1024)) {
        probe->safe++;
    }
    snprintf(unsafe.path, sizeof(unsafe.path), "../escape.bmp");
    if (!archive_entry_bounds_ok(&unsafe, 1024 * 1024)) {
        probe->unsafe_rejected++;
    }
    probe->members++;
    return true;
}

static void probe_archive(const char *dir)
{
    char path[512];
    FILE *file;
    static uint8_t data[16 * 1024];
    size_t len;
    archive_probe_t probe = {0, 0, 0};
    esp_err_t err;

    snprintf(path, sizeof(path), "%s/frames.p4a", dir);
    file = fopen(path, "rb");
    if (file == NULL) return;
    len = fread(data, 1, sizeof(data), file);
    fclose(file);
    err = archive_ustar_scan_mem(data, len, archive_row, &probe);
    printf("video.archive.summary rc=%d members=%d safe=%d unsafe_rejected=%d\n",
           err, probe.members, probe.safe, probe.unsafe_rejected);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: host_video_probe <fixture-dir>\n");
        return 2;
    }
    printf("video.report.begin fixtures=%s\n", argv[1]);
    probe_frames(argv[1]);
    probe_timing();
    probe_epub(argv[1]);
    probe_archive(argv[1]);
    printf("video.report.end\n");
    return 0;
}

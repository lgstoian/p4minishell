/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file font_fs.c
 * @brief Tiny LVGL file-system driver bridging sd:/FONTS to tiny_ttf.
 *
 * Why owned instead of LV_USE_FS_FATFS: the in-tree lv_fs_fatfs.c includes
 * ff.h, whose component edge would require editing the managed lvgl
 * CMakeLists. This driver does the same job in ~80 lines over stdio/VFS
 * (which already reaches the FATFS SD card): read/seek/tell for the
 * read-only TTF stream tiny_ttf needs. Registered as drive letter "F:".
 */

#include "font_fs.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static lv_fs_drv_t s_font_fs_drv;
static bool s_font_fs_ready = false;

static void font_fs_host_path(const char *path, char *out, size_t out_size)
{
    const char *rel = path;

    /* Tolerate a caller that kept the drive prefix. */
    if (rel[0] != '\0' && rel[1] == ':') {
        rel += 2;
    }
    if (rel[0] != '/') {
        snprintf(out, out_size, "%s/FONTS/%s", BSP_SD_MOUNT_POINT, rel);
    } else {
        snprintf(out, out_size, "%s%s", BSP_SD_MOUNT_POINT, rel);
    }
}

static void *font_fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    char host[256];
    (void)drv;

    if ((mode & LV_FS_MODE_RD) == 0) {
        return NULL;
    }
    font_fs_host_path(path, host, sizeof(host));
    return (void *)fopen(host, "rb");
}

static lv_fs_res_t font_fs_close(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    if (file_p == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    return fclose((FILE *)file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t font_fs_read(lv_fs_drv_t *drv, void *file_p, void *buf,
                                uint32_t btr, uint32_t *br)
{
    size_t got;
    (void)drv;

    if (file_p == NULL || buf == NULL || br == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    got = fread(buf, 1, btr, (FILE *)file_p);
    *br = (uint32_t)got;
    if (got != btr && ferror((FILE *)file_p)) {
        return LV_FS_RES_FS_ERR;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t font_fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                                lv_fs_whence_t whence)
{
    int origin;
    (void)drv;

    if (file_p == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    switch (whence) {
        case LV_FS_SEEK_SET: origin = SEEK_SET; break;
        case LV_FS_SEEK_CUR: origin = SEEK_CUR; break;
        case LV_FS_SEEK_END: origin = SEEK_END; break;
        default: return LV_FS_RES_INV_PARAM;
    }
    return fseek((FILE *)file_p, (long)pos, origin) == 0 ? LV_FS_RES_OK
                                                          : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t font_fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    long pos;
    (void)drv;

    if (file_p == NULL || pos_p == NULL) {
        return LV_FS_RES_INV_PARAM;
    }
    pos = ftell((FILE *)file_p);
    if (pos < 0) {
        return LV_FS_RES_FS_ERR;
    }
    *pos_p = (uint32_t)pos;
    return LV_FS_RES_OK;
}

void font_fs_init(void)
{
    if (s_font_fs_ready) {
        return;
    }
    lv_fs_drv_init(&s_font_fs_drv);
    s_font_fs_drv.letter = 'F';
    s_font_fs_drv.cache_size = 0;
    s_font_fs_drv.open_cb = font_fs_open;
    s_font_fs_drv.close_cb = font_fs_close;
    s_font_fs_drv.read_cb = font_fs_read;
    s_font_fs_drv.write_cb = NULL;
    s_font_fs_drv.seek_cb = font_fs_seek;
    s_font_fs_drv.tell_cb = font_fs_tell;
    lv_fs_drv_register(&s_font_fs_drv);
    s_font_fs_ready = true;
}

/**
 * @file font_fs.h
 * @brief Tiny LVGL file-system driver bridging sd:/FONTS to tiny_ttf.
 */

#ifndef P4MINISHELL_FONT_FS_H
#define P4MINISHELL_FONT_FS_H

/** Register the "F:" lv_fs driver (idempotent). Fonts resolve as
 * "F:/FONTS/<stem>.ttf" onto the SD card mount. */
void font_fs_init(void);

#endif /* P4MINISHELL_FONT_FS_H */

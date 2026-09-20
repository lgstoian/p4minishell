# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "storage.c" "storage_nav.c" "storage_files.c" "storage_disk.c" "storage_text.c" "storage_fam.c" "storage_ini.c" "storage_csv.c" "trash.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        header
        board_bsp
        fatfs
        markdown
        filetype
        esp_driver_sdmmc
)

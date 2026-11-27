# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "video_core.c" "video.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        applib
        gfx
        imagefile
        p4heap
        shell
        storage
        strutil
        tui
    PRIV_REQUIRES fatfs heap
)

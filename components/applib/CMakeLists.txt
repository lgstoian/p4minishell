# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "applib.c" "applib_tui.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        clock
        storage
        db
        tui
        heap
        p4heap
        esp_timer
        freertos
)

# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "selection.c" "selection_ui.c"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}"
    REQUIRES
        lvgl__lvgl
        p4heap
)

# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "markdown.c" "markdown_html.c" "markdown_print.c" "html_read.c" "dom.c" "css.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        esp_system
        p4heap
        strutil
)

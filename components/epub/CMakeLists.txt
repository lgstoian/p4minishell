# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "epub.c" "epub_opf.c" "epub_ncx.c" "epub_nav.c" "epub_render.c"
    INCLUDE_DIRS "include"
    REQUIRES zip archive storage strutil markdown
    PRIV_REQUIRES expat zlib
)
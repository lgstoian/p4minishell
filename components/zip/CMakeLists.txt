# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "zip.c" "zip_writer.c"
    INCLUDE_DIRS "include"
    REQUIRES storage strutil
    PRIV_REQUIRES zlib
)
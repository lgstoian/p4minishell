# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "pim_doc.c" "pim_parse.c" "pim_store.c" "pim_vcard.c" "pim_ical.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        db
        alarm
        p4heap
)

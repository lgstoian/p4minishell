# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# RSS / Atom / JSON Feed reader.
#
# XML: expat, the same streaming parser components/epub uses.
# JSON: components/json's json_walk, the firmware's single JSON reader.
# HTTP: networking, the single owner of the esp_http_client surface.
idf_component_register(
    SRCS "src/rss_parser.c" "src/rss_fetch.c"
    INCLUDE_DIRS "include"
    REQUIRES networking json
    PRIV_REQUIRES expat
)
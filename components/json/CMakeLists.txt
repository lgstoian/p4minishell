# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# The firmware's single JSON reader: validate, pretty-print, path walk, and
# escape decoding. Shared by the `json` verb (components/command) and by
# library consumers such as the RSS reader.
idf_component_register(
    SRCS "json.c"
    INCLUDE_DIRS "include"
)
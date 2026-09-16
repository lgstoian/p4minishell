# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "boot.c"
    INCLUDE_DIRS "."
    REQUIRES
        storage
        batch
        command
        font
        display
        networking
        usb
        keyboard
        header
        ansi
)

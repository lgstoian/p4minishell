# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# The unit tests link the real shell and command components, so they must be
# built for the same target as the firmware.
set(IDF_TARGET esp32p4)

# Stage the shared headers into the generated config directory, which ESP-IDF
# puts on the global include path. The firmware project does the same for
# board_config.h; the tests also need p4minishell_config.h because the
# components resolve it through ${CMAKE_SOURCE_DIR}, which points at this
# directory rather than the repository root.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/config)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../board_config.h
               ${CMAKE_BINARY_DIR}/config/board_config.h COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../p4minishell_config.h
               ${CMAKE_BINARY_DIR}/config/p4minishell_config.h COPYONLY)

# Include the ESP-IDF project cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set test component directories.
# components/usb builds both the vendored USB host stack and the shell-facing
# USB module (usb.c); shell and command depend on it directly.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ansi
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/shell
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/storage
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/batch
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/boot
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/command
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/networking
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/led
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/keyboard
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/editor
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/font
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/filetype
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gfx
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/markdown
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/modal
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/windows
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/header
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/clock
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/applib
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/tui
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/db
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/alarm
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/archive
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/usb
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/c6ota
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uitest
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_hosted
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_wifi_remote
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_lvgl_port
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_lcd_jd9165
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_lcd_touch_gt911
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_lcd_touch
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__usb_host_hid
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__usb_host_msc
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp32_p4_function_ev_board
)

project(p4minishell_tests)

# The shared p4minishell_config.h lives in the repository root.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

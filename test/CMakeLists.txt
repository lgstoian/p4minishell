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
# Board profile selection, mirroring the firmware project: the same
# boards/<name>/board_config.h is staged. Override with -DP4_BOARD=<name>.
set(P4_BOARD "jc1060p470c" CACHE STRING "P4MiniShell board profile (see boards/ + PORTING.md)")

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/config)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../boards/${P4_BOARD}/board_config.h
               ${CMAKE_BINARY_DIR}/config/board_config.h COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../p4minishell_config.h
               ${CMAKE_BINARY_DIR}/config/p4minishell_config.h COPYONLY)

# Layer the board-specific sdkconfig defaults on top of the shared test
# defaults (mirrors the firmware project; later files win).
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_SOURCE_DIR}/../boards/${P4_BOARD}/sdkconfig.defaults")

# Per-board generated sdkconfig (mirrors the firmware project). Re-derived from
# P4_BOARD on every configure so a reused build directory follows -DP4_BOARD;
# override with -DP4_SDKCONFIG=<path>.
if(NOT P4_SDKCONFIG)
    set(P4_SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.${P4_BOARD}")
endif()
set(SDKCONFIG "${P4_SDKCONFIG}" CACHE FILEPATH
    "sdkconfig file for the active board profile (sdkconfig.<P4_BOARD>)" FORCE)

# Include the ESP-IDF project cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set test component directories.
# components/usb builds both the vendored USB host stack and the shell-facing
# USB module (usb.c); shell and command depend on it directly.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/${P4_BOARD}/board_bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ansi
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/shell
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/storage
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/strutil
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/batch
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/boot
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/command
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/networking
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/power_monitor
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/tab5kbd
${CMAKE_CURRENT_SOURCE_DIR}/../components/imu
${CMAKE_CURRENT_SOURCE_DIR}/../components/imagefmt
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/db
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/alarm
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/archive
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/usb
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/c6ota
${CMAKE_CURRENT_SOURCE_DIR}/../components/camera
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
)

# Let the active board profile add its backing BSP component dir(s); the same
# hook the firmware project includes, so the two projects never disagree.
include(${CMAKE_CURRENT_SOURCE_DIR}/../boards/${P4_BOARD}/board_bsp/extra_dirs.cmake)

project(p4minishell_tests)

# The shared p4minishell_config.h lives in the repository root.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Board profile selection: boards/<name>/board_config.h is staged into the
# generated config include path, and boards/<name>/board_bsp/ provides the
# matching BSP component. Override with -DP4_BOARD=<name>.
set(P4_BOARD "jc1060p470c" CACHE STRING "P4MiniShell board profile (see boards/ + PORTING.md)")

set(EXTRA_COMPONENT_DIRS
	managed_components/espressif__esp_lvgl_port
	managed_components/espressif__esp_lcd_jd9165
	managed_components/espressif__esp_lcd_touch_gt911
	managed_components/lvgl__lvgl
  boards/${P4_BOARD}/board_bsp
  components/alarm
  components/ansi
  components/applib
  components/archive
  components/audio
  components/batch
  components/board_caps
  components/boot
  components/c6ota
  components/camera
  components/command
  components/clock
  components/db
  components/display
  components/editor
  components/font
  components/filetype
  components/gfx
  components/header
  components/imagefmt
  components/imu
  components/keyboard
  components/led
  components/markdown
  components/modal
  components/networking
  components/power_monitor
  components/p4heap
  components/shell
  components/storage
  components/strutil
  components/swgcm
  components/tab5kbd
  components/tui
  components/uitest
  components/usb
  components/windows
  samples/whoami
)

# Let the active board profile add the backing BSP component dir(s) to the
# build (for example the managed EV-board BSP for jc1060p470c). Only one BSP
# may be in the build, or their bsp_* symbols collide at link time; keeping the
# list in the board directory stops board knowledge leaking into the root.
include(${CMAKE_SOURCE_DIR}/boards/${P4_BOARD}/board_bsp/extra_dirs.cmake)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/config)
configure_file(${CMAKE_SOURCE_DIR}/boards/${P4_BOARD}/board_config.h ${CMAKE_BINARY_DIR}/config/board_config.h COPYONLY)

# Layer the board-specific sdkconfig defaults on top of the repository ones.
# The per-board file carries board-only ESP-IDF options (e.g. ESP-Hosted SDIO
# pins); later files win, so the board values override the shared defaults.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
    "${CMAKE_SOURCE_DIR}/boards/${P4_BOARD}/sdkconfig.defaults")

# Give each board its own generated sdkconfig so two boards can be configured /
# flashed side by side without the second reconfigure overwriting the first's
# board-only options (ESP-Hosted SDIO pins differ between boards). The path is
# re-derived from P4_BOARD on every configure, so reusing a build directory after
# switching -DP4_BOARD retargets the sdkconfig instead of silently keeping the
# previous board's file. Point -DP4_SDKCONFIG at an explicit file to override.
# The files are git-ignored generated artifacts.
if(NOT P4_SDKCONFIG)
    set(P4_SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig.${P4_BOARD}")
endif()
set(SDKCONFIG "${P4_SDKCONFIG}" CACHE FILEPATH
    "sdkconfig file for the active board profile (sdkconfig.<P4_BOARD>)" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4minishell)

include_directories(${CMAKE_SOURCE_DIR})

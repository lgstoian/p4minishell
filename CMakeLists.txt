# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
	managed_components/espressif__esp32_p4_function_ev_board
	managed_components/espressif__esp_lvgl_port
	managed_components/espressif__esp_lcd_jd9165
	managed_components/espressif__esp_lcd_touch_gt911
	managed_components/lvgl__lvgl
  components/alarm
  components/ansi
  components/applib
  components/archive
  components/audio
  components/batch
  components/boot
  components/c6ota
  components/command
  components/clock
  components/db
  components/display
  components/editor
  components/font
  components/filetype
  components/gfx
  components/header
  components/keyboard
  components/led
  components/markdown
  components/modal
  components/networking
  components/shell
  components/storage
  components/tui
  components/uitest
  components/usb
  components/windows
  samples/whoami
)

# Board profile selection: boards/<name>/board_config.h is staged into the
# generated config include path. Override with -DP4_BOARD=<name>.
set(P4_BOARD "jc1060p470c" CACHE STRING "P4MiniShell board profile (see boards/ + PORTING.md)")

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/config)
configure_file(${CMAKE_SOURCE_DIR}/boards/${P4_BOARD}/board_config.h ${CMAKE_BINARY_DIR}/config/board_config.h COPYONLY)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4minishell)

include_directories(${CMAKE_SOURCE_DIR})



# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
	managed_components/espressif__esp32_p4_function_ev_board
	managed_components/espressif__esp_lvgl_port
	managed_components/espressif__esp_lcd_jd9165
	managed_components/espressif__esp_lcd_touch_gt911
	managed_components/lvgl__lvgl
	components/ansi
	components/command
	components/display
	components/keyboard
	components/shell
	components/clock
	components/windows
	components/storage
)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/config)
configure_file(${CMAKE_SOURCE_DIR}/board_config.h ${CMAKE_BINARY_DIR}/config/board_config.h COPYONLY)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4minishell)

include_directories(${CMAKE_SOURCE_DIR})



idf_component_register(
    SRCS "ui_test.c"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}"
    REQUIRES
        display
        espressif__esp_lvgl_port
        lvgl__lvgl
)

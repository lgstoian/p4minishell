idf_component_register(
    SRCS "header.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        lvgl__lvgl
        espressif__esp_lvgl_port
)
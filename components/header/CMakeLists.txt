idf_component_register(
    SRCS "header.c" "header_layout.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        lvgl__lvgl
        espressif__esp_lvgl_port
        esp_app_format
        font
)
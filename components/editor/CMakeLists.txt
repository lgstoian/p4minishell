idf_component_register(
    SRCS "editor.c" "editor_view.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        shell
        storage
        windows
        keyboard
        display
        modal
        espressif__esp_lvgl_port
        lvgl__lvgl
)

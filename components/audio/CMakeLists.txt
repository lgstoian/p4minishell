idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        ansi
        freertos
        espressif__esp32_p4_function_ev_board
        espressif__esp_codec_dev
)

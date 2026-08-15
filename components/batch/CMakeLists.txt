idf_component_register(
    SRCS "batch.c" "calc.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        storage
        espressif__esp32_p4_function_ev_board
)

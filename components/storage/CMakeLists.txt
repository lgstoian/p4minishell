idf_component_register(
    SRCS "storage.c" "storage_commands.c" "trash.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        header
        espressif__esp32_p4_function_ev_board
        fatfs
)

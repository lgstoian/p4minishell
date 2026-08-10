idf_component_register(
    SRCS "boot.c"
    INCLUDE_DIRS "."
    REQUIRES
        storage
        batch
        command
        display
        networking
        usb
        ansi
)

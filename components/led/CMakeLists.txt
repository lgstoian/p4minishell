idf_component_register(
    SRCS "led.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        esp_driver_rmt
        esp_timer
        freertos
        log
        espressif__led_strip
)

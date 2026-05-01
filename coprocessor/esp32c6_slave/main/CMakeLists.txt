set(esp_hosted_slave_main_dir "${CMAKE_CURRENT_LIST_DIR}/../../../managed_components/espressif__esp_hosted/slave/main")
set(common_dir "${esp_hosted_slave_main_dir}/common")

set(COMPONENT_SRCS
    "${common_dir}/proto/esp_hosted_rpc.pb-c.c"
    "${common_dir}/utils/esp_hosted_cli.c"
    "${esp_hosted_slave_main_dir}/protocomm_pserial.c"
    "${esp_hosted_slave_main_dir}/esp_hosted_coprocessor.c"
    "${esp_hosted_slave_main_dir}/mempool.c"
    "${esp_hosted_slave_main_dir}/stats.c"
    "${esp_hosted_slave_main_dir}/mempool_ll.c"
    "${esp_hosted_slave_main_dir}/host_power_save.c"
    "${esp_hosted_slave_main_dir}/slave_light_sleep.c"
    "${esp_hosted_slave_main_dir}/nw_split_router.c"
    "${esp_hosted_slave_main_dir}/slave_transport_gpio_pin_guard.c"
    "${esp_hosted_slave_main_dir}/slave_gpio_expander.c"
    "${esp_hosted_slave_main_dir}/slave_ext_coex.c"
    "${esp_hosted_slave_main_dir}/slave_control.c"
)

if(CONFIG_SOC_BT_SUPPORTED)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/slave_bt.c")
endif()

if(CONFIG_ESP_HOSTED_CP_WIFI)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/slave_wifi_std.c")
endif()

if(CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/slave_wifi_enterprise.c")
endif()

if(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/slave_network_split.c")
endif()

set(COMPONENT_ADD_INCLUDEDIRS
    "${esp_hosted_slave_main_dir}"
    "${common_dir}"
    "${common_dir}/log"
    "${common_dir}/proto"
    "${common_dir}/rpc"
    "${common_dir}/transport"
)

if(CONFIG_SOC_BT_SUPPORTED)
    if(CONFIG_IDF_TARGET_ESP32)
        list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/slave_bt_uart_esp32.c")
    elseif(CONFIG_IDF_TARGET_ESP32C3 OR CONFIG_IDF_TARGET_ESP32S3)
        list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/slave_bt_uart_esp32c3_s3.c")
    else()
        list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/slave_bt_uart_esp32xx.c")
    endif()
endif()

if(CONFIG_ESP_SDIO_HOST_INTERFACE)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/sdio_slave_api.c")
elseif(CONFIG_ESP_SPI_HOST_INTERFACE)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/spi_slave_api.c")
elseif(CONFIG_ESP_SPI_HD_MODE)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/spi_hd_slave_api.c")
else()
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/uart_slave_api.c")
endif()

if(CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/example_peer_data_transfer.c")
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_LIGHT_SLEEP)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/example_light_sleep.c")
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_MQTT)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/example_mqtt_client.c")
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT)
    list(APPEND COMPONENT_SRCS "${esp_hosted_slave_main_dir}/example_http_client.c")
endif()

list(APPEND COMPONENT_ADD_INCLUDEDIRS "${common_dir}/utils")
list(APPEND COMPONENT_SRCS "${common_dir}/utils/esp_hosted_cli.c")

register_component()

idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")
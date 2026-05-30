idf_component_register(
    SRCS "app_main.c" "daylog.c" "aurora_favicon_data.c" "chart_umd_min_js.c" "pcf85063_rtc.c" "inverter_status_labels.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi nvs_flash esp_netif esp_event esp_http_server esp_http_client esp_driver_uart esp_driver_i2c esp_timer heap mdns spi_flash mbedtls joltwallet__littlefs esp_pm
)

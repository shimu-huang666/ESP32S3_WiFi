idf_component_register(
    SRCS "wifi.c" "uart.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi nvs_flash esp_event esp_netif
)
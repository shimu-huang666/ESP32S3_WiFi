idf_component_register(
    SRCS "lvgl_api.c" "wifi_service.c" "cmd.c" "mqtt_app.c" "time_sync.c" "wifi.c" "uart.c" 
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi nvs_flash esp_event esp_netif mqtt
)
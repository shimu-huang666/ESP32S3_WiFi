#include "nvs_flash.h"
#include "esp_err.h"
#include "wifi.h"
#include "uart.h"

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    ESP_ERROR_CHECK(uart_app_init());
    ESP_ERROR_CHECK(wifi_init_once());

    // 可选：启动后台自动扫一次
    ESP_ERROR_CHECK(wifi_start_bg_scan_task(NULL, 8192, 9));

    // 启动 UART 命令任务
    ESP_ERROR_CHECK(wifi_start_cmd_task(NULL, 8192, 10));

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

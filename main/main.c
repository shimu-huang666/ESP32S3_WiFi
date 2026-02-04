/* WiFi Scan + Sort by RSSI (strong -> weak) + formatted output + UART cmd: conn <index> <psw> connect wifi by scan index
//2026/2/2 实现内容：上电初始化wifi并扫描附近的wifi，返回wifi列表。 存在问题：返回内容包含logI信息--->已解决。任务栈太小，改成8192 

2/3 
    实现：从station_example_main示例移植wifi连接，编译通过，已实现账密wifi连接 
    跟进：从串口获取用户输入的账密 

2/4 实现：  从串口获取用户输入的账密，编译通过;
            增加连接wifi后显示wifi基本参数，并可以通过info指令查询；
            增加连接wifi后可获取当前时间，并可以通过time指令；
*/

#include "nvs_flash.h"
#include "esp_err.h"
#include "wifi.h"
#include "uart.h"
#include "esp_log.h"
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
    esp_err_t e = wifi_auto_connect_last();
    if (e == ESP_ERR_NOT_FOUND) {
        ESP_LOGI("main", "No last wifi record yet. Please run: scan, then conn <i> <psw> once.");
    } else if (e != ESP_OK) {
        ESP_LOGW("main", "Auto connect failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI("main", "Auto connect started/ok.");
    }

    // 可选：启动后台自动扫一次
    ESP_ERROR_CHECK(wifi_start_bg_scan_task(NULL, 8192, 9));

    // 启动 UART 命令任务
    ESP_ERROR_CHECK(wifi_start_cmd_task(NULL, 8192, 10));

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

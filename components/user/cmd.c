#include "cmd.h"

static const char *TAG_CMD  = "cmd";
static const char *TAG_WIFI = "wifi";
static const char *TAG_TIME = "time";
static const char *TAG_mqtt= "mqtt";

static wifi_ctx_t s = {0};


void print_help(void)
{
    const char *h =
        "scan                   - wifi scan\r\n"
        "conn <i> [psw]         - connect to AP by index\r\n"
        "connssid <ssid> <psw>  - connect by ssid and password\r\n"
        "info                   - show current wifi info\r\n"
        "time                   - show time\r\n"
        "disconn                - manual disconnect (no auto-reconnect)\r\n"
        "reconn                 - reconnect using saved STA cfg (flash)\r\n"
        "forget                 - erase last saved wifi (NVS) and disconnect\r\n"
        "mem                    - show saved wifi memory (NVS + STA flash cfg)\r\n"
        "mqtt [message]         - mqtt send message\r\n"
        "mqtt hb <ON/OFF>       - heartbeat on off \r\n"
        "help                   - show help\r\n"
        ;
    uart_app_write(h, strlen(h));
}

void cmd_task(void *arg)
{
    (void)arg;

    QueueHandle_t q = uart_app_get_cmd_queue();
    uart_cmd_msg_t msg;

    uart_app_write("\r\n==== UART CMD READY ====\r\n", strlen("\r\n==== UART CMD READY ====\r\n"));
    print_help();
    uart_app_write("========================\r\n", strlen("========================\r\n"));

    while (1) {
        if (xQueueReceive(q, &msg, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG_CMD, "CMD: %s", msg.line);

            uart_app_write(">", 1);
            uart_app_write(msg.line, strlen(msg.line));
            uart_app_write("\r\n", 2);

            if (strcmp(msg.line, "scan") == 0) {
                uart_app_write("Scanning...\r\n", strlen("Scanning...\r\n"));
                wifi_scan_once_and_print_sorted();
                uart_app_write("Scan done\r\n", strlen("Scan done\r\n"));
                continue;
            }
            if (strncmp(msg.line, "connssid", 8) == 0) {
                char ssid[33] = {0};
                char psw[65]  = {0};
                int n = sscanf(msg.line, "connssid %32s %64s", ssid, psw);
                if (n != 2) {
                    uart_app_write("Usage: connssid <ssid> <psw>\r\n", strlen("Usage: connssid <ssid> <psw>\r\n"));
                } else {
                    esp_err_t e = wifi_connect_by_ssid(ssid, psw);
                    if (e != ESP_OK) logi_both(TAG_WIFI, "connssid failed: %s", esp_err_to_name(e));
                }
                continue;
            }
            if (strncmp(msg.line, "conn", 4) == 0) {
                int idx = 0;
                char psw[65] = {0}; // WPA2 password max 63
                int n = sscanf(msg.line, "conn %d %64s", &idx, psw);

                if (n <= 0) {
                    uart_app_write("Usage: conn <index> [psw]\r\n", strlen("Usage: conn <index> [psw]\r\n"));
                } else if (n == 1) {
                    esp_err_t e = wifi_connect_by_index(idx, NULL);
                    if (e != ESP_OK) logi_both(TAG_WIFI, "conn failed: %s", esp_err_to_name(e));
                } else {
                    esp_err_t e = wifi_connect_by_index(idx, psw);
                    if (e != ESP_OK) logi_both(TAG_WIFI, "conn failed: %s", esp_err_to_name(e));
                }
                continue;
            }
            if (strcmp(msg.line, "reconn") == 0) {
                esp_err_t e = wifi_reconnect_saved();
                if (e != ESP_OK) {
                    logi_both(TAG_WIFI, "reconn failed: %s", esp_err_to_name(e));
                }
                continue;
            }
            if (strcmp(msg.line, "info") == 0) {
                wifi_print_info();
                continue;
            }
            if (strcmp(msg.line, "time") == 0) {
                if (!time_is_valid()) logi_both(TAG_TIME, "SNTP not synced yet.");
                else print_time_now();
                continue;
            }
            if (strcmp(msg.line, "disconn") == 0) {
                s.manual_disconnect = true;
                s.retry_num = 0;
                esp_wifi_disconnect();
                logi_both(TAG_WIFI, "WiFi disconnected (manual)!");
                continue;
            }

            if (strcmp(msg.line, "help") == 0) {
                print_help();
                continue;
            }

            if (strcmp(msg.line, "forget") == 0) {
                // 你也可以改成 true：同时清空 flash 里保存的 STA 配置（ssid/psw）
                esp_err_t e = wifi_forget_last(true);
                if (e != ESP_OK) logi_both(TAG_WIFI, "forget failed: %s", esp_err_to_name(e));
                continue;
            }
            if (strcmp(msg.line, "mem") == 0) {
                wifi_print_memory();
                continue;
            }
            if(strncmp(msg.line, "mqtt hb off", 12) == 0){
                mqtt_app_hb_stop();
                continue;
            }
            if(strncmp(msg.line, "mqtt hb on", 11) == 0){
                mqtt_app_hb_start();
                continue;
            }
            if(strncmp(msg.line, "mqttsend", 8) == 0){
                char mqtt_messg[65] = {0};
                int n = sscanf(msg.line, "mqttsend %64s", mqtt_messg);
                if(n <= 0){
                    logi_both(TAG_mqtt, "message empty");
                }
                if(mqtt_app_publish(MQTT_SUB_TOPIC, mqtt_messg, 0, 1) < 0){
                    logi_both(TAG_mqtt, "mqtt sending failed");
                }
                continue;
            }

            uart_app_write("Unknown cmd\r\n", strlen("Unknown cmd\r\n"));
        }
    }
}


esp_err_t start_cmd_task(const char *task_name, uint32_t stack_words, UBaseType_t prio)
{
    if (!task_name) task_name = "cmd_task";
    if (stack_words == 0) stack_words = 8192;
    if (prio == 0) prio = 10;

    return (xTaskCreate(cmd_task, task_name, stack_words, NULL, prio, NULL) == pdPASS)
           ? ESP_OK : ESP_FAIL;
}
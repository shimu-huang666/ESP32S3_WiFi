#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"


#include "wifi.h"
#include "uart.h"   // uart_app_write / uart_app_get_cmd_queue / uart_cmd_msg_t
#include "time_sync.h"
#include "mqtt_app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"

// #include "portmacro.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    EventGroupHandle_t ev;
    bool inited;
    bool connected;          // got IP
    bool manual_disconnect;
    int  retry_num;

    esp_netif_t *sta_netif;

    wifi_ap_record_t ap_cache[CONFIG_EXAMPLE_SCAN_LIST_SIZE];
    uint16_t ap_cache_num;

    esp_event_handler_instance_t h_wifi_any;
    esp_event_handler_instance_t h_got_ip;
} wifi_ctx_t;


void print_help(void);
void cmd_task(void *arg);
esp_err_t start_cmd_task(const char *task_name, uint32_t stack_words, UBaseType_t prio);

#ifdef __cplusplus
}
#endif
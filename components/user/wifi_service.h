#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_REQ_SCAN = 1,
    WIFI_REQ_CONN_INDEX,
    WIFI_REQ_CONN_SSID,
    WIFI_REQ_DISCONN,
} wifi_req_type_t;

typedef struct {
    wifi_req_type_t type;
    union {
        struct { int index; char psw[65]; bool has_psw; } conn_index;
        struct { char ssid[33]; char psw[65]; } conn_ssid;
    } u;
} wifi_req_t;

// 启动 WiFi Service 任务（只需调用一次）
esp_err_t wifi_service_start(void);

// 往 WiFi Service 投递请求（非阻塞：队列满则返回错误）
esp_err_t wifi_service_post(const wifi_req_t *req);

#ifdef __cplusplus
}
#endif

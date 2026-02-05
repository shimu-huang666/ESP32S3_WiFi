#pragma once
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "mqtt_client.h"

#include "uart.h"
// ========== MQTT CONFIG ==========
#define MQTT_BROKER_URI      "mqtt://broker.emqx.io:1883"

#define MQTT_SUB_TOPIC       "/shimu_test"     // 不需要订阅可改为 NULL
#define MQTT_HB_TOPIC        "/shimu_heartbeat"

#define MQTT_HB_ENABLE       1
#define MQTT_HB_PERIOD_MS    5000
#define MQTT_HB_QOS          0
#define MQTT_HB_RETAIN       0
// ================================
typedef struct {
    const char *broker_uri;     // e.g. "mqtt://broker.emqx.io:1883"
    const char *sub_topic;      // e.g. "/topic/qos0"
    const char *hb_topic;       // e.g. "/shimu_test"
    uint32_t    hb_period_ms;   // e.g. 5000
    int         hb_qos;         // 0 recommended
    int         hb_retain;      // 0
    bool        enable_hb;      // true/false
} mqtt_app_cfg_t;

// Wi-Fi 已连上（拿到IP）之后调用一次
esp_err_t mqtt_app_init(const char *broker_uri,
                                   const char *sub_topic,
                                   const char *hb_topic,
                                   uint32_t hb_period_ms,
                                   bool enable_hb);
void mqtt_app_start(const mqtt_app_cfg_t *cfg);

// 可在任何地方调用（会自动判断连接状态）
int  mqtt_app_publish(const char *topic, const char *payload, int qos, int retain);

// 连接状态
bool mqtt_app_is_connected(void);

void mqtt_app_hb_stop(void);
void mqtt_app_hb_start(void); // 可选


#ifdef __cplusplus
}
#endif

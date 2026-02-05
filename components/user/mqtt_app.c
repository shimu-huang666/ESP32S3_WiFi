#include "mqtt_app.h"


static const char *TAG_mqtt = "mqtt_app";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static mqtt_app_cfg_t s_cfg = {0};
static bool s_started = false;
static TaskHandle_t s_hb_task = NULL;
static volatile bool s_hb_running = false;
static int HEARTBEAT_DEFAULT_OFF = 1;

static void make_client_id(char *out, size_t n)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, n, "esp32s3_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool mqtt_app_is_connected(void)
{
    return (s_client != NULL) && s_connected;
}

int mqtt_app_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!mqtt_app_is_connected()) return -1;
    logi_both(TAG_mqtt, "publish topic=%s payload=%s", topic, payload);

    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    s_hb_running = true;

    while (s_hb_running) {

        if (mqtt_app_is_connected() && s_cfg.enable_hb && s_cfg.hb_topic) {
            int64_t up_ms = esp_timer_get_time() / 1000;

            char payload[128];
            snprintf(payload, sizeof(payload),
                     "{\"type\":\"hb\",\"uptime_ms\":%" PRId64 "}", up_ms);

            int msg_id = mqtt_app_publish(s_cfg.hb_topic, payload, s_cfg.hb_qos, s_cfg.hb_retain);
            logi_both(TAG_mqtt, "HB msg_id=%d payload=%s", msg_id, payload);
        }

        // 等待：超时=周期；如果收到通知(关闭)，立刻醒来
        uint32_t period = (s_cfg.hb_period_ms ? s_cfg.hb_period_ms : 5000);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(period));
    }

    logi_both(TAG_mqtt, "HB task exit");
    s_hb_task = NULL;
    vTaskDelete(NULL);
}



static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        logi_both(TAG_mqtt, "MQTT_EVENT_CONNECTED");
        s_connected = true;

        // 重连后重新订阅（非常关键）
        if (s_cfg.sub_topic) {
            esp_mqtt_client_subscribe(event->client, s_cfg.sub_topic, 0);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG_mqtt, "MQTT_EVENT_DISCONNECTED");
        s_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        logi_both(TAG_mqtt, "MQTT_EVENT_SUBSCRIBED msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        logi_both(TAG_mqtt, "MQTT_EVENT_PUBLISHED msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        logi_both(TAG_mqtt, "mqtt event data received");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG_mqtt, "MQTT_EVENT_ERROR");
        // 这里不做重启/重建 client，让 esp-mqtt 自动重连
        break;

    default:
        // logi_both(TAG_mqtt, "Other event id:%d", event->event_id);
        break;
    }
}
#include "esp_err.h"

esp_err_t mqtt_app_init(const char *broker_uri,
                                   const char *sub_topic,
                                   const char *hb_topic,
                                   uint32_t hb_period_ms,
                                   bool enable_hb)
{
    // 防止重复初始化
    if (s_started) {
        ESP_LOGW(TAG_mqtt, "mqtt already started, skip init");
        return ESP_OK;
    }

    if (!broker_uri || broker_uri[0] == '\0') {
        ESP_LOGE(TAG_mqtt, "broker_uri is NULL/empty");
        return ESP_ERR_INVALID_ARG;
    }

    mqtt_app_cfg_t cfg = {
        .broker_uri   = broker_uri,
        .sub_topic    = sub_topic,
        .hb_topic     = hb_topic,
        .hb_period_ms = (hb_period_ms ? hb_period_ms : 5000),
        .hb_qos       = 0,   // 心跳建议 QoS0
        .hb_retain    = 0,
        .enable_hb    = enable_hb,
    };

    mqtt_app_start(&cfg);
    if (HEARTBEAT_DEFAULT_OFF)
        mqtt_app_hb_stop();
    return ESP_OK;
}

void mqtt_app_start(const mqtt_app_cfg_t *cfg)
{
    if (s_started) {
        ESP_LOGW(TAG_mqtt, "mqtt already started");
        return;
    }
    s_started = true;

    if (!cfg || !cfg->broker_uri) {
        ESP_LOGE(TAG_mqtt, "cfg/broker_uri is NULL");
        return;
    }
    s_cfg = *cfg;

    static char client_id[64];
    make_client_id(client_id, sizeof(client_id));
    logi_both(TAG_mqtt, "client_id=%s", client_id);
    logi_both(TAG_mqtt, "broker=%s", s_cfg.broker_uri);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_cfg.broker_uri,
        .credentials.client_id = client_id,
        .session.keepalive = 60,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    if (s_cfg.enable_hb) {
        if (s_hb_task == NULL) {
            xTaskCreate(heartbeat_task, "mqtt_hb", 4096, NULL, 5, &s_hb_task);
            logi_both(TAG_mqtt, "HB task created: %p", s_hb_task);
        } else {
            logi_both(TAG_mqtt, "HB task already exists: %p", s_hb_task);
        }
    }

}
void mqtt_app_hb_stop(void)
{
    s_cfg.enable_hb = false;
    s_hb_running = false;

    if (s_hb_task) {
        // 让任务立刻从 ulTaskNotifyTake 醒来，马上退出
        xTaskNotifyGive(s_hb_task);
        logi_both(TAG_mqtt, "HB stop requested, task=%p", s_hb_task);
    } else {
        logi_both(TAG_mqtt, "HB already stopped (task NULL)");
    }
}

void mqtt_app_hb_start(void)
{
    s_cfg.enable_hb = true;
    if (s_hb_task == NULL) {
        xTaskCreate(heartbeat_task, "mqtt_hb", 4096, NULL, 5, &s_hb_task);
        logi_both(TAG_mqtt, "MQTT heartbeat started");
    }
}

#include "mqtt_app.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "uart.h"
#define MQTT_APP_MAX_SUB_TOPICS 8
#define MQTT_APP_TOPIC_MAX_LEN 128
/* -------------------- user log helper (your existing) -------------------- */
// 你已有 logi_both(...) 就不重复实现了

static const char *TAG_mqtt = "mqtt_app";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static mqtt_app_cfg_t s_cfg = {0};
static bool s_started = false;

static TaskHandle_t s_hb_task = NULL;
static volatile bool s_hb_running = false;

static int HEARTBEAT_DEFAULT_OFF = 1;

#ifndef MQTT_APP_NVS_NAMESPACE
#define MQTT_APP_NVS_NAMESPACE "mqtt_app"
#endif

#define NVS_KEY_SUB_BLOB   "subs_blob"
#define NVS_KEY_AUTO_RESUB "auto_resub"
typedef struct {
    char topic[MQTT_APP_TOPIC_MAX_LEN];
    int  qos;
    bool used;
} sub_item_t;

typedef struct {
    uint32_t magic;   // 用于版本/有效性判断
    uint16_t max;
    uint16_t count;
    sub_item_t items[MQTT_APP_MAX_SUB_TOPICS];
} subs_blob_t;

#define SUBS_BLOB_MAGIC 0x53425553u  // 'SUBS'

/* -------------------- subscription registry -------------------- */
/**
 * 默认自动重连后恢复订阅：1=开，0=关
 * 你想默认关就改成 0，或者在编译选项里 -DMQTT_APP_AUTO_RESUB_DEFAULT=0
 */
#ifndef MQTT_APP_AUTO_RESUB_DEFAULT
#define MQTT_APP_AUTO_RESUB_DEFAULT  1
#endif

#ifndef MQTT_APP_MAX_SUB_TOPICS
#define MQTT_APP_MAX_SUB_TOPICS      8
#endif

#ifndef MQTT_APP_TOPIC_MAX_LEN
#define MQTT_APP_TOPIC_MAX_LEN       128
#endif


static sub_item_t s_subs[MQTT_APP_MAX_SUB_TOPICS];
static SemaphoreHandle_t s_subs_lock = NULL;
static bool s_auto_resub = (MQTT_APP_AUTO_RESUB_DEFAULT != 0);

static void subs_lock_init_once(void)
{
    if (s_subs_lock == NULL) {
        s_subs_lock = xSemaphoreCreateMutex();
    }
}

static int subs_find_nolock(const char *topic)
{
    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        if (s_subs[i].used && (strcmp(s_subs[i].topic, topic) == 0)) return i;
    }
    return -1;
}

static int subs_alloc_nolock(void)
{
    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        if (!s_subs[i].used) return i;
    }
    return -1;
}

static void subs_add_or_update(const char *topic, int qos)
{
    subs_lock_init_once();
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    int idx = subs_find_nolock(topic);
    if (idx < 0) {
        idx = subs_alloc_nolock();
        if (idx >= 0) {
            strlcpy(s_subs[idx].topic, topic, sizeof(s_subs[idx].topic));
            s_subs[idx].qos = qos;
            s_subs[idx].used = true;
        } else {
            ESP_LOGW(TAG_mqtt, "subs list full (max=%d), cannot record: %s",
                     MQTT_APP_MAX_SUB_TOPICS, topic);
        }
    } else {
        s_subs[idx].qos = qos; // update qos
    }

    xSemaphoreGive(s_subs_lock);
}

static void subs_remove(const char *topic)
{
    subs_lock_init_once();
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    int idx = subs_find_nolock(topic);
    if (idx >= 0) {
        s_subs[idx].used = false;
        s_subs[idx].topic[0] = '\0';
        s_subs[idx].qos = 0;
    }

    xSemaphoreGive(s_subs_lock);
}

static void resubscribe_all_if_needed(esp_mqtt_client_handle_t client)
{
    if (!s_auto_resub) return;

    subs_lock_init_once();
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        if (s_subs[i].used && s_subs[i].topic[0]) {
            int msg_id = esp_mqtt_client_subscribe(client, s_subs[i].topic, s_subs[i].qos);
            ESP_LOGI(TAG_mqtt, "auto-resub: topic=%s qos=%d msg_id=%d",
                     s_subs[i].topic, s_subs[i].qos, msg_id);
        }
    }

    xSemaphoreGive(s_subs_lock);
}
static esp_err_t nvs_open_mqtt(nvs_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    // 注意：如果你工程别处已 nvs_flash_init()，这里不会出问题
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 可选：做一次擦除恢复（更激进），不想擦就直接 return err
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    return nvs_open(MQTT_APP_NVS_NAMESPACE, NVS_READWRITE, out);
}

esp_err_t mqtt_app_save_subscriptions_to_nvs(void)
{
    subs_lock_init_once();
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    subs_blob_t blob = {0};
    blob.magic = SUBS_BLOB_MAGIC;
    blob.max   = MQTT_APP_MAX_SUB_TOPICS;

    // 把 used 的都打包进去（为了简单，直接保存整个 items 数组）
    int used_cnt = 0;
    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        if (s_subs[i].used && s_subs[i].topic[0]) used_cnt++;
        blob.items[i] = s_subs[i];
    }
    blob.count = (uint16_t)used_cnt;

    bool auto_resub = s_auto_resub;

    xSemaphoreGive(s_subs_lock);

    nvs_handle_t h;
    esp_err_t err = nvs_open_mqtt(&h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, NVS_KEY_SUB_BLOB, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_AUTO_RESUB, (uint8_t)(auto_resub ? 1 : 0));
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG_mqtt, "subs saved to NVS: count=%u auto_resub=%d",
                 (unsigned)blob.count, (int)auto_resub);
    } else {
        ESP_LOGW(TAG_mqtt, "subs save to NVS failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t mqtt_app_load_subscriptions_from_nvs(void)
{
    subs_lock_init_once();

    nvs_handle_t h;
    esp_err_t err = nvs_open_mqtt(&h);
    if (err != ESP_OK) return err;

    subs_blob_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY_SUB_BLOB, &blob, &sz);

    uint8_t ar = (uint8_t)(MQTT_APP_AUTO_RESUB_DEFAULT != 0);
    esp_err_t err2 = nvs_get_u8(h, NVS_KEY_AUTO_RESUB, &ar);
    if (err2 != ESP_OK) {
        // 没有这个键也正常：用默认值
    }

    nvs_close(h);

    if (err != ESP_OK || sz != sizeof(blob) || blob.magic != SUBS_BLOB_MAGIC || blob.max != MQTT_APP_MAX_SUB_TOPICS) {
        ESP_LOGW(TAG_mqtt, "no valid subs in NVS (err=%s), keep default",
                 esp_err_to_name(err));
        // 仍然把 auto_resub 载入（如果有）
        s_auto_resub = (ar != 0);
        return (err == ESP_OK ? ESP_FAIL : err);
    }

    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    // 清空再恢复
    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        s_subs[i].used = false;
        s_subs[i].topic[0] = '\0';
        s_subs[i].qos = 0;
    }

    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        // 只恢复合法项
        if (blob.items[i].used && blob.items[i].topic[0]) {
            s_subs[i] = blob.items[i];
            s_subs[i].topic[MQTT_APP_TOPIC_MAX_LEN - 1] = '\0';
            if (s_subs[i].qos < 0) s_subs[i].qos = 0;
            if (s_subs[i].qos > 2) s_subs[i].qos = 2;
        }
    }

    s_auto_resub = (ar != 0);

    xSemaphoreGive(s_subs_lock);

    ESP_LOGI(TAG_mqtt, "subs loaded from NVS: count=%u auto_resub=%d",
             (unsigned)blob.count, (int)s_auto_resub);
    return ESP_OK;
}

esp_err_t mqtt_app_clear_subscriptions_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_mqtt(&h);
    if (err != ESP_OK) return err;

    nvs_erase_key(h, NVS_KEY_SUB_BLOB);
    nvs_erase_key(h, NVS_KEY_AUTO_RESUB);
    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG_mqtt, "subs cleared in NVS");
    return err;
}
/* -------------------- client id -------------------- */
static void make_client_id(char *out, size_t n)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, n, "esp32s3_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* -------------------- public APIs -------------------- */
bool mqtt_app_is_connected(void)
{
    return (s_client != NULL) && s_connected;
}

int mqtt_app_publish(const char *topic, const char *payload, int qos, int retain)
{
    return mqtt_app_publish_to(topic, payload, qos, retain);
}

int mqtt_app_publish_to(const char *topic, const char *payload, int qos, int retain)
{
    if (!topic || !topic[0] || !payload) return -1;
    if (!mqtt_app_is_connected()) return -1;

    logi_both(TAG_mqtt, "publish topic=%s payload=%s", topic, payload);
    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

esp_err_t mqtt_app_subscribe_topic(const char *topic, int qos)
{
    if (!topic || !topic[0]) return ESP_ERR_INVALID_ARG;
    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;

    // 记录到订阅列表（用于重连恢复）
    subs_add_or_update(topic, qos);
    mqtt_app_save_subscriptions_to_nvs();

    if (mqtt_app_is_connected()) {
        int msg_id = esp_mqtt_client_subscribe(s_client, topic, qos);
        logi_both(TAG_mqtt, "subscribe topic=%s qos=%d msg_id=%d", topic, qos, msg_id);
        return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
    }

    // 未连接也没关系：下次 CONNECTED 时会自动订阅（若 auto 开启）
    logi_both(TAG_mqtt, "subscribe recorded (offline). topic=%s qos=%d", topic, qos);
    return ESP_OK;
}

esp_err_t mqtt_app_unsubscribe_topic(const char *topic)
{
    if (!topic || !topic[0]) return ESP_ERR_INVALID_ARG;

    // 从订阅列表移除
    subs_remove(topic);
    mqtt_app_save_subscriptions_to_nvs();

    if (mqtt_app_is_connected()) {
        int msg_id = esp_mqtt_client_unsubscribe(s_client, topic);
        logi_both(TAG_mqtt, "unsubscribe topic=%s msg_id=%d", topic, msg_id);
        return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
    }

    logi_both(TAG_mqtt, "unsubscribe recorded (offline). topic=%s", topic);
    return ESP_OK;
}

void mqtt_app_set_auto_resubscribe(bool enable)
{
    s_auto_resub = enable;
    logi_both(TAG_mqtt, "auto_resubscribe=%d", (int)s_auto_resub);
    mqtt_app_save_subscriptions_to_nvs();

}

bool mqtt_app_get_auto_resubscribe(void)
{
    return s_auto_resub;
}

void mqtt_app_dump_subscriptions(void)
{
    subs_lock_init_once();
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    logi_both(TAG_mqtt, "==== SUBSCRIPTIONS (auto_resub=%d) ====", (int)s_auto_resub);
    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        if (s_subs[i].used) {
            logi_both(TAG_mqtt, "[%d] topic=%s qos=%d", i, s_subs[i].topic, s_subs[i].qos);
        }
    }
    logi_both(TAG_mqtt, "======================================");

    xSemaphoreGive(s_subs_lock);
}

int mqtt_app_get_subscription_count(void)
{
    int cnt = 0;
    subs_lock_init_once();
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        if (s_subs[i].used) cnt++;
    }

    xSemaphoreGive(s_subs_lock);
    return cnt;
}

esp_err_t mqtt_app_get_subscription_topic(int index, char *out, size_t n)
{
    if (!out || n == 0) return ESP_ERR_INVALID_ARG;

    subs_lock_init_once();
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    int cur = 0;
    for (int i = 0; i < MQTT_APP_MAX_SUB_TOPICS; i++) {
        if (!s_subs[i].used) continue;
        if (cur == index) {
            strlcpy(out, s_subs[i].topic, n);
            xSemaphoreGive(s_subs_lock);
            return ESP_OK;
        }
        cur++;
    }

    xSemaphoreGive(s_subs_lock);
    return ESP_ERR_INVALID_ARG;
}

/* -------------------- heartbeat task (your original) -------------------- */
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

            int msg_id = mqtt_app_publish_to(s_cfg.hb_topic, payload, s_cfg.hb_qos, s_cfg.hb_retain);
            logi_both(TAG_mqtt, "HB msg_id=%d payload=%s", msg_id, payload);
        }

        uint32_t period = (s_cfg.hb_period_ms ? s_cfg.hb_period_ms : 5000);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(period));
    }

    logi_both(TAG_mqtt, "HB task exit");
    s_hb_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------- mqtt event handler -------------------- */
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

        // 1) 兼容：如果 cfg 给了 sub_topic，则也加入订阅列表并订阅
        if (s_cfg.sub_topic && s_cfg.sub_topic[0]) {
            subs_add_or_update(s_cfg.sub_topic, 0);
        }

        // 2) 自动订阅历史 topic（重连恢复）
        resubscribe_all_if_needed(event->client);
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
        logi_both(TAG_mqtt, "mqtt event data received: topic=%.*s data=%.*s ",
                 event->topic_len, event->topic, event->data_len, event->data);
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG_mqtt, "MQTT_EVENT_ERROR");
        break;

    default:
        break;
    }
}

/* -------------------- init/start (your original + minor hook) -------------------- */
esp_err_t mqtt_app_init(const char *broker_uri,
                        const char *sub_topic,
                        const char *hb_topic,
                        uint32_t hb_period_ms,
                        bool enable_hb)
{
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
        .hb_qos       = 0,
        .hb_retain    = 0,
        .enable_hb    = enable_hb,
    };

    mqtt_app_start(&cfg);

    if (HEARTBEAT_DEFAULT_OFF) {
        mqtt_app_hb_stop();
    }
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

    subs_lock_init_once();  
    mqtt_app_load_subscriptions_from_nvs();

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
        }
    }

    // 如果 init 传了 sub_topic，这里也提前记录（即使未连接）
    if (s_cfg.sub_topic && s_cfg.sub_topic[0]) {
        subs_add_or_update(s_cfg.sub_topic, 0);
    }
}

void mqtt_app_hb_stop(void)
{
    s_cfg.enable_hb = false;
    s_hb_running = false;

    if (s_hb_task) {
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

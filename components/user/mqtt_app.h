#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT app configuration structure.
 */
typedef struct {
    const char *broker_uri;      /*!< Broker URI, e.g. "mqtt://192.168.1.10" */
    const char *sub_topic;       /*!< Optional: initial subscribe topic (single topic). Can be NULL. */
    const char *hb_topic;        /*!< Optional: heartbeat publish topic. Can be NULL. */
    uint32_t    hb_period_ms;    /*!< Heartbeat period (ms). 0 -> default 5000ms */
    int         hb_qos;          /*!< Heartbeat QoS (0/1/2) */
    int         hb_retain;       /*!< Heartbeat retain flag (0/1) */
    bool        enable_hb;       /*!< Enable heartbeat task */
} mqtt_app_cfg_t;

/**
 * @brief Initialize MQTT module (one-shot). Will start MQTT internally.
 *
 * @param broker_uri    Broker URI string. Must not be NULL/empty.
 * @param sub_topic     Optional initial subscribe topic. Can be NULL.
 * @param hb_topic      Optional heartbeat publish topic. Can be NULL.
 * @param hb_period_ms  Heartbeat period (ms). 0 -> default 5000ms.
 * @param enable_hb     Whether to enable heartbeat feature.
 * @return ESP_OK on success; otherwise error code.
 */
esp_err_t mqtt_app_init(const char *broker_uri,
                        const char *sub_topic,
                        const char *hb_topic,
                        uint32_t hb_period_ms,
                        bool enable_hb);

/**
 * @brief Start MQTT with full configuration.
 *
 * @param cfg Pointer to config. cfg and cfg->broker_uri must not be NULL.
 */
void mqtt_app_start(const mqtt_app_cfg_t *cfg);

/**
 * @brief Check MQTT connection state.
 *
 * @return true if client exists and connected.
 */
bool mqtt_app_is_connected(void);

/**
 * @brief Publish to a topic with qos/retain.
 *
 * @param topic   Topic string. Must not be NULL.
 * @param payload Payload string. Can be empty but not NULL.
 * @param qos     MQTT QoS (0/1/2)
 * @param retain  Retain flag (0/1)
 * @return msg_id (>=0) if queued, or -1 on failure.
 */
int mqtt_app_publish_to(const char *topic, const char *payload, int qos, int retain);

/**
 * @brief Backward compatible publish (kept if you already use it).
 */
int mqtt_app_publish(const char *topic, const char *payload, int qos, int retain);

/**
 * @brief Subscribe a topic (and record it into subscription list).
 *
 * If auto-resubscribe is enabled, the topic will be automatically re-subscribed
 * after reconnect.
 *
 * @param topic Topic string (NULL/empty not allowed)
 * @param qos   MQTT QoS (0/1/2)
 * @return ESP_OK on success; otherwise error code.
 */
esp_err_t mqtt_app_subscribe_topic(const char *topic, int qos);

/**
 * @brief Unsubscribe a topic (and remove it from subscription list).
 *
 * @param topic Topic string (NULL/empty not allowed)
 * @return ESP_OK on success; otherwise error code.
 */
esp_err_t mqtt_app_unsubscribe_topic(const char *topic);

/**
 * @brief Enable/disable auto-resubscribe after reconnect.
 *
 * @param enable true to enable; false to disable.
 */
void mqtt_app_set_auto_resubscribe(bool enable);

/**
 * @brief Get auto-resubscribe enable state.
 */
bool mqtt_app_get_auto_resubscribe(void);

/**
 * @brief List current subscribed topics (print to log).
 */
void mqtt_app_dump_subscriptions(void);

/**
 * @brief Get subscription count.
 *
 * @return number of topics currently stored.
 */
int mqtt_app_get_subscription_count(void);

/**
 * @brief Get a subscription topic by index.
 *
 * @param index 0..count-1
 * @param out   output buffer
 * @param n     output buffer size
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG on bad index/params.
 */
esp_err_t mqtt_app_get_subscription_topic(int index, char *out, size_t n);

/**
 * @brief Stop MQTT heartbeat task (if running) and disable heartbeat.
 */
void mqtt_app_hb_stop(void);

/**
 * @brief Start MQTT heartbeat task (if enabled in config).
 */
void mqtt_app_hb_start(void);
esp_err_t mqtt_app_load_subscriptions_from_nvs(void);
esp_err_t mqtt_app_save_subscriptions_to_nvs(void);
esp_err_t mqtt_app_clear_subscriptions_nvs(void);

#ifdef __cplusplus
}
#endif

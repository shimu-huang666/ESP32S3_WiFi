#include "wifi_service.h"

#include <string.h>

#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"

// 你已有的 WiFi API（来自 wifi.c / wifi.h）
#include "wifi.h"

// 你已有的 UART 打印（来自 uart.c）
extern void logi_both(const char *tag, const char *fmt, ...);

static const char *TAG = "wifi_svc";

static QueueHandle_t s_q = NULL;
static TaskHandle_t  s_task = NULL;

// 防止同时 scan/conn 互相干扰（最简单的“忙碌锁”）
static volatile bool s_busy = false;

/* ---------------- worker: scan ---------------- */

static void scan_worker_task(void *arg)
{
    (void)arg;

    logi_both("scan", "Scan worker started...");
    wifi_scan_once_and_print_sorted();
    logi_both("scan", "Scan worker done.");

    s_busy = false;
    vTaskDelete(NULL);
}

/* ---------------- wifi_service task ---------------- */

static void wifi_service_task(void *arg)
{
    (void)arg;

    wifi_req_t r;

    while (1) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // 简单策略：正在执行长操作时，拒绝新请求（你也可以改成排队/覆盖）
        if (s_busy) {
            logi_both(TAG, "Busy, ignore req=%d", (int)r.type);
            continue;
        }

        switch (r.type) {
        case WIFI_REQ_SCAN: {
            s_busy = true;
            // 开一个独立线程扫描，不阻塞 cmd_task
            BaseType_t ok = xTaskCreate(scan_worker_task, "wifi_scan_w", 4096, NULL, 9, NULL);
            if (ok != pdPASS) {
                s_busy = false;
                logi_both(TAG, "Create scan worker failed");
            }
            break;
        }

        case WIFI_REQ_CONN_INDEX: {
            s_busy = true;

            logi_both("wifi", "Connect request: index=%d", r.u.conn_index.index);
            esp_err_t e = wifi_connect_by_index(
                r.u.conn_index.index,
                r.u.conn_index.has_psw ? r.u.conn_index.psw : NULL
            );
            logi_both("wifi", "Connect result: %s", esp_err_to_name(e));

            s_busy = false;
            break;
        }

        case WIFI_REQ_CONN_SSID: {
            s_busy = true;

            logi_both("wifi", "Connect request: ssid='%s'", r.u.conn_ssid.ssid);
            esp_err_t e = wifi_connect_by_ssid(r.u.conn_ssid.ssid, r.u.conn_ssid.psw);
            logi_both("wifi", "Connect result: %s", esp_err_to_name(e));

            s_busy = false;
            break;
        }

        case WIFI_REQ_DISCONN: {
            // 手动断开：不要在 cmd.c 里直接改 wifi_ctx_t
            // 这里最保守、最通用的做法：直接调用 esp_wifi_disconnect()
            // 如果你在 wifi.c 里做了 wifi_disconnect_manual()，这里换成那个更好
            logi_both("wifi", "Manual disconnect requested");
            esp_wifi_disconnect();
            s_busy = false;
            break;
        }

        default:
            logi_both(TAG, "Unknown req=%d", (int)r.type);
            break;
        }
    }
}

esp_err_t wifi_service_start(void)
{
    if (s_q) return ESP_OK;

    s_q = xQueueCreate(8, sizeof(wifi_req_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(wifi_service_task, "wifi_svc", 4096, NULL, 9, &s_task);
    if (ok != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        s_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "wifi_service started");
    return ESP_OK;
}

esp_err_t wifi_service_post(const wifi_req_t *req)
{
    if (!s_q || !req) return ESP_ERR_INVALID_STATE;

    // 非阻塞投递：满了就失败（你也可以改成等待）
    BaseType_t ok = xQueueSend(s_q, req, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

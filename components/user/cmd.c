#include "cmd.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "uart.h"
#include "wifi.h"
#include "wifi_service.h"
#include "mqtt_app.h"
// 你已有的时间函数（如果你有 time_sync.h 就 include；没有就保持 extern）
extern bool time_is_valid(void);
extern void print_time_now(void);

// MQTT
#include "mqtt_app.h"

static const char *TAG_CMD  = "cmd";
static const char *TAG_WIFI = "wifi";
static const char *TAG_TIME = "time";
static const char *TAG_MQTT = "mqtt";
static const char *MQTT_SUB_TOPIC = "/shimu_test";
static const char *MQTT_HB_TOPIC = "/shimu_test/hb";
/* ------------------------ UART printf helper ------------------------ */
static void uart_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) uart_app_write(buf, (size_t)n);
}

/* ------------------------ token helpers (in-place) ------------------------ */
static char *skip_spaces(char *p)
{
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

static char *next_token(char **ps)
{
    char *p = skip_spaces(*ps);
    if (!p || !*p) { *ps = p; return NULL; }

    char *tok = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) { *p = '\0'; p++; }
    *ps = p;
    return tok;
}

static char *rest_text(char **ps)
{
    char *p = skip_spaces(*ps);
    *ps = p;
    return (p && *p) ? p : NULL;
}

/* pub: 支持 pub <topic> <payload...> [qos] [retain] （qos/retain放最后）
   从payload尾部剥离最多2个纯数字token：先retain再qos
*/
static void strip_tail_ints(char *s, int *qos_opt, int *retain_opt)
{
    if (!s) return;

    int qos = *qos_opt;
    int retain = *retain_opt;

    for (int pass = 0; pass < 2; pass++) {
        int len = (int)strlen(s);

        while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
        if (len == 0) break;

        int i = len - 1;
        while (i >= 0 && !isspace((unsigned char)s[i])) i--;
        char *last = (i >= 0) ? (s + i + 1) : s;

        bool all_digit = (*last != '\0');
        for (char *p = last; *p; p++) {
            if (!isdigit((unsigned char)*p)) { all_digit = false; break; }
        }
        if (!all_digit) break;

        int v = atoi(last);

        if (i >= 0) s[i] = '\0';
        else s[0] = '\0';

        if (pass == 0) retain = v;
        else qos = v;
    }

    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;
    retain = (retain != 0);

    *qos_opt = qos;
    *retain_opt = retain;
}

/* ------------------------ HELP ------------------------ */
void print_help(void)
{
    const char *h =
        "scan                       - wifi scan (async)\r\n"
        "conn <i> [psw]             - connect to AP by index (async)\r\n"
        "connssid <ssid> <psw>      - connect by ssid and password (async)\r\n"
        "info                       - show current wifi info\r\n"
        "time                       - show time\r\n"
        "disconn                    - manual disconnect (async)\r\n"
        "reconn                     - reconnect using saved STA cfg (flash)\r\n"
        "forget                     - erase last saved wifi (NVS) and disconnect\r\n"
        "mem                        - show saved wifi memory (NVS + STA flash cfg)\r\n"
        "reboot                     - reboot the device\r\n"
        "savesubs                   - save subscriptions to flash\r\n"
        "\r\n"
        "MQTT (new):\r\n"
        "  sub <topic> [qos]         - subscribe topic (recorded, auto-resub if enabled)\r\n"
        "  unsub <topic>             - unsubscribe topic (and remove from list)\r\n"
        "  autosub [on|off]          - auto re-subscribe after reconnect (no arg -> show)\r\n"
        "  subs                      - list current subscribed topics\r\n"
        "  pub <topic> <payload...> [qos] [retain]\r\n"
        "  hb on|off                 - heartbeat on/off\r\n"
        "\r\n"
        "Legacy MQTT:\r\n"
        "  mqtt hb <on|off>          - heartbeat on/off\r\n"
        "  mqttsend [message]        - publish message to MQTT_SUB_TOPIC\r\n"
        "\r\n"
        "help                       - show help\r\n";
    uart_app_write(h, strlen(h));
}

/* ------------------------ MQTT command handlers ------------------------ */
static bool handle_mqtt_cmd(char *line_mutable)
{
    if (!line_mutable || !line_mutable[0]) return false;

    // We parse first token
    char *p = line_mutable;
    char *cmd = next_token(&p);
    if (!cmd) return false;

    /* ---- new style mqtt cmds ---- */
    if (strcmp(cmd, "sub") == 0) {
        char *topic = next_token(&p);
        char *qos_s = next_token(&p);
        int qos = qos_s ? atoi(qos_s) : 0;

        if (!topic) {
            uart_printf("Usage: sub <topic> [qos]\r\n");
            return true;
        }

        esp_err_t e = mqtt_app_subscribe_topic(topic, qos);
        uart_printf("sub topic=%s qos=%d -> %s\r\n", topic, qos, esp_err_to_name(e));
        return true;
    }

    if (strcmp(cmd, "unsub") == 0) {
        char *topic = next_token(&p);
        if (!topic) {
            uart_printf("Usage: unsub <topic>\r\n");
            return true;
        }
        esp_err_t e = mqtt_app_unsubscribe_topic(topic);
        uart_printf("unsub topic=%s -> %s\r\n", topic, esp_err_to_name(e));
        return true;
    }

    if (strcmp(cmd, "autosub") == 0) {
        char *sw = next_token(&p);
        if (!sw) {
            uart_printf("autosub is %s\r\n", mqtt_app_get_auto_resubscribe() ? "on" : "off");
            uart_printf("Usage: autosub on|off\r\n");
            return true;
        }
        if (strcmp(sw, "on") == 0 || strcmp(sw, "1") == 0) {
            mqtt_app_set_auto_resubscribe(true);
            uart_printf("autosub -> on\r\n");
        } else if (strcmp(sw, "off") == 0 || strcmp(sw, "0") == 0) {
            mqtt_app_set_auto_resubscribe(false);
            uart_printf("autosub -> off\r\n");
        } else {
            uart_printf("Usage: autosub on|off\r\n");
        }
        return true;
    }

    if (strcmp(cmd, "subs") == 0) {
        mqtt_app_dump_subscriptions();
        uart_printf("subs dumped.\r\n");
        return true;
    }
    if(strcmp(cmd, "savesubs") == 0){
        mqtt_app_save_subscriptions_to_nvs();
        uart_printf("subs saved to flash.\r\n");
        return true;
    }
    if (strcmp(cmd, "pub") == 0) {
        char *topic = next_token(&p);
        char *payload = rest_text(&p);
        int qos = 0;
        int retain = 0;

        if (!topic || !payload) {
            uart_printf("Usage: pub <topic> <payload...> [qos] [retain]\r\n");
            uart_printf("Example: pub dev/log hello world 0 0\r\n");
            return true;
        }

        strip_tail_ints(payload, &qos, &retain);
        int msg_id = mqtt_app_publish_to(topic, payload, qos, retain);
        uart_printf("pub topic=%s qos=%d retain=%d msg_id=%d\r\n", topic, qos, retain, msg_id);
        return true;
    }

    if (strcmp(cmd, "hb") == 0) {
        char *sw = next_token(&p);
        if (!sw) {
            uart_printf("Usage: hb on|off\r\n");
            return true;
        }
        if (strcasecmp(sw, "on") == 0 || strcmp(sw, "1") == 0) {
            mqtt_app_hb_start();
            uart_printf("hb -> on\r\n");
        } else if (strcasecmp(sw, "off") == 0 || strcmp(sw, "0") == 0) {
            mqtt_app_hb_stop();
            uart_printf("hb -> off\r\n");
        } else {
            uart_printf("Usage: hb on|off\r\n");
        }
        return true;
    }

    /* ---- legacy mqtt cmds you already had ---- */
    // keep: "mqtt hb on/off"
    if (strcmp(cmd, "mqtt") == 0) {
        char *sub = next_token(&p);
        char *sw  = next_token(&p);

        if (sub && strcmp(sub, "hb") == 0 && sw) {
            if (strcasecmp(sw, "off") == 0) {
                mqtt_app_hb_stop();
                uart_printf("mqtt hb -> off\r\n");
                return true;
            }
            if (strcasecmp(sw, "on") == 0) {
                mqtt_app_hb_start();
                uart_printf("mqtt hb -> on\r\n");
                return true;
            }
            uart_printf("Usage: mqtt hb <on|off>\r\n");
            return true;
        }

        uart_printf("Usage: mqtt hb <on|off>\r\n");
        return true;
    }

    // keep: "mqttsend [message]"
    if (strcmp(cmd, "mqttsend") == 0) {
        char *m = next_token(&p);
        if (!m) {
            logi_both(TAG_MQTT, "message empty");
            uart_printf("Usage: mqttsend <message>\r\n");
            return true;
        }
        if (mqtt_app_publish(MQTT_SUB_TOPIC, m, 0, 1) < 0) {
            logi_both(TAG_MQTT, "mqtt sending failed");
            uart_printf("mqttsend failed\r\n");
        } else {
            uart_printf("mqttsend ok\r\n");
        }
        return true;
    }
    if(strcmp(cmd, "reboot") == 0){
        esp_restart();
        return true;
    }
    return false; // not handled
}

/* ------------------------ Main CMD Task (integrated) ------------------------ */
void cmd_task(void *arg)
{
    (void)arg;

    // 启动 WiFi Service（只需一次）
    (void)wifi_service_start();

    QueueHandle_t q = uart_app_get_cmd_queue();
    uart_cmd_msg_t msg;

    uart_app_write("\r\n==== UART CMD READY ====\r\n", strlen("\r\n==== UART CMD READY ====\r\n"));
    print_help();
    uart_app_write("========================\r\n", strlen("========================\r\n"));

    while (1) {
        if (xQueueReceive(q, &msg, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGI(TAG_CMD, "CMD: %s", msg.line);

        uart_app_write(">", 1);
        uart_app_write(msg.line, strlen(msg.line));
        uart_app_write("\r\n", 2);

        /* 重要：MQTT命令解析会“就地写\0切token”，所以必须用可写buffer。
           如果 msg.line 是数组 OK；如果是指针且指向只读区，会崩。
         */
        char line_buf[256];
        strlcpy(line_buf, msg.line, sizeof(line_buf));

        /* -------- 先处理新 MQTT 命令（不影响你原本命令） -------- */
        if (handle_mqtt_cmd(line_buf)) {
            continue;
        }

        /* -------- scan：投递请求，立刻返回 -------- */
        if (strcmp(msg.line, "scan") == 0) {
            wifi_req_t r = {.type = WIFI_REQ_SCAN};
            esp_err_t e = wifi_service_post(&r);
            if (e == ESP_OK) uart_app_write("Scan requested\r\n", strlen("Scan requested\r\n"));
            else logi_both(TAG_WIFI, "scan req failed: %s", esp_err_to_name(e));
            continue;
        }

        /* -------- connssid：投递请求 -------- */
        if (strncmp(msg.line, "connssid", 8) == 0) {
            wifi_req_t r = {.type = WIFI_REQ_CONN_SSID};
            int n = sscanf(msg.line, "connssid %32s %64s", r.u.conn_ssid.ssid, r.u.conn_ssid.psw);
            if (n != 2) {
                uart_app_write("Usage: connssid <ssid> <psw>\r\n", strlen("Usage: connssid <ssid> <psw>\r\n"));
            } else {
                esp_err_t e = wifi_service_post(&r);
                if (e == ESP_OK) uart_app_write("Connect requested\r\n", strlen("Connect requested\r\n"));
                else logi_both(TAG_WIFI, "connssid req failed: %s", esp_err_to_name(e));
            }
            continue;
        }

        /* -------- conn：投递请求 -------- */
        if (strncmp(msg.line, "conn", 4) == 0) {
            int idx = 0;
            char psw[65] = {0};
            int n = sscanf(msg.line, "conn %d %64s", &idx, psw);

            if (n <= 0) {
                uart_app_write("Usage: conn <index> [psw]\r\n", strlen("Usage: conn <index> [psw]\r\n"));
            } else {
                wifi_req_t r = {.type = WIFI_REQ_CONN_INDEX};
                r.u.conn_index.index = idx;
                if (n == 2) {
                    r.u.conn_index.has_psw = true;
                    strncpy(r.u.conn_index.psw, psw, sizeof(r.u.conn_index.psw) - 1);
                } else {
                    r.u.conn_index.has_psw = false;
                }

                esp_err_t e = wifi_service_post(&r);
                if (e == ESP_OK) uart_app_write("Connect requested\r\n", strlen("Connect requested\r\n"));
                else logi_both(TAG_WIFI, "conn req failed: %s", esp_err_to_name(e));
            }
            continue;
        }

        /* -------- disconn：投递请求 -------- */
        if (strcmp(msg.line, "disconn") == 0) {
            wifi_req_t r = {.type = WIFI_REQ_DISCONN};
            esp_err_t e = wifi_service_post(&r);
            if (e == ESP_OK) uart_app_write("Disconnect requested\r\n", strlen("Disconnect requested\r\n"));
            else logi_both(TAG_WIFI, "disconn req failed: %s", esp_err_to_name(e));
            continue;
        }

        /* -------- 下面这些通常很快，直接调用即可 -------- */
        if (strcmp(msg.line, "reconn") == 0) {
            esp_err_t e = wifi_reconnect_saved();
            if (e != ESP_OK) logi_both(TAG_WIFI, "reconn failed: %s", esp_err_to_name(e));
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

        if (strcmp(msg.line, "forget") == 0) {
            esp_err_t e = wifi_forget_last(true);
            if (e != ESP_OK) logi_both(TAG_WIFI, "forget failed: %s", esp_err_to_name(e));
            continue;
        }

        if (strcmp(msg.line, "mem") == 0) {
            wifi_print_memory();
            continue;
        }

        if (strcmp(msg.line, "help") == 0) {
            print_help();
            continue;
        }

        uart_app_write("Unknown cmd\r\n", strlen("Unknown cmd\r\n"));
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

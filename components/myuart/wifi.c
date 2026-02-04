/*
    WiFi Scan + Sort by RSSI (strong -> weak) + formatted output
    + UART cmd: conn <index> <psw>  connect wifi by scan index
*/

#include "wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"


#include "uart.h"   // 依赖：uart_app_write / uart_app_get_cmd_queue / uart_cmd_msg_t

static const char *TAG_scan = "scan";
static const char *TAG_wifi = "wifi";
static const char *TAG_CMD  = "cmd";

/* 如果你没在 menuconfig 里配置 CONFIG_EXAMPLE_SCAN_LIST_SIZE，就用默认值 */
#ifndef CONFIG_EXAMPLE_SCAN_LIST_SIZE
#define CONFIG_EXAMPLE_SCAN_LIST_SIZE 20
#endif
#define DEFAULT_SCAN_LIST_SIZE CONFIG_EXAMPLE_SCAN_LIST_SIZE

/* 重试次数：你也可以改成 Kconfig */
#ifndef WIFI_MAXIMUM_RETRY
#define WIFI_MAXIMUM_RETRY  3
#endif

// 扫描缓存（打印顺序=索引顺序）
static wifi_ap_record_t g_ap_cache[DEFAULT_SCAN_LIST_SIZE];
static uint16_t g_ap_cache_num = 0;

/* event group bits */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_wifi_connected = false;   // 是否已获取 IP（认为连接成功）
static esp_netif_t *s_sta_netif = NULL; // 保存 STA netif 句柄，后面查 IP/DNS 用

/* 防重复初始化 */
static bool s_wifi_inited = false;

/* 事件句柄（可选：如果你以后要注销） */
static esp_event_handler_instance_t s_instance_any_id;
static esp_event_handler_instance_t s_instance_got_ip;

static void logi_both(const char *tag, const char *fmt, ...)
{
    char buf[180];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) return;

    ESP_LOGI(tag, "%s", buf);
    uart_app_write(buf, strnlen(buf, sizeof(buf)));
    uart_app_write("\r\n", 2);
}

// ---------- 字符串辅助：AUTH / CIPHER / BSSID ----------
static const char *authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:                 return "OPEN";
    case WIFI_AUTH_OWE:                  return "OWE";
    case WIFI_AUTH_WEP:                  return "WEP";
    case WIFI_AUTH_WPA_PSK:              return "WPA";
    case WIFI_AUTH_WPA2_PSK:             return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:         return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:             return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:        return "WPA2/WPA3";
    case WIFI_AUTH_ENTERPRISE:           return "ENT";
    case WIFI_AUTH_WPA3_ENTERPRISE:      return "WPA3-ENT";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/WPA3-ENT";
    case WIFI_AUTH_WPA3_ENT_192:         return "WPA3-192";
    default:                             return "UNK";
    }
}

static const char *cipher_str(wifi_cipher_type_t c)
{
    switch (c) {
    case WIFI_CIPHER_TYPE_NONE:          return "NONE";
    case WIFI_CIPHER_TYPE_WEP40:         return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104:        return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP:          return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:          return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP:     return "TKIP/CCMP";
    case WIFI_CIPHER_TYPE_AES_CMAC128:   return "AES-CMAC";
    case WIFI_CIPHER_TYPE_SMS4:          return "SMS4";
    case WIFI_CIPHER_TYPE_GCMP:          return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256:       return "GCMP256";
    default:                             return "UNK";
    }
}

static void bssid_to_str(const uint8_t bssid[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

// ---------- 排序：RSSI 从强到弱 ----------
static int cmp_ap_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *rb = (const wifi_ap_record_t *)b;

    if (ra->rssi > rb->rssi) return -1;
    if (ra->rssi < rb->rssi) return  1;

    if (ra->primary < rb->primary) return -1;
    if (ra->primary > rb->primary) return  1;

    return strncmp((const char *)ra->ssid, (const char *)rb->ssid, sizeof(ra->ssid));
}

/* -------- Wi-Fi 事件回调 -------- */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // 不在这里自动 connect，交给 conn 命令显式触发
        ESP_LOGI(TAG_wifi, "STA_START");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG_wifi, "DISCONNECTED, reason=%d", dis ? dis->reason : -1);

        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG_wifi, "retry %d/%d", s_retry_num, WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true; 
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_wifi, "GOT_IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    
}

/* -------- Wi-Fi 初始化（只做一次） -------- */
esp_err_t wifi_init_once(void)
{
    if (s_wifi_inited) return ESP_OK;

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());

    // 如果你工程里别处已经 create_default 过，重复会报错
    // 这里做一个“已创建则忽略”的策略：直接尝试创建，失败就返回（通常是 ESP_ERR_INVALID_STATE）
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();


    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &s_instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &s_instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_inited = true;
    ESP_LOGI(TAG_wifi, "wifi_init_once done");
    return ESP_OK;
}
static const char *ip4_to_str(const esp_ip4_addr_t *ip, char out[16])
{
    // 16 bytes: "255.255.255.255\0"
    snprintf(out, 16, IPSTR, IP2STR(ip));
    return out;
}

static void wifi_print_info(void)
{
    // 1) 连接状态
    wifi_ap_record_t ap;
    esp_err_t err_ap = esp_wifi_sta_get_ap_info(&ap);

    if (!s_wifi_connected || err_ap != ESP_OK) {
        // 仍然输出一些基础信息
        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, mac);

        logi_both(TAG_wifi, "WiFi status: NOT CONNECTED");
        logi_both(TAG_wifi, "STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        if (err_ap != ESP_OK) {
            logi_both(TAG_wifi, "sta_get_ap_info failed: %s", esp_err_to_name(err_ap));
        }
        return;
    }

    // 2) 已连接：SSID/BSSID/CH/RSSI/Auth
    char bssid[18];
    bssid_to_str(ap.bssid, bssid);

    logi_both(TAG_wifi, "WiFi status: CONNECTED");
    logi_both(TAG_wifi, "SSID: %s", (char *)ap.ssid);
    logi_both(TAG_wifi, "BSSID: %s", bssid);
    logi_both(TAG_wifi, "Channel: %d", ap.primary);
    logi_both(TAG_wifi, "RSSI: %d dBm", ap.rssi);
    logi_both(TAG_wifi, "Auth: %s", authmode_str(ap.authmode));
    logi_both(TAG_wifi, "Pairwise: %s", cipher_str(ap.pairwise_cipher));
    logi_both(TAG_wifi, "Group: %s", cipher_str(ap.group_cipher));

    // 3) MAC
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    logi_both(TAG_wifi, "STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 4) IP / GW / NETMASK / DNS
    if (s_sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            char ip[16], gw[16], mask[16];
            ip4_to_str(&ip_info.ip, ip);
            ip4_to_str(&ip_info.gw, gw);
            ip4_to_str(&ip_info.netmask, mask);

            logi_both(TAG_wifi, "IP: %s", ip);
            logi_both(TAG_wifi, "GW: %s", gw);
            logi_both(TAG_wifi, "MASK: %s", mask);
        } else {
            logi_both(TAG_wifi, "get_ip_info failed");
        }

        esp_netif_dns_info_t dns;
        char dns_ip[16];

        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            ip4_to_str(&dns.ip.u_addr.ip4, dns_ip);
            logi_both(TAG_wifi, "DNS1: %s", dns_ip);
        }
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
            ip4_to_str(&dns.ip.u_addr.ip4, dns_ip);
            logi_both(TAG_wifi, "DNS2: %s", dns_ip);
        }
    } else {
        logi_both(TAG_wifi, "STA netif is NULL (init issue)");
    }
}
static bool time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return (now > 1577836800); // 2020-01-01
}

static void print_time_now(void)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    logi_both("time", "now: %s (UTC+8)", buf);
}

static void time_sync_init(void)
{
    // 台北/北京时间（UTC+8），不考虑夏令时
    setenv("TZ", "CST-8", 1);
    tzset();

    // 设置 SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    // 服务器 1（最常用）
    esp_sntp_setservername(0, "pool.ntp.org");
    // 也可以加备份：
    // esp_sntp_setservername(1, "time.google.com");

    esp_sntp_init();
}


/* -------- 扫描 + 排序 + 打印 -------- */
void wifi_scan_once_and_print_sorted(void)
{
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        logi_both(TAG_scan, "wifi init failed: %s", esp_err_to_name(err));
        return;
    }

    memset(g_ap_cache, 0, sizeof(g_ap_cache));
    g_ap_cache_num = 0;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    ESP_LOGI(TAG_scan, "Start scan...");
    err = esp_wifi_scan_start(&scan_cfg, true); // 阻塞直到扫描结束
    if (err != ESP_OK) {
        logi_both(TAG_scan, "scan start failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_LOGI(TAG_scan, "Total APs scanned = %u", ap_count);

    uint16_t number = DEFAULT_SCAN_LIST_SIZE;
    if (number > ap_count) number = ap_count;

    wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
    memset(ap_info, 0, sizeof(ap_info));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));

    qsort(ap_info, number, sizeof(wifi_ap_record_t), cmp_ap_rssi_desc);

    g_ap_cache_num = number;
    if (number > 0) {
        memcpy(g_ap_cache, ap_info, number * sizeof(wifi_ap_record_t));
    }

    logi_both(TAG_scan, "-------- WIFI SCAN RESULT (sorted by RSSI) --------------");
    logi_both(TAG_scan, "%-3s %-32s %-3s %-10s %-9s %-9s %-17s %-5s",
              "No", "SSID", "CH", "AUTH", "PAIR", "GROUP", "BSSID", "RSSI");
    logi_both(TAG_scan, "------------------------------------------------------------------------------------------------------");

    for (int i = 0; i < number; i++) {
        char bssid[18];
        bssid_to_str(ap_info[i].bssid, bssid);

        logi_both(TAG_scan, "%-3d %-32s %-3d %-10s %-9s %-9s %-17s %-5d",
                  i + 1,
                  (char *)ap_info[i].ssid,
                  ap_info[i].primary,
                  authmode_str(ap_info[i].authmode),
                  cipher_str(ap_info[i].pairwise_cipher),
                  cipher_str(ap_info[i].group_cipher),
                  bssid,
                  ap_info[i].rssi);
    }

    logi_both(TAG_scan, "------------------------------------------------------------------------------------------------------");
}

/* -------- 根据 scan 索引发起连接 -------- */
esp_err_t wifi_connect_by_index(int idx_1based, const char *psw_opt)
{
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) return err;

    if (g_ap_cache_num == 0) {
        logi_both(TAG_scan, "No scan cache. Please run: scan");
        return ESP_FAIL;
    }
    if (idx_1based < 1 || idx_1based > (int)g_ap_cache_num) {
        logi_both(TAG_scan, "Index out of range. Valid: 1..%u", g_ap_cache_num);
        return ESP_ERR_INVALID_ARG;
    }

    const wifi_ap_record_t *ap = &g_ap_cache[idx_1based - 1];

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    // SSID
    memcpy(wifi_config.sta.ssid, ap->ssid, sizeof(wifi_config.sta.ssid));
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';

    // Password
    if (psw_opt && psw_opt[0]) {
        strncpy((char *)wifi_config.sta.password, psw_opt, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    } else {
        wifi_config.sta.password[0] = '\0';
    }

    // 用扫描到的 authmode 做阈值
    wifi_config.sta.threshold.authmode = ap->authmode;

    // 锁定 BSSID：避免同名网络连错
    wifi_config.sta.bssid_set = 1;
    memcpy(wifi_config.sta.bssid, ap->bssid, 6);

    // 清事件位
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    logi_both(TAG_wifi, "Target AP: ssid='%s' bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d auth=%s",
              (char *)ap->ssid,
              ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5],
              ap->primary, ap->rssi, authmode_str(ap->authmode));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // 先断开再连
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    // 这里短等待用于提示，不阻塞太久
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        logi_both(TAG_wifi, "Connected OK.");
        wifi_print_info();
        time_sync_init();
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        logi_both(TAG_wifi, "Connect failed.");
        return ESP_FAIL;
    } else {
        logi_both(TAG_wifi, "Connecting... (timeout, keep retry in background)");
        return ESP_OK;
    }
}

uint16_t wifi_get_scan_cache_count(void)
{
    return g_ap_cache_num;
}

/* -------- cmd task -------- */
static void cmd_task(void *arg)
{
    (void)arg;

    QueueHandle_t q = uart_app_get_cmd_queue();
    uart_cmd_msg_t msg;

    const char *hello =
        "\r\n==== UART CMD READY ====\r\n"
        "cmd list:\r\n"
        "  scan                  - wifi scan\r\n"
        "  conn <i> <psw>        - connect by index\r\n"
        "  conn <i>              - connect open AP\r\n"
        "  info                  - show current wifi info\r\n"
        "  time                  - show time\r\n"
        "  help                  - show help\r\n"
        "========================\r\n";
    uart_app_write(hello, strlen(hello));

    while (1) {
        if (xQueueReceive(q, &msg, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG_CMD, "CMD: %s", msg.line);

            uart_app_write(">", 1);
            uart_app_write(msg.line, strlen(msg.line));
            uart_app_write("\r\n", 2);

            if      (strcmp(msg.line, "scan") == 0) {
                uart_app_write("Scanning...\r\n", strlen("Scanning...\r\n"));
                wifi_scan_once_and_print_sorted();
                uart_app_write("Scan done\r\n", strlen("Scan done\r\n"));
            }
            else if (strncmp(msg.line, "conn", 4) == 0) {
                int idx = 0;
                char psw[65] = {0}; // WPA2 password max 63
                int n = sscanf(msg.line, "conn %d %64s", &idx, psw);

                if (n <= 0) {
                    uart_app_write("Usage: conn <index> <psw>\r\n", strlen("Usage: conn <index> <psw>\r\n"));
                    uart_app_write("   or: conn <index>\r\n", strlen("   or: conn <index>\r\n"));
                } else if (n == 1) {
                    esp_err_t err = wifi_connect_by_index(idx, NULL);
                    if (err != ESP_OK) logi_both(TAG_wifi, "conn failed: %s", esp_err_to_name(err));
                } else {
                    esp_err_t err = wifi_connect_by_index(idx, psw);
                    if (err != ESP_OK) logi_both(TAG_wifi, "conn failed: %s", esp_err_to_name(err));
                }
            }
            else if (strcmp(msg.line, "time") == 0) {


            if (!time_is_valid()) {
                logi_both("time", "SNTP timeout, not synced yet.");
            } else {
                print_time_now();
            }
        }
            else if (strcmp(msg.line, "help") == 0) {
                const char *h =
                    "scan                - wifi scan\r\n"
                    "conn <i> <psw>      - connect to AP by index\r\n"
                    "conn <i>            - connect open AP\r\n"
                    "info                - show current wifi info\r\n"
                    "time                - show time\r\n"
                    "help                - show help\r\n";
                uart_app_write(h, strlen(h));
            }
            else if (strcmp(msg.line, "info") == 0) {
                wifi_print_info();
            }

            else {
                uart_app_write("Unknown cmd\r\n", strlen("Unknown cmd\r\n"));
            }
        }
    }
}

/* -------- bg scan task -------- */
static void wifi_bg_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_scan_once_and_print_sorted();
    vTaskDelete(NULL);
}

esp_err_t wifi_start_bg_scan_task(const char *task_name, uint32_t stack_words, UBaseType_t prio)
{
    if (!task_name) task_name = "wifi_bg_task";
    if (stack_words == 0) stack_words = 8192;
    if (prio == 0) prio = 9;

    BaseType_t ok = xTaskCreate(wifi_bg_task, task_name, stack_words, NULL, prio, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_start_cmd_task(const char *task_name, uint32_t stack_words, UBaseType_t prio)
{
    if (!task_name) task_name = "cmd_task";
    if (stack_words == 0) stack_words = 8192;
    if (prio == 0) prio = 10;

    BaseType_t ok = xTaskCreate(cmd_task, task_name, stack_words, NULL, prio, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

#include "time_sync.h"

#include <uart.h>

#include <stdlib.h>

#include "esp_log.h"
#include "esp_sntp.h"

#include "user_type.h"

#ifndef TAG_TIME
#define TAG_TIME "time"
#endif

/* -------------------------- SNTP time -------------------------- */

bool time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return (now > 1577836800); // 2020-01-01 00:00:00 UTC
}

void print_time_now(void)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    logi_both(TAG_TIME, "now: %s (UTC+8)", buf);
}

void time_sync_init(void)
{
    // UTC+8: 传统写法 CST-8 表示 UTC+8（注意 POSIX 时区字符串的符号是“反的”）
    setenv("TZ", "CST-8", 1);
    tzset();

    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG_TIME, "SNTP already running, skip init.");
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    ESP_LOGI(TAG_TIME, "SNTP init done.");
}
//------------lvgl_api------------
esp_err_t lv_get_current_time_info(struct tm* tm_info){

    time_t now;
    time(&now);
    localtime_r(&now, tm_info);
    return ESP_OK;
}
esp_err_t lv_tm_to_lv_time_info(const struct tm* tm_info, lv_time_info* time_info){
    if(tm_info == NULL || time_info == NULL){
        return ESP_ERR_INVALID_ARG;
    }
    time_info->year = tm_info->tm_year + 1900;
    time_info->month = tm_info->tm_mon + 1;
    time_info->day = tm_info->tm_mday;
    time_info->hour = tm_info->tm_hour;
    time_info->min = tm_info->tm_min;
    time_info->sec = tm_info->tm_sec;
    return ESP_OK;
}
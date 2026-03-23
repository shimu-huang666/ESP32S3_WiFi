/*
 * LVGL API - Helper functions for LVGL integration
 */

#include "lvgl_api.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "lv_api";

esp_err_t lv_get_current_time_info(struct tm* tm_info)
{
    if (tm_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now;
    time(&now);
    localtime_r(&now, tm_info);

    return ESP_OK;
}

esp_err_t lv_tm_to_lv_time_info(const struct tm* tm_info, lv_time_info_t* time_info)
{
    if (tm_info == NULL || time_info == NULL) {
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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL time info structure
typedef struct {
    int16_t year;
    int8_t month;
    int8_t day;
    int8_t hour;
    int8_t min;
    int8_t sec;
} lv_time_info_t;

/**
 * @brief Get current time info from system
 * @param tm_info Pointer to struct tm to store result
 * @return ESP_OK on success
 */
esp_err_t lv_get_current_time_info(struct tm* tm_info);

/**
 * @brief Convert struct tm to lv_time_info_t
 * @param tm_info Source tm structure
 * @param time_info Destination lv_time_info structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if params are NULL
 */
esp_err_t lv_tm_to_lv_time_info(const struct tm* tm_info, lv_time_info_t* time_info);

#ifdef __cplusplus
}
#endif

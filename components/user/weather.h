#pragma once

#ifdef __cplusplus
extern “C” {
#endif

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define WEATHER_CITY_MAX_LEN  64

/**
 * @brief 启动独立的 weather 任务（内部使用大缓冲区，不占用 cmd 任务栈）
 * @param stack_words 任务栈大小（words，非 bytes），建议 >= 4096
 * @return ESP_OK 成功
 */
esp_err_t weather_start_task(uint32_t stack_words, UBaseType_t prio);

/**
 * @brief 向 weather 任务投递一次”获取并打印天气”请求（自动定位），立即返回
 */
void weather_request(void);

/**
 * @brief 向 weather 任务投递一次”获取指定城市天气”请求，立即返回
 * @param city 城市名（如 “Beijing”, “Shanghai”, “New York”）
 */
void weather_request_city(const char *city);

#ifdef __cplusplus
}
#endif

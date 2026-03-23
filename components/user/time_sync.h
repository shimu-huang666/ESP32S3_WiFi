#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief 判断当前系统时间是否已经被校准到一个"可信时间"
 * @return true: 时间有效(>=2020-01-01)；false: 仍是默认/未同步
 */
bool time_is_valid(void);

/**
 * @brief 打印当前时间（按 TZ 配置输出）
 * @note 需要你工程中提供 logi_both(TAG, fmt, ...)
 */
void print_time_now(void);

/**
 * @brief 初始化 SNTP，并设置时区
 * @note 若 SNTP 已启用会跳过重复初始化
 */
void time_sync_init(void);

#ifdef __cplusplus
}
#endif

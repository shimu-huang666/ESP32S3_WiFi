#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"

/**
 * @brief 初始化 Wi-Fi（只会执行一次）
 */
esp_err_t wifi_init_once(void);

/**
 * @brief 扫描一次 + 按 RSSI 从强到弱排序 + 打印结果，并更新内部缓存（索引顺序=打印顺序）
 */
void wifi_scan_once_and_print_sorted(void);

/**
 * @brief 根据 scan 的 1-based 索引连接（conn <index> <psw>）
 * @param idx_1based  1..N
 * @param psw_opt     NULL 或 "" 表示空密码（用于开放 AP）
 */
esp_err_t wifi_connect_by_index(int idx_1based, const char *psw_opt);

/**
 * @brief 返回当前扫描缓存条目数
 */
uint16_t wifi_get_scan_cache_count(void);

/**
 * @brief 启动一个后台扫描任务（可选）
 *        等价于你原来的 wifi_bg_task：delay 一下然后扫一次
 */
esp_err_t wifi_start_bg_scan_task(const char *task_name, uint32_t stack_words, UBaseType_t prio);

/**
 * @brief 启动 UART 命令任务（可选）：支持 scan/conn/help
 */
esp_err_t wifi_start_cmd_task(const char *task_name, uint32_t stack_words, UBaseType_t prio);

#ifdef __cplusplus
}
#endif

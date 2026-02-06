#pragma once
/*
 * cmd.h - UART command task API
 *
 * This module provides:
 *  - print_help(): print command list to UART
 *  - cmd_task():   FreeRTOS task entry (usually started via start_cmd_task)
 *  - start_cmd_task(): create command task
 *
 * Notes:
 *  - This header assumes you are using ESP-IDF + FreeRTOS.
 */

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"   // for UBaseType_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Print all supported commands to UART.
 */
void print_help(void);

/**
 * @brief Command task entry. Normally you should not call this directly.
 *        Use start_cmd_task() to create the task.
 *
 * @param arg Unused.
 */
void cmd_task(void *arg);

/**
 * @brief Start the command task.
 *
 * @param task_name   Task name. If NULL, default "cmd_task".
 * @param stack_words Task stack size in *words* (not bytes). If 0, default 8192.
 * @param prio        Task priority. If 0, default 10.
 *
 * @return ESP_OK if task created; ESP_FAIL otherwise.
 */
esp_err_t start_cmd_task(const char *task_name, uint32_t stack_words, UBaseType_t prio);

#ifdef __cplusplus
}
#endif

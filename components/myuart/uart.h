#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_err.h"

/* ========================= 用户可配置区 =========================
   建议：
   - UART0 留给日志/下载/monitor
   - UART1 或 UART2 做用户命令口
*/
#ifndef UART_APP_PORT
#define UART_APP_PORT        UART_NUM_1
#endif

#ifndef UART_APP_TX_PIN
#define UART_APP_TX_PIN      GPIO_NUM_43
#endif

#ifndef UART_APP_RX_PIN
#define UART_APP_RX_PIN      GPIO_NUM_44
#endif

#ifndef UART_APP_BAUDRATE
#define UART_APP_BAUDRATE    115200
#endif

#ifndef UART_APP_BUF_SIZE
#define UART_APP_BUF_SIZE    1024
#endif

// 命令行最大长度（含 '\0'）
#ifndef UART_CMD_MAX_LEN
#define UART_CMD_MAX_LEN     64
#endif

// 命令队列长度
#ifndef UART_CMD_QUEUE_LEN
#define UART_CMD_QUEUE_LEN   8
#endif

// 是否在 UART 事件任务中回显（强烈建议默认关闭，避免回环）
#ifndef UART_ECHO_IN_TASK
#define UART_ECHO_IN_TASK    0
#endif
/* =============================================================== */

// 一条命令消息（以 '\0' 结尾）
typedef struct {
    char line[UART_CMD_MAX_LEN];
} uart_cmd_msg_t;

/**
 * @brief 初始化 UART（安装驱动、创建事件任务、创建命令队列）
 */
esp_err_t uart_app_init(void);

/**
 * @brief 反初始化 UART（删除任务、删除驱动、删除队列）
 */
esp_err_t uart_app_deinit(void);

/**
 * @brief 获取“按行命令”队列句柄（cmd_task 用它 xQueueReceive）
 * @return QueueHandle_t  若未初始化则返回 NULL
 */
QueueHandle_t uart_app_get_cmd_queue(void);

/**
 * @brief 写 UART（线程安全由 driver 保证）
 * @return 实际写入字节数（<0 表示失败）
 */
int uart_app_write(const void *data, size_t len);

/**
 * @brief 读 UART 原始字节（一般不需要，除非你要自己处理二进制协议）
 * @return 实际读取字节数（<0 表示失败）
 */
int uart_app_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait);

/**
 * @brief 立即清空 UART RX 缓冲（用于异常恢复）
 */
void uart_app_flush_rx(void);

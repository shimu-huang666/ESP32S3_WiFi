#include "uart.h"

// ============ 内部参数 ============
#define UART_EVT_TASK_STACK     4096
#define UART_EVT_TASK_PRIO      12
#define UART_EVT_QUEUE_LEN      20

static const char *TAG = "uart_app";

static QueueHandle_t s_uart_evt_queue = NULL;
static QueueHandle_t s_cmd_queue      = NULL;
static TaskHandle_t  s_evt_task        = NULL;
static int s_inited = 0;

// 行缓冲（跨 UART_DATA 事件累计）
static char s_line_buf[UART_CMD_MAX_LEN];
static int  s_line_len = 0;

static void push_line_to_cmd_queue(void)
{
    if (!s_cmd_queue) return;
    if (s_line_len <= 0) return;

    uart_cmd_msg_t msg;
    int n = s_line_len;

    if (n > UART_CMD_MAX_LEN - 1) n = UART_CMD_MAX_LEN - 1;
    memcpy(msg.line, s_line_buf, n);
    msg.line[n] = '\0';

    // 清空行缓冲
    s_line_len = 0;

    // 空白行丢弃
    if (msg.line[0] == '\0') return;

    // 队列满：这里选择“丢弃新命令”，避免阻塞 UART 事件任务
    (void)xQueueSend(s_cmd_queue, &msg, 0);
}

static void uart_event_task(void *arg)
{
    (void)arg;

    uart_event_t event;
    uint8_t *tmp = (uint8_t *)malloc(UART_APP_BUF_SIZE);
    if (!tmp) {
        ESP_LOGE(TAG, "malloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(s_uart_evt_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UART_DATA: {
            int to_read = event.size;
            if (to_read > UART_APP_BUF_SIZE) to_read = UART_APP_BUF_SIZE;

            int rlen = uart_read_bytes(UART_APP_PORT, tmp, to_read, portMAX_DELAY);
            if (rlen <= 0) break;

#if UART_ECHO_IN_TASK
            // ⚠️谨慎开启：如果 TX/RX 回环或同口监视，可能自激
            uart_write_bytes(UART_APP_PORT, (const char *)tmp, rlen);
#endif

            // 逐字节喂入“行组包器”
            for (int i = 0; i < rlen; i++) {
                char c = (char)tmp[i];

                // 兼容 CRLF：忽略 '\r'
                if (c == '\r') continue;

                // '\n' 视为一条命令结束
                if (c == '\n') {
                    // 可选：去掉末尾空格（你需要的话再开）
                    // while (s_line_len > 0 && (s_line_buf[s_line_len-1] == ' ' || s_line_buf[s_line_len-1] == '\t'))
                    //     s_line_len--;

                    push_line_to_cmd_queue();
                    continue;
                }

                // 普通字符入行缓冲（超长则丢弃本行剩余字符，直到遇到 '\n'）
                if (s_line_len < UART_CMD_MAX_LEN - 1) {
                    s_line_buf[s_line_len++] = c;
                } else {
                    // 超长：不再接收本行字符，等待 '\n' 来结束本行
                }
            }
            break;
        }

        case UART_FIFO_OVF:
            ESP_LOGW(TAG, "hw fifo overflow");
            uart_flush_input(UART_APP_PORT);
            xQueueReset(s_uart_evt_queue);
            s_line_len = 0;
            break;

        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "ring buffer full");
            uart_flush_input(UART_APP_PORT);
            xQueueReset(s_uart_evt_queue);
            s_line_len = 0;
            break;

        case UART_PARITY_ERR:
            ESP_LOGW(TAG, "parity error");
            break;

        case UART_FRAME_ERR:
            ESP_LOGW(TAG, "frame error");
            break;

        default:
            // 其它事件通常不需要处理
            break;
        }
    }

    // 正常不会到这里
    free(tmp);
    vTaskDelete(NULL);
}

esp_err_t uart_app_init(void)
{
    if (s_inited) return ESP_ERR_INVALID_STATE;

    esp_log_level_set(TAG, ESP_LOG_INFO);

    const uart_config_t cfg = {
        .baud_rate  = UART_APP_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 1) 安装 UART 驱动并创建事件队列
    esp_err_t err = uart_driver_install(
        UART_APP_PORT,
        UART_APP_BUF_SIZE * 2,   // rx ring buffer
        0,                       // tx ring buffer：0=不建（你也可以改成 UART_APP_BUF_SIZE*2）
        UART_EVT_QUEUE_LEN,
        &s_uart_evt_queue,
        0
    );
    if (err != ESP_OK) return err;

    // 2) 参数配置
    err = uart_param_config(UART_APP_PORT, &cfg);
    if (err != ESP_OK) goto fail;

    // 3) 配置引脚
    err = uart_set_pin(UART_APP_PORT, UART_APP_TX_PIN, UART_APP_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) goto fail;

    // 4) 创建“命令队列”
    s_cmd_queue = xQueueCreate(UART_CMD_QUEUE_LEN, sizeof(uart_cmd_msg_t));
    if (!s_cmd_queue) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    // 5) 创建 UART 事件任务
    BaseType_t ok = xTaskCreate(uart_event_task, "uart_evt", UART_EVT_TASK_STACK,
                               NULL, UART_EVT_TASK_PRIO, &s_evt_task);
    if (ok != pdPASS) {
        err = ESP_FAIL;
        goto fail;
    }

    s_line_len = 0;
    s_inited = 1;
    ESP_LOGI(TAG, "init ok: port=%d tx=%d rx=%d baud=%d",
             (int)UART_APP_PORT, (int)UART_APP_TX_PIN, (int)UART_APP_RX_PIN, UART_APP_BAUDRATE);
    return ESP_OK;

fail:
    if (s_evt_task) {
        vTaskDelete(s_evt_task);
        s_evt_task = NULL;
    }
    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    uart_driver_delete(UART_APP_PORT);
    s_uart_evt_queue = NULL;
    s_line_len = 0;
    s_inited = 0;
    return err;
}

esp_err_t uart_app_deinit(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (s_evt_task) {
        vTaskDelete(s_evt_task);
        s_evt_task = NULL;
    }

    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }

    uart_driver_delete(UART_APP_PORT);
    s_uart_evt_queue = NULL;

    s_line_len = 0;
    s_inited = 0;
    return ESP_OK;
}

QueueHandle_t uart_app_get_cmd_queue(void)
{
    return s_cmd_queue;
}

int uart_app_write(const void *data, size_t len)
{
    if (!s_inited || !data || len == 0) return -1;
    return uart_write_bytes(UART_APP_PORT, (const char *)data, len);
}

int uart_app_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait)
{
    if (!s_inited || !buf || len == 0) return -1;
    return uart_read_bytes(UART_APP_PORT, buf, len, ticks_to_wait);
}
void logi_both(const char *tag, const char *fmt, ...)
{
    char buf[192];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) return;

    ESP_LOGI(tag, "%s", buf);
    uart_app_write(buf, strnlen(buf, sizeof(buf)));
    uart_app_write("\r\n", 2);
}
void uart_app_flush_rx(void)
{
    if (!s_inited) return;
    uart_flush_input(UART_APP_PORT);
    s_line_len = 0;
}

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define BT_SCAN_MAX_DEVICES 20

// Bluetooth device info list
typedef struct {
    int8_t size;                          // actual count
    char *name_list[BT_SCAN_MAX_DEVICES]; // device names (dynamically allocated)
    char mac_list[BT_SCAN_MAX_DEVICES][18]; // MAC addresses (XX:XX:XX:XX:XX:XX format)
    int8_t rssi_list[BT_SCAN_MAX_DEVICES];// signal strength
} device_info_list_t;

/**
 * @brief Initialize Bluetooth (BLE) controller and host
 * @return ESP_OK on success
 */
esp_err_t bluetooth_init(void);

/**
 * @brief Start a BLE scan and wait for completion
 * @param duration_sec Scan duration in seconds (0 = use default 5s)
 * @return ESP_OK on success
 */
esp_err_t bluetooth_scan_start(uint32_t duration_sec);

/**
 * @brief Get the scanned device list
 * @return Pointer to device_info_list_t (read-only, do not free)
 */
const device_info_list_t *bluetooth_get_device_list(void);

/**
 * @brief Print scanned device list to UART and log
 */
void bluetooth_print_device_list(void);

/**
 * @brief Free device list memory (names are dynamically allocated)
 */
void bluetooth_free_device_list(void);

/**
 * @brief Deinitialize Bluetooth
 * @return ESP_OK on success
 */
esp_err_t bluetooth_deinit(void);

#ifdef __cplusplus
}
#endif

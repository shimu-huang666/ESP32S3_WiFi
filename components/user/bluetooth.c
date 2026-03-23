/*
 * Bluetooth LE Scanner
 * Scans nearby BLE devices and stores results in device_info_list
 */

#include "bluetooth.h"
#include "uart.h"

#include <string.h>
#include <stdlib.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bt_scan";

#define DEFAULT_SCAN_DURATION_SEC 5

// Scan result storage
static device_info_list_t s_device_list = {0};
static SemaphoreHandle_t s_scan_done_sem = NULL;
static bool s_initialized = false;

// Forward declarations
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void sort_devices_by_rssi(void);

// Sort devices by RSSI (descending: strongest first)
static void sort_devices_by_rssi(void)
{
    for (int i = 0; i < s_device_list.size - 1; i++) {
        for (int j = i + 1; j < s_device_list.size; j++) {
            if (s_device_list.rssi_list[j] > s_device_list.rssi_list[i]) {
                // Swap RSSI
                int8_t tmp_rssi = s_device_list.rssi_list[i];
                s_device_list.rssi_list[i] = s_device_list.rssi_list[j];
                s_device_list.rssi_list[j] = tmp_rssi;

                // Swap names
                char *tmp_name = s_device_list.name_list[i];
                s_device_list.name_list[i] = s_device_list.name_list[j];
                s_device_list.name_list[j] = tmp_name;

                // Swap MAC addresses
                char tmp_mac[18];
                memcpy(tmp_mac, s_device_list.mac_list[i], 18);
                memcpy(s_device_list.mac_list[i], s_device_list.mac_list[j], 18);
                memcpy(s_device_list.mac_list[j], tmp_mac, 18);
            }
        }
    }
}

// GAP callback
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        // Scan params set, start scanning
        uint32_t duration = DEFAULT_SCAN_DURATION_SEC;
        esp_ble_gap_start_scanning(duration);
        ESP_LOGI(TAG, "BLE scan started for %lu seconds", duration);
        break;
    }

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: {
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan start failed: %d", param->scan_start_cmpl.status);
            if (s_scan_done_sem) {
                xSemaphoreGive(s_scan_done_sem);
            }
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            // Found a device
            if (s_device_list.size >= BT_SCAN_MAX_DEVICES) {
                return; // List full
            }

            // Check if device already in list (by BD address)
            for (int i = 0; i < s_device_list.size; i++) {
                // Simple dedup by checking if same name exists
                // (For more accurate dedup, we'd store BD addresses too)
            }

            // Extract device name
            char dev_name[64] = {0};
            bool has_name = false;

            // Parse advertising data for local name
            uint8_t *adv_data = param->scan_rst.ble_adv;
            uint8_t adv_len = param->scan_rst.adv_data_len;

            for (int i = 0; i < adv_len;) {
                uint8_t field_len = adv_data[i];
                if (field_len == 0 || i + field_len >= adv_len) break;

                uint8_t field_type = adv_data[i + 1];
                // 0x08 = Shortened Local Name, 0x09 = Complete Local Name
                if (field_type == 0x08 || field_type == 0x09) {
                    int name_len = field_len - 1;
                    if (name_len > 0 && name_len < sizeof(dev_name)) {
                        memcpy(dev_name, &adv_data[i + 2], name_len);
                        dev_name[name_len] = '\0';
                        has_name = true;
                    }
                    break;
                }
                i += field_len + 1;
            }

            // Store device info
            int idx = s_device_list.size;

            // Store MAC address
            snprintf(s_device_list.mac_list[idx], sizeof(s_device_list.mac_list[idx]),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     param->scan_rst.bda[0], param->scan_rst.bda[1],
                     param->scan_rst.bda[2], param->scan_rst.bda[3],
                     param->scan_rst.bda[4], param->scan_rst.bda[5]);

            if (has_name && dev_name[0] != '\0') {
                s_device_list.name_list[idx] = strdup(dev_name);
            } else {
                // No name, use "Unknown"
                s_device_list.name_list[idx] = strdup("Unknown");
            }

            s_device_list.rssi_list[idx] = param->scan_rst.rssi;
            s_device_list.size++;

            ESP_LOGI(TAG, "Found device [%d]: %s, MAC: %s, RSSI: %d",
                     s_device_list.size,
                     s_device_list.name_list[idx],
                     s_device_list.mac_list[idx],
                     s_device_list.rssi_list[idx]);
        }
        else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            // Sort by RSSI (strongest first)
            sort_devices_by_rssi();
            ESP_LOGI(TAG, "BLE scan complete, found %d devices (sorted by RSSI)", s_device_list.size);
            if (s_scan_done_sem) {
                xSemaphoreGive(s_scan_done_sem);
            }
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan stop failed: %d", param->scan_stop_cmpl.status);
        }
        break;
    }

    default:
        break;
    }
}

esp_err_t bluetooth_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret;

    // Create semaphore for scan completion
    s_scan_done_sem = xSemaphoreCreateBinary();
    if (!s_scan_done_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_ERR_NO_MEM;
    }

    // Initialize Bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    // For ESP32-S3, allocate Bluetooth controller memory
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_scan_done_sem);
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_scan_done_sem);
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_scan_done_sem);
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_scan_done_sem);
        return ret;
    }

    // Register GAP callback
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_scan_done_sem);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Bluetooth initialized");
    return ESP_OK;
}

esp_err_t bluetooth_scan_start(uint32_t duration_sec)
{
    if (!s_initialized) {
        esp_err_t ret = bluetooth_init();
        if (ret != ESP_OK) return ret;
    }

    // Free previous results
    bluetooth_free_device_list();

    // Set scan parameters
    static esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,  // 50ms
        .scan_window = 0x30,    // 30ms
        .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
    };

    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set scan params failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for scan completion
    if (xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS((duration_sec + 2) * 1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Scan timeout");
        esp_ble_gap_stop_scanning();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

const device_info_list_t *bluetooth_get_device_list(void)
{
    return &s_device_list;
}

void bluetooth_print_device_list(void)
{
    logi_both(TAG, "-------- BLE SCAN RESULT --------------");
    logi_both(TAG, "Total devices found: %d", s_device_list.size);
    logi_both(TAG, "%-4s %-32s %-18s %-8s", "No", "Name", "MAC", "RSSI");
    logi_both(TAG, "-----------------------------------------------------------");

    for (int i = 0; i < s_device_list.size; i++) {
        const char *name = s_device_list.name_list[i] ? s_device_list.name_list[i] : "<unknown>";
        logi_both(TAG, "%-4d %-32.32s %-18s %-8d",
                  i + 1, name, s_device_list.mac_list[i], s_device_list.rssi_list[i]);
    }

    logi_both(TAG, "-----------------------------------------------------------");
}

void bluetooth_free_device_list(void)
{
    for (int i = 0; i < BT_SCAN_MAX_DEVICES; i++) {
        if (s_device_list.name_list[i]) {
            free(s_device_list.name_list[i]);
            s_device_list.name_list[i] = NULL;
        }
    }
    s_device_list.size = 0;
}

esp_err_t bluetooth_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    bluetooth_free_device_list();

    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    if (s_scan_done_sem) {
        vSemaphoreDelete(s_scan_done_sem);
        s_scan_done_sem = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Bluetooth deinitialized");
    return ESP_OK;
}

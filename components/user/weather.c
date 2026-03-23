/*
 * weather.c - 根据当前 WiFi 出口 IP 获取地理位置，再请求该位置天气并输出
 * 使用 ip-api.com 获取地址（无需 key），Open-Meteo 获取天气（无需 key）
 * 在独立任务中执行，大缓冲区在 weather 任务栈上，不占用 cmd 任务
 * 支持手动指定城市名
 */

#include "weather.h"
#include "uart.h"
#include "wifi.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "freertos/task.h"

#define WEATHER_RECV_BUF_SIZE  2048
static char s_recv_buf[WEATHER_RECV_BUF_SIZE];
#define WEATHER_TAG            "weather"
#define WEATHER_QUEUE_LEN      4

/* 天气请求消息结构 */
typedef struct {
    char city[WEATHER_CITY_MAX_LEN];  /* 空字符串表示自动定位 */
} weather_req_t;

static QueueHandle_t s_weather_queue = NULL;

/* 从 JSON 字符串中简单提取 "key":"value" 的 value（字符串），out 需足够大，返回长度 */
static int json_get_string(const char *json, const char *key, char *out, int out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    const char *end = strchr(p, '"');
    if (!end) return -1;
    int len = (int)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    return len;
}

/* 从 JSON 提取 "key":number */
static int json_get_double(const char *json, const char *key, double *val)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    return (sscanf(p, "%lf", val) == 1) ? 0 : -1;
}

static int json_get_int(const char *json, const char *key, int *val)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    return (sscanf(p, "%d", val) == 1) ? 0 : -1;
}

/* WMO 天气现象代码 -> 简短描述（中文） */
static const char *wmo_weather_desc(int code)
{
    if (code == 0) return "Clear sky";
    if (code == 1) return "Mainly clear";
    if (code == 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 67) return "Drizzle or rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Rain showers";
    if (code >= 85 && code <= 86) return "Snow showers";
    if (code >= 95 && code <= 99) return "Thunderstorm";
    return "Unknown";
}

static esp_err_t http_get_to_buffer(const char *url, char *buf, size_t buf_size, size_t *out_len)
{
    *out_len = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .buffer_size = 512,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);

    size_t total = 0;
    while (1) {
        int r = esp_http_client_read(client, buf + total, buf_size - 1 - total);
        if (r < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
        if (total >= buf_size - 1) {
            break;
        }
    }

    buf[total] = '\0';
    *out_len = total;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}
/**
 * @brief 执行一次获取并打印（使用调用者提供缓冲区，供 weather 任务调用）
 * @param city 城市名，如果为空则使用 IP 自动定位
 */
static esp_err_t do_fetch_and_print(char *recv_buf, size_t buf_size, const char *city)
{
    if (!wifi_is_connected()) {
        uart_app_write("weather: WiFi not connected\r\n", 30);
        return ESP_ERR_INVALID_STATE;
    }

    size_t len = 0;
    esp_err_t err;
    double lat = 0, lon = 0;
    char country[64] = {0}, region[64] = {0}, city_name[64] = {0};
    char district[64] = {0}, zip[32] = {0}, isp[96] = {0}, query[48] = {0};
    char line[192];

    /* 1) 获取经纬度：手动指定城市 或 IP自动定位 */
    if (city && city[0] != '\0') {
        /* 使用 Open-Meteo 地理编码 API */
        char geo_url[256];
        snprintf(geo_url, sizeof(geo_url),
                 "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json",
                 city);

        err = http_get_to_buffer(geo_url, recv_buf, buf_size, &len);
        if (err != ESP_OK) {
            ESP_LOGE(WEATHER_TAG, "geocoding request failed: %s", esp_err_to_name(err));
            uart_app_write("weather: city lookup failed\r\n", 30);
            return err;
        }

        /* 解析地理编码结果 */
        const char *results = strstr(recv_buf, "\"results\":[");
        if (!results) {
            uart_app_write("weather: city not found\r\n", 25);
            return ESP_ERR_NOT_FOUND;
        }

        json_get_double(results, "latitude", &lat);
        json_get_double(results, "longitude", &lon);
        json_get_string(results, "name", city_name, sizeof(city_name));
        json_get_string(results, "country", country, sizeof(country));
        json_get_string(results, "admin1", region, sizeof(region));

        if (lat == 0 && lon == 0) {
            uart_app_write("weather: invalid coordinates\r\n", 30);
            return ESP_ERR_NOT_FOUND;
        }

        snprintf(line, sizeof(line), "City found: %s, %s\r\n",
                 city_name[0] ? city_name : city,
                 country[0] ? country : "");
        uart_app_write(line, strlen(line));
    } else {
        /* 使用 IP 自动定位 */
        const char *geo_url = "http://ip-api.com/json/?lang=en&fields=status,message,country,regionName,city,district,zip,lat,lon,isp,org,query";
        err = http_get_to_buffer(geo_url, recv_buf, buf_size, &len);
        if (err != ESP_OK) {
            ESP_LOGE(WEATHER_TAG, "geo request failed: %s", esp_err_to_name(err));
            uart_app_write("weather: geo request failed\r\n", 30);
            return err;
        }

        char status[16] = {0};
        json_get_string(recv_buf, "status", status, sizeof(status));
        if (strcmp(status, "success") != 0) {
            char msg[64] = {0};
            json_get_string(recv_buf, "message", msg, sizeof(msg));
            snprintf(line, sizeof(line), "weather: geo failed: %s\r\n", msg[0] ? msg : "unknown");
            uart_app_write(line, strlen(line));
            return ESP_ERR_INVALID_RESPONSE;
        }

        json_get_string(recv_buf, "country", country, sizeof(country));
        json_get_string(recv_buf, "regionName", region, sizeof(region));
        json_get_string(recv_buf, "city", city_name, sizeof(city_name));
        json_get_string(recv_buf, "district", district, sizeof(district));
        json_get_string(recv_buf, "zip", zip, sizeof(zip));
        json_get_string(recv_buf, "isp", isp, sizeof(isp));
        json_get_string(recv_buf, "query", query, sizeof(query));
        json_get_double(recv_buf, "lat", &lat);
        json_get_double(recv_buf, "lon", &lon);
    }

    /* 2) 用经纬度请求 Open-Meteo 当前天气 */
    char meteo_url[256];
    snprintf(meteo_url, sizeof(meteo_url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m",
             lat, lon);

    err = http_get_to_buffer(meteo_url, recv_buf, buf_size, &len);
    if (err != ESP_OK) {
        ESP_LOGE(WEATHER_TAG, "weather request failed: %s", esp_err_to_name(err));
        uart_app_write("weather: weather request failed\r\n", 34);
        return err;
    }

    double temp = 0;
    int humidity = 0, weather_code = 0;
    double wind_speed = 0;

    const char *cur = strstr(recv_buf, "\"current\":{");
    if (cur) {
        json_get_double(cur, "temperature_2m", &temp);
        json_get_int(cur, "relative_humidity_2m", &humidity);
        json_get_int(cur, "weather_code", &weather_code);
        json_get_double(cur, "wind_speed_10m", &wind_speed);
    }

    /* 3) 输出位置和天气信息 */
    uart_app_write("\r\n--- Location and Weather ---\r\n", strlen("\r\n--- Location and Weather ---\r\n"));

    if (query[0]) {
        snprintf(line, sizeof(line), "Public IP: %s\r\n", query);
        uart_app_write(line, strlen(line));
    }
    snprintf(line, sizeof(line), "Country: %s\r\n", country[0] ? country : "-");
    uart_app_write(line, strlen(line));
    snprintf(line, sizeof(line), "Region/State: %s\r\n", region[0] ? region : "-");
    uart_app_write(line, strlen(line));
    snprintf(line, sizeof(line), "City: %s\r\n", city_name[0] ? city_name : "-");
    uart_app_write(line, strlen(line));
    if (district[0]) {
        snprintf(line, sizeof(line), "District: %s\r\n", district);
        uart_app_write(line, strlen(line));
    }
    if (zip[0]) {
        snprintf(line, sizeof(line), "ZIP: %s\r\n", zip);
        uart_app_write(line, strlen(line));
    }

    snprintf(line, sizeof(line),
             "Lat: %d deg %d'%.2f\"  Lon: %d deg %d'%.2f\"\r\n",
             (int)lat, (int)((lat - (int)lat) * 60),
             ((lat - (int)lat) * 60 - (int)((lat - (int)lat) * 60)) * 60,
             (int)lon, (int)((lon - (int)lon) * 60),
             ((lon - (int)lon) * 60 - (int)((lon - (int)lon) * 60)) * 60);
    uart_app_write(line, strlen(line));

    if (isp[0]) {
        snprintf(line, sizeof(line), "ISP: %s\r\n", isp);
        uart_app_write(line, strlen(line));
    }

    uart_app_write("\r\n--- Current Weather ---\r\n", strlen("\r\n--- Current Weather ---\r\n"));
    snprintf(line, sizeof(line), "Weather: %s (code %d)\r\n", wmo_weather_desc(weather_code), weather_code);
    uart_app_write(line, strlen(line));
    snprintf(line, sizeof(line), "Temperature: %.1f C\r\n", temp);
    uart_app_write(line, strlen(line));
    snprintf(line, sizeof(line), "Humidity: %d %%\r\n", humidity);
    uart_app_write(line, strlen(line));
    snprintf(line, sizeof(line), "Wind speed: %.1f km/h\r\n", wind_speed);
    uart_app_write(line, strlen(line));
    uart_app_write("------------------------\r\n", strlen("------------------------\r\n"));

    return ESP_OK;
}

static void weather_task(void *arg)
{
    (void)arg;
    weather_req_t req;

    while (1) {
        if (xQueueReceive(s_weather_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        do_fetch_and_print(s_recv_buf, sizeof(s_recv_buf), req.city);
    }
}

esp_err_t weather_start_task(uint32_t stack_words, UBaseType_t prio)
{
    if (s_weather_queue != NULL) {
        return ESP_OK;
    }

    s_weather_queue = xQueueCreate(WEATHER_QUEUE_LEN, sizeof(weather_req_t));
    if (s_weather_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(weather_task, "weather", stack_words, NULL, prio, NULL) != pdPASS) {
        vQueueDelete(s_weather_queue);
        s_weather_queue = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void weather_request(void)
{
    if (s_weather_queue != NULL) {
        weather_req_t req = {.city = ""};
        xQueueSend(s_weather_queue, &req, 0);
    }
}

void weather_request_city(const char *city)
{
    if (s_weather_queue != NULL && city != NULL) {
        weather_req_t req;
        strncpy(req.city, city, WEATHER_CITY_MAX_LEN - 1);
        req.city[WEATHER_CITY_MAX_LEN - 1] = '\0';
        xQueueSend(s_weather_queue, &req, 0);
    }
}

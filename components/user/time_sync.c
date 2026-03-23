#include "time_sync.h"
#include "uart.h"
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_sntp.h"

#ifndef TAG_TIME
#define TAG_TIME "time"
#endif

/* -------------------------- SNTP time -------------------------- */

bool time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return (now > 1577836800); // 2020-01-01 00:00:00 UTC
}

void print_time_now(void)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    logi_both(TAG_TIME, "now: %s (UTC+8)", buf);
}

void time_sync_init(void)
{
    // UTC+8: CST-8 means UTC+8 (POSIX timezone sign is reversed)
    setenv("TZ", "CST-8", 1);
    tzset();

    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG_TIME, "SNTP already running, skip init.");
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    ESP_LOGI(TAG_TIME, "SNTP init done.");
}

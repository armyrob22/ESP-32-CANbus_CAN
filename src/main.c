// Kia CAN bridge: listen-only CAN capture streamed over a phone hotspot
// through WireGuard to server2.

#include "nvs_flash.h"
#include "esp_log.h"
#include "ring.h"
#include "net.h"
#include "can_capture.h"
#include "sender.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (!ring_init(CONFIG_KCAN_RING_RECORDS)) {
        ESP_LOGE(TAG, "halting: no outage buffer");
        return;
    }

    net_start();
    sender_start();

    // Capture starts right away. Frames seen before SNTP syncs carry
    // uptime timestamps without KCAN_RF_TIME_VALID set.
    if (!can_capture_start()) {
        ESP_LOGE(TAG, "CAN capture failed to start");
    }
}

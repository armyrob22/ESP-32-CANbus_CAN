// Drains the outage buffer to server2 as batched UDP packets through the
// WireGuard tunnel. Records are only removed from the buffer after
// sendto() succeeds; packets lost in flight show up as seq gaps on the
// server side.

#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "sender.h"
#include "ring.h"
#include "net.h"
#include "can_capture.h"
#include "proto.h"

static const char *TAG = "send";

#define MAX_PACKETS_PER_WAKE 20

static int s_sock = -1;
static struct sockaddr_in s_dst;
static uint32_t s_seq;
static uint32_t s_boot_id;
static uint8_t s_pkt[sizeof(kcan_hdr_t) + 56 * sizeof(kcan_rec_t)];

static void fill_hdr(uint8_t type, uint16_t count)
{
    kcan_hdr_t h = {
        .magic = KCAN_MAGIC,
        .version = KCAN_VERSION,
        .type = type,
        .count = count,
        .seq = s_seq,
        .boot_id = s_boot_id,
    };
    memcpy(s_pkt, &h, sizeof(h));
}

static bool send_pkt(size_t len)
{
    int r = sendto(s_sock, s_pkt, len, 0, (struct sockaddr *)&s_dst, sizeof(s_dst));
    if (r == (int)len) {
        s_seq++;
        return true;
    }
    return false;   // typically ENOMEM (lwIP out of pbufs) or no route yet
}

static void send_status(void)
{
    kcan_status_t st = {0};
    int64_t up = esp_timer_get_time();
    st.uptime_us = (uint64_t)up;
    st.time_valid = net_time_valid();
    if (st.time_valid) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        st.ts_us = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    } else {
        st.ts_us = (uint64_t)up;
    }
    st.twai_state      = can_capture_state();   // also refreshes driver counters
    st.twai_rx_err     = can_capture_rx_err();
    st.frames_rx       = atomic_load(&g_stats.frames_rx);
    st.records_queued  = atomic_load(&g_stats.records_queued);
    st.records_dropped = atomic_load(&g_stats.records_dropped);
    st.twai_rx_missed  = atomic_load(&g_stats.twai_rx_missed);
    st.twai_bus_errors = atomic_load(&g_stats.twai_bus_errors);
    st.marks           = atomic_load(&g_stats.marks);
    st.ring_used       = ring_used();
    st.ring_capacity   = ring_capacity();
    st.free_heap       = esp_get_free_heap_size();
    st.wifi_rssi       = net_rssi();

    fill_hdr(KCAN_PKT_STATUS, 0);
    memcpy(s_pkt + sizeof(kcan_hdr_t), &st, sizeof(st));
    send_pkt(sizeof(kcan_hdr_t) + sizeof(st));
}

static void sender_task(void *arg)
{
    int64_t last_flush = 0, last_status = 0;
    bool was_up = false;

    for (;;) {
        if (!net_link_up()) {
            if (was_up) ESP_LOGW(TAG, "link down, buffering (%u queued)", (unsigned)ring_used());
            was_up = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!was_up) ESP_LOGI(TAG, "link up, draining %u records", (unsigned)ring_used());
        was_up = true;

        int64_t now = esp_timer_get_time();
        if (now - last_status >= 1000000) {
            send_status();
            last_status = now;
        }

        bool backoff = false;
        for (int sent = 0; sent < MAX_PACKETS_PER_WAKE; sent++) {
            uint32_t used = ring_used();
            if (used == 0) break;
            if (used < CONFIG_KCAN_BATCH_RECORDS &&
                now - last_flush < CONFIG_KCAN_FLUSH_MS * 1000LL) {
                break;   // wait for a fuller packet
            }
            kcan_rec_t *recs = (kcan_rec_t *)(s_pkt + sizeof(kcan_hdr_t));
            uint32_t n = ring_peek(recs, CONFIG_KCAN_BATCH_RECORDS);
            fill_hdr(KCAN_PKT_FRAMES, (uint16_t)n);
            if (!send_pkt(sizeof(kcan_hdr_t) + n * sizeof(kcan_rec_t))) {
                backoff = true;   // keep the records, try again shortly
                break;
            }
            ring_consume(n);
            last_flush = now;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff ? 20 : 5));
    }
}

bool sender_start(void)
{
    s_boot_id = esp_random();
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return false;
    }
    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port = htons(CONFIG_KCAN_SERVER_PORT);
    inet_aton(CONFIG_KCAN_SERVER_IP, &s_dst.sin_addr);

    ESP_LOGI(TAG, "boot_id %08x -> %s:%d", (unsigned)s_boot_id,
             CONFIG_KCAN_SERVER_IP, CONFIG_KCAN_SERVER_PORT);
    xTaskCreatePinnedToCore(sender_task, "sender", 4096, NULL, 6, NULL, 0);
    return true;
}

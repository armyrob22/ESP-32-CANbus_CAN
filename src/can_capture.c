// TWAI capture in LISTEN-ONLY mode. The controller never drives the bus:
// no ACKs, no error frames, and the TX queue is disabled, so there is no
// code path in this firmware that can put a frame on the Kia's network.

#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "can_capture.h"
#include "ring.h"
#include "net.h"
#include "proto.h"

static const char *TAG = "can";

static QueueHandle_t s_mark_q;
static volatile int64_t s_last_mark_us;
static uint32_t s_mark_no;

static int64_t s_epoch_off;   // epoch_us - esp_timer_us
static bool    s_off_valid;

#if CONFIG_KCAN_CHANGE_ONLY
typedef struct {
    uint8_t  data[8];
    uint8_t  dlc;
    uint8_t  seen;
    uint16_t suppressed;
    uint32_t last_sent_ms;
} id_slot_t;
static id_slot_t *s_slots;    // one per 11-bit ID
#endif

// ---------------------------------------------------------------- time

static void refresh_offset(void)
{
    if (!net_time_valid()) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s_epoch_off = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - esp_timer_get_time();
    s_off_valid = true;
}

static inline void stamp(kcan_rec_t *r, int64_t mono_us)
{
    if (s_off_valid) {
        r->ts_us = (uint64_t)(mono_us + s_epoch_off);
        r->rflags |= KCAN_RF_TIME_VALID;
    } else {
        r->ts_us = (uint64_t)mono_us;
    }
}

// ---------------------------------------------------------------- marks

static void mark_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_mark_us < 250000) return;   // 250 ms debounce
    s_last_mark_us = now;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_mark_q, &now, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void push_mark(int64_t mono_us)
{
    kcan_rec_t r = {0};
    s_mark_no++;
    r.id = KCAN_ID_MARK | (s_mark_no & KCAN_ID_MASK);
    memcpy(r.data, &s_mark_no, sizeof(s_mark_no));
    stamp(&r, mono_us);
    ring_push(&r);
    atomic_fetch_add(&g_stats.marks, 1);
    ESP_LOGI(TAG, "mark %u", (unsigned)s_mark_no);
}

// ---------------------------------------------------------------- frames

static void handle_frame(const twai_message_t *m, int64_t mono_us, uint8_t extra_flags)
{
    atomic_fetch_add(&g_stats.frames_rx, 1);

    kcan_rec_t r = {0};
    r.id = m->identifier & KCAN_ID_MASK;
    if (m->extd) r.id |= KCAN_ID_EXT;
    if (m->rtr)  r.id |= KCAN_ID_RTR;
    r.dlc = m->data_length_code;
    if (!m->rtr) {
        uint8_t n = m->data_length_code > 8 ? 8 : m->data_length_code;
        memcpy(r.data, m->data, n);
    }
    r.rflags = extra_flags;

#if CONFIG_KCAN_CHANGE_ONLY
    if (!m->extd && !m->rtr) {
        id_slot_t *s = &s_slots[m->identifier & 0x7FF];
        uint32_t now_ms = (uint32_t)(mono_us / 1000);
        bool changed = !s->seen || s->dlc != r.dlc || memcmp(s->data, r.data, 8) != 0;
        if (!changed && (now_ms - s->last_sent_ms) < CONFIG_KCAN_KEEPALIVE_MS) {
            if (s->suppressed < UINT16_MAX) s->suppressed++;
            return;
        }
        if (!changed) r.rflags |= KCAN_RF_KEEPALIVE;
        r.suppressed = s->suppressed;
        stamp(&r, mono_us);
        if (ring_push(&r)) {
            // Only commit on success, so a change lost to a full buffer is
            // re-detected on the next frame instead of silently vanishing.
            s->seen = 1;
            s->dlc = r.dlc;
            memcpy(s->data, r.data, 8);
            s->suppressed = 0;
            s->last_sent_ms = now_ms;
        }
        return;
    }
#endif
    stamp(&r, mono_us);
    ring_push(&r);
}

// ---------------------------------------------------------------- sim

#if CONFIG_KCAN_SIM_TRAFFIC
// Called every 1 ms. A few IDs with realistic-ish behaviour so the
// change-only filter, keepalives and the server diff all get exercised.
static void sim_step(void)
{
    static uint32_t tick;
    static uint8_t counter;
    static uint16_t rpm = 750;
    tick++;
    int64_t now = esp_timer_get_time();
    twai_message_t m = {0};

    if (tick % 10 == 0) {                       // 0x316: RPM, slow drift, 100 Hz
        rpm = 750 + (uint16_t)((tick / 100) % 400);
        m.identifier = 0x316; m.data_length_code = 8;
        m.data[2] = rpm & 0xFF; m.data[3] = rpm >> 8;
        handle_frame(&m, now, KCAN_RF_SIM);
    }
    if (tick % 10 == 5) {                       // 0x329: rolling counter, changes every frame
        memset(&m, 0, sizeof(m));
        m.identifier = 0x329; m.data_length_code = 8;
        m.data[0] = counter++ & 0x0F;
        handle_frame(&m, now, KCAN_RF_SIM);
    }
    if (tick % 100 == 0) {                      // 0x541: "door" bit toggles every 5 s, 10 Hz
        memset(&m, 0, sizeof(m));
        m.identifier = 0x541; m.data_length_code = 8;
        m.data[1] = ((tick / 5000) & 1) ? 0x01 : 0x00;
        handle_frame(&m, now, KCAN_RF_SIM);
    }
    if (tick % 1000 == 0) {                     // 0x5F0: static, 1 Hz
        memset(&m, 0, sizeof(m));
        m.identifier = 0x5F0; m.data_length_code = 4;
        m.data[0] = 0xAA; m.data[1] = 0x55;
        handle_frame(&m, now, KCAN_RF_SIM);
    }
}
#endif

// ---------------------------------------------------------------- task

static void capture_task(void *arg)
{
    int64_t last_off = 0;
#if !CONFIG_KCAN_SIM_TRAFFIC
    int64_t last_alert = 0;
    twai_message_t m;
#endif

    for (;;) {
        int64_t now = esp_timer_get_time();
        if (!s_off_valid || now - last_off > 1000000) {
            refresh_offset();
            last_off = now;
        }

        int64_t mk;
        while (xQueueReceive(s_mark_q, &mk, 0) == pdTRUE) {
            push_mark(mk);
        }

#if CONFIG_KCAN_SIM_TRAFFIC
        sim_step();
        vTaskDelay(1);
#else
        if (twai_receive(&m, pdMS_TO_TICKS(10)) == ESP_OK) {
            handle_frame(&m, esp_timer_get_time(), 0);
            // Drain whatever else is queued, but come back up for marks.
            for (int i = 0; i < 64 && twai_receive(&m, 0) == ESP_OK; i++) {
                handle_frame(&m, esp_timer_get_time(), 0);
            }
        }
        if (now - last_alert > 100000) {
            last_alert = now;
            uint32_t a;
            if (twai_read_alerts(&a, 0) == ESP_OK) {
                if (a & TWAI_ALERT_BUS_OFF) {
                    ESP_LOGW(TAG, "bus-off, recovering");
                    twai_initiate_recovery();
                }
                if (a & TWAI_ALERT_BUS_RECOVERED) {
                    twai_start();
                }
            }
        }
#endif
    }
}

// ---------------------------------------------------------------- public

bool can_capture_start(void)
{
    s_mark_q = xQueueCreate(16, sizeof(int64_t));

#if CONFIG_KCAN_CHANGE_ONLY
    s_slots = calloc(2048, sizeof(id_slot_t));
    if (!s_slots) {
        ESP_LOGE(TAG, "no memory for change-only table");
        return false;
    }
#endif

#if CONFIG_KCAN_SIM_TRAFFIC
    ESP_LOGW(TAG, "SIM_TRAFFIC: TWAI not started, generating fake frames");
#else
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)CONFIG_KCAN_CAN_TX_GPIO,
        (gpio_num_t)CONFIG_KCAN_CAN_RX_GPIO,
        TWAI_MODE_LISTEN_ONLY);
    g.tx_queue_len = 0;          // no TX queue at all
    g.rx_queue_len = 256;
    g.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;

#if CONFIG_KCAN_BITRATE_100K
    twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS();
#elif CONFIG_KCAN_BITRATE_125K
    twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS();
#elif CONFIG_KCAN_BITRATE_250K
    twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
#elif CONFIG_KCAN_BITRATE_1M
    twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
#else
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
#endif
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_driver_install: %s", esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "TWAI listen-only, TX=%d RX=%d", CONFIG_KCAN_CAN_TX_GPIO, CONFIG_KCAN_CAN_RX_GPIO);
#endif

#if CONFIG_KCAN_MARK_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_KCAN_MARK_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)CONFIG_KCAN_MARK_GPIO, mark_isr, NULL);
    ESP_LOGI(TAG, "mark button on GPIO%d", CONFIG_KCAN_MARK_GPIO);
#endif

    // Own core (core 1 on dual-core parts); WiFi and lwIP live on core 0.
    xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 12, NULL,
                            portNUM_PROCESSORS - 1);
    return true;
}

uint8_t can_capture_state(void)
{
#if CONFIG_KCAN_SIM_TRAFFIC
    return 0xFF;
#else
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return 0;
    atomic_store(&g_stats.twai_rx_missed, st.rx_missed_count + st.rx_overrun_count);
    atomic_store(&g_stats.twai_bus_errors, st.bus_error_count);
    return (uint8_t)st.state;
#endif
}

uint8_t can_capture_rx_err(void)
{
#if CONFIG_KCAN_SIM_TRAFFIC
    return 0;
#else
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return 0;
    return (uint8_t)(st.rx_error_counter > 255 ? 255 : st.rx_error_counter);
#endif
}

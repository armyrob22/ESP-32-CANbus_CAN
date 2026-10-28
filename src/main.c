// CANopen node simulator for bench-testing a listen-only telematics unit.
// ESP-IDF v5.x, legacy TWAI driver. ESP32 + Waveshare SN65HVD230 transceiver.
//
// Bus:   125 kbit/s, standard 11-bit IDs, little-endian payloads (CANopen).
// Node:  NODE_ID below. Heartbeat 0x700+ID, TPDO1 0x180+ID, TPDO2 0x280+ID.
//
// TPDO1 (100 ms)                      TPDO2 (1000 ms)
//   [0..1] RPM        u16 1 rpm/bit     [0..3] Engine hours u32 0.1 h/bit
//   [2..3] Coolant    s16 0.1 C/bit     [4]    Fuel level   u8  1 %/bit
//   [4..5] Oil press  u16 1 kPa/bit     [5..6] Battery      u16 0.01 V/bit
//   [6]    Status     bit0 1 Hz toggle  [7]    Sequence     u8
//                     bit7 engine run
//   [7]    Sequence   u8
//
// Serial keys (idf.py monitor):
//   h  toggle heartbeat      s  toggle silent (no TX at all)
//   b  toggle TPDO1 burst    e  toggle edge values (min / max / N/A, 5 s each)
//   a  toggle accel hours    v  toggle 1 Hz value print
//   p  print bus status      r  reset all modes to normal
//   ?  help

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "driver/uart.h"
#include "esp_log.h"

#define TAG "canopen_sim"

// ---- Config ----
#define CAN_TX_GPIO   GPIO_NUM_5   // ESP32 TX -> transceiver CTX / TXD
#define CAN_RX_GPIO   GPIO_NUM_4   // ESP32 RX <- transceiver CRX / RXD
#define NODE_ID       0x10

// TWAI_MODE_NORMAL needs another node on the bus to ACK frames.
// If the unit's controller is truly silent, TEC climbs and frames retry forever:
// switch to TWAI_MODE_NO_ACK.
#define CAN_MODE      TWAI_MODE_NORMAL

#define COB_HB        (0x700 + NODE_ID)
#define COB_TPDO1     (0x180 + NODE_ID)
#define COB_TPDO2     (0x280 + NODE_ID)

#define TICK_MS       10
#define TPDO1_MS      100
#define TPDO2_MS      1000
#define HB_MS         1000
#define BURST_MS      10

#define HOURS_START_X10  12345u    // 1234.5 h
#define MS_PER_TENTH_H   360000u   // 0.1 h = 360 s

// ---- State ----
static bool hb_on = true, silent = false, burst = false;
static bool edge = false, accel = false, verbose = false;
static uint32_t tx_ok = 0, tx_fail = 0;
static uint8_t seq1 = 0, seq2 = 0;
static uint32_t hours_x10 = HOURS_START_X10;
static uint32_t hours_accum_ms = 0;

// ---- Helpers ----
static void put_u16(uint8_t *b, uint16_t v) { b[0] = v & 0xFF; b[1] = v >> 8; }
static void put_u32(uint8_t *b, uint32_t v)
{
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = v >> 24;
}

static void send(uint32_t id, const uint8_t *d, uint8_t len)
{
    if (silent) return;
    twai_message_t m = {0};
    m.identifier = id;
    m.data_length_code = len;
    memcpy(m.data, d, len);
    if (twai_transmit(&m, 0) == ESP_OK) tx_ok++; else tx_fail++;
}

// ---- Signal models (t = ms since boot) ----
static uint16_t sim_rpm(uint32_t t)        // triangle 800 -> 2200 -> 800, 60 s
{
    uint32_t p = t % 60000, x = p < 30000 ? p : 60000 - p;
    return 800 + 1400u * x / 30000;
}
static int16_t sim_coolant(uint32_t t)     // 20.0 -> 90.0 C over 5 min, then hold
{
    if (t >= 300000) return 900;
    return 200 + (int16_t)(700u * t / 300000);
}
static uint16_t sim_oil(uint16_t rpm)      // 150 kPa idle -> 450 kPa at 2200
{
    return 150 + (uint16_t)((uint32_t)(rpm - 800) * 300 / 1400);
}
static uint8_t sim_fuel(uint32_t t)        // 100 -> 0 % over 30 min, wraps
{
    return 100 - (uint8_t)((t % 1800000) * 100u / 1800000);
}
static uint16_t sim_batt(uint32_t t)       // 13.80 V, 2 s dip to 11.50 V every 2 min
{
    return (t % 120000) < 2000 ? 1150 : 1380;
}
static uint8_t sim_status(uint32_t t)
{
    return (uint8_t)(0x80 | ((t / 1000) & 1));
}

// ---- Frames ----
static void send_tpdo1(uint32_t t)
{
    uint8_t d[8];
    uint16_t rpm, oil; int16_t clt; uint8_t st;

    if (edge) {
        switch ((t / 5000) % 3) {
        case 0:  rpm = 0;      clt = -400;   oil = 0;      st = 0x00; break; // min
        case 1:  rpm = 8000;   clt = 1500;   oil = 1000;   st = 0x7F; break; // max valid
        default: rpm = 0xFFFF; clt = 0x7FFF; oil = 0xFFFF; st = 0xFF; break; // N/A
        }
    } else {
        rpm = sim_rpm(t); clt = sim_coolant(t); oil = sim_oil(rpm); st = sim_status(t);
    }
    put_u16(&d[0], rpm);
    put_u16(&d[2], (uint16_t)clt);
    put_u16(&d[4], oil);
    d[6] = st;
    d[7] = seq1++;
    send(COB_TPDO1, d, 8);
}

static void send_tpdo2(uint32_t t)
{
    uint8_t d[8];
    uint32_t hrs; uint8_t fuel; uint16_t batt;

    if (edge) {
        switch ((t / 5000) % 3) {
        case 0:  hrs = 0;          fuel = 0;    batt = 0;      break;
        case 1:  hrs = 0xFFFFFFFE; fuel = 100;  batt = 3200;   break;
        default: hrs = 0xFFFFFFFF; fuel = 0xFF; batt = 0xFFFF; break;
        }
    } else {
        hrs = hours_x10; fuel = sim_fuel(t); batt = sim_batt(t);
    }
    put_u32(&d[0], hrs);
    d[4] = fuel;
    put_u16(&d[5], batt);
    d[7] = seq2++;
    send(COB_TPDO2, d, 8);
}

static void send_heartbeat(uint8_t state)
{
    send(COB_HB, &state, 1);
}

// ---- Bus health ----
static void print_status(void)
{
    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) return;
    static const char *st[] = {"STOPPED", "RUNNING", "BUS_OFF", "RECOVERING"};
    ESP_LOGI(TAG, "bus=%s ok=%" PRIu32 " fail=%" PRIu32 " TEC=%" PRIu32 " REC=%" PRIu32
             " queued=%" PRIu32 " | hb=%d silent=%d burst=%d edge=%d accel=%d",
             st[s.state], tx_ok, tx_fail, s.tx_error_counter, s.rx_error_counter,
             s.msgs_to_tx, hb_on, silent, burst, edge, accel);
}

static void check_bus(void)
{
    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) return;
    if (s.state == TWAI_STATE_BUS_OFF) {
        ESP_LOGW(TAG, "bus-off, recovering");
        twai_initiate_recovery();
    } else if (s.state == TWAI_STATE_STOPPED) {
        twai_start();
        ESP_LOGI(TAG, "recovered, restarted");
    }
}

static void drain_rx(void)
{
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) { /* listen-only target; ignore */ }
}

// ---- Console ----
static void help(void)
{
    printf("\nh=heartbeat s=silent b=burst e=edge a=accel-hours v=verbose p=status r=reset ?=help\n\n");
}

static void handle_key(uint8_t c)
{
    switch (c) {
    case 'h': hb_on  = !hb_on;  ESP_LOGI(TAG, "heartbeat %s", hb_on ? "on" : "off"); break;
    case 's': silent = !silent; ESP_LOGI(TAG, "silent %s", silent ? "on" : "off"); break;
    case 'b': burst  = !burst;  ESP_LOGI(TAG, "burst %s (TPDO1 every %d ms)", burst ? "on" : "off",
                                         burst ? BURST_MS : TPDO1_MS); break;
    case 'e': edge   = !edge;   ESP_LOGI(TAG, "edge values %s", edge ? "on" : "off"); break;
    case 'a': accel  = !accel;  ESP_LOGI(TAG, "accel hours %s (1 s = 1 h)", accel ? "on" : "off"); break;
    case 'v': verbose = !verbose; break;
    case 'p': print_status(); break;
    case 'r': hb_on = true; silent = burst = edge = accel = false;
              ESP_LOGI(TAG, "all modes normal"); break;
    case '?': help(); break;
    default: break;
    }
}

// ---- Main ----
void app_main(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, CAN_MODE);
    g.tx_queue_len = 32;
    twai_timing_config_t tm = TWAI_TIMING_CONFIG_125KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g, &tm, &f));
    ESP_ERROR_CHECK(twai_start());
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "CANopen sim: node 0x%02X @ 125k, TX=%d RX=%d", NODE_ID, CAN_TX_GPIO, CAN_RX_GPIO);
    help();

    send_heartbeat(0x00);   // boot-up message

    TickType_t last = xTaskGetTickCount();
    uint32_t t = 0;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        t += TICK_MS;

        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) handle_key(c);

        // Engine hours: real time, or 1 s = 1 h in accel mode
        hours_accum_ms += TICK_MS * (accel ? 3600u : 1u);
        while (hours_accum_ms >= MS_PER_TENTH_H) {
            hours_accum_ms -= MS_PER_TENTH_H;
            hours_x10++;
        }

        if (t % (burst ? BURST_MS : TPDO1_MS) == 0) send_tpdo1(t);
        if (t % TPDO2_MS == 0) send_tpdo2(t);
        if (hb_on && t % HB_MS == 0) send_heartbeat(0x05);   // operational

        if (t % 1000 == 0) {
            drain_rx();
            check_bus();
            if (verbose && !edge) {
                uint16_t rpm = sim_rpm(t);
                ESP_LOGI(TAG, "rpm=%u clt=%.1fC oil=%ukPa hrs=%.1f fuel=%u%% batt=%.2fV",
                         rpm, sim_coolant(t) / 10.0, sim_oil(rpm), hours_x10 / 10.0,
                         sim_fuel(t), sim_batt(t) / 100.0);
            }
        }
    }
}
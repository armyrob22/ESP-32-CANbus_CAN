// Wire format between the ESP32 and the server2 logger. All fields are
// little-endian and packed. See PROTOCOL.md for the server-side contract.
#pragma once
#include <stdint.h>

#define KCAN_MAGIC   0x4E41434Bu   // bytes on the wire: 'K' 'C' 'A' 'N'
#define KCAN_VERSION 1

enum {
    KCAN_PKT_FRAMES = 1,   // header + count * kcan_rec_t
    KCAN_PKT_STATUS = 2,   // header + kcan_status_t, count = 0
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t count;      // records that follow (FRAMES only)
    uint32_t seq;        // +1 per packet sent this boot, shared by both types
    uint32_t boot_id;    // random per boot; seq restarts at 0 when this changes
    uint32_t reserved0;
    uint32_t reserved1;
} kcan_hdr_t;            // 24 bytes

// id field
#define KCAN_ID_MASK  0x1FFFFFFFu
#define KCAN_ID_EXT   (1u << 29)  // 29-bit identifier
#define KCAN_ID_RTR   (1u << 30)  // remote frame
#define KCAN_ID_MARK  (1u << 31)  // not a CAN frame: mark-button press, low bits = mark number

// rflags field
#define KCAN_RF_TIME_VALID (1u << 0)  // ts_us is unix epoch; else it's uptime
#define KCAN_RF_KEEPALIVE  (1u << 1)  // unchanged payload, resent by change-only filter
#define KCAN_RF_SIM        (1u << 2)  // synthetic frame from SIM_TRAFFIC mode

typedef struct __attribute__((packed)) {
    uint64_t ts_us;       // unix epoch microseconds (see KCAN_RF_TIME_VALID)
    uint32_t id;
    uint8_t  dlc;
    uint8_t  rflags;
    uint16_t suppressed;  // identical frames on this ID dropped since the previous record for it
    uint8_t  data[8];
} kcan_rec_t;             // 24 bytes

typedef struct __attribute__((packed)) {
    uint64_t ts_us;            // epoch us when time is valid, else uptime
    uint64_t uptime_us;
    uint32_t frames_rx;        // frames read from the bus (before change-only filter)
    uint32_t records_queued;   // records pushed into the outage buffer
    uint32_t records_dropped;  // buffer was full
    uint32_t twai_rx_missed;   // driver/hardware overruns
    uint32_t twai_bus_errors;
    uint32_t ring_used;        // records waiting to send right now
    uint32_t ring_capacity;
    uint32_t free_heap;
    uint8_t  twai_state;       // 0 stopped, 1 running, 2 bus-off, 3 recovering, 0xFF sim
    uint8_t  twai_rx_err;      // receive error counter
    int8_t   wifi_rssi;        // dBm, 0 if unknown
    uint8_t  time_valid;
    uint32_t marks;            // mark-button presses this boot
} kcan_status_t;               // 56 bytes

_Static_assert(sizeof(kcan_hdr_t) == 24, "hdr size");
_Static_assert(sizeof(kcan_rec_t) == 24, "rec size");
_Static_assert(sizeof(kcan_status_t) == 56, "status size");

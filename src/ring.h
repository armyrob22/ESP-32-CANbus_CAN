// Single-producer (capture task) / single-consumer (sender task) record
// buffer. Holds data while the hotspot or tunnel is down.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "proto.h"

bool     ring_init(uint32_t capacity);
bool     ring_push(const kcan_rec_t *r);                 // producer only
uint32_t ring_peek(kcan_rec_t *out, uint32_t max);       // consumer only, copies, does not remove
void     ring_consume(uint32_t n);                       // consumer only
uint32_t ring_used(void);
uint32_t ring_capacity(void);

// Counters shared between tasks (all monotonically increasing this boot).
typedef struct {
    atomic_uint_fast32_t frames_rx;
    atomic_uint_fast32_t records_queued;
    atomic_uint_fast32_t records_dropped;
    atomic_uint_fast32_t twai_rx_missed;
    atomic_uint_fast32_t twai_bus_errors;
    atomic_uint_fast32_t marks;
} kcan_stats_t;

extern kcan_stats_t g_stats;

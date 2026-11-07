#include "ring.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ring";

kcan_stats_t g_stats;

static kcan_rec_t *s_buf;
static uint32_t s_cap;
// Free-running indices; used = head - tail (wraps correctly in uint32).
static atomic_uint_fast32_t s_head;
static atomic_uint_fast32_t s_tail;

bool ring_init(uint32_t capacity)
{
    // Power of two so the free-running uint32 indices stay correct when
    // they wrap (index & mask instead of index % cap).
    uint32_t pow2 = 1;
    while (pow2 < capacity) pow2 <<= 1;
    capacity = pow2;
    size_t bytes = (size_t)capacity * sizeof(kcan_rec_t);
#if CONFIG_SPIRAM
    s_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_buf) {
        s_buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_buf) {
        ESP_LOGE(TAG, "can't allocate %u records (%u bytes)", (unsigned)capacity, (unsigned)bytes);
        return false;
    }
    s_cap = capacity;
    atomic_store(&s_head, 0);
    atomic_store(&s_tail, 0);
    ESP_LOGI(TAG, "outage buffer: %u records (%u KB)", (unsigned)capacity, (unsigned)(bytes / 1024));
    return true;
}

bool ring_push(const kcan_rec_t *r)
{
    uint32_t h = atomic_load_explicit(&s_head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&s_tail, memory_order_acquire);
    if (h - t >= s_cap) {
        // Full: drop the newest. Old data stays contiguous, which is what
        // you want when reconstructing what happened before a dropout.
        atomic_fetch_add(&g_stats.records_dropped, 1);
        return false;
    }
    s_buf[h & (s_cap - 1)] = *r;
    atomic_store_explicit(&s_head, h + 1, memory_order_release);
    atomic_fetch_add(&g_stats.records_queued, 1);
    return true;
}

uint32_t ring_peek(kcan_rec_t *out, uint32_t max)
{
    uint32_t t = atomic_load_explicit(&s_tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&s_head, memory_order_acquire);
    uint32_t n = h - t;
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_buf[(t + i) & (s_cap - 1)];
    }
    return n;
}

void ring_consume(uint32_t n)
{
    uint32_t t = atomic_load_explicit(&s_tail, memory_order_relaxed);
    atomic_store_explicit(&s_tail, t + n, memory_order_release);
}

uint32_t ring_used(void)
{
    return atomic_load(&s_head) - atomic_load(&s_tail);
}

uint32_t ring_capacity(void)
{
    return s_cap;
}

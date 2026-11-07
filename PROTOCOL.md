# KCAN protocol: ESP32 → server2

This is the contract the server2 logger has to implement. The firmware side is done. `tools/kcan_tool.py` is a working reference decoder (`listen`) and a fake device (`fake`), so the server can be built and tested without the car.

## Transport

- UDP, sent by the ESP32 to `10.88.0.1:47000` inside the WireGuard tunnel (interface `wg-kcan` on server2). The ESP32's tunnel address is `10.88.0.2`.
- The ESP32 never listens and expects no replies. There's no ACK in v1.
- Bind the logger to `10.88.0.1` only, not `0.0.0.0`.

## Packet layout (little-endian, packed)

Every packet starts with a 24-byte header:

| off | type | field | notes |
|---|---|---|---|
| 0 | u32 | magic | `0x4E41434B` (bytes `K C A N`) |
| 4 | u8 | version | 1 |
| 5 | u8 | type | 1 = FRAMES, 2 = STATUS |
| 6 | u16 | count | records that follow (FRAMES), 0 for STATUS |
| 8 | u32 | seq | +1 per packet, shared across both types |
| 12 | u32 | boot_id | random per ESP32 boot |
| 16 | u32 | reserved0 | 0 |
| 20 | u32 | reserved1 | 0 |

Python: `struct.Struct("<IBBHIIII")`

Drop any packet with the wrong magic or version, or with `len != 24 + count*24` (FRAMES) or `len != 24 + 56` (STATUS).

### FRAMES record (24 bytes each, max 56 per packet)

| off | type | field | notes |
|---|---|---|---|
| 0 | u64 | ts_us | Unix epoch µs if `TIME_VALID`, else µs since ESP32 boot |
| 8 | u32 | id | bits 0–28 CAN ID; bit 29 EXT (29-bit); bit 30 RTR; bit 31 MARK |
| 12 | u8 | dlc | raw DLC |
| 13 | u8 | rflags | bit 0 TIME_VALID, bit 1 KEEPALIVE, bit 2 SIM |
| 14 | u16 | suppressed | identical frames on this ID dropped since the previous record for it |
| 16 | u8[8] | data | first `min(dlc,8)` bytes valid, rest zero |

Python: `struct.Struct("<QIBBH8s")`

**MARK records** (bit 31 set) are presses of the physical button on the ESP32. They aren't CAN frames: the low 29 bits hold the mark number (1, 2, 3… per boot), and they share the ESP32 clock with the frames. Use them as the high-precision version of an action event.

### STATUS (56 bytes, sent ~1/s while the tunnel is up)

`struct.Struct("<QQIIIIIIIIBBbBI")` →
`ts_us, uptime_us, frames_rx, records_queued, records_dropped, twai_rx_missed, twai_bus_errors, ring_used, ring_capacity, free_heap, twai_state, twai_rx_err, wifi_rssi, time_valid, marks`

- `twai_state`: 0 stopped, 1 running, 2 bus-off, 3 recovering, 0xFF simulation mode.
- Counters are cumulative since boot. Compute deltas server-side.
- `records_dropped > 0` means the ESP32's outage buffer overflowed, so data is missing on the device side.

## Semantics the logger must respect

1. **Change-only filtering is on by default.** For standard 11-bit IDs the ESP32 sends a record only when DLC or payload changes, plus a `KEEPALIVE` copy every ~1 s if nothing changed. So:
   - No record in a window does **not** mean the ID was silent. Its payload is the last one received.
   - Reconstruct state per ID as "last known payload at time t".
   - Frame count for an ID over an interval = (records in the interval) + (sum of their `suppressed`). Use that for rate/period.
   - Extended IDs and RTR frames are always passed through unfiltered.
2. **Loss detection:** track `(boot_id, seq)`. A new `boot_id` means the ESP32 rebooted and seq restarts. A gap in seq means packets were lost in flight. Log gaps as events with their time range, so analysis can ignore windows that overlap a gap.
3. **Ordering:** records inside a packet are in capture order, but packets can arrive out of order. Sort by `ts_us` before analysis.
4. **Time:** discard or quarantine records without `TIME_VALID`. They only occur in the first seconds after boot, before SNTP sync.
5. **Buffered bursts:** after a hotspot dropout the ESP32 drains its buffer quickly, so many seconds of old data can arrive at once. Use `ts_us`, never arrival time.

## Action logging (phone → server2, over Tailscale)

Not part of the ESP32 protocol, but this is what it pairs with:

- A small web page served by server2 and reachable over Tailscale. It has big buttons for common actions plus a free-text box, so you can define labels on the fly.
- Each tap POSTs `{label, note, client_ts_ms}`. The server stores that plus its own receive time. The phone and server2 are both NTP-synced, so either clock lines up with the ESP32 `ts_us` to within a few ms. Human reaction time (0.5–2 s) dominates the error.
- Optional "trial" grouping: `start trial "left window down"` → repeated taps → `end trial`.

## Analysis the data is shaped for

For each action event at time T (app tap or MARK):

- Take a pre-window `[T-W, T)` and a post-window `[T, T+W]`, with W ≈ 2 s and adjustable. Allow ±1 s of slop for the human.
- Per ID, per byte, per bit: did the last-known value change across T?
- Repeat the same action N times and rank candidates by how consistently they flip with the action, versus how often they flip with no action nearby. The baseline flip rate filters out counters and checksums.
- Report the top candidate IDs, bytes and bits per label.

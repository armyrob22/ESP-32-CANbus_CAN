#pragma once
#include <stdbool.h>
#include <stdint.h>

void   net_start(void);       // WiFi -> SNTP -> WireGuard supervisor
bool   net_link_up(void);     // WiFi has IP and the WireGuard peer is up
bool   net_time_valid(void);  // SNTP has synced at least once this boot
int8_t net_rssi(void);

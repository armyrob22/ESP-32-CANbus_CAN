#pragma once
#include <stdbool.h>
#include <stdint.h>

bool    can_capture_start(void);
uint8_t can_capture_state(void);   // matches kcan_status_t.twai_state
uint8_t can_capture_rx_err(void);

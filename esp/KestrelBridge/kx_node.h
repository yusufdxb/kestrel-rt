// Kestrel Phase 2 mailbox node (ESP32-S3 side). See docs/PHASE2_PROTOCOL.md.
#pragma once
#include <stdint.h>
void kx_start(volatile uint32_t *usb_baud);

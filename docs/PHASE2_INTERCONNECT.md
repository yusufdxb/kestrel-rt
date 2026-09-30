# Phase 2 interconnect: RA4M1 <-> ESP32-S3 on the UNO R4 WiFi

All connections pass through the board's TXB0108 level translator (RA4 5 V side, ESP 3.3 V side).
Sources: official bridge 0.5.2 (`UNOR4USBBridge.ino`, `at_handler.h`, `dap_config.h`), Arduino Renesas
core 1.6.0 variant `UNOWIFIR4/variant.cpp`, and hardware checks listed in the last column.

| Function | ESP32-S3 | RA4M1 | Stock bridge use | Phase 2 use | Verified on this board |
|---|---|---|---|---|---|
| USB bridge serial | UART0 TX GPIO43 / RX GPIO44 | SCI9 RXD9 P110 / TXD9 P109 (D23/D22) | USB CDC <-> RA4 `Serial` | unchanged: Kestrel host commands + telemetry (Phase 1 path) | Phase 1 campaign; all Phase 2 runs |
| Second serial link | UART1 TX GPIO5 / RX GPIO6 | SCI1 RXD1 P502 / TXD1 P501 (D25 "RX WIFI" / D24 "TX WIFI") | AT-command server (115200) | **KX mailbox, 1,000,000 baud 8N1** | frames flow both ways, 0 CRC errors in B1/B2 |
| RA4 reset | GPIO4 (`GPIO_RST`) | RES# | double reset (1200 baud), ROM boot (2400 baud), SWD reset | unchanged (host-requested only); boot glitch fixed | stock boot reset the RA4 twice (09-30); fork boots did not (see PHASE2_RESULTS) |
| RA4 boot mode | GPIO9 (`GPIO_BOOT`) | MD | ROM boot entry | unchanged | not exercised |
| SWD | SWDIO / SWCLK (dap_config.h) | SWDIO / SWCLK | CMSIS-DAP | unchanged (flash entry + read-only verify) | every RA4 flash in Phase 2 |

## Why SCI1 for the mailbox
- It is the only second UART physically wired between the chips; UART0/SCI9 must stay the USB
  bridge (host, bossac upload, Phase 1 telemetry).
- SCI1 is free in Kestrel. DMAC2/DMAC3 are free (DMAC0/1 serve SCI9).
- Exact baud on both ends: RA4 PCLKA 48 MHz / (8 x (5 + 1)) with ABCS + BGDM = 1,000,000; ESP32-S3
  APB 80 MHz / 80 = 1,000,000. At 1 Mbaud one byte is 10 us, so the whole 1 KB ring holds 10 ms of
  continuous traffic, twice the 5 ms link period.
- No flow-control lines exist; flow control is the protocol budget and rate limiter, and loss is
  counted rather than prevented.

## RA4 interrupt and DMA use (Phase 2 lines 12-15)
| Line | Event | Handler | Rate |
|---|---|---|---|
| 12 | SCI1_TEI | TX end | once per TX burst (<= 1 per 5 ms) |
| 13 | SCI1_ERI | receive error, error-storm guard | bounded: mute after 32 per 5 ms window, 20 ms mute |
| 14 | DMAC2_INT | TX DMA end | once per burst |
| 15 | DMAC3_INT | RX repeat count exhausted (re-arm) | every 64 MB received |
SCI1 RXI drives DMAC3 directly (not the NVIC): no per-byte interrupt at any traffic rate.

# Drivers (register level, no FSP)

| Driver | Hardware | Init | ISR work (bounded) | Errors | Portable/testable part |
|---|---|---|---|---|---|
| icu | ICU IELSRn -> NVIC 0..31 | `icu_route[_mode]` | one table dispatch; IR clear before (edge) or after (level) | out-of-range line ignored | - |
| gpio | PmnPFS, PCNTR1/2/3 | `gpio_init_output/input`, `gpio_set_peripheral` | none | `GPIO_EINVAL`; `GPIO_EFORBIDDEN` for P408 (USB mux: driving it cuts the ESP32 bridge and SWD) | - |
| timebase | GPT320 | `kt_init` | overflow: one increment; compare: kernel alarm | - | 64-bit extension (`kt_extend`), host-tested across the wrap |
| sci_uart | SCI9 async 460800 8N1 + DMAC0 | `uart_init` | RXI: one byte to SPSC; TEI: ring advance + next DMA start | `UART_EBUSY/EINVAL`, overrun/framing counters | SPSC queue, frame codec (fuzzed) |
| adc | ADC14 AN009 + GPT2 2 kHz trigger | `adc_init` | trigger: one register write; scan end: one SPSC push | queue-full counter, sequence gaps | SPSC queue |
| wdt | WDT register-start mode | `wdt_start` (once) | none (reset on underflow) | `WDT_EALREADY` | voting logic |
| timer (storm) | GPT321 | `storm_set_rate` | one increment | - | - |
| can | not implemented | - | - | - | - |

Notes
- UART TX: DMA is started by setting TE and TIE with one write while SPTR holds TXD high, which raises TXI and
  starts DMAC0; the TX ring is advanced only at TEI (all bytes out of the shift register).
- CAN and native USB are out of scope for Phase 1 (docs/LIMITATIONS.md).

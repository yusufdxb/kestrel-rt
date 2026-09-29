# Architecture

![Architecture](figures/architecture.svg)

## Boot chain
1. Arduino bootloader (0x0000-0x3FFF, never written by Kestrel): on a non-power-on reset it waits 500 ms for
   a double tap, then `boot5()` sets VTOR = 0x4000, MSP = vector[0] and jumps to vector[1] with PRIMASK = 1.
2. `kestrel_reset_handler` (startup/startup.c): VTOR, CP10/CP11 access, `.data` copy, `.bss` and stack zeroing,
   `.app_shared` copy, NVIC/ICU cleanup (the bootloader's state is not trusted), clocks, flash cache.
3. `kestrel_boot` (kernel/boot.c): DWT on, reset cause + crash record, fault setup, trace, timebase, telemetry,
   then the application's `kestrel_main()`, which creates tasks and calls `kestrel_start()`.

## Rules the code follows (MISRA-inspired, not a compliance claim)
- **Allocation**: static only. No heap exists (`-nostdlib`, the image gate rejects `malloc`/`_sbrk`).
- **Integer widths**: fixed-width types for all hardware and protocol data; timestamps are `uint32_t` deltas
  (modulo 2^32, valid for < 89 s) or `uint64_t` absolute. `-Wconversion` is on with `-Werror`.
- **volatile**: only for hardware registers and data shared with ISRs that is not already accessed through
  C11 atomics. Lock-free queues use `<stdatomic.h>` acquire/release instead.
- **ISR rules**: bounded work, no loops depending on external input, no blocking, no string formatting.
  Level-type ICU sources clear `IELSR.IR` after the handler, edge-type before (drivers/icu/icu.c).
- **Critical sections**: PRIMASK, privileged code only, bounded (the longest is measured and reported).
  No system call may be issued inside one.
- **Errors**: every driver/kernel API returns an explicit status; configuration errors at boot call
  `kestrel_panic()` (crash record, halt, no reset loop); run-time faults follow docs/FAULT_HANDLING.md.
- **Concurrency**: single core. Kernel state changes only inside a critical section in the timer ISR, SVC
  dispatch or PendSV selection. Tasks share data only through the IPC primitives in `.app_shared`.
- **Floating point**: application tasks only; the runtime is built with `-mgeneral-regs-only`.

## Trust boundaries
- Unprivileged tasks can read flash and the GPT320 counter, read/write their own stack and `.app_shared`,
  nothing else (MPU). Kernel pointers passed in system calls are validated (`k_user_ptr_ok`).
- The comms task is a privileged system service: it reads kernel rings and drives DMA. A fault in it resets.

## Channels and full policies
| Channel | Primitive | Full policy |
|---|---|---|
| ADC ISR -> sensor | SPSC (in `.app_shared`) | REJECT + counter; consumer detects the gap by sequence number |
| sensor -> control, control -> safety | seqlock mailbox | overwrite (latest value); stale value detected by sequence |
| UART RX ISR -> comms | SPSC bytes | REJECT + counter; frame CRC detects loss |
| kernel -> host trace / job records | ring (kernel producers) | drop newest + exact counter |
| safety faults, crash records | crash log + trace + counters | never dropped silently |

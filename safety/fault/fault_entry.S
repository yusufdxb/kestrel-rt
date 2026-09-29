/* Fault vectors: hand the C handler the stacked frame, EXC_RETURN and the exception number.
 * The frame is on PSP if the fault interrupted thread mode on the process stack, else on MSP. */
    .syntax unified
    .cpu cortex-m4
    .thumb
    .text

    .macro FAULT_ENTRY name
    .global \name
    .type \name, %function
    .thumb_func
\name:
    ldr     r3, =0xE0001004        /* DWT_CYCCNT at entry, for capture latency */
    ldr     r3, [r3]
    mov     r1, lr
    tst     lr, #0x4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    mrs     r2, ipsr
    b       kfault_dispatch
    .size \name, .-\name
    .endm

    FAULT_ENTRY HardFault_Handler
    FAULT_ENTRY MemManage_Handler
    FAULT_ENTRY BusFault_Handler
    FAULT_ENTRY UsageFault_Handler
    FAULT_ENTRY NMI_Handler

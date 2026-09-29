/* Kestrel ARMv7-M context switch, SVC entry and first-task start.
 *
 * Saved task context on the task's own stack (PSP), lowest address first:
 *   [EXC_RETURN] [CONTROL] [R4 R5 R6 R7 R8 R9 R10 R11]    <- software (PendSV), 10 words
 *   [S16..S31]                                            <- software, only if EXC_RETURN[4] == 0
 *   [R0 R1 R2 R3 R12 LR PC xPSR] (+ [S0..S15 FPSCR pad])  <- hardware exception frame
 * The FP high registers are pushed only for tasks whose frame is an extended (FP) frame, so tasks
 * that never touch the FPU pay nothing. Lazy stacking (FPCCR.LSPEN) defers the S0-S15 save.
 * See docs/CONTEXT_SWITCH.md.
 */
    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

    .equ DWT_CYCCNT, 0xE0001004
    .equ SCB_ICSR,   0xE000ED04
    .equ PENDSVSET,  0x10000000

    .section .bss.k_pendsv_stamps, "aw", %nobits
    .align 2
    .global k_pendsv_entry
    .global k_pendsv_exit
k_pendsv_entry: .space 4
k_pendsv_exit:  .space 4

    .text

/* PendSV: lowest priority, so it only runs when no other exception is active. */
    .global PendSV_Handler
    .type PendSV_Handler, %function
    .thumb_func
PendSV_Handler:
    ldr     r12, =DWT_CYCCNT
    ldr     r12, [r12]
    ldr     r1, =k_pendsv_entry
    str     r12, [r1]

    mrs     r0, psp
    tst     lr, #0x10              /* EXC_RETURN[4] == 0: extended frame, FP context active */
    it      eq
    vstmdbeq r0!, {s16-s31}
    mrs     r3, control
    mov     r2, lr
    stmdb   r0!, {r2-r11}

    bl      kestrel_sched_switch   /* r0 = saved PSP in, next task's PSP out */

    ldmia   r0!, {r2-r11}
    msr     control, r3            /* nPRIV of the incoming task */
    isb
    tst     r2, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
    msr     psp, r0

    ldr     r12, =DWT_CYCCNT
    ldr     r12, [r12]
    ldr     r1, =k_pendsv_exit
    str     r12, [r1]
    bx      r2
    .size PendSV_Handler, .-PendSV_Handler

/* SVC: decode the immediate from the SVC instruction and pass the caller's stacked frame to C. */
    .global SVC_Handler
    .type SVC_Handler, %function
    .thumb_func
SVC_Handler:
    tst     lr, #0x4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    ldr     r1, [r0, #24]          /* stacked PC (instruction after SVC) */
    ldrb    r1, [r1, #-2]          /* SVC imm8 */
    mov     r2, lr
    push    {r4, lr}               /* keep MSP 8-byte aligned for AAPCS */
    bl      k_svc_dispatch
    pop     {r4, pc}
    .size SVC_Handler, .-SVC_Handler

/* k_start_first(boot_psp_top): move thread mode onto a throwaway PSP, reclaim the whole main
 * stack for handlers, then let PendSV save this boot context and switch to the first task. */
    .global k_start_first
    .type k_start_first, %function
    .thumb_func
k_start_first:
    msr     psp, r0
    movs    r0, #2                 /* CONTROL.SPSEL = 1 (thread uses PSP), privileged */
    msr     control, r0
    isb
    ldr     r0, =__main_stack_top
    msr     msp, r0
    ldr     r0, =SCB_ICSR
    ldr     r1, =PENDSVSET
    str     r1, [r0]
    dsb
    cpsie   i
    isb
1:  b       1b                     /* never reached: PendSV switches away first */
    .size k_start_first, .-k_start_first

/* A task entry function returned: report it like any other task fault (SVC 14). */
    .global k_task_exit_trap
    .type k_task_exit_trap, %function
    .thumb_func
k_task_exit_trap:
    svc     #14
2:  b       2b
    .size k_task_exit_trap, .-k_task_exit_trap

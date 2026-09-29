/* FreeRTOS V11.1.0 configuration for the Kestrel comparison baseline (ARM_CM4F port, no MPU).
 * Chosen to be a normal, well-tuned FreeRTOS setup, not a handicapped one:
 *   - preemptive, 1 kHz tick driven by GPT320 compare-match (same hardware timer as Kestrel's
 *     release timer, so nominal release instants lie on the same timebase), port-optimised task
 *     selection (CLZ), static allocation only (no heap), no stack-overflow checking (Kestrel's
 *     equivalent protection costs are reported separately), task notifications available.
 *   - interrupt priorities follow the FreeRTOS Cortex-M rules: the tick and PendSV at the lowest
 *     priority, device ISRs that use FromISR APIs at or below configMAX_SYSCALL_INTERRUPT_PRIORITY.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configUSE_PREEMPTION                      1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION   1
#define configUSE_TICKLESS_IDLE                   0
#define configCPU_CLOCK_HZ                        48000000UL
#define configTICK_RATE_HZ                        1000U
#define configMAX_PRIORITIES                      8
#define configMINIMAL_STACK_SIZE                  128
#define configMAX_TASK_NAME_LEN                   12
#define configTICK_TYPE_WIDTH_IN_BITS             TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                   1
#define configUSE_TASK_NOTIFICATIONS              1
#define configUSE_MUTEXES                         1
#define configUSE_RECURSIVE_MUTEXES               0
#define configUSE_COUNTING_SEMAPHORES             0
#define configQUEUE_REGISTRY_SIZE                 0
#define configUSE_QUEUE_SETS                      0
#define configUSE_TIME_SLICING                    0
#define configUSE_NEWLIB_REENTRANT                0
#define configENABLE_BACKWARD_COMPATIBILITY       0
#define configUSE_APPLICATION_TASK_TAG            1
#define configUSE_TRACE_FACILITY                  0
#define configSUPPORT_STATIC_ALLOCATION           1
#define configSUPPORT_DYNAMIC_ALLOCATION          0
#define configKERNEL_PROVIDED_STATIC_MEMORY       1
#define configUSE_IDLE_HOOK                       0
#define configUSE_TICK_HOOK                       0
#define configCHECK_FOR_STACK_OVERFLOW            0
#define configUSE_MALLOC_FAILED_HOOK              0
#define configUSE_DAEMON_TASK_STARTUP_HOOK        0
#define configUSE_TIMERS                          0
#define configTIMER_TASK_STACK_DEPTH              128
#define configUSE_CO_ROUTINES                     0
#define configGENERATE_RUN_TIME_STATS             0
#define configUSE_STATS_FORMATTING_FUNCTIONS      0
#define configOVERRIDE_DEFAULT_TICK_CONFIGURATION 1 /* tick from GPT320, see main.c */

/* Cortex-M interrupt priorities: 4 implemented bits. */
#define configPRIO_BITS                              4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
#define configKERNEL_INTERRUPT_PRIORITY                                                            \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY                                                       \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define INCLUDE_vTaskDelayUntil           1
#define INCLUDE_vTaskDelay                1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_vTaskSuspend              1

/* Handler names used by Kestrel's vector table (startup/startup.c). */
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler

/* Per-task CPU-time accounting through the official trace hooks (same method as Kestrel's
 * switch-time accounting): the task tag holds the benchmark task index + 1. */
void bench_switched_in(uint32_t task);
void bench_switched_out(uint32_t task);
#define traceTASK_SWITCHED_IN()  bench_switched_in((uint32_t)pxCurrentTCB->pxTaskTag - 1u)
#define traceTASK_SWITCHED_OUT() bench_switched_out((uint32_t)pxCurrentTCB->pxTaskTag - 1u)

#define configASSERT(x)                                                                            \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            __asm volatile("cpsid i");                                                             \
            for (;;) {                                                                             \
            }                                                                                      \
        }                                                                                          \
    } while (0)

#endif

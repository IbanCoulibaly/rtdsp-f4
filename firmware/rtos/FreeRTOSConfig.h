/**
 * @file FreeRTOSConfig.h
 * @brief FreeRTOS V11 kernel configuration for RTDSP-F4.
 *
 * Design choices (see docs/ARCHITECTURE.md):
 *  - static allocation only: every kernel object and stack is sized at link
 *    time, so the memory map printed by the linker is the whole story;
 *  - stack overflow detection method 2 (watermark pattern check);
 *  - configASSERT routed to the fault recorder (log + reset, not a hang);
 *  - run-time stats from the BSP timestamp for the CPU-load figure.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t bsp_timestamp(void);
extern void app_assert_failed(const char *file, int line);

#define configCPU_CLOCK_HZ 168000000UL
/* configSYSTICK_CLOCK_HZ is deliberately NOT defined: defining it (even equal
 * to the CPU clock) makes the port select the HCLK/8 reference clock, and the
 * tick would run 8x slow. Undefined = SysTick on the core clock. */
#define configTICK_RATE_HZ 1000U
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE 0
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 128
#define configMAX_TASK_NAME_LEN 12
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configQUEUE_REGISTRY_SIZE 0
#define configENABLE_BACKWARD_COMPATIBILITY 0
#define configUSE_NEWLIB_REENTRANT 0
#define configUSE_MINI_LIST_ITEM 1
#define configSTACK_DEPTH_TYPE uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE 0
#define configENABLE_FPU 1
#define configENABLE_MPU 0

/* Memory: static only. */
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 0

/* Hooks. */
#define configUSE_IDLE_HOOK 1
#if defined(BSP_TARGET_QEMU) && BSP_TARGET_QEMU
#define configUSE_TICK_HOOK 1 /* drives the emulated sample clock, see bsp_timer.c */
#else
#define configUSE_TICK_HOOK 0
#endif
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW 2

/* Run-time statistics (CPU load). */
#define configGENERATE_RUN_TIME_STATS 1
#define configRUN_TIME_COUNTER_TYPE uint32_t
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE() bsp_timestamp()
#define configUSE_TRACE_FACILITY 1
#define configUSE_STATS_FORMATTING_FUNCTIONS 0

/* Kernel objects. */
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 0
#define configUSE_QUEUE_SETS 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_EVENT_GROUPS 1
#define configUSE_STREAM_BUFFERS 1
#define configUSE_TIMERS 0
#define configUSE_CO_ROUTINES 0

/* Cortex-M interrupt priorities. STM32 implements 4 priority bits; numerically
 * lower = more urgent. ISRs that call FreeRTOS *FromISR() APIs must have a
 * priority value >= MAX_SYSCALL (5). Anything at 0..4 is never masked by the
 * kernel (zero-latency) but must not touch the kernel. */
#define configPRIO_BITS 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define configASSERT(x)                                                                            \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            app_assert_failed(__FILE__, __LINE__);                                                 \
        }                                                                                          \
    } while (0)

/* API inclusion. */
#define INCLUDE_vTaskPrioritySet 0
#define INCLUDE_uxTaskPriorityGet 0
#define INCLUDE_vTaskDelete 0
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle 1

/* Map the port's handlers straight onto the CMSIS vector names. */
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */

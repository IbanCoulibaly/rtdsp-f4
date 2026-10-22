/**
 * @file app_config.h
 * @brief Every tunable of the application in one place.
 *
 * Timing budget (docs/DSP_MATH.md section 8):
 *   sample period     Ts = 1 / 48 kHz           = 20.8 us
 *   block period      Tb = 32 * Ts              = 667 us  (also the added latency)
 *   cycle budget          168 MHz / 48 kHz      = 3500 cycles per sample
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "dsp/filter_engine.h"

#define FW_VERSION_MAJOR 1u
#define FW_VERSION_MINOR 0u
#define FW_VERSION_PATCH 0u

/* ---- signal path ---- */
#define APP_FS_HZ 48000u
#define APP_BLOCK_SIZE 32u               /**< siggen block = DMA half-buffer size */
#define APP_BLOCK_MAX FILTER_BLOCK_MAX   /**< largest PROCESS frame, in samples */
#define APP_POOL_BLOCKS 8u               /**< blocks in flight across ISR/DSP/TX */
_Static_assert(APP_BLOCK_SIZE <= APP_BLOCK_MAX, "block size");
_Static_assert(2u * 2u * APP_BLOCK_MAX <= 600u, "STREAM frame must fit LINK_MAX_PAYLOAD");

/* ---- queues ---- */
#define APP_DSP_QUEUE_LEN (APP_POOL_BLOCKS + 4u)
#define APP_TX_QUEUE_LEN (APP_POOL_BLOCKS + 8u)
#define APP_TX_INLINE_MAX 80u
#define APP_LOG_QUEUE_LEN 16u
#define APP_LOG_LINE_MAX 96u

/* ---- UARTs ---- */
#define APP_DATA_BAUD 921600u
#define APP_CONSOLE_BAUD 115200u
#define APP_DATA_RX_BUF 2048u
#define APP_DATA_TX_BUF 1024u
#define APP_CONSOLE_TX_BUF 1024u

/* ---- RTOS tasks: rate-monotonic priorities (shortest period = highest) ---- */
#define PRIO_DSP 6u        /* period = one block (667 us) */
#define PRIO_LINK_RX 5u    /* must drain the UART before the rx buffer fills */
#define PRIO_LINK_TX 4u
#define PRIO_SUPERVISOR 2u /* 500 ms */
#define PRIO_LOG 1u        /* best effort */

#define STACK_DSP 384u /* words */
#define STACK_LINK_RX 512u
#define STACK_LINK_TX 384u
#define STACK_SUPERVISOR 384u
#define STACK_LOG 384u

/* ---- NVIC priorities: all >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY ---- */
#define IRQ_PRIO_SAMPLE_TIMER 5u
#define IRQ_PRIO_UART_DATA 6u
#define IRQ_PRIO_UART_CONSOLE 7u

/* ---- health ---- */
#define APP_TASK_WAIT_MS 100u        /**< every blocking wait is bounded by this */
#define APP_SUPERVISOR_PERIOD_MS 500u
#define APP_HANG_TIMEOUT_MS 3000u
#define APP_STATS_LOG_PERIOD_MS 5000u
#define APP_WATCHDOG_TIMEOUT_MS 2000u

#ifndef APP_DEBUG_CMDS
#define APP_DEBUG_CMDS 1 /* MSG_DEBUG_FAULT; disable for production */
#endif

#endif /* APP_CONFIG_H */

/**
 * @file app.h
 * @brief Shared application types, kernel objects and statistics.
 *
 * Ownership rules (who may write what) are what keep this design free of
 * mutexes on the data path:
 *   - a sample_block_t belongs to whoever holds its pointer; pointers move
 *     through queues (pool -> producer -> DSP -> TX -> pool), never shared;
 *   - g_engines[g_active_engine] is touched only by the DSP task;
 *     the other engine only by link_rx, and only while !g_swap_pending;
 *   - every g_stats field has exactly one writer task. 32-bit aligned
 *     loads/stores are atomic on Cortex-M4, so readers need no lock.
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_config.h"
#include "bsp/bsp_uart.h"
#include "dsp/filter_engine.h"

typedef enum { SRC_HOST = 0, SRC_SIGGEN = 1 } block_src_t;

typedef struct {
    uint16_t len;
    uint8_t seq;
    uint8_t src; /**< block_src_t */
    q15_t in[APP_BLOCK_MAX];
    q15_t out[APP_BLOCK_MAX];
} sample_block_t;

typedef enum { DSP_MSG_BLOCK = 0, DSP_MSG_SWAP = 1, DSP_MSG_RESET = 2 } dsp_msg_kind_t;

/** One queue carries data and control so commands take effect in stream order. */
typedef struct {
    uint8_t kind; /**< dsp_msg_kind_t */
    uint8_t seq;
    uint16_t reserved;
    sample_block_t *blk;
} dsp_msg_t;

typedef enum { TX_INLINE = 0, TX_BLOCK_RESULT = 1, TX_BLOCK_STREAM = 2 } tx_kind_t;

typedef struct {
    uint8_t kind; /**< tx_kind_t */
    uint8_t type; /**< MSG_* for TX_INLINE */
    uint8_t seq;
    uint8_t reserved;
    uint16_t len;
    sample_block_t *blk;
    uint8_t data[APP_TX_INLINE_MAX];
} tx_item_t;

typedef enum {
    TASK_DSP = 0,
    TASK_LINK_RX,
    TASK_LINK_TX,
    TASK_SUPERVISOR,
    TASK_LOG,
    TASK_COUNT
} app_task_id_t;

typedef struct {
    /* written by dsp task */
    volatile uint32_t blocks_processed;
    volatile uint32_t samples_processed;
    volatile uint32_t cycles_last_block;
    volatile uint32_t cycles_max_block;
    volatile uint32_t cycles_per_sample_q8;
    volatile uint32_t filter_swaps;
    /* written by link_rx */
    volatile uint32_t rx_frames_ok;
    volatile uint32_t rx_crc_errors;
    volatile uint32_t rx_framing_errors;
    volatile uint32_t pool_exhausted;
    /* written by dsp + link_tx (separate counters, summed on read) */
    volatile uint32_t tx_dropped_dsp;
    volatile uint32_t tx_dropped_link;
    /* written by the sample-clock ISR */
    volatile uint32_t siggen_overruns;
    /* written by supervisor */
    volatile uint16_t stack_free_words[TASK_COUNT];
    volatile uint8_t cpu_load_pct;
} app_stats_t;

extern app_stats_t g_stats;
extern bsp_uart_t g_uart_data;
extern bsp_uart_t g_uart_console;
extern QueueHandle_t g_dsp_q;
extern QueueHandle_t g_tx_q;
extern TaskHandle_t g_task[TASK_COUNT];

extern filter_engine_t g_engines[2];
extern volatile uint8_t g_active_engine;
extern volatile uint8_t g_swap_pending;
extern volatile uint8_t g_mode;
extern uint8_t g_post_result;

/* Task entry points. */
void dsp_task(void *arg);
void link_rx_task(void *arg);
void link_tx_task(void *arg);
void supervisor_task(void *arg);
void log_task(void *arg);

/* link_tx helpers: callable from any task. */
BaseType_t tx_post_inline(uint8_t type, uint8_t seq, const uint8_t *data, uint16_t len, TickType_t timeout);
BaseType_t tx_post_ack(uint8_t acked_type, uint8_t seq, uint8_t status);

/* Wire payload builders (app_info.c). */
uint16_t app_build_devinfo(uint8_t *out);
uint16_t app_build_stats(uint8_t *out);

#endif /* APP_H */

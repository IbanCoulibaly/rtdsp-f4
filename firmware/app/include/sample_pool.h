/**
 * @file sample_pool.h
 * @brief Fixed pool of sample blocks: zero-copy hand-off between contexts.
 *
 * A FreeRTOS queue of block pointers acts as the free list. Queues are
 * ISR-safe and FIFO, so the sample-clock ISR, link_rx, the DSP task and
 * link_tx can all take and return blocks without a mutex. Pool exhaustion
 * is the system's back-pressure signal and is counted, never hidden.
 */
#ifndef SAMPLE_POOL_H
#define SAMPLE_POOL_H

#include "app.h"

void pool_init(void);
sample_block_t *pool_get(TickType_t timeout);
sample_block_t *pool_get_from_isr(BaseType_t *woken);
void pool_put(sample_block_t *blk);
void pool_put_from_isr(sample_block_t *blk, BaseType_t *woken);
uint32_t pool_free_count(void);

#endif /* SAMPLE_POOL_H */

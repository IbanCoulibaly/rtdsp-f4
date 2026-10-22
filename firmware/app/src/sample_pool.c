/**
 * @file sample_pool.c
 * @brief Free list of sample blocks backed by a pointer queue.
 */
#include "sample_pool.h"

static sample_block_t s_blocks[APP_POOL_BLOCKS];
static StaticQueue_t s_qcb;
static uint8_t s_qbuf[APP_POOL_BLOCKS * sizeof(sample_block_t *)];
static QueueHandle_t s_free;

void pool_init(void)
{
    s_free = xQueueCreateStatic(APP_POOL_BLOCKS, sizeof(sample_block_t *), s_qbuf, &s_qcb);
    configASSERT(s_free != NULL);
    for (uint32_t i = 0u; i < APP_POOL_BLOCKS; i++) {
        sample_block_t *b = &s_blocks[i];
        (void)xQueueSend(s_free, &b, 0u);
    }
}

sample_block_t *pool_get(TickType_t timeout)
{
    sample_block_t *b = NULL;
    return (xQueueReceive(s_free, &b, timeout) == pdTRUE) ? b : NULL;
}

sample_block_t *pool_get_from_isr(BaseType_t *woken)
{
    sample_block_t *b = NULL;
    return (xQueueReceiveFromISR(s_free, &b, woken) == pdTRUE) ? b : NULL;
}

void pool_put(sample_block_t *blk)
{
    /* Cannot fail: the queue is exactly as deep as the pool. */
    configASSERT(xQueueSend(s_free, &blk, 0u) == pdTRUE);
}

void pool_put_from_isr(sample_block_t *blk, BaseType_t *woken)
{
    configASSERT(xQueueSendFromISR(s_free, &blk, woken) == pdTRUE);
}

uint32_t pool_free_count(void)
{
    return (uint32_t)uxQueueMessagesWaiting(s_free);
}

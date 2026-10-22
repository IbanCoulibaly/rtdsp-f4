/**
 * @file dsp_task.c
 * @brief Highest-priority task: runs the active filter on each block.
 *
 * Real-time rules followed here:
 *   - never blocks on a slower consumer: if the TX queue is full the block
 *     is dropped and counted (tx_dropped) rather than stalling the pipeline;
 *   - no logging, no allocation, no unbounded loops in the per-block path;
 *   - coefficient changes are applied *between* blocks by swapping which of
 *     two pre-built engines is active, so no block is filtered with a
 *     half-written coefficient set (double-buffered "ping-pong" banks).
 */
#include "app.h"

#include "app_services.h"
#include "bsp/bsp.h"
#include "link/protocol_defs.h"
#include "sample_pool.h"

static void update_timing(uint32_t cycles, uint16_t len)
{
    g_stats.cycles_last_block = cycles;
    if (cycles > g_stats.cycles_max_block) {
        g_stats.cycles_max_block = cycles;
    }
    /* Exponential moving average of cycles/sample in Q8:
     *   avg += (x - avg) / 16      (alpha = 1/16, ~16-block memory) */
    const int32_t cps_q8 = (int32_t)((cycles << 8) / len);
    const int32_t avg = (int32_t)g_stats.cycles_per_sample_q8;
    g_stats.cycles_per_sample_q8 = (avg == 0) ? (uint32_t)cps_q8 : (uint32_t)(avg + ((cps_q8 - avg) / 16));
}

static void process_block(sample_block_t *blk)
{
    filter_engine_t *e = &g_engines[g_active_engine];

    const uint32_t t0 = bsp_timestamp();
    filter_engine_process(e, blk->in, blk->out, blk->len);
    const uint32_t cycles = bsp_timestamp_to_cycles(bsp_timestamp() - t0);

    update_timing(cycles, blk->len);
    g_stats.blocks_processed++;
    g_stats.samples_processed += blk->len;

    tx_item_t item = {
        .kind = (blk->src == SRC_HOST) ? TX_BLOCK_RESULT : TX_BLOCK_STREAM,
        .seq = blk->seq,
        .blk = blk,
    };
    if (xQueueSend(g_tx_q, &item, 0u) != pdTRUE) {
        g_stats.tx_dropped_dsp++;
        pool_put(blk);
    }
}

void dsp_task(void *arg)
{
    (void)arg;
    for (;;) {
        dsp_msg_t m;
        if (xQueueReceive(g_dsp_q, &m, pdMS_TO_TICKS(APP_TASK_WAIT_MS)) == pdTRUE) {
            switch (m.kind) {
            case DSP_MSG_BLOCK:
                process_block(m.blk);
                break;
            case DSP_MSG_SWAP:
                /* link_rx finished building the inactive engine before queueing
                 * this message; the queue hand-off orders those writes before
                 * this read. */
                g_active_engine ^= 1u;
                g_stats.filter_swaps++;
                g_swap_pending = 0u;
                (void)tx_post_ack(MSG_SET_FILTER, m.seq, ST_OK);
                break;
            case DSP_MSG_RESET:
                filter_engine_reset(&g_engines[g_active_engine]);
                (void)tx_post_ack(MSG_RESET_STATE, m.seq, ST_OK);
                break;
            default:
                break;
            }
        }
        supervisor_heartbeat(TASK_DSP);
    }
}

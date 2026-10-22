/**
 * @file link_rx.c
 * @brief Command interpreter: de-frames the data UART and dispatches.
 *
 * Commands that must be ordered with the sample stream (SET_FILTER swap,
 * RESET_STATE, PROCESS) are forwarded through the DSP queue, and their
 * ACK/result is produced by the DSP task. Everything else is answered here.
 */
#include <string.h>

#include "app.h"

#include "app_services.h"
#include "link/byteorder.h"
#include "link/frame.h"
#include "link/protocol_defs.h"
#include "sample_pool.h"

/* The filter error codes double as ACK status codes. */
_Static_assert(FILTER_ERR_TYPE == ST_ERR_FILTER_TYPE, "status map");
_Static_assert(FILTER_ERR_ORDER == ST_ERR_FILTER_ORDER, "status map");
_Static_assert(FILTER_ERR_LEN == ST_ERR_FILTER_LEN, "status map");
_Static_assert(FILTER_ERR_PARAM == ST_ERR_FILTER_PARAM, "status map");
_Static_assert(FILTER_ERR_UNSTABLE == ST_ERR_UNSTABLE, "status map");

static uint8_t s_frame[FRAME_WIRE_MAX];
static size_t s_frame_len;
static uint8_t s_frame_overflow;
static filter_desc_t s_desc; /* 520 bytes: static, not on the task stack */

static void on_set_filter(const frame_t *f)
{
    if (g_swap_pending) {
        (void)tx_post_ack(f->type, f->seq, ST_ERR_BUSY);
        return;
    }
    const filter_status_t st = filter_desc_parse(&s_desc, f->payload, f->payload_len);
    if (st != FILTER_OK) {
        (void)tx_post_ack(f->type, f->seq, (uint8_t)st);
        return;
    }
    /* Build the inactive bank now (outside the DSP task's time budget),
     * then ask the DSP task to swap at the next block boundary. */
    const uint8_t inactive = (uint8_t)(g_active_engine ^ 1u);
    (void)filter_engine_configure(&g_engines[inactive], &s_desc);
    g_swap_pending = 1u;
    const dsp_msg_t m = {.kind = DSP_MSG_SWAP, .seq = f->seq};
    if (xQueueSend(g_dsp_q, &m, pdMS_TO_TICKS(APP_TASK_WAIT_MS)) != pdTRUE) {
        g_swap_pending = 0u;
        (void)tx_post_ack(f->type, f->seq, ST_ERR_BUSY);
    }
}

static void on_process(const frame_t *f)
{
    if (g_mode != MODE_HOST_STREAM) {
        (void)tx_post_ack(f->type, f->seq, ST_ERR_MODE);
        return;
    }
    const size_t n = f->payload_len / 2u;
    if ((n == 0u) || (n > APP_BLOCK_MAX) || ((f->payload_len & 1u) != 0u)) {
        (void)tx_post_ack(f->type, f->seq, ST_ERR_LEN);
        return;
    }
    sample_block_t *blk = pool_get(pdMS_TO_TICKS(APP_TASK_WAIT_MS));
    if (blk == NULL) {
        g_stats.pool_exhausted++;
        (void)tx_post_ack(f->type, f->seq, ST_ERR_BUSY);
        return;
    }
    for (size_t i = 0u; i < n; i++) {
        blk->in[i] = (q15_t)get_le16(&f->payload[2u * i]);
    }
    blk->len = (uint16_t)n;
    blk->seq = f->seq;
    blk->src = SRC_HOST;
    const dsp_msg_t m = {.kind = DSP_MSG_BLOCK, .seq = f->seq, .blk = blk};
    if (xQueueSend(g_dsp_q, &m, pdMS_TO_TICKS(APP_TASK_WAIT_MS)) != pdTRUE) {
        pool_put(blk);
        (void)tx_post_ack(f->type, f->seq, ST_ERR_BUSY);
    }
}

static void on_set_mode(const frame_t *f)
{
    if (f->payload_len < 1u) {
        (void)tx_post_ack(f->type, f->seq, ST_ERR_LEN);
        return;
    }
    const uint8_t mode = f->payload[0];
    if (mode == MODE_SIGGEN) {
        if (f->payload_len != LINK_SET_MODE_LEN) {
            (void)tx_post_ack(f->type, f->seq, ST_ERR_LEN);
            return;
        }
        const uint8_t *p = &f->payload[1];
        siggen_stop();
        siggen_configure(get_le16(&p[0]), (q15_t)get_le16(&p[2]), get_le16(&p[4]), (q15_t)get_le16(&p[6]),
                         (q15_t)get_le16(&p[8]));
        g_mode = MODE_SIGGEN;
        siggen_start();
        log_printf("mode: siggen");
    } else if (mode == MODE_HOST_STREAM) {
        siggen_stop();
        g_mode = MODE_HOST_STREAM;
        log_printf("mode: host stream");
    } else {
        (void)tx_post_ack(f->type, f->seq, ST_ERR_FILTER_PARAM);
        return;
    }
    (void)tx_post_ack(f->type, f->seq, ST_OK);
}

static void on_debug_fault(const frame_t *f)
{
#if APP_DEBUG_CMDS
    const uint8_t kind = (f->payload_len > 0u) ? f->payload[0] : 0u;
    log_printf("debug: injecting fault kind %u", kind);
    vTaskDelay(pdMS_TO_TICKS(20u)); /* let the log line out */
    if (kind == DBG_FAULT_UDF) {
        __asm volatile("udf #0");
    } else if (kind == DBG_FAULT_ASSERT) {
        /* Deliberately, always false: this is the fault-injection hook that
         * test_pil_faults.py uses to make the board crash an assert on
         * command, then checks that it reboots and reports correctly. */
        // cppcheck-suppress knownConditionTrueFalse
        configASSERT(kind == 0u);
    }
    (void)tx_post_ack(f->type, f->seq, ST_ERR_FILTER_PARAM);
#else
    (void)tx_post_ack(f->type, f->seq, ST_ERR_UNKNOWN_MSG);
#endif
}

static void handle_frame(uint8_t *buf, size_t len)
{
    frame_t f;
    const frame_status_t st = frame_parse(buf, len, &f);
    if (st == FRAME_ERR_CRC) {
        g_stats.rx_crc_errors++;
        return;
    }
    if (st != FRAME_OK) {
        g_stats.rx_framing_errors++;
        return;
    }
    g_stats.rx_frames_ok++;

    uint8_t out[APP_TX_INLINE_MAX];
    switch (f.type) {
    case MSG_PING:
        (void)tx_post_inline(MSG_PONG, f.seq, out, app_build_devinfo(out), pdMS_TO_TICKS(APP_TASK_WAIT_MS));
        break;
    case MSG_GET_STATS:
        (void)tx_post_inline(MSG_STATS, f.seq, out, app_build_stats(out), pdMS_TO_TICKS(APP_TASK_WAIT_MS));
        break;
    case MSG_SET_FILTER:
        on_set_filter(&f);
        break;
    case MSG_PROCESS:
        on_process(&f);
        break;
    case MSG_RESET_STATE: {
        const dsp_msg_t m = {.kind = DSP_MSG_RESET, .seq = f.seq};
        if (xQueueSend(g_dsp_q, &m, pdMS_TO_TICKS(APP_TASK_WAIT_MS)) != pdTRUE) {
            (void)tx_post_ack(f.type, f.seq, ST_ERR_BUSY);
        }
        break;
    }
    case MSG_SET_MODE:
        on_set_mode(&f);
        break;
    case MSG_DEBUG_FAULT:
        on_debug_fault(&f);
        break;
    default:
        (void)tx_post_ack(f.type, f.seq, ST_ERR_UNKNOWN_MSG);
        break;
    }
}

void link_rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[64];
    for (;;) {
        const size_t n = bsp_uart_read(&g_uart_data, chunk, sizeof chunk, pdMS_TO_TICKS(APP_TASK_WAIT_MS));
        for (size_t i = 0u; i < n; i++) {
            const uint8_t b = chunk[i];
            if (b == 0u) { /* delimiter: frame complete */
                if (s_frame_overflow) {
                    g_stats.rx_framing_errors++;
                } else if (s_frame_len > 0u) {
                    handle_frame(s_frame, s_frame_len);
                }
                s_frame_len = 0u;
                s_frame_overflow = 0u;
            } else if (s_frame_len < sizeof s_frame) {
                s_frame[s_frame_len++] = b;
            } else {
                s_frame_overflow = 1u; /* discard until the next delimiter */
            }
        }
        supervisor_heartbeat(TASK_LINK_RX);
    }
}

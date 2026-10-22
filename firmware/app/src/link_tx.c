/**
 * @file link_tx.c
 * @brief Sole owner of the data UART's transmit side.
 *
 * Every other task hands it a tx_item_t; only this task frames bytes and
 * writes the UART. Single ownership replaces a UART mutex and makes frame
 * interleaving impossible.
 */
#include <string.h>

#include "app.h"

#include "app_services.h"
#include "link/byteorder.h"
#include "link/frame.h"
#include "link/protocol_defs.h"
#include "sample_pool.h"

static uint8_t s_raw[FRAME_RAW_MAX];
static uint8_t s_wire[FRAME_WIRE_MAX];

BaseType_t tx_post_inline(uint8_t type, uint8_t seq, const uint8_t *data, uint16_t len, TickType_t timeout)
{
    tx_item_t item = {.kind = TX_INLINE, .type = type, .seq = seq, .len = len};
    configASSERT(len <= APP_TX_INLINE_MAX);
    memcpy(item.data, data, len);
    return xQueueSend(g_tx_q, &item, timeout);
}

BaseType_t tx_post_ack(uint8_t acked_type, uint8_t seq, uint8_t status)
{
    const uint8_t p[2] = {acked_type, status};
    return tx_post_inline(MSG_ACK, seq, p, 2u, pdMS_TO_TICKS(APP_TASK_WAIT_MS));
}

static void send_item(const tx_item_t *it)
{
    uint8_t *p = &s_raw[FRAME_PAYLOAD_OFFSET];
    size_t len = 0u;
    uint8_t type = it->type;

    switch (it->kind) {
    case TX_INLINE:
        memcpy(p, it->data, it->len);
        len = it->len;
        break;
    case TX_BLOCK_RESULT:
        type = MSG_PROCESS_RESULT;
        for (uint16_t i = 0u; i < it->blk->len; i++) {
            p = put_le16(p, (uint16_t)it->blk->out[i]);
        }
        len = 2u * it->blk->len;
        pool_put(it->blk);
        break;
    case TX_BLOCK_STREAM:
        type = MSG_STREAM;
        for (uint16_t i = 0u; i < it->blk->len; i++) {
            p = put_le16(p, (uint16_t)it->blk->in[i]);
        }
        for (uint16_t i = 0u; i < it->blk->len; i++) {
            p = put_le16(p, (uint16_t)it->blk->out[i]);
        }
        len = 4u * it->blk->len;
        pool_put(it->blk);
        break;
    default:
        return;
    }

    const size_t n = frame_finalize(s_raw, len, type, it->seq, s_wire, sizeof s_wire);
    if ((n == 0u) || (bsp_uart_write(&g_uart_data, s_wire, n, pdMS_TO_TICKS(1000u)) != n)) {
        g_stats.tx_dropped_link++;
    }
}

void link_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        tx_item_t it;
        if (xQueueReceive(g_tx_q, &it, pdMS_TO_TICKS(APP_TASK_WAIT_MS)) == pdTRUE) {
            send_item(&it);
        }
        supervisor_heartbeat(TASK_LINK_TX);
    }
}

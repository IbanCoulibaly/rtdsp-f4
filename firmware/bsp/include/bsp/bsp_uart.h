/**
 * @file bsp_uart.h
 * @brief Interrupt-driven USART driver on top of FreeRTOS stream buffers.
 *
 * RX:  ISR -> rx stream buffer -> exactly one reader task.
 * TX:  exactly one writer task -> tx stream buffer -> ISR (TXE interrupt).
 * Stream buffers are lock-free for one writer and one reader, which is
 * exactly the ISR/task split here, so no mutex is needed on the data path.
 *
 * RX back-pressure: when the rx buffer is full the ISR stops reading DR and
 * masks RXNEIE. On real silicon the next byte then sets ORE (counted as a
 * hardware overrun); under QEMU the emulator simply holds the byte, which
 * gives lossless flow control.
 */
#ifndef BSP_UART_H
#define BSP_UART_H

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "stream_buffer.h"

#include "bsp/stm32f4_regs.h"

typedef struct {
    usart_t *regs;
    StreamBufferHandle_t rx;
    StreamBufferHandle_t tx;
    StaticStreamBuffer_t rx_cb;
    StaticStreamBuffer_t tx_cb;
    volatile uint32_t rx_dropped;  /**< bytes lost to a full rx buffer / ORE */
    volatile uint8_t rx_throttled; /**< RXNEIE masked because rx buffer was full */
    uint8_t rx_enabled;
} bsp_uart_t;

typedef struct {
    usart_t *regs;
    uint32_t irqn;
    uint32_t pclk_hz;
    uint32_t baud;
    uint8_t irq_priority;
    uint8_t rx_enabled;
    uint8_t *rx_storage; /**< rx_size + 1 bytes (stream buffer needs one spare) */
    size_t rx_size;
    uint8_t *tx_storage; /**< tx_size + 1 bytes */
    size_t tx_size;
} bsp_uart_cfg_t;

void bsp_uart_init(bsp_uart_t *u, const bsp_uart_cfg_t *cfg);

/** Queue bytes for transmission; blocks up to @p timeout for space. Before
 *  the scheduler starts it falls back to polled output (boot banner). */
size_t bsp_uart_write(bsp_uart_t *u, const uint8_t *data, size_t len, TickType_t timeout);

/** Read up to @p max bytes, blocking up to @p timeout for the first one. */
size_t bsp_uart_read(bsp_uart_t *u, uint8_t *data, size_t max, TickType_t timeout);

/** Polled write that bypasses the buffer. Fault/panic paths only. */
void bsp_uart_write_polled(usart_t *regs, const uint8_t *data, size_t len);

/** Call from the USARTx_IRQHandler. */
void bsp_uart_isr(bsp_uart_t *u);

#endif /* BSP_UART_H */

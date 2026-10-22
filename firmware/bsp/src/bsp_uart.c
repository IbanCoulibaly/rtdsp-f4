/**
 * @file bsp_uart.c
 * @brief Interrupt-driven USART driver. See bsp_uart.h.
 */
#include "bsp/bsp_uart.h"

#include "task.h"

#include "bsp/bsp.h"

void bsp_uart_init(bsp_uart_t *u, const bsp_uart_cfg_t *cfg)
{
    u->regs = cfg->regs;
    u->rx_dropped = 0u;
    u->rx_throttled = 0u;
    u->rx_enabled = cfg->rx_enabled;
    u->rx = xStreamBufferCreateStatic(cfg->rx_size, 1u, cfg->rx_storage, &u->rx_cb);
    u->tx = xStreamBufferCreateStatic(cfg->tx_size, 1u, cfg->tx_storage, &u->tx_cb);
    configASSERT(u->rx != NULL && u->tx != NULL);

    if (cfg->regs == USART1) {
        RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    } else {
        RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    }

    usart_t *r = cfg->regs;
    r->CR1 = 0u;
    r->BRR = (cfg->pclk_hz + (cfg->baud / 2u)) / cfg->baud; /* 16x oversampling: BRR = fPCLK / baud */
    r->CR2 = 0u;
    r->CR3 = 0u;
    r->CR1 = USART_CR1_UE | USART_CR1_TE | (cfg->rx_enabled ? (USART_CR1_RE | USART_CR1_RXNEIE) : 0u);
    bsp_nvic_enable(cfg->irqn, cfg->irq_priority);
}

void bsp_uart_write_polled(usart_t *regs, const uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        uint32_t guard = 1000000u;
        while (((regs->SR & USART_SR_TXE) == 0u) && (guard-- > 0u)) {
        }
        regs->DR = data[i];
    }
}

size_t bsp_uart_write(bsp_uart_t *u, const uint8_t *data, size_t len, TickType_t timeout)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        bsp_uart_write_polled(u->regs, data, len);
        return len;
    }
    size_t sent = 0u;
    while (sent < len) {
        const size_t n = xStreamBufferSend(u->tx, &data[sent], len - sent, timeout);
        if (n == 0u) {
            break; /* timed out with no progress */
        }
        /* The ISR clears TXEIE when it drains the buffer; re-arm it. The
         * critical section makes the read-modify-write atomic w.r.t. the ISR. */
        taskENTER_CRITICAL();
        u->regs->CR1 |= USART_CR1_TXEIE;
        taskEXIT_CRITICAL();
        sent += n;
    }
    return sent;
}

size_t bsp_uart_read(bsp_uart_t *u, uint8_t *data, size_t max, TickType_t timeout)
{
    const size_t n = xStreamBufferReceive(u->rx, data, max, timeout);
    if (u->rx_throttled && (xStreamBufferSpacesAvailable(u->rx) > 0u)) {
        taskENTER_CRITICAL();
        u->rx_throttled = 0u;
        u->regs->CR1 |= USART_CR1_RXNEIE; /* pending RXNE re-raises the IRQ immediately */
        taskEXIT_CRITICAL();
    }
    return n;
}

void bsp_uart_isr(bsp_uart_t *u)
{
    BaseType_t woken = pdFALSE;
    usart_t *r = u->regs;
    const uint32_t sr = r->SR;
    const uint32_t cr1 = r->CR1;

    if ((cr1 & USART_CR1_RXNEIE) && (sr & (USART_SR_RXNE | USART_SR_ORE))) {
        if (xStreamBufferSpacesAvailable(u->rx) == 0u) {
            /* Leave the byte in DR and stop listening until the reader catches up. */
            r->CR1 = cr1 & ~USART_CR1_RXNEIE;
            u->rx_throttled = 1u;
        } else {
            const uint8_t b = (uint8_t)r->DR; /* SR-then-DR read also clears ORE */
            if (sr & USART_SR_ORE) {
                u->rx_dropped++;
            }
            (void)xStreamBufferSendFromISR(u->rx, &b, 1u, &woken);
        }
    }

    if ((r->CR1 & USART_CR1_TXEIE) && (sr & USART_SR_TXE)) {
        uint8_t b;
        if (xStreamBufferReceiveFromISR(u->tx, &b, 1u, &woken) == 1u) {
            r->DR = b;
        } else {
            r->CR1 &= ~USART_CR1_TXEIE;
        }
    }

    portYIELD_FROM_ISR(woken);
}

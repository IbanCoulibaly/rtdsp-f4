/**
 * @file log.c
 * @brief Console logging that never blocks the caller.
 *
 * Producers format into a fixed-size record and post it with a zero
 * timeout; if the queue is full the line is dropped and counted. The
 * low-priority log task is the only writer of the console UART. Not
 * callable from ISRs (formatting is too slow for interrupt context).
 */
#include <stdarg.h>
#include <string.h>

#include "app.h"

#include "app_services.h"
#include "mini_printf.h"

typedef struct {
    uint16_t len;
    char text[APP_LOG_LINE_MAX];
} log_line_t;

static StaticQueue_t s_qcb;
static uint8_t s_qbuf[APP_LOG_QUEUE_LEN * sizeof(log_line_t)];
static QueueHandle_t s_q;
static volatile uint32_t s_dropped;

void log_init(void)
{
    s_q = xQueueCreateStatic(APP_LOG_QUEUE_LEN, sizeof(log_line_t), s_qbuf, &s_qcb);
    configASSERT(s_q != NULL);
}

uint32_t log_dropped(void)
{
    return s_dropped;
}

void log_printf(const char *fmt, ...)
{
    log_line_t line;
    const uint32_t ms = (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ);
    int n = mini_snprintf(line.text, sizeof line.text, "[%6lu.%03lu] ", ms / 1000u, ms % 1000u);

    va_list ap;
    va_start(ap, fmt);
    n += mini_vsnprintf(&line.text[n], sizeof line.text - (size_t)n - 2u, fmt, ap);
    va_end(ap);
    if ((size_t)n > sizeof line.text - 3u) {
        n = (int)(sizeof line.text - 3u); /* truncated */
    }
    line.text[n++] = '\r';
    line.text[n++] = '\n';
    line.len = (uint16_t)n;

    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        bsp_uart_write_polled(g_uart_console.regs, (const uint8_t *)line.text, line.len);
    } else if (xQueueSend(s_q, &line, 0u) != pdTRUE) {
        s_dropped++;
    }
}

void log_task(void *arg)
{
    (void)arg;
    for (;;) {
        log_line_t line;
        if (xQueueReceive(s_q, &line, pdMS_TO_TICKS(APP_TASK_WAIT_MS)) == pdTRUE) {
            (void)bsp_uart_write(&g_uart_console, (const uint8_t *)line.text, line.len,
                                 pdMS_TO_TICKS(APP_TASK_WAIT_MS));
        }
        supervisor_heartbeat(TASK_LOG);
    }
}

/**
 * @file main.c
 * @brief Boot sequence and static creation of every RTOS object.
 *
 *   bsp_init -> crash report -> UARTs/log -> pool/queues -> default filter
 *   -> POST -> sample clock -> tasks -> BOOT frame -> scheduler
 *
 * Nothing is allocated at run time. If it boots, it has all the memory it
 * will ever use; `make fw` prints the resulting RAM/flash/CCM budget.
 */
#include "app.h"

#include "app_services.h"
#include "bsp/bsp.h"
#include "bsp/bsp_timer.h"
#include "bsp/bsp_uart.h"
#include "fault.h"
#include "fw_tables.h"
#include "link/protocol_defs.h"
#include "sample_pool.h"

/* ---- shared state (declared in app.h) ---- */
app_stats_t g_stats;
bsp_uart_t g_uart_data;
bsp_uart_t g_uart_console;
QueueHandle_t g_dsp_q;
QueueHandle_t g_tx_q;
TaskHandle_t g_task[TASK_COUNT];

/* Filter coefficients + state in zero-wait-state CCM, next to the core. */
__attribute__((section(".ccmram"))) filter_engine_t g_engines[2];
volatile uint8_t g_active_engine;
volatile uint8_t g_swap_pending;
volatile uint8_t g_mode = MODE_HOST_STREAM;
uint8_t g_post_result;

/* ---- static storage ---- */
static uint8_t s_data_rx_buf[APP_DATA_RX_BUF + 1u];
static uint8_t s_data_tx_buf[APP_DATA_TX_BUF + 1u];
static uint8_t s_console_rx_buf[16u + 1u];
static uint8_t s_console_tx_buf[APP_CONSOLE_TX_BUF + 1u];

static StaticQueue_t s_dsp_qcb;
static uint8_t s_dsp_qbuf[APP_DSP_QUEUE_LEN * sizeof(dsp_msg_t)];
static StaticQueue_t s_tx_qcb;
static uint8_t s_tx_qbuf[APP_TX_QUEUE_LEN * sizeof(tx_item_t)];

typedef struct {
    TaskFunction_t fn;
    const char *name;
    uint32_t stack_words;
    UBaseType_t prio;
} task_def_t;

static const task_def_t k_tasks[TASK_COUNT] = {
    [TASK_DSP] = {dsp_task, "dsp", STACK_DSP, PRIO_DSP},
    [TASK_LINK_RX] = {link_rx_task, "link_rx", STACK_LINK_RX, PRIO_LINK_RX},
    [TASK_LINK_TX] = {link_tx_task, "link_tx", STACK_LINK_TX, PRIO_LINK_TX},
    [TASK_SUPERVISOR] = {supervisor_task, "supervisor", STACK_SUPERVISOR, PRIO_SUPERVISOR},
    [TASK_LOG] = {log_task, "log", STACK_LOG, PRIO_LOG},
};

static StackType_t s_stack_dsp[STACK_DSP] __attribute__((aligned(8)));
static StackType_t s_stack_rx[STACK_LINK_RX] __attribute__((aligned(8)));
static StackType_t s_stack_tx[STACK_LINK_TX] __attribute__((aligned(8)));
static StackType_t s_stack_sup[STACK_SUPERVISOR] __attribute__((aligned(8)));
static StackType_t s_stack_log[STACK_LOG] __attribute__((aligned(8)));
static StackType_t *const k_stacks[TASK_COUNT] = {s_stack_dsp, s_stack_rx, s_stack_tx, s_stack_sup, s_stack_log};
static StaticTask_t s_tcb[TASK_COUNT];

void USART1_IRQHandler(void)
{
    bsp_uart_isr(&g_uart_data);
}

void USART2_IRQHandler(void)
{
    bsp_uart_isr(&g_uart_console);
}

static void uarts_init(void)
{
    const bsp_uart_cfg_t console = {
        .regs = USART2, .irqn = IRQN_USART2, .pclk_hz = BSP_APB1_HZ, .baud = APP_CONSOLE_BAUD,
        .irq_priority = IRQ_PRIO_UART_CONSOLE, .rx_enabled = 0u,
        .rx_storage = s_console_rx_buf, .rx_size = sizeof s_console_rx_buf - 1u,
        .tx_storage = s_console_tx_buf, .tx_size = sizeof s_console_tx_buf - 1u,
    };
    const bsp_uart_cfg_t data = {
        .regs = USART1, .irqn = IRQN_USART1, .pclk_hz = BSP_APB2_HZ, .baud = APP_DATA_BAUD,
        .irq_priority = IRQ_PRIO_UART_DATA, .rx_enabled = 1u,
        .rx_storage = s_data_rx_buf, .rx_size = sizeof s_data_rx_buf - 1u,
        .tx_storage = s_data_tx_buf, .tx_size = sizeof s_data_tx_buf - 1u,
    };
    bsp_uart_init(&g_uart_console, &console);
    bsp_uart_init(&g_uart_data, &data);
}

int main(void)
{
    bsp_init();
    fault_boot_init();
    uarts_init();
    log_init();

    const fault_info_t *last = fault_last();
    log_printf("RTDSP-F4 v%u.%u.%u (%s) fs=%u Hz block=%u boot#%lu", FW_VERSION_MAJOR, FW_VERSION_MINOR,
               FW_VERSION_PATCH, BSP_TARGET_QEMU ? "qemu" : "hw", APP_FS_HZ, APP_BLOCK_SIZE,
               last->boot_count);
    log_printf("last reset: %s pc=%08lx lr=%08lx cfsr=%08lx info=%08lx", fault_reason_str(last->reason),
               last->pc, last->lr, last->cfsr, last->info);

    const uint32_t tim_hz = bsp_timer_calibrate();
    log_printf("timer clock: %lu Hz measured (nominal %lu)", tim_hz, (uint32_t)BSP_TIM_CLOCK_HZ);

    pool_init();
    g_dsp_q = xQueueCreateStatic(APP_DSP_QUEUE_LEN, sizeof(dsp_msg_t), s_dsp_qbuf, &s_dsp_qcb);
    g_tx_q = xQueueCreateStatic(APP_TX_QUEUE_LEN, sizeof(tx_item_t), s_tx_qbuf, &s_tx_qcb);
    configASSERT(g_dsp_q != NULL && g_tx_q != NULL);

    g_post_result = post_run(&g_engines[1]);
    log_printf("POST %s (0x%02x)", g_post_result == 0u ? "PASS" : "FAIL", g_post_result);

    configASSERT(filter_engine_configure(&g_engines[0], &k_default_filter) == FILTER_OK);
    g_active_engine = 0u;

    bsp_sample_timer_init(APP_FS_HZ, IRQ_PRIO_SAMPLE_TIMER, siggen_isr);
    supervisor_init();

    for (uint32_t i = 0u; i < TASK_COUNT; i++) {
        g_task[i] = xTaskCreateStatic(k_tasks[i].fn, k_tasks[i].name, k_tasks[i].stack_words, NULL,
                                      k_tasks[i].prio, k_stacks[i], &s_tcb[i]);
        configASSERT(g_task[i] != NULL);
    }

    /* Queued now, transmitted by link_tx as soon as the scheduler runs. */
    uint8_t info[LINK_DEVINFO_LEN];
    (void)tx_post_inline(MSG_BOOT, 0u, info, app_build_devinfo(info), 0u);

    vTaskStartScheduler();
    configASSERT(0); /* only reached if the scheduler could not start */
    return 0;
}

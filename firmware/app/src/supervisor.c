/**
 * @file supervisor.c
 * @brief Health monitor: task heartbeats, CPU load, stack margins, IWDG.
 *
 * Two layers of watchdog:
 *   1. software: every task sets its heartbeat bit at least once per
 *      APP_TASK_WAIT_MS (all of their waits are bounded). A task silent for
 *      APP_HANG_TIMEOUT_MS is a hang -> crash record + reset, naming the task.
 *   2. hardware: the IWDG is only kicked while every task is healthy, so a
 *      hang of the supervisor itself (or of the whole scheduler) still resets.
 */
#include "app.h"

#include "app_services.h"
#include "bsp/bsp.h"
#include "event_groups.h"
#include "fault.h"
#include "link/protocol_defs.h"
#include "sample_pool.h"

#define ALL_TASK_BITS ((EventBits_t)((1u << TASK_COUNT) - 1u))

static StaticEventGroup_t s_hb_cb;
static EventGroupHandle_t s_hb;

void supervisor_init(void)
{
    s_hb = xEventGroupCreateStatic(&s_hb_cb);
    configASSERT(s_hb != NULL);
}

void supervisor_heartbeat(app_task_id_t id)
{
    (void)xEventGroupSetBits(s_hb, (EventBits_t)(1u << id));
}

static uint8_t cpu_load_pct(uint32_t idle_delta, uint32_t total_delta)
{
    /* load = 1 - idle_time / total_time, both measured in timestamp ticks. */
    if (total_delta < 100u) {
        return 0u;
    }
    uint32_t idle_pct = idle_delta / (total_delta / 100u);
    if (idle_pct > 100u) {
        idle_pct = 100u;
    }
    return (uint8_t)(100u - idle_pct);
}

void supervisor_task(void *arg)
{
    (void)arg;
    TickType_t last_seen[TASK_COUNT];
    TickType_t wake = xTaskGetTickCount();
    for (uint32_t i = 0u; i < TASK_COUNT; i++) {
        last_seen[i] = wake;
    }
    uint32_t prev_idle = ulTaskGetIdleRunTimeCounter();
    uint32_t prev_ts = bsp_timestamp();
    uint32_t since_log_ms = 0u;

    bsp_watchdog_start(APP_WATCHDOG_TIMEOUT_MS);

    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_SUPERVISOR_PERIOD_MS));
        supervisor_heartbeat(TASK_SUPERVISOR);

        const EventBits_t seen = xEventGroupClearBits(s_hb, ALL_TASK_BITS);
        const TickType_t now = xTaskGetTickCount();
        for (uint32_t i = 0u; i < TASK_COUNT; i++) {
            if (seen & (1u << i)) {
                last_seen[i] = now;
            } else if ((now - last_seen[i]) > pdMS_TO_TICKS(APP_HANG_TIMEOUT_MS)) {
                log_printf("HANG: task %lu silent for %lu ms", i, (uint32_t)(now - last_seen[i]));
                fault_record_and_reset(RESET_TASK_HANG, 0u, 0u, 0u, i);
            }
        }

        const uint32_t idle = ulTaskGetIdleRunTimeCounter();
        const uint32_t ts = bsp_timestamp();
        g_stats.cpu_load_pct = cpu_load_pct(idle - prev_idle, ts - prev_ts);
        prev_idle = idle;
        prev_ts = ts;

        for (uint32_t i = 0u; i < TASK_COUNT; i++) {
            g_stats.stack_free_words[i] = (uint16_t)uxTaskGetStackHighWaterMark(g_task[i]);
        }

        bsp_watchdog_kick();

        since_log_ms += APP_SUPERVISOR_PERIOD_MS;
        if (since_log_ms >= APP_STATS_LOG_PERIOD_MS) {
            since_log_ms = 0u;
            const uint32_t budget = BSP_CPU_CLOCK_HZ / APP_FS_HZ;
            const uint32_t cps = g_stats.cycles_per_sample_q8 >> 8;
            log_printf("stats: blocks=%lu cyc/sample=%lu/%lu (%lu%%) load=%u%% pool=%lu/%u crc_err=%lu drop=%lu",
                       g_stats.blocks_processed, cps, budget, (cps * 100u) / budget, g_stats.cpu_load_pct,
                       pool_free_count(), APP_POOL_BLOCKS, g_stats.rx_crc_errors,
                       g_stats.tx_dropped_dsp + g_stats.tx_dropped_link + g_stats.siggen_overruns);
        }
    }
}

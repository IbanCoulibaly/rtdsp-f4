/**
 * @file app_info.c
 * @brief Serialise DEVINFO and STATS payloads (layouts in protocol_defs.h).
 */
#include <string.h>

#include "app.h"

#include "app_services.h"
#include "bsp/bsp.h"
#include "fault.h"
#include "link/byteorder.h"
#include "link/protocol_defs.h"

uint16_t app_build_devinfo(uint8_t *out)
{
    const fault_info_t *last = fault_last();
    uint8_t *p = out;
    *p++ = LINK_PROTOCOL_VERSION;
    *p++ = FW_VERSION_MAJOR;
    *p++ = FW_VERSION_MINOR;
    *p++ = FW_VERSION_PATCH;
    p = put_le32(p, APP_FS_HZ);
    p = put_le16(p, APP_BLOCK_SIZE);
    p = put_le16(p, APP_BLOCK_MAX);
    p = put_le16(p, FILTER_MAX_TAPS);
    *p++ = FILTER_MAX_STAGES;
    *p++ = last->reason;
    p = put_le32(p, last->pc);
    p = put_le32(p, last->lr);
    p = put_le32(p, last->cfsr);
    p = put_le32(p, last->info);
    *p++ = g_post_result;
    *p++ = BSP_TARGET_QEMU ? 0u : 1u;
    static const char k_build[] = __DATE__; /* "Mmm dd yyyy" */
    memset(p, 0, 16u);
    memcpy(p, k_build, sizeof k_build - 1u);
    p += 16;

    const uint16_t n = (uint16_t)(p - out);
    configASSERT(n == LINK_DEVINFO_LEN);
    return n;
}

uint16_t app_build_stats(uint8_t *out)
{
    uint8_t *p = out;
    p = put_le32(p, (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ));
    p = put_le32(p, g_stats.blocks_processed);
    p = put_le32(p, g_stats.samples_processed);
    p = put_le32(p, g_stats.cycles_last_block);
    p = put_le32(p, g_stats.cycles_max_block);
    p = put_le32(p, g_stats.cycles_per_sample_q8);
    p = put_le32(p, BSP_CPU_CLOCK_HZ / APP_FS_HZ);
    p = put_le32(p, g_stats.rx_frames_ok);
    p = put_le32(p, g_stats.rx_crc_errors);
    p = put_le32(p, g_stats.rx_framing_errors);
    p = put_le32(p, g_uart_data.rx_dropped);
    p = put_le32(p, g_stats.pool_exhausted);
    p = put_le32(p, g_stats.tx_dropped_dsp + g_stats.tx_dropped_link);
    p = put_le32(p, g_stats.siggen_overruns);
    p = put_le32(p, g_stats.filter_swaps);
    for (uint32_t i = 0u; i < TASK_COUNT; i++) {
        p = put_le16(p, g_stats.stack_free_words[i]);
    }
    *p++ = g_engines[g_active_engine].desc.type;
    *p++ = g_stats.cpu_load_pct;
    *p++ = g_mode;
    *p++ = 0u;

    const uint16_t n = (uint16_t)(p - out);
    configASSERT(n == LINK_STATS_LEN);
    return n;
}

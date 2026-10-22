/**
 * @file protocol_defs.h
 * @brief Host <-> device wire protocol: the single source of truth.
 *
 * python/rtdsp/protocol.py mirrors these values and
 * python/tests/test_contract.py parses this header to prove they agree.
 *
 * Frame (before COBS):  [type u8][seq u8][payload 0..LINK_MAX_PAYLOAD][crc16 LE]
 * On the wire:          COBS(frame) 0x00
 * CRC: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type..payload.
 * All multi-byte fields are little-endian.
 */
#ifndef LINK_PROTOCOL_DEFS_H
#define LINK_PROTOCOL_DEFS_H

#define LINK_PROTOCOL_VERSION 1

#define LINK_MAX_PAYLOAD 600
#define LINK_FRAME_OVERHEAD 4 /* type + seq + crc16 */

/* ---- host -> device ---------------------------------------------------- */
#define MSG_PING 0x01        /* -> MSG_PONG(devinfo) */
#define MSG_SET_FILTER 0x02  /* filter descriptor -> MSG_ACK, sent after the swap */
#define MSG_PROCESS 0x03     /* n * q15 -> MSG_PROCESS_RESULT(n * q15) */
#define MSG_GET_STATS 0x04   /* -> MSG_STATS */
#define MSG_RESET_STATE 0x05 /* -> MSG_ACK (delay lines cleared, in stream order) */
#define MSG_SET_MODE 0x06    /* mode u8 [+ siggen params] -> MSG_ACK */
#define MSG_DEBUG_FAULT 0x7E /* kind u8: deliberately crash (debug builds only) */

/* ---- device -> host ---------------------------------------------------- */
#define MSG_PONG 0x81
#define MSG_ACK 0x82 /* [acked type u8][status u8] */
#define MSG_PROCESS_RESULT 0x83
#define MSG_STATS 0x84
#define MSG_STREAM 0x85 /* siggen mode: [n * q15 in][n * q15 out] */
#define MSG_BOOT 0x87   /* sent once at boot: devinfo */

/* ---- ACK status codes -------------------------------------------------- */
#define ST_OK 0
#define ST_ERR_FILTER_TYPE 1
#define ST_ERR_FILTER_ORDER 2
#define ST_ERR_FILTER_LEN 3
#define ST_ERR_FILTER_PARAM 4
#define ST_ERR_UNSTABLE 5
#define ST_ERR_BUSY 6
#define ST_ERR_MODE 7
#define ST_ERR_UNKNOWN_MSG 8
#define ST_ERR_LEN 9

/* ---- modes ------------------------------------------------------------- */
#define MODE_HOST_STREAM 0
#define MODE_SIGGEN 1

/* SET_MODE payload: mode u8, f1_hz u16, a1 q15, f2_hz u16, a2 q15, noise q15 */
#define LINK_SET_MODE_LEN 11

/* ---- debug fault kinds ------------------------------------------------- */
#define DBG_FAULT_UDF 1    /* undefined instruction -> UsageFault */
#define DBG_FAULT_ASSERT 2 /* configASSERT(0) */

/* ---- reset reasons (devinfo.reset_reason) ------------------------------ */
#define RESET_POWER_ON 0
#define RESET_SOFTWARE 1
#define RESET_WATCHDOG 2
#define RESET_FAULT 3
#define RESET_ASSERT 4
#define RESET_STACK_OVERFLOW 5
#define RESET_TASK_HANG 6
#define RESET_PIN 7

/* ---- POST result bits (devinfo.post_result, 0 = pass) ------------------ */
#define POST_FAIL_CRC_ENGINE 0x01
#define POST_FAIL_FIR_Q15 0x02
#define POST_FAIL_BIQUAD_Q31 0x04
#define POST_FAIL_FIR_F32 0x08
#define POST_FAIL_CLOCK 0x10 /* timer clock off nominal by > 2 % (hardware only) */

/*
 * DEVINFO payload (PONG, BOOT) -- python struct "<4BI3H2B4I2B16s":
 *   u8  protocol_version, fw_major, fw_minor, fw_patch
 *   u32 fs_hz
 *   u16 block_size, block_max, max_taps
 *   u8  max_stages, reset_reason
 *   u32 fault_pc, fault_lr, fault_cfsr, fault_info
 *   u8  post_result, target (0 = qemu, 1 = hardware)
 *   char build[16]
 */
#define LINK_DEVINFO_LEN 50

/*
 * STATS payload -- python struct "<15I5H4B":
 *   u32 uptime_ms, blocks_processed, samples_processed,
 *       cycles_last_block, cycles_max_block,
 *       cycles_per_sample_q8 (EMA, x256), cycles_budget_per_sample,
 *       rx_frames_ok, rx_crc_errors, rx_framing_errors, rx_uart_dropped,
 *       pool_exhausted, tx_dropped, siggen_overruns, filter_swaps
 *   u16 stack_free_words[5]  (dsp, link_rx, link_tx, supervisor, log)
 *   u8  active_filter_type, cpu_load_pct, mode, reserved
 */
#define LINK_STATS_LEN 74

#endif /* LINK_PROTOCOL_DEFS_H */

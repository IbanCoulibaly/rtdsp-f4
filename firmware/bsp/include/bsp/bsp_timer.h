/**
 * @file bsp_timer.h
 * @brief TIM2 as the sample clock: one update interrupt every 1/fs seconds.
 *
 * On a production board TIM2 would trigger the ADC through TRGO and DMA
 * would move the samples; here the ISR itself produces the sample (see
 * app/src/siggen.c), which exercises the same ISR -> RTOS hand-off.
 * On QEMU the sample clock is derived from SysTick instead (see bsp_timer.c).
 */
#ifndef BSP_TIMER_H
#define BSP_TIMER_H

#include <stdint.h>

#include "bsp/bsp.h"

typedef void (*bsp_sample_cb_t)(void);

/** Measure the timer input clock against SysTick (core cycles). Call once
 *  before the scheduler starts, since it borrows SysTick. On hardware this
 *  is also a clock-tree sanity check (POST); on QEMU it discovers the
 *  emulator's timer rate, which is not the silicon's. */
uint32_t bsp_timer_calibrate(void);
uint32_t bsp_timer_clock_hz(void);

/** Program ARR = f_tim / fs - 1 (calibrated f_tim) and hook the callback. Timer stays stopped. */
void bsp_sample_timer_init(uint32_t fs_hz, uint8_t irq_priority, bsp_sample_cb_t cb);
void bsp_sample_timer_start(void);
void bsp_sample_timer_stop(void);

#if BSP_TARGET_QEMU
/** QEMU build only: sample-clock substitute, called from the SysTick hook. */
void bsp_sample_timer_tick_isr(void);
#endif

#endif /* BSP_TIMER_H */

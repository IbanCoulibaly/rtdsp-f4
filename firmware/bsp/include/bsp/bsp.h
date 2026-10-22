/**
 * @file bsp.h
 * @brief Board support: clocks, fault traps, timebase, reset, watchdog, NVIC.
 *
 * Everything above this layer is target-independent. The two supported
 * targets differ only in a handful of BSP decisions, selected at compile
 * time with BSP_TARGET_QEMU:
 *
 *   setting          QEMU netduinoplus2            real STM32F405
 *   ---------------  ----------------------------  ------------------------
 *   SYSCLK           fixed 168 MHz by the machine  HSE 8 MHz -> PLL 168 MHz
 *   timer clock      measured at boot (emulator    84 MHz (APB1 x2), checked
 *                    rate, not the silicon's)      against SysTick by POST
 *   cycle counter    TIM5 free-running, scaled     DWT->CYCCNT
 *   IWDG             not modelled (writes ignored) real watchdog
 */
#ifndef BSP_H
#define BSP_H

#include <stdint.h>

#ifndef BSP_TARGET_QEMU
#define BSP_TARGET_QEMU 0
#endif

#define BSP_CPU_CLOCK_HZ 168000000u
#define BSP_TIM_CLOCK_HZ 84000000u /* nominal; bsp_timer_calibrate() measures the real one */
#if !BSP_TARGET_QEMU
#define BSP_HSE_HZ 8000000u
#endif
#define BSP_APB1_HZ 42000000u
#define BSP_APB2_HZ 84000000u

/** Bring up clocks, fault traps, GPIO alternate functions and the timebase. */
void bsp_init(void);

/** Free-running timestamp (wraps). Use only differences. */
uint32_t bsp_timestamp(void);

/** Convert a bsp_timestamp() difference to CPU cycles. */
uint32_t bsp_timestamp_to_cycles(uint32_t delta);

/** Hardware reset cause (RESET_* from protocol_defs.h); clears the flags. */
uint8_t bsp_reset_cause(void);

/** Request a system reset through AIRCR.SYSRESETREQ. */
void bsp_system_reset(void) __attribute__((noreturn));

void bsp_watchdog_start(uint32_t timeout_ms);
void bsp_watchdog_kick(void);

/** Enable an IRQ at a given priority (0 = most urgent, 15 = least). */
void bsp_nvic_enable(uint32_t irq, uint8_t priority);
void bsp_nvic_disable(uint32_t irq);

static inline void bsp_dsb(void)
{
    __asm volatile("dsb 0xF" ::: "memory");
}

static inline void bsp_isb(void)
{
    __asm volatile("isb 0xF" ::: "memory");
}

#endif /* BSP_H */

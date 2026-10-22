/**
 * @file bsp_timer.c
 * @brief TIM2 sample clock.
 */
#include "bsp/bsp_timer.h"

#include "bsp/bsp.h"
#include "bsp/stm32f4_regs.h"

static bsp_sample_cb_t s_cb;
static uint32_t s_tim_hz = BSP_TIM_CLOCK_HZ;

/* 8M core cycles (~48 ms): long enough for 0.01 % resolution at 84 MHz,
 * short enough to fit SysTick's 24-bit counter without a wrap. */
#define CAL_CYCLES 8000000u

uint32_t bsp_timer_calibrate(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->CR1 = 0u;
    TIM2->DIER = 0u;
    TIM2->PSC = 0u;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CNT = 0u;
    TIM2->CR1 = TIM_CR1_CEN;

    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0u;
    SYST_CSR = 5u; /* enable, clock = core, no interrupt */
    const uint32_t s0 = SYST_CVR;
    const uint32_t t0 = TIM2->CNT;
    uint32_t elapsed;
    do {
        elapsed = (s0 - SYST_CVR) & 0x00FFFFFFu; /* SysTick counts down */
    } while (elapsed < CAL_CYCLES);
    const uint32_t t1 = TIM2->CNT;
    SYST_CSR = 0u;
    TIM2->CR1 = 0u;

    /* f_tim = timer ticks / elapsed core cycles * f_cpu */
    const uint64_t hz = ((uint64_t)(t1 - t0) * BSP_CPU_CLOCK_HZ) / elapsed;
    if ((hz > 1000000u) && (hz < 4000000000u)) {
        s_tim_hz = (uint32_t)hz;
    }
    return s_tim_hz;
}

uint32_t bsp_timer_clock_hz(void)
{
    return s_tim_hz;
}

#if BSP_TARGET_QEMU
/*
 * QEMU 11.1's stm32f2xx_timer model arms its next alarm at
 * "now + absolute deadline" instead of "now + delay", so periodic update
 * interrupts drift ever further apart (and CNT never reloads at ARR). The
 * QEMU build therefore derives the sample clock from SysTick, which QEMU
 * models correctly: each 1 ms tick delivers fs / tick_rate samples from
 * interrupt context, exactly like a DMA half-transfer interrupt would. The
 * ISR -> pool -> DSP-queue path is identical; only the burst size differs.
 */
static volatile uint8_t s_running;
static uint32_t s_fs_hz;
static uint32_t s_tick_hz;
static uint32_t s_phase;

void bsp_sample_timer_init(uint32_t fs_hz, uint8_t irq_priority, bsp_sample_cb_t cb)
{
    (void)irq_priority;
    s_cb = cb;
    s_fs_hz = fs_hz;
    s_tick_hz = 1000u;
}

void bsp_sample_timer_start(void)
{
    s_phase = 0u;
    s_running = 1u;
}

void bsp_sample_timer_stop(void)
{
    s_running = 0u;
}

void bsp_sample_timer_tick_isr(void)
{
    if (!s_running) {
        return;
    }
    /* Bresenham-style accumulator: exactly fs samples per second even when
     * fs is not a multiple of the tick rate. */
    s_phase += s_fs_hz;
    while (s_phase >= s_tick_hz) {
        s_phase -= s_tick_hz;
        s_cb();
    }
}

#else

void bsp_sample_timer_init(uint32_t fs_hz, uint8_t irq_priority, bsp_sample_cb_t cb)
{
    s_cb = cb;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->CR1 = TIM_CR1_URS; /* only overflow raises UIF, not the UG below */
    TIM2->PSC = 0u;
    /* f_update = f_tim / ((PSC + 1) * (ARR + 1)), rounded to the nearest count. */
    TIM2->ARR = ((s_tim_hz + (fs_hz / 2u)) / fs_hz) - 1u;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0u;
    TIM2->DIER = TIM_DIER_UIE;
    bsp_nvic_enable(IRQN_TIM2, irq_priority);
}

void bsp_sample_timer_start(void)
{
    TIM2->CNT = 0u;
    TIM2->SR = 0u;
    TIM2->CR1 |= TIM_CR1_CEN;
}

void bsp_sample_timer_stop(void)
{
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->SR = 0u;
    NVIC_ICPR(IRQN_TIM2 >> 5) = 1u << (IRQN_TIM2 & 31u);
}

void TIM2_IRQHandler(void)
{
    TIM2->SR = ~TIM_SR_UIF; /* rc_w0: write 0 to clear, 1 leaves others alone */
    if (s_cb != 0) {
        s_cb();
    }
}

#endif

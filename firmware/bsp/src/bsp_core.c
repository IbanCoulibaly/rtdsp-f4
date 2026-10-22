/**
 * @file bsp_core.c
 * @brief Clocks, fault configuration, timebase, reset and watchdog.
 */
#include "bsp/bsp.h"

#include "bsp/bsp_timer.h"
#include "bsp/stm32f4_regs.h"
#include "link/protocol_defs.h"

#if !BSP_TARGET_QEMU
/* Every hardware wait is bounded: a dead crystal must not hang boot. */
#define CLOCK_WAIT_LOOPS 500000u

static int wait_set(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    for (uint32_t i = 0u; i < CLOCK_WAIT_LOOPS; i++) {
        if ((*reg & mask) == value) {
            return 1;
        }
    }
    return 0;
}

/* HSE 8 MHz -> PLL: VCO = HSE / M * N = 8 / 8 * 336 = 336 MHz
 *                   SYSCLK = VCO / P = 168 MHz, USB/SDIO = VCO / Q = 48 MHz
 * AHB = 168 MHz, APB1 = 42 MHz (timers x2 = 84 MHz), APB2 = 84 MHz. */
static void clock_init(void)
{
    RCC->CR |= RCC_CR_HSEON;
    if (!wait_set(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY)) {
        return; /* stay on HSI 16 MHz; the supervisor will report a wrong clock */
    }
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR_CR |= (1u << 14);                                  /* VOS: scale 1 for 168 MHz */
    FLASH_ACR = (1u << 8) | (1u << 9) | (1u << 10) | 5u;   /* prefetch, I/D cache, 5 wait states */
    RCC->CFGR = (0u << 4) | (5u << 10) | (4u << 13);       /* HPRE /1, PPRE1 /4, PPRE2 /2 */
    RCC->PLLCFGR = 8u | (336u << 6) | (((2u / 2u) - 1u) << 16) | (1u << 22) | (7u << 24);
    RCC->CR |= RCC_CR_PLLON;
    if (!wait_set(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) {
        return;
    }
    RCC->CFGR = (RCC->CFGR & ~3u) | RCC_CFGR_SW_PLL;
    (void)wait_set(&RCC->CFGR, RCC_CFGR_SWS_MASK, RCC_CFGR_SWS_PLL);
}
#endif

static void gpio_af(gpio_t *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3u << (2u * pin))) | (2u << (2u * pin));
    port->OSPEEDR |= (3u << (2u * pin));
    volatile uint32_t *afr = &port->AFR[pin >> 3];
    const uint32_t shift = 4u * (pin & 7u);
    *afr = (*afr & ~(0xFu << shift)) | (af << shift);
}

static void timebase_init(void)
{
#if BSP_TARGET_QEMU
    /* QEMU does not model the DWT; a 32-bit timer counting at the timer clock stands in. */
    RCC->APB1ENR |= RCC_APB1ENR_TIM5EN;
    TIM5->PSC = 0u;
    TIM5->ARR = 0xFFFFFFFFu;
    TIM5->EGR = TIM_EGR_UG;
    TIM5->CR1 = TIM_CR1_CEN;
#else
    DEMCR |= DEMCR_TRCENA;
    DWT_LAR = 0xC5ACCE55u; /* unlock on parts that implement the lock */
    DWT_CYCCNT = 0u;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
#endif
}

void bsp_init(void)
{
#if !BSP_TARGET_QEMU
    clock_init();
#endif
    /* All 4 priority bits are pre-emption priority (FreeRTOS requirement). */
    SCB->AIRCR = SCB_AIRCR_VECTKEY | (SCB->AIRCR & ~(SCB_AIRCR_PRIGROUP_MASK | 0xFFFF0000u)) | (3u << 8);
    /* Route MemManage/BusFault/UsageFault to their own handlers instead of
     * escalating to HardFault, and trap integer division by zero. */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA | SCB_SHCSR_BUSFAULTENA | SCB_SHCSR_USGFAULTENA;
    SCB->CCR |= SCB_CCR_DIV_0_TRP;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    gpio_af(GPIOA, 9u, 7u);  /* USART1_TX */
    gpio_af(GPIOA, 10u, 7u); /* USART1_RX */
    gpio_af(GPIOA, 2u, 7u);  /* USART2_TX */
    gpio_af(GPIOA, 3u, 7u);  /* USART2_RX */

    timebase_init();
}

uint32_t bsp_timestamp(void)
{
#if BSP_TARGET_QEMU
    return TIM5->CNT;
#else
    return DWT_CYCCNT;
#endif
}

uint32_t bsp_timestamp_to_cycles(uint32_t delta)
{
#if BSP_TARGET_QEMU
    return (uint32_t)(((uint64_t)delta * BSP_CPU_CLOCK_HZ) / bsp_timer_clock_hz());
#else
    return delta;
#endif
}

uint8_t bsp_reset_cause(void)
{
    const uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;
    if (csr & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) {
        return RESET_WATCHDOG;
    }
    if (csr & RCC_CSR_SFTRSTF) {
        return RESET_SOFTWARE;
    }
    if (csr & RCC_CSR_PORRSTF) {
        return RESET_POWER_ON;
    }
    if (csr & RCC_CSR_PINRSTF) {
        return RESET_PIN;
    }
    return RESET_POWER_ON; /* QEMU: RCC is not modelled, reads as 0 */
}

void bsp_system_reset(void)
{
    bsp_dsb();
    SCB->AIRCR = SCB_AIRCR_VECTKEY | (SCB->AIRCR & SCB_AIRCR_PRIGROUP_MASK) | SCB_AIRCR_SYSRESETREQ;
    bsp_dsb();
    for (;;) {
    }
}

void bsp_watchdog_start(uint32_t timeout_ms)
{
    /* LSI ~32 kHz, prescaler /64 -> 2 ms per count, 12-bit reload. */
    uint32_t reload = timeout_ms / 2u;
    if (reload > 0xFFFu) {
        reload = 0xFFFu;
    }
    IWDG_KR = 0x5555u; /* unlock PR/RLR */
    IWDG_PR = 4u;      /* /64 */
    IWDG_RLR = reload;
    IWDG_KR = 0xCCCCu; /* start: from here on it cannot be stopped */
    IWDG_KR = 0xAAAAu;
}

void bsp_watchdog_kick(void)
{
    IWDG_KR = 0xAAAAu;
}

void bsp_nvic_enable(uint32_t irq, uint8_t priority)
{
    NVIC_IPR(irq) = (uint8_t)(priority << 4); /* STM32 implements the top 4 bits */
    NVIC_ICPR(irq >> 5) = 1u << (irq & 31u);
    NVIC_ISER(irq >> 5) = 1u << (irq & 31u);
}

void bsp_nvic_disable(uint32_t irq)
{
    NVIC_ICER(irq >> 5) = 1u << (irq & 31u);
    bsp_dsb();
    bsp_isb();
}

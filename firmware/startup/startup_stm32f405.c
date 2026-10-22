/**
 * @file startup_stm32f405.c
 * @brief Vector table and reset handler, in C (no assembly startup file).
 *
 * Reset sequence:
 *   1. enable the FPU (CP10/CP11 full access) before any float instruction
 *   2. copy .data from flash to RAM, zero .bss and .ccmram
 *   3. point VTOR at the flash vector table
 *   4. main()
 */
#include <stdint.h>

#include "bsp/stm32f4_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _sccm, _eccm, _estack;
extern int main(void);
extern void fault_unexpected_irq(void) __attribute__((noreturn));

void Reset_Handler(void);
void Default_Handler(void);

/* Fault handlers live in app/src/fault.c; FreeRTOS provides SVC/PendSV/SysTick. */
void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void TIM2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM5_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

typedef void (*vector_t)(void);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used)) static const vector_t k_vectors[16u + IRQ_COUNT] = {
    [0] = (vector_t)(uintptr_t)&_estack,
    [1] = Reset_Handler,
    [2] = NMI_Handler,
    [3] = HardFault_Handler,
    [4] = MemManage_Handler,
    [5] = BusFault_Handler,
    [6] = UsageFault_Handler,
    [11] = SVC_Handler,
    [12] = DebugMon_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    [16 ... 16u + IRQ_COUNT - 1u] = Default_Handler,
    [16u + IRQN_TIM2] = TIM2_IRQHandler,
    [16u + IRQN_TIM5] = TIM5_IRQHandler,
    [16u + IRQN_USART1] = USART1_IRQHandler,
    [16u + IRQN_USART2] = USART2_IRQHandler,
};
#pragma GCC diagnostic pop

/* Any IRQ without a driver is a configuration bug: record it and reset
 * rather than spinning silently. */
void Default_Handler(void)
{
    fault_unexpected_irq();
}

void Reset_Handler(void)
{
    SCB_CPACR |= (0xFu << 20); /* CP10 + CP11 full access */
    __asm volatile("dsb\n isb" ::: "memory");

    const uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) {
        *dst++ = *src++;
    }
    for (uint32_t *dst = &_sbss; dst < &_ebss;) {
        *dst++ = 0u;
    }
    for (uint32_t *dst = &_sccm; dst < &_eccm;) {
        *dst++ = 0u;
    }

    SCB->VTOR = (uint32_t)(uintptr_t)k_vectors;
    __asm volatile("dsb\n isb" ::: "memory");

    (void)main();
    for (;;) {
    }
}

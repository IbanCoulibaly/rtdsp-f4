/**
 * @file stm32f4_regs.h
 * @brief Minimal STM32F405 / Cortex-M4 register map (RM0090, ARM DDI0403).
 *
 * Only the peripherals this firmware touches. Written from the reference
 * manual instead of pulling in the vendor HAL so every register access in
 * the project is visible and reviewable; offsets are pinned with static
 * asserts so a typo in a struct cannot silently shift a register.
 */
#ifndef BSP_STM32F4_REGS_H
#define BSP_STM32F4_REGS_H

#include <stddef.h>
#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ------------------------------------------------------------- Cortex-M4 */
typedef struct {
    volatile uint32_t CPUID, ICSR, VTOR, AIRCR, SCR, CCR;
    volatile uint8_t SHPR[12];
    volatile uint32_t SHCSR, CFSR, HFSR, DFSR, MMFAR, BFAR, AFSR;
} scb_t;
_Static_assert(offsetof(scb_t, CFSR) == 0x28, "SCB layout");
#define SCB ((scb_t *)0xE000ED00u)
#define SCB_CPACR REG32(0xE000ED88u)
#define SCB_AIRCR_VECTKEY (0x05FAu << 16)
#define SCB_AIRCR_SYSRESETREQ (1u << 2)
#define SCB_AIRCR_PRIGROUP_MASK (7u << 8)
#define SCB_CCR_DIV_0_TRP (1u << 4)
#define SCB_SHCSR_MEMFAULTENA (1u << 16)
#define SCB_SHCSR_BUSFAULTENA (1u << 17)
#define SCB_SHCSR_USGFAULTENA (1u << 18)

#define NVIC_ISER(n) REG32(0xE000E100u + 4u * (n))
#define NVIC_ICER(n) REG32(0xE000E180u + 4u * (n))
#define NVIC_ICPR(n) REG32(0xE000E280u + 4u * (n))
#define NVIC_IPR(irq) (*(volatile uint8_t *)(0xE000E400u + (irq)))

#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)

#define DEMCR REG32(0xE000EDFCu)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL REG32(0xE0001000u)
#define DWT_CYCCNT REG32(0xE0001004u)
#define DWT_LAR REG32(0xE0001FB0u)
#define DWT_CTRL_CYCCNTENA 1u

/* ------------------------------------------------------------------ RCC */
typedef struct {
    volatile uint32_t CR, PLLCFGR, CFGR, CIR, AHB1RSTR, AHB2RSTR, AHB3RSTR, RES0;
    volatile uint32_t APB1RSTR, APB2RSTR, RES1[2], AHB1ENR, AHB2ENR, AHB3ENR, RES2;
    volatile uint32_t APB1ENR, APB2ENR, RES3[2], AHB1LPENR, AHB2LPENR, AHB3LPENR, RES4;
    volatile uint32_t APB1LPENR, APB2LPENR, RES5[2], BDCR, CSR;
} rcc_t;
_Static_assert(offsetof(rcc_t, AHB1ENR) == 0x30, "RCC layout");
_Static_assert(offsetof(rcc_t, APB1ENR) == 0x40, "RCC layout");
_Static_assert(offsetof(rcc_t, CSR) == 0x74, "RCC layout");
#define RCC ((rcc_t *)0x40023800u)
#define RCC_CR_HSEON (1u << 16)
#define RCC_CR_HSERDY (1u << 17)
#define RCC_CR_PLLON (1u << 24)
#define RCC_CR_PLLRDY (1u << 25)
#define RCC_CFGR_SW_PLL 2u
#define RCC_CFGR_SWS_MASK (3u << 2)
#define RCC_CFGR_SWS_PLL (2u << 2)
#define RCC_AHB1ENR_GPIOAEN (1u << 0)
#define RCC_APB1ENR_TIM2EN (1u << 0)
#define RCC_APB1ENR_TIM5EN (1u << 3)
#define RCC_APB1ENR_USART2EN (1u << 17)
#define RCC_APB1ENR_PWREN (1u << 28)
#define RCC_APB2ENR_USART1EN (1u << 4)
#define RCC_CSR_RMVF (1u << 24)
#define RCC_CSR_PINRSTF (1u << 26)
#define RCC_CSR_PORRSTF (1u << 27)
#define RCC_CSR_SFTRSTF (1u << 28)
#define RCC_CSR_IWDGRSTF (1u << 29)
#define RCC_CSR_WWDGRSTF (1u << 30)

#define FLASH_ACR REG32(0x40023C00u)
#define PWR_CR REG32(0x40007000u)

/* ----------------------------------------------------------------- GPIO */
typedef struct {
    volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2];
} gpio_t;
_Static_assert(offsetof(gpio_t, AFR) == 0x20, "GPIO layout");
#define GPIOA ((gpio_t *)0x40020000u)

/* ---------------------------------------------------------------- USART */
typedef struct {
    volatile uint32_t SR, DR, BRR, CR1, CR2, CR3, GTPR;
} usart_t;
_Static_assert(offsetof(usart_t, CR1) == 0x0C, "USART layout");
#define USART1 ((usart_t *)0x40011000u)
#define USART2 ((usart_t *)0x40004400u)
#define USART_SR_ORE (1u << 3)
#define USART_SR_RXNE (1u << 5)
#define USART_SR_TC (1u << 6)
#define USART_SR_TXE (1u << 7)
#define USART_CR1_RE (1u << 2)
#define USART_CR1_TE (1u << 3)
#define USART_CR1_RXNEIE (1u << 5)
#define USART_CR1_TXEIE (1u << 7)
#define USART_CR1_UE (1u << 13)

/* ------------------------------------------------------------ TIM2..5 */
typedef struct {
    volatile uint32_t CR1, CR2, SMCR, DIER, SR, EGR, CCMR1, CCMR2, CCER, CNT, PSC, ARR;
} tim_t;
_Static_assert(offsetof(tim_t, ARR) == 0x2C, "TIM layout");
#define TIM2 ((tim_t *)0x40000000u)
#define TIM5 ((tim_t *)0x40000C00u)
#define TIM_CR1_CEN (1u << 0)
#define TIM_CR1_URS (1u << 2)
#define TIM_DIER_UIE (1u << 0)
#define TIM_SR_UIF (1u << 0)
#define TIM_EGR_UG (1u << 0)

/* ----------------------------------------------------------------- IWDG */
#define IWDG_KR REG32(0x40003000u)
#define IWDG_PR REG32(0x40003004u)
#define IWDG_RLR REG32(0x40003008u)

/* ------------------------------------------------------------ IRQ numbers */
#define IRQN_TIM2 28u
#define IRQN_TIM5 50u
#define IRQN_USART1 37u
#define IRQN_USART2 38u
#define IRQ_COUNT 82u

#endif /* BSP_STM32F4_REGS_H */

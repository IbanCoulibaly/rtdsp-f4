/**
 * @file rtos_hooks.c
 * @brief Application-provided FreeRTOS hooks (static memory, idle).
 *
 * Stack-overflow and assert hooks live in app/src/fault.c.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "bsp/bsp_timer.h"

static StaticTask_t s_idle_tcb;
static StackType_t s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, configSTACK_DEPTH_TYPE *depth)
{
    *tcb = &s_idle_tcb;
    *stack = s_idle_stack;
    *depth = configMINIMAL_STACK_SIZE;
}

/* Sleep until the next interrupt: the idle time measured by the run-time
 * counter is then genuinely idle, and the core draws far less current. */
void vApplicationIdleHook(void)
{
    __asm volatile("dsb\n wfi\n isb" ::: "memory");
}

#if configUSE_TICK_HOOK
/* Runs inside the SysTick ISR (FromISR APIs only). */
void vApplicationTickHook(void)
{
    bsp_sample_timer_tick_isr();
}
#endif

/**
 * @file fault.c
 * @brief Fault handlers, assert/stack-overflow hooks, persistent crash record.
 */
#include "fault.h"

#include "FreeRTOS.h"
#include "task.h"

#include "app.h"
#include "bsp/bsp.h"
#include "bsp/stm32f4_regs.h"
#include "link/protocol_defs.h"
#include "mini_printf.h"

#define CRASH_MAGIC 0xC0FFEE42u
#define BOOT_MAGIC 0xB007B007u

typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    uint32_t cfsr;
    uint32_t info;
    uint32_t check;
} crash_record_t;

/* .noinit: survives SYSRESETREQ, not zeroed by Reset_Handler. */
__attribute__((section(".noinit"))) static crash_record_t s_crash;
__attribute__((section(".noinit"))) static uint32_t s_boot_magic;
__attribute__((section(".noinit"))) static uint32_t s_boot_count;

static fault_info_t s_last;

static uint32_t record_check(const crash_record_t *r)
{
    return r->magic ^ r->reason ^ r->pc ^ r->lr ^ r->cfsr ^ r->info ^ 0xA5A5A5A5u;
}

void fault_boot_init(void)
{
    const uint8_t hw_cause = bsp_reset_cause();
    if (s_boot_magic != BOOT_MAGIC) {
        s_boot_magic = BOOT_MAGIC;
        s_boot_count = 0u;
    }
    s_boot_count++;
    s_last.boot_count = s_boot_count;

    if ((s_crash.magic == CRASH_MAGIC) && (s_crash.check == record_check(&s_crash))) {
        s_last.reason = (uint8_t)s_crash.reason;
        s_last.pc = s_crash.pc;
        s_last.lr = s_crash.lr;
        s_last.cfsr = s_crash.cfsr;
        s_last.info = s_crash.info;
    } else {
        s_last.reason = hw_cause;
    }
    s_crash.magic = 0u; /* consume */
}

const fault_info_t *fault_last(void)
{
    return &s_last;
}

const char *fault_reason_str(uint8_t reason)
{
    switch (reason) {
    case RESET_POWER_ON:
        return "power-on";
    case RESET_SOFTWARE:
        return "software";
    case RESET_WATCHDOG:
        return "watchdog";
    case RESET_FAULT:
        return "FAULT";
    case RESET_ASSERT:
        return "ASSERT";
    case RESET_STACK_OVERFLOW:
        return "STACK OVERFLOW";
    case RESET_TASK_HANG:
        return "TASK HANG";
    case RESET_PIN:
        return "reset pin";
    default:
        return "?";
    }
}

void fault_record_and_reset(uint8_t reason, uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t info)
{
    __asm volatile("cpsid i" ::: "memory");
    s_crash.magic = CRASH_MAGIC;
    s_crash.reason = reason;
    s_crash.pc = pc;
    s_crash.lr = lr;
    s_crash.cfsr = cfsr;
    s_crash.info = info;
    s_crash.check = record_check(&s_crash);

    /* Last words on the console, polled: the log task may be the one that died. */
    char msg[96];
    const int n = mini_snprintf(msg, sizeof msg, "\r\n*** %s pc=%08lx lr=%08lx cfsr=%08lx info=%08lx -> reset\r\n",
                                fault_reason_str(reason), pc, lr, cfsr, info);
    bsp_uart_write_polled(USART2, (const uint8_t *)msg, (size_t)n);

    bsp_system_reset();
}

static int addr_in_ram(uint32_t a)
{
    return ((a >= 0x20000000u) && (a < 0x20020000u - 32u)) || ((a >= 0x10000000u) && (a < 0x10010000u - 32u));
}

/* Called by the naked handler with the stacked exception frame:
 * frame[0..7] = r0 r1 r2 r3 r12 lr pc xpsr. */
__attribute__((used)) void fault_exception_c(const uint32_t *frame, uint32_t exc_return)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    uint32_t pc = 0u;
    uint32_t lr = exc_return;
    if (addr_in_ram((uint32_t)(uintptr_t)frame)) { /* do not double-fault on a smashed SP */
        pc = frame[6];
        lr = frame[5];
    }
    const uint32_t cfsr = SCB->CFSR;
    /* info: exception number (3 Hard, 4 MemManage, 5 Bus, 6 Usage) | fault address if valid */
    uint32_t info = ipsr & 0x1FFu;
    if (cfsr & (1u << 15)) {
        info = SCB->BFAR; /* BFARVALID */
    } else if (cfsr & (1u << 7)) {
        info = SCB->MMFAR; /* MMARVALID */
    }
    fault_record_and_reset(RESET_FAULT, pc, lr, cfsr, info);
}

/* Pick the stack that was in use (EXC_RETURN bit 2: 0 = MSP, 1 = PSP). */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile("tst lr, #4      \n"
                   "ite eq          \n"
                   "mrseq r0, msp   \n"
                   "mrsne r0, psp   \n"
                   "mov r1, lr      \n"
                   "b fault_exception_c \n");
}

void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));

void fault_unexpected_irq(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    fault_record_and_reset(RESET_FAULT, 0u, 0u, 0u, 0x10000u | (ipsr & 0x1FFu));
}

void app_assert_failed(const char *file, int line)
{
    /* The file-name pointer stays valid across the reset (same flash image). */
    uint32_t lr;
    __asm volatile("mov %0, lr" : "=r"(lr));
    fault_record_and_reset(RESET_ASSERT, (uint32_t)(uintptr_t)file, lr, 0u, (uint32_t)line);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    uint32_t tag = 0u; /* first 4 chars of the task name */
    for (uint32_t i = 0u; (i < 4u) && (name[i] != '\0'); i++) {
        tag |= (uint32_t)(uint8_t)name[i] << (8u * i);
    }
    fault_record_and_reset(RESET_STACK_OVERFLOW, 0u, 0u, 0u, tag);
}

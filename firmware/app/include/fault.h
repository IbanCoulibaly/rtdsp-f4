/**
 * @file fault.h
 * @brief Crash capture: fault/assert/stack-overflow -> record -> reset -> report.
 *
 * The record lives in .noinit RAM, which the startup code does not clear,
 * so it survives the software reset and is sent to the host in the BOOT
 * frame of the next boot. A check word guards against reading garbage
 * after a real power cycle.
 */
#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

typedef struct {
    uint8_t reason; /**< RESET_* from protocol_defs.h */
    uint32_t pc;
    uint32_t lr;
    uint32_t cfsr;
    uint32_t info;
    uint32_t boot_count;
} fault_info_t;

/** Call first thing in main(): consumes any crash record from the previous run. */
void fault_boot_init(void);

/** Why the previous run ended. */
const fault_info_t *fault_last(void);

void fault_record_and_reset(uint8_t reason, uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t info)
    __attribute__((noreturn));

void fault_unexpected_irq(void) __attribute__((noreturn));
void app_assert_failed(const char *file, int line) __attribute__((noreturn));

const char *fault_reason_str(uint8_t reason);

#endif /* FAULT_H */

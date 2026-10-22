/**
 * @file app_services.h
 * @brief Logging, supervision, signal generator and power-on self test.
 */
#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include <stdint.h>

#include "app.h"

/* ---- log.c: multi-producer, non-blocking; drained by the log task ---- */
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t log_dropped(void);

/* ---- supervisor.c: software heartbeat watchdog + hardware IWDG ---- */
void supervisor_init(void);
void supervisor_heartbeat(app_task_id_t id);

/* ---- siggen.c: sample-clock ISR acting as the "ADC" ---- */
void siggen_configure(uint16_t f1_hz, q15_t a1, uint16_t f2_hz, q15_t a2, q15_t noise);
void siggen_start(void);
void siggen_stop(void);
void siggen_isr(void);

/* ---- post.c: power-on self test against Python-generated golden vectors ---- */
uint8_t post_run(filter_engine_t *scratch);

#endif /* APP_SERVICES_H */

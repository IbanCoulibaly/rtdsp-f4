/**
 * @file stdlib.h
 * @brief Placeholder so FreeRTOS sources compile freestanding.
 *
 * Intentionally declares no malloc/free: the firmware uses static
 * allocation only (configSUPPORT_DYNAMIC_ALLOCATION = 0), so any heap use
 * is a link error instead of a field failure.
 */
#ifndef RTDSP_STDLIB_H
#define RTDSP_STDLIB_H

#include <stddef.h>

#endif /* RTDSP_STDLIB_H */

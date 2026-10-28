/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SYSTEM_DEFS_H
#define ESY_SYSTEM_DEFS_H

/**
 * @file esy_system_defs.h
 * @brief System-level common type definitions.
 *
 * This file defines system initialization configuration types,
 * mainly used to configure the system clock frequency and
 * port-specific timebase selection during early platform startup.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief System initialization configuration.
 *
 * This structure is used during platform startup to configure
 * the system clock and optional native microsecond timebase selection.
 */
typedef struct {
    /**
     * @brief System clock frequency in kilohertz (kHz).
     *
     * Examples:
     * - 72 MHz  -> 72000
     * - 125 MHz -> 125000
     *
     * @note
     * The selected clock frequency directly affects peripheral
     * prescalers, timers, baud rate generation, and overall
     * timing accuracy.
     */
    uint32_t sys_clock_khz;

    /**
     * @brief Request the native microsecond timebase where selectable.
     *
     * ESP32 selects its native microsecond counter when true and a
     * millisecond-derived counter when false. Pico always uses its native
     * microsecond counter. STM32F4 ignores this flag and returns its
     * millisecond HAL tick multiplied by 1000 for the 32-bit microsecond API.
     *
     * A counter expressed in microseconds does not guarantee microsecond
     * resolution. This flag does not configure a hardware timer/capture or
     * certify the resolution of every delay or timestamp API.
     */
    bool enable_us_tick;
} esy_system_config_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_SYSTEM_DEFS_H */

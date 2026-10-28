/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TOUCH_DEFS_H
#define ESY_TOUCH_DEFS_H

/**
 * @file esy_touch_defs.h
 * @brief Shared touch sample values, independent of touch runtime storage.
 */

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Touch sample data.
 */
typedef struct {
  bool is_pressed; /**< Pressed state. */
  uint16_t x;      /**< X coordinate in pixels. */
  uint16_t y;      /**< Y coordinate in pixels. */
} esy_touch_data_t;

#endif /* ESY_TOUCH_DEFS_H */

/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_RTC_DEFS_H
#define ESY_RTC_DEFS_H

#include <stdint.h>

/** @brief Optional operations implemented by an RTC provider. */
enum {
  ESY_RTC_CAP_ALARM = UINT32_C(1) << 0, /**< One absolute, one-shot UTC alarm. */
  ESY_RTC_CAP_CALIBRATION = UINT32_C(1) << 1 /**< Signed frequency correction. */
};

#endif /* ESY_RTC_DEFS_H */

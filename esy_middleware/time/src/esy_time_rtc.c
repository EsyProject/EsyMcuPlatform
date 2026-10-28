/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/time/esy_time_rtc.h"

#include "esy_common/utils/time/esy_datetime.h"

esy_err_t esy_time_rtc_load(esy_time_t *time, esy_hal_rtc_t *rtc) {
  esy_datetime_t datetime;
  esy_timestamp_t timestamp;
  esy_err_t result;
  if (time == NULL || rtc == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  result = esy_hal_rtc_get_datetime(rtc, &datetime);
  if (result != ESY_OK) {
    return result;
  }
  result = esy_datetime_to_timestamp(&datetime, &timestamp);
  return result == ESY_OK ? esy_time_set(time, &timestamp) : result;
}

esy_err_t esy_time_rtc_store(esy_time_t *time, esy_hal_rtc_t *rtc) {
  esy_datetime_t datetime;
  esy_timestamp_t timestamp;
  esy_err_t result;
  if (time == NULL || rtc == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  result = esy_time_now(time, &timestamp);
  if (result != ESY_OK) {
    return result;
  }
  result = esy_datetime_from_timestamp(&timestamp, &datetime);
  return result == ESY_OK ? esy_hal_rtc_set_datetime(rtc, &datetime) : result;
}

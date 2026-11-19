/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/adc/esy_adc_ad7276.h"

esy_err_t esy_adc_ad7276_decode(const uint16_t *raw_words, size_t word_count,
                                  esy_adc_ad7276_frame_t frame,
                                  uint16_t *samples, size_t sample_capacity) {
  if ((frame != ESY_ADC_AD7276_FRAME_14_CLOCKS &&
       frame != ESY_ADC_AD7276_FRAME_16_CLOCKS) ||
      word_count > sample_capacity ||
      word_count > SIZE_MAX / sizeof(*raw_words) ||
      word_count > UINTPTR_MAX / sizeof(*raw_words) ||
      sample_capacity > SIZE_MAX / sizeof(*samples)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (word_count == 0u) {
    return ESY_OK;
  }
  if (raw_words == NULL || samples == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  const uintptr_t source = (uintptr_t)raw_words;
  const uintptr_t destination = (uintptr_t)samples;
  const uintptr_t bytes = (uintptr_t)(word_count * sizeof(*raw_words));
  if (source % _Alignof(uint16_t) != 0u ||
      destination % _Alignof(uint16_t) != 0u ||
      source > UINTPTR_MAX - bytes || destination > UINTPTR_MAX - bytes) {
    return ESY_ERR_INVALID_ARG;
  }
  if (source != destination && source < destination + bytes &&
      destination < source + bytes) {
    return ESY_ERR_INVALID_ARG;
  }

  const unsigned shift = frame == ESY_ADC_AD7276_FRAME_16_CLOCKS ? 2u : 0u;
  for (size_t index = 0u; index < word_count; ++index) {
    samples[index] = (uint16_t)((raw_words[index] >> shift) &
                               ESY_ADC_AD7276_MAX_CODE);
  }
  return ESY_OK;
}

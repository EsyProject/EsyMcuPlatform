/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ADC_AD7276_H
#define ESY_ADC_AD7276_H

#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_adc_ad7276.h
 * @brief Stateless decoder for completed AD7276 serial acquisitions.
 *
 * The caller owns acquisition, CS/SCLK timing, buffer lifetime and scheduling.
 * Each input uint16_t holds one complete MSB-first serial frame, with its last
 * captured bit at bit zero. No byte swapping, bit reversal, hardware access,
 * allocation, initialization, callback or background work occurs here. These
 * operations support baremetal and OS callers without either dependency.
 */

/** @brief Largest unsigned 12-bit conversion code; the supply is the reference. */
#define ESY_ADC_AD7276_MAX_CODE UINT16_C(4095)

/** @brief Number of captured serial clocks in each right-aligned input word. */
typedef enum {
  ESY_ADC_AD7276_FRAME_14_CLOCKS = 14, /**< Conversion data occupies bits 11:0. */
  ESY_ADC_AD7276_FRAME_16_CLOCKS = 16 /**< Conversion data occupies bits 13:2. */
} esy_adc_ad7276_frame_t;

/**
 * @brief Extract one 12-bit code per captured word, preserving frame order.
 *
 * Only conversion bits are used. Leading and trailing captures are ignored;
 * this function cannot validate timing, power state or electrical data quality.
 * Finish DMA and transfer exclusive access to these ranges before calling.
 * Nothing is retained after return. Concurrent calls are permitted only when
 * their written ranges do not overlap any other call's accessed ranges. This
 * pure operation is ISR-safe if its bounded O(word_count) work fits the ISR
 * budget; the caller normally decodes completed blocks in its main loop/task.
 *
 * @param raw_words Borrowed, aligned array with at least word_count elements.
 * @param word_count Number of frames and resulting samples; zero is accepted.
 * @param frame Exactly FRAME_14_CLOCKS or FRAME_16_CLOCKS. The selected format
 * must match the capture transport; this does not configure that transport.
 * @param samples Caller-owned, aligned output array. It may equal raw_words for
 * exact in-place conversion; other overlap between the two used ranges fails.
 * @param sample_capacity Number of uint16_t elements available at samples.
 * Must be at least word_count and representable as a byte size. Only the first
 * word_count elements are written. Zero words permits either pointer to be NULL.
 * @return ESY_OK after decoding every word. ESY_ERR_INVALID_ARG for an invalid
 * frame, insufficient capacity, unrepresentable size/address range, misaligned
 * buffer, forbidden overlap, or a NULL buffer with nonzero word_count. Every
 * error is detected before writing output. Buffers must name actual accessible
 * objects of the declared sizes; numeric range checks cannot verify that.
 */
esy_err_t esy_adc_ad7276_decode(const uint16_t *raw_words, size_t word_count,
                                  esy_adc_ad7276_frame_t frame,
                                  uint16_t *samples, size_t sample_capacity);

#ifdef __cplusplus
}
#endif
#endif /* ESY_ADC_AD7276_H */

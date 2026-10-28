/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fixture_io.h
 * @brief CSV fixture reader for unit tests.
 *
 * This module reads unsigned 16-bit values from simple CSV fixture files.
 * Tests commonly use it to load sample waveforms or captured input data.
 *
 * Supported behavior:
 * - whitespace, newlines, and commas are treated as separators;
 * - lines starting with '#' are comments;
 * - values beyond the output capacity are truncated;
 * - non-numeric tokens are ignored so fixture headers are tolerated.
 *
 * @note This file belongs to the test support module.
 */

#ifndef ESY_FIXTURE_IO_H_
#define ESY_FIXTURE_IO_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Read a sequence of unsigned 16-bit values from a CSV file.
 *
 * @param[in]  path       Relative or absolute CSV path.
 * @param[out] out        Output array.
 * @param[in]  max        Output array capacity in elements.
 * @param[out] out_count  Number of values read.
 *
 * @return true on success; false when the file cannot be opened or parsed.
 *
 * ### File format
 * - Values may be separated by commas, spaces, or newlines.
 * - A leading `#` starts a comment line.
 * - Non-numeric tokens are ignored.
 * - Values beyond `max` are truncated.
 *
 * ### Example
 * @code
 * uint16_t buf[1024];
 * size_t count = 0;
 * if (esy_fixture_read_u16_csv("fixtures/input_sample_data.csv", buf, 1024, &count)) {
 *     // count samples were read.
 * }
 * @endcode
 */
bool esy_fixture_read_u16_csv(const char *path, uint16_t *out, size_t max, size_t *out_count);

#endif /* ESY_FIXTURE_IO_H_ */

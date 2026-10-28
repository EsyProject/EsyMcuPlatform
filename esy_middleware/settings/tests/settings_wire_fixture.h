/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SETTINGS_WIRE_FIXTURE_H
#define SETTINGS_WIRE_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

/* Independent encoders for documented bytes; never include private headers. */
void settings_fixture_u32(uint8_t *output, uint32_t value);
size_t settings_fixture_header(uint8_t *output, uint32_t version,
                              uint32_t entries, uint32_t length);
size_t settings_fixture_entry(uint8_t *output, const void *key, uint32_t key_size,
                             const void *value, uint32_t value_size);

#endif /* SETTINGS_WIRE_FIXTURE_H */

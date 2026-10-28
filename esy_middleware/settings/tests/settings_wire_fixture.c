/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "settings_wire_fixture.h"

#include <string.h>

void settings_fixture_u32(uint8_t *output, uint32_t value) {
  for (uint32_t index = 0u; index < 4u; ++index) {
    output[index] = (uint8_t)(value >> (index * 8u));
  }
}

size_t settings_fixture_header(uint8_t *output, uint32_t version,
                              uint32_t entries, uint32_t length) {
  settings_fixture_u32(output, UINT32_C(0x53595345));
  settings_fixture_u32(output + 4u, version);
  settings_fixture_u32(output + 8u, entries);
  settings_fixture_u32(output + 12u, length);
  return 16u;
}

size_t settings_fixture_entry(uint8_t *output, const void *key, uint32_t key_size,
                             const void *value, uint32_t value_size) {
  settings_fixture_u32(output, key_size);
  settings_fixture_u32(output + 4u, value_size);
  if (key_size != 0u) memcpy(output + 8u, key, key_size);
  if (value_size != 0u) memcpy(output + 8u + key_size, value, value_size);
  return 8u + key_size + value_size;
}

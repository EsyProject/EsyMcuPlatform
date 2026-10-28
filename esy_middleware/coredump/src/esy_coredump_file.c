/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/coredump/esy_coredump_file.h"
#include "esy_coredump_internal.h"

#include <stdbool.h>
#include <string.h>

#define COREDUMP_FILE_HANDLE_MAGIC UINT32_C(0x43444649)
#define COREDUMP_FILE_COMPARE_BYTES 64u

typedef struct {
  uintptr_t owner;
  esy_coredump_t *store;
  esy_file_t *file;
  uint8_t *record;
  size_t capacity;
  uint32_t cookie;
} coredump_file_private_t;

_Static_assert(sizeof(coredump_file_private_t) <= ESY_COREDUMP_FILE_STORAGE_BYTES,
               "Coredump file handle is too small");

static bool overlaps(const void *a, size_t an, const void *b, size_t bn) {
  const uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
  if (an == 0u || bn == 0u) return false;
  return an > UINTPTR_MAX - aa || bn > UINTPTR_MAX - bb ||
         (aa < bb + bn && bb < aa + an);
}

static uint32_t load_u32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
         ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static esy_err_t get_private(esy_coredump_file_t *adapter,
                             coredump_file_private_t *state) {
  if (adapter == NULL) return ESY_ERR_INVALID_ARG;
  memcpy(state, adapter->storage.bytes, sizeof(*state));
  return state->cookie == COREDUMP_FILE_HANDLE_MAGIC &&
         state->owner == (uintptr_t)adapter ? ESY_OK : ESY_ERR_STATE;
}

static esy_err_t read_stored(const coredump_file_private_t *state,
                             esy_coredump_info_t *stored, bool *absent) {
  *absent = false;
  esy_file_info_t info;
  esy_err_t result = esy_file_get_info(state->file, &info);
  if (result != ESY_OK) return result;
  if (!info.exists) {
    *absent = true;
    return ESY_ERR_STATE;
  }
  if (info.length < COREDUMP_MIN_VALIDATION_BYTES) return ESY_ERR_CRC_FAIL;
  uint8_t prefix[COREDUMP_MIN_VALIDATION_BYTES];
  result = esy_file_read(state->file, 0u, prefix, sizeof(prefix));
  if (result != ESY_OK) return result;
  if (load_u32(prefix) == COREDUMP_MAGIC && load_u32(prefix + 4u) != COREDUMP_VERSION)
    return ESY_ERR_NOT_SUPPORTED;
  if (info.length < ESY_COREDUMP_HEADER_BYTES ||
      info.length > ESY_COREDUMP_MAX_RECORD_BYTES || info.length > state->capacity)
    return ESY_ERR_CRC_FAIL;
  result = esy_file_read(state->file, 0u, state->record, info.length);
  if (result != ESY_OK) return result;
  result = esy_coredump_validate(state->record, info.length, stored);
  return result == ESY_ERR_STATE ? ESY_ERR_CRC_FAIL : result;
}

static esy_err_t compare_ram(const coredump_file_private_t *state,
                             const esy_coredump_info_t *ram,
                             const esy_coredump_info_t *stored) {
  if (ram->record_size != stored->record_size || ram->record_crc != stored->record_crc)
    return ESY_ERR_BUSY;
  uint8_t chunk[COREDUMP_FILE_COMPARE_BYTES];
  for (uint32_t offset = 0u; offset < ram->record_size;) {
    size_t length = ram->record_size - offset;
    if (length > sizeof(chunk)) length = sizeof(chunk);
    const esy_err_t result = esy_coredump_read(state->store, &ram->record_id,
                                             offset, chunk, length);
    if (result != ESY_OK) return result;
    if (memcmp(chunk, state->record + offset, length) != 0) return ESY_ERR_BUSY;
    offset += (uint32_t)length;
  }
  return ESY_OK;
}

esy_err_t esy_coredump_file_init(esy_coredump_file_t *adapter,
                               const esy_coredump_file_config_t *config) {
  if (adapter == NULL || config == NULL || config->store == NULL ||
      config->file == NULL || config->static_resources == NULL)
    return ESY_ERR_INVALID_ARG;
  for (size_t i = 0u; i < sizeof(adapter->storage.bytes); ++i)
    if (adapter->storage.bytes[i] != 0u) return ESY_ERR_STATE;
  const esy_coredump_file_static_resources_t *resources = config->static_resources;
  if (resources->record_buffer == NULL ||
      overlaps(adapter, sizeof(*adapter), config->store, sizeof(*config->store)) ||
      overlaps(adapter, sizeof(*adapter), config->file, sizeof(*config->file)) ||
      overlaps(config->store, sizeof(*config->store), config->file, sizeof(*config->file)) ||
      overlaps(adapter, sizeof(*adapter), resources->record_buffer, resources->record_capacity) ||
      overlaps(config->file, sizeof(*config->file), resources->record_buffer, resources->record_capacity))
    return ESY_ERR_INVALID_ARG;
  if (resources->record_capacity < ESY_COREDUMP_MAX_RECORD_BYTES) return ESY_ERR_NO_MEM;
  esy_err_t result = esy_coredump_check_external_storage(config->store,
      resources->record_buffer, resources->record_capacity);
  if (result == ESY_OK)
    result = esy_coredump_check_external_storage(config->store, adapter, sizeof(*adapter));
  if (result == ESY_OK)
    result = esy_coredump_check_external_storage(config->store, config->file, sizeof(*config->file));
  if (result != ESY_OK) return result;
  uint32_t capabilities;
  result = esy_file_get_capabilities(config->file, &capabilities);
  if (result != ESY_OK) return result;
  const uint32_t required = ESY_FILE_CAP_READ | ESY_FILE_CAP_CREATE;
  if ((capabilities & required) != required) return ESY_ERR_NOT_SUPPORTED;
  const coredump_file_private_t state = {
      .owner = (uintptr_t)adapter, .store = config->store, .file = config->file,
      .record = resources->record_buffer, .capacity = resources->record_capacity,
      .cookie = COREDUMP_FILE_HANDLE_MAGIC};
  memcpy(adapter->storage.bytes, &state, sizeof(state));
  return ESY_OK;
}

esy_err_t esy_coredump_file_deinit(esy_coredump_file_t *adapter) {
  if (adapter == NULL) return ESY_ERR_INVALID_ARG;
  bool zero = true;
  for (size_t i = 0u; i < sizeof(adapter->storage.bytes); ++i)
    zero = zero && adapter->storage.bytes[i] == 0u;
  if (zero) return ESY_OK;
  coredump_file_private_t state;
  const esy_err_t result = get_private(adapter, &state);
  if (result == ESY_OK) memset(adapter->storage.bytes, 0, sizeof(adapter->storage.bytes));
  return result;
}

esy_err_t esy_coredump_file_restore(esy_coredump_file_t *adapter) {
  coredump_file_private_t state;
  esy_err_t result = get_private(adapter, &state);
  if (result != ESY_OK) return result;
  esy_coredump_info_t stored;
  bool absent;
  result = read_stored(&state, &stored, &absent);
  return result == ESY_OK
      ? esy_coredump_restore(state.store, state.record, stored.record_size) : result;
}

esy_err_t esy_coredump_file_persist(esy_coredump_file_t *adapter) {
  coredump_file_private_t state;
  esy_err_t result = get_private(adapter, &state);
  if (result != ESY_OK) return result;
  esy_coredump_info_t ram, stored;
  result = esy_coredump_get_info(state.store, &ram);
  if (result != ESY_OK) return result;
  bool absent;
  result = read_stored(&state, &stored, &absent);
  if (result == ESY_OK) return compare_ram(&state, &ram, &stored);
  if (!absent) return result;
  result = esy_coredump_read(state.store, &ram.record_id, 0u,
                            state.record, ram.record_size);
  return result == ESY_OK
      ? esy_file_create(state.file, state.record, ram.record_size) : result;
}

esy_err_t esy_coredump_file_clear(esy_coredump_file_t *adapter,
                                const esy_coredump_record_id_t *record_id) {
  if (record_id == NULL) return ESY_ERR_INVALID_ARG;
  const esy_coredump_record_id_t expected = *record_id;
  coredump_file_private_t state;
  esy_err_t result = get_private(adapter, &state);
  if (result != ESY_OK) return result;
  result = esy_coredump_read(state.store, &expected, 0u, NULL, 0u);
  if (result != ESY_OK) return result;
  uint32_t capabilities;
  result = esy_file_get_capabilities(state.file, &capabilities);
  if (result != ESY_OK) return result;
  if ((capabilities & ESY_FILE_CAP_REMOVE) == 0u) return ESY_ERR_NOT_SUPPORTED;
  esy_coredump_info_t stored;
  bool absent;
  result = read_stored(&state, &stored, &absent);
  if (result == ESY_OK) {
    if (memcmp(expected.bytes, stored.record_id.bytes, sizeof(expected.bytes)) != 0)
      return ESY_ERR_STATE;
  } else if (!absent) {
    return result;
  }
  result = esy_file_remove(state.file);
  return result == ESY_OK ? esy_coredump_clear(state.store, &expected) : result;
}

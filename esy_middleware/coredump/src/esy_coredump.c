/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_coredump_internal.h"

#include <stdbool.h>
#include <string.h>

#define COREDUMP_HANDLE_MAGIC UINT32_C(0x4344524d)
#define COREDUMP_CRC_POLYNOMIAL UINT32_C(0xedb88320)
#define COREDUMP_REGISTERS_OFFSET 96u
#define COREDUMP_BUILD_ID_OFFSET 48u
#define COREDUMP_RECORD_ID_OFFSET 268u

/* Only the handle lives in ordinary RAM; the persistent bytes have no C ABI. */
typedef struct {
  uintptr_t owner;
  uint8_t *retained;
  size_t capacity;
  uint32_t cookie;
  uint32_t stack_limit;
  uint32_t image_base;
  uint32_t firmware_version;
  uint8_t build_id[ESY_COREDUMP_BUILD_ID_BYTES];
  esy_coredump_record_id_t prepared_id;
  esy_err_t (*new_record_id)(void *context, esy_coredump_record_id_t *id);
  void *record_id_context;
} coredump_private_t;

_Static_assert(sizeof(coredump_private_t) <= ESY_COREDUMP_STORAGE_BYTES,
               "Coredump handle capacity is too small");

/* Volatile byte accesses also prohibit compiler-generated libc copy calls. */
ESY_FAULT_RAM_FUNC static void copy_bytes(void *to, const void *from, size_t count) {
  volatile uint8_t *out = to;
  const volatile uint8_t *in = from;
  for (size_t i = 0u; i < count; ++i) {
    out[i] = in[i];
  }
}

ESY_FAULT_RAM_FUNC static bool overlaps(const void *a, size_t a_size,
                                       const void *b, size_t b_size) {
  uintptr_t first = (uintptr_t)a;
  uintptr_t second = (uintptr_t)b;
  if (a_size == 0u || b_size == 0u) {
    return false;
  }
  if (a_size > UINTPTR_MAX - first || b_size > UINTPTR_MAX - second) {
    return true;
  }
  return first < second + b_size && second < first + a_size;
}

ESY_FAULT_RAM_FUNC static uint32_t load_u32(const uint8_t *buffer) {
  const volatile uint8_t *p = buffer;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

ESY_FAULT_RAM_FUNC static void store_u32(uint8_t *buffer, uint32_t value) {
  volatile uint8_t *p = buffer;
  for (size_t i = 0u; i < sizeof(value); ++i) {
    p[i] = (uint8_t)(value >> (i * 8u));
  }
}

ESY_FAULT_RAM_FUNC static bool id_nonzero(const uint8_t *bytes) {
  const volatile uint8_t *id = bytes;
  uint8_t combined = 0u;
  for (size_t i = 0u; i < ESY_COREDUMP_RECORD_ID_BYTES; ++i) combined |= id[i];
  return combined != 0u;
}

ESY_FAULT_RAM_FUNC static void commit_record(uint8_t *buffer, uint32_t magic) {
  __atomic_thread_fence(__ATOMIC_RELEASE);
  *(volatile uint32_t *)(void *)buffer = magic;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* A table in Flash is unavailable during a fault caused by Flash programming. */
ESY_FAULT_RAM_FUNC static uint32_t record_crc(const uint8_t *buffer, size_t size) {
  uint32_t crc = UINT32_MAX;
  const volatile uint8_t *bytes = buffer;
  for (size_t i = 4u; i < size; ++i) {
    if (i >= 12u && i < 16u) {
      continue;
    }
    crc ^= bytes[i];
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
      uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1u) ^ (COREDUMP_CRC_POLYNOMIAL & mask);
    }
  }
  return crc ^ UINT32_MAX;
}

ESY_FAULT_RAM_FUNC static esy_err_t get_private(esy_coredump_t *store,
                                               coredump_private_t *state) {
  if (store == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  copy_bytes(state, store->storage.bytes, sizeof(*state));
  if (state->cookie != COREDUMP_HANDLE_MAGIC || state->owner != (uintptr_t)store) {
    return ESY_ERR_STATE;
  }
  return ESY_OK;
}

ESY_FAULT_RAM_FUNC static esy_err_t validate_record(const uint8_t *record,
                                                   size_t capacity, bool exact) {
  if (record == NULL || capacity < COREDUMP_MIN_VALIDATION_BYTES) {
    return ESY_ERR_INVALID_ARG;
  }
  if (load_u32(record) != COREDUMP_MAGIC) {
    return ESY_ERR_STATE;
  }
  if (load_u32(record + 4u) != COREDUMP_VERSION) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  if (capacity < ESY_COREDUMP_HEADER_BYTES) {
    return ESY_ERR_CRC_FAIL;
  }
  uint32_t size = load_u32(record + 8u);
  uint32_t stack_size = load_u32(record + 44u);
  uint32_t arch = load_u32(record + 16u);
  if (stack_size > ESY_FAULT_STACK_MAX_BYTES ||
      size != ESY_COREDUMP_HEADER_BYTES + stack_size ||
      size > capacity || (exact && size != capacity) ||
      (arch != ESY_FAULT_ARCH_ARM_M && arch != ESY_FAULT_ARCH_XTENSA) ||
      !id_nonzero(record + COREDUMP_RECORD_ID_OFFSET)) {
    return ESY_ERR_CRC_FAIL;
  }
  if (record_crc(record, size) != load_u32(record + 12u)) {
    return ESY_ERR_CRC_FAIL;
  }
  return ESY_OK;
}

static void read_info(const uint8_t *p, esy_coredump_info_t *info) {
  esy_coredump_info_t result = {
      .record_size = load_u32(p + 8u), .record_crc = load_u32(p + 12u),
      .architecture = load_u32(p + 16u), .reason = load_u32(p + 20u),
      .core_id = load_u32(p + 24u), .flags = load_u32(p + 28u),
      .image_base = load_u32(p + 32u), .firmware_version = load_u32(p + 36u),
      .stack_address = load_u32(p + 40u), .stack_length = load_u32(p + 44u)};
  memcpy(result.build_id, p + COREDUMP_BUILD_ID_OFFSET, sizeof(result.build_id));
  memcpy(result.record_id.bytes, p + COREDUMP_RECORD_ID_OFFSET,
         sizeof(result.record_id.bytes));
  *info = result;
}

esy_err_t esy_coredump_init(esy_coredump_t *store,
                          const esy_coredump_config_t *config) {
  if (store == NULL || config == NULL || config->static_resources == NULL ||
      config->new_record_id == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  for (size_t i = 0u; i < sizeof(store->storage.bytes); ++i) {
    if (store->storage.bytes[i] != 0u) {
      return ESY_ERR_STATE;
    }
  }
  const esy_coredump_static_resources_t *resources = config->static_resources;
  if (resources->retained_buffer == NULL ||
      ((uintptr_t)resources->retained_buffer & 3u) != 0u ||
      config->stack_limit > ESY_FAULT_STACK_MAX_BYTES ||
      resources->retained_size > UINTPTR_MAX - (uintptr_t)resources->retained_buffer ||
      overlaps(store, sizeof(*store), resources->retained_buffer,
               resources->retained_size)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (resources->retained_size < ESY_COREDUMP_HEADER_BYTES + config->stack_limit) {
    return ESY_ERR_NO_MEM;
  }
  coredump_private_t state = {
      .owner = (uintptr_t)store, .retained = resources->retained_buffer,
      .capacity = resources->retained_size, .cookie = COREDUMP_HANDLE_MAGIC,
      .stack_limit = config->stack_limit, .image_base = config->image_base,
      .firmware_version = config->firmware_version,
      .new_record_id = config->new_record_id,
      .record_id_context = config->record_id_context};
  esy_err_t result = config->new_record_id(config->record_id_context,
                                           &state.prepared_id);
  if (result != ESY_OK) return result;
  if (!id_nonzero(state.prepared_id.bytes) ||
      (validate_record(state.retained, state.capacity, false) == ESY_OK &&
       memcmp(state.prepared_id.bytes,
              state.retained + COREDUMP_RECORD_ID_OFFSET,
              ESY_COREDUMP_RECORD_ID_BYTES) == 0)) return ESY_ERR_STATE;
  memcpy(state.build_id, config->build_id, sizeof(state.build_id));
  memcpy(store->storage.bytes, &state, sizeof(state));
  return ESY_OK;
}

esy_err_t esy_coredump_deinit(esy_coredump_t *store) {
  coredump_private_t state;
  if (store == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  bool zero = true;
  for (size_t i = 0u; i < sizeof(store->storage.bytes); ++i) {
    zero = zero && store->storage.bytes[i] == 0u;
  }
  if (zero) {
    return ESY_OK;
  }
  esy_err_t status = get_private(store, &state);
  if (status == ESY_OK) {
    memset(store->storage.bytes, 0, sizeof(store->storage.bytes));
  }
  return status;
}

ESY_FAULT_RAM_FUNC esy_err_t esy_coredump_capture_fault(
    const esy_fault_snapshot_t *snapshot, void *store) {
  coredump_private_t state;
  esy_err_t status = get_private(store, &state);
  if (status != ESY_OK) {
    return status;
  }
  if (snapshot == NULL ||
      (snapshot->architecture != ESY_FAULT_ARCH_ARM_M &&
       snapshot->architecture != ESY_FAULT_ARCH_XTENSA)) {
    return ESY_ERR_INVALID_ARG;
  }
  status = validate_record(state.retained, state.capacity, false);
  if (status == ESY_OK) {
    return ESY_ERR_BUSY;
  }
  if (status == ESY_ERR_NOT_SUPPORTED) {
    return status;
  }
  uint32_t stack_size = 0u;
  if ((snapshot->flags & ESY_FAULT_FLAG_STACK_VALID) != 0u) {
    if (snapshot->stack == NULL || snapshot->stack_length > ESY_FAULT_STACK_MAX_BYTES) {
      return ESY_ERR_INVALID_ARG;
    }
    stack_size = snapshot->stack_length < state.stack_limit ?
                 snapshot->stack_length : state.stack_limit;
  }
  if (overlaps(snapshot, sizeof(*snapshot), state.retained, state.capacity) ||
      overlaps(snapshot->stack, stack_size, state.retained, state.capacity) ||
      overlaps(snapshot->stack, stack_size, store, sizeof(esy_coredump_t))) {
    return ESY_ERR_INVALID_ARG;
  }
  uint8_t *p = state.retained;
  commit_record(p, 0u);
  for (size_t i = 4u; i < ESY_COREDUMP_HEADER_BYTES; ++i) {
    ((volatile uint8_t *)p)[i] = 0u;
  }
  uint32_t size = ESY_COREDUMP_HEADER_BYTES + stack_size;
  store_u32(p + 4u, COREDUMP_VERSION);
  store_u32(p + 8u, size);
  store_u32(p + 16u, snapshot->architecture);
  store_u32(p + 20u, snapshot->reason);
  store_u32(p + 24u, snapshot->core_id);
  store_u32(p + 28u, snapshot->flags |
            (stack_size < snapshot->stack_length ? ESY_COREDUMP_FLAG_STACK_TRUNCATED : 0u));
  store_u32(p + 32u, state.image_base);
  store_u32(p + 36u, state.firmware_version);
  store_u32(p + 40u, stack_size != 0u ? snapshot->stack_address : 0u);
  store_u32(p + 44u, stack_size);
  copy_bytes(p + COREDUMP_BUILD_ID_OFFSET, state.build_id, sizeof(state.build_id));
  store_u32(p + 68u, snapshot->exc_return);
  store_u32(p + 72u, snapshot->msp);
  store_u32(p + 76u, snapshot->psp);
  store_u32(p + 80u, snapshot->control);
  store_u32(p + 84u, snapshot->primask);
  store_u32(p + 88u, snapshot->basepri);
  store_u32(p + 92u, snapshot->faultmask);
  for (size_t i = 0u; i < ESY_FAULT_REGISTER_COUNT; ++i) {
    store_u32(p + COREDUMP_REGISTERS_OFFSET + i * sizeof(uint32_t), snapshot->r[i]);
  }
  store_u32(p + 224u, snapshot->cfsr);
  store_u32(p + 228u, snapshot->hfsr);
  store_u32(p + 232u, snapshot->dfsr);
  store_u32(p + 236u, snapshot->afsr);
  store_u32(p + 240u, snapshot->mmfar);
  store_u32(p + 244u, snapshot->bfar);
  store_u32(p + 248u, snapshot->shcsr);
  store_u32(p + 252u, snapshot->detail0);
  store_u32(p + 256u, snapshot->detail1);
  store_u32(p + 260u, snapshot->msplim);
  store_u32(p + 264u, snapshot->psplim);
  copy_bytes(p + COREDUMP_RECORD_ID_OFFSET, state.prepared_id.bytes,
             ESY_COREDUMP_RECORD_ID_BYTES);
  copy_bytes(p + ESY_COREDUMP_HEADER_BYTES, snapshot->stack, stack_size);
  store_u32(p + 12u, record_crc(p, size));
  commit_record(p, COREDUMP_MAGIC);
  return ESY_OK;
}

esy_err_t esy_coredump_validate(const void *record, size_t length,
                              esy_coredump_info_t *info) {
  if (info != NULL && overlaps(record, length, info, sizeof(*info))) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t status = validate_record(record, length, true);
  if (status == ESY_OK && info != NULL) {
    read_info(record, info);
  }
  return status;
}

esy_err_t esy_coredump_get_info(esy_coredump_t *store, esy_coredump_info_t *info) {
  coredump_private_t state;
  esy_err_t status = get_private(store, &state);
  if (status != ESY_OK) {
    return status;
  }
  if (info == NULL || overlaps(info, sizeof(*info), store, sizeof(*store)) ||
      overlaps(info, sizeof(*info), state.retained, state.capacity)) {
    return ESY_ERR_INVALID_ARG;
  }
  status = validate_record(state.retained, state.capacity, false);
  if (status == ESY_OK) {
    read_info(state.retained, info);
  }
  return status;
}

esy_err_t esy_coredump_read(esy_coredump_t *store,
                          const esy_coredump_record_id_t *record_id, size_t offset,
                          void *buffer, size_t length) {
  if (record_id == NULL || !id_nonzero(record_id->bytes)) return ESY_ERR_INVALID_ARG;
  coredump_private_t state;
  esy_err_t status = get_private(store, &state);
  if (status != ESY_OK) {
    return status;
  }
  if ((buffer == NULL && length != 0u) ||
      overlaps(buffer, length, store, sizeof(*store)) ||
      overlaps(buffer, length, state.retained, state.capacity)) {
    return ESY_ERR_INVALID_ARG;
  }
  status = validate_record(state.retained, state.capacity, false);
  if (status != ESY_OK) {
    return status;
  }
  if (memcmp(record_id->bytes, state.retained + COREDUMP_RECORD_ID_OFFSET,
             ESY_COREDUMP_RECORD_ID_BYTES) != 0) return ESY_ERR_STATE;
  uint32_t size = load_u32(state.retained + 8u);
  if (offset > size || length > size - offset) {
    return ESY_ERR_INVALID_ARG;
  }
  if (length != 0u) {
    memcpy(buffer, state.retained + offset, length);
  }
  return ESY_OK;
}

esy_err_t esy_coredump_restore(esy_coredump_t *store, const void *record,
                             size_t length) {
  coredump_private_t state;
  esy_err_t status = get_private(store, &state);
  if (status != ESY_OK) {
    return status;
  }
  if (overlaps(record, length, state.retained, state.capacity) ||
      overlaps(record, length, store, sizeof(*store))) {
    return ESY_ERR_INVALID_ARG;
  }
  status = validate_record(record, length, true);
  if (status != ESY_OK) {
    return status;
  }
  if (length > state.capacity) {
    return ESY_ERR_NO_MEM;
  }
  commit_record(state.retained, 0u);
  copy_bytes(state.retained + 4u, (const uint8_t *)record + 4u, length - 4u);
  commit_record(state.retained, COREDUMP_MAGIC);
  return ESY_OK;
}

esy_err_t esy_coredump_clear(esy_coredump_t *store,
                           const esy_coredump_record_id_t *record_id) {
  if (record_id == NULL || !id_nonzero(record_id->bytes)) return ESY_ERR_INVALID_ARG;
  const esy_coredump_record_id_t expected = *record_id;
  coredump_private_t state;
  esy_err_t status = get_private(store, &state);
  if (status != ESY_OK) return status;
  status = validate_record(state.retained, state.capacity, false);
  if (status != ESY_OK) return status;
  if (memcmp(expected.bytes, state.retained + COREDUMP_RECORD_ID_OFFSET,
             ESY_COREDUMP_RECORD_ID_BYTES) != 0) return ESY_ERR_STATE;
  esy_coredump_record_id_t next = {0};
  status = state.new_record_id(state.record_id_context, &next);
  if (status != ESY_OK) return status;
  if (!id_nonzero(next.bytes) ||
      memcmp(next.bytes, expected.bytes, sizeof(next.bytes)) == 0 ||
      memcmp(next.bytes, state.prepared_id.bytes, sizeof(next.bytes)) == 0)
    return ESY_ERR_STATE;
  state.prepared_id = next;
  memcpy(store->storage.bytes, &state, sizeof(state));
  commit_record(state.retained, 0u);
  return ESY_OK;
}

esy_err_t esy_coredump_check_external_storage(esy_coredump_t *store,
                                             const void *buffer, size_t length) {
  coredump_private_t state;
  esy_err_t ret = get_private(store, &state);
  if (ret != ESY_OK) {
    return ret;
  }
  if ((buffer == NULL && length != 0u) ||
      overlaps(buffer, length, store, sizeof(*store)) ||
      overlaps(buffer, length, state.retained, state.capacity)) {
    return ESY_ERR_INVALID_ARG;
  }
  return ESY_OK;
}

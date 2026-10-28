/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COREDUMP_H
#define ESY_COREDUMP_H

/**
 * @file esy_coredump.h
 * @brief Bounded first-fault recording in caller-owned retained RAM.
 *
 * The consumer reserves the same retained range in its bootloader and every
 * application image. No init/deinit operation clears an existing record.
 * Capture is terminal-fault-only, externally serialized by the fault port;
 * normal APIs require one serialized owner and no simultaneous capture.
 * The fault port must place the capture call chain and its state in accessible
 * RAM while executing from unavailable Flash, and provide a readable snapshot.
 * No allocation, OS call, logging, Flash operation or clock is used by capture.
 */

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/diagnostics/esy_coredump_defs.h"
#include "esy_common/defs/diagnostics/esy_fault_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Version-one fixed header size, followed by captured stack bytes. */
#define ESY_COREDUMP_HEADER_BYTES 384u
/** @brief GNU SHA-1 build-ID bytes required for exact ELF identification. */
#define ESY_COREDUMP_BUILD_ID_BYTES 20u
/** @brief Private RAM handle capacity; independent of the persistent format. */
#define ESY_COREDUMP_STORAGE_BYTES 128u
/** @brief Snapshot flag indicating stack bytes were limited by configuration. */
#define ESY_COREDUMP_FLAG_STACK_TRUNCATED (UINT32_C(1) << 16u)

/** @brief Caller-owned handle, initialized with `{0}`; do not copy while live. */
typedef struct {
  union {
    uint8_t bytes[ESY_COREDUMP_STORAGE_BYTES]; /**< Opaque state. */
    max_align_t alignment; /**< Private-state alignment. */
  } storage; /**< Implementation storage. */
} esy_coredump_t;

/** @brief Retained storage borrowed until successful deinit. */
typedef struct {
  uint8_t *retained_buffer; /**< Four-byte-aligned, readable/writable retained RAM. */
  size_t retained_size; /**< At least HEADER_BYTES + configured stack_limit. */
} esy_coredump_static_resources_t;

/** @brief Configuration copied by init, including identity copied into ordinary RAM. */
typedef struct {
  const esy_coredump_static_resources_t *static_resources; /**< Required backing RAM. */
  uint8_t build_id[ESY_COREDUMP_BUILD_ID_BYTES]; /**< Exact faulting image identity. */
  uint32_t image_base; /**< Faulting image execution base; retained across OTA rollback. */
  uint32_t firmware_version; /**< Consumer-defined firmware version code. */
  uint32_t stack_limit; /**< Maximum stack bytes, at most ESY_FAULT_STACK_MAX_BYTES. */
  esy_err_t (*new_record_id)(void *context, esy_coredump_record_id_t *id); /**< Required normal-context fresh identity source. */
  void *record_id_context; /**< Borrowed source context, kept alive until deinit. */
} esy_coredump_config_t;

/** @brief Validated persistent metadata; does not use the currently running identity. */
typedef struct {
  uint32_t record_size; /**< Total header and stack bytes. */
  uint32_t record_crc; /**< CRC32 covering version/length and the record body. */
  esy_coredump_record_id_t record_id; /**< Opaque identity of this captured occurrence. */
  uint32_t architecture; /**< Snapshot architecture code. */
  uint32_t reason; /**< esy_fault_reason_t value captured at the fault. */
  uint32_t core_id; /**< Faulting CPU index. */
  uint32_t flags; /**< Snapshot validity/truncation flags. */
  uint32_t image_base; /**< Execution base of the faulting image. */
  uint32_t firmware_version; /**< Version of the faulting image. */
  uint32_t stack_address; /**< Original address of captured stack bytes. */
  uint32_t stack_length; /**< Bytes after the fixed header. */
  uint8_t build_id[ESY_COREDUMP_BUILD_ID_BYTES]; /**< Faulting ELF build ID. */
} esy_coredump_info_t;

/**
 * @brief Borrow retained storage without changing its bytes.
 * @param[in,out] store Zero/deinitialized handle; caller retains its address.
 * @param[in] config Valid backing RAM, identity and bounded stack policy.
 * @return ESY_OK, INVALID_ARG, NO_MEM or STATE (already live/nonzero handle).
 * @note Configuration descriptors may expire after return; buffers may not.
 * A failed first init leaves the zero handle reusable. Normal startup only.
 * The required identity source runs synchronously here and on a matching clear,
 * never in the fault callback. It supplies a fresh nonzero 128-bit identity,
 * distinct across capture lifetimes, including resets and image changes. The
 * source chooses durable sequencing or sufficient entropy and owns collision
 * guarantees; no clock, RNG or Flash partition is selected by this module.
 * The source must not reenter store APIs. Its failure, a zero identity or reuse
 * of the current valid record identity leaves handle/record unchanged.
 */
esy_err_t esy_coredump_init(esy_coredump_t *store,
                          const esy_coredump_config_t *config);

/**
 * @brief Release borrowed RAM without clearing the record; repeated zero deinit succeeds.
 * @param[in,out] store Live or zero handle; normal serialized context only.
 * @return ESY_OK, INVALID_ARG or STATE for an invalid/copied handle.
 */
esy_err_t esy_coredump_deinit(esy_coredump_t *store);

/**
 * @brief HAL-compatible terminal-fault callback; preserve the first committed record.
 * @param[in] snapshot Port-validated snapshot. Stack source remains readable through return.
 * @param[in,out] store Initialized esy_coredump_t, passed as the HAL callback context.
 * @return ESY_OK if committed, BUSY for an existing valid record, NOT_SUPPORTED
 * for a committed unknown version, or INVALID_ARG/STATE for invalid inputs.
 * @note Uses bounded integer-only copies/CRC and a final aligned commit word.
 * The port owns cross-core admission, exception stack, memory barriers and reset.
 * It must not resume normal execution after calling this function from a fault.
 */
esy_err_t esy_coredump_capture_fault(const esy_fault_snapshot_t *snapshot,
                                   void *store);

/**
 * @brief Validate a complete wire record without a live handle.
 * @param[in] record Readable immutable record, including its commit word.
 * @param[in] length Exact byte length; trailing bytes are rejected.
 * @param[out] info Optional output, disjoint from record; unchanged on error.
 * @return ESY_OK, INVALID_ARG, STATE, CRC_FAIL or NOT_SUPPORTED.
 * @note Normal serialized context; used by persistent adapters and consumers.
 */
esy_err_t esy_coredump_validate(const void *record, size_t length,
                              esy_coredump_info_t *info);

/**
 * @brief Validate the retained record; output is unchanged on error.
 * @param[in,out] store Initialized, serialized handle.
 * @param[out] info Output disjoint from handle and retained storage.
 * @return ESY_OK, STATE if no committed record, CRC_FAIL, NOT_SUPPORTED,
 * INVALID_ARG or invalid-handle STATE. Normal context only.
 */
esy_err_t esy_coredump_get_info(esy_coredump_t *store, esy_coredump_info_t *info);

/**
 * @brief Copy a bounded range of the validated complete wire record.
 * @param[in,out] store Initialized handle.
 * @param[in] record_id Required nonzero identity from a successful get_info.
 * @param[in] offset Byte offset into header/stack data.
 * @param[out] buffer Destination disjoint from handle and retained storage; NULL only for zero length.
 * @param[in] length Bytes to copy, fully within the record.
 * @return ESY_OK, INVALID_ARG, STATE for a stale identity, or the get_info
 * validation error. Output unchanged on error. IDs are opaque, never CRCs.
 */
esy_err_t esy_coredump_read(esy_coredump_t *store,
                          const esy_coredump_record_id_t *record_id, size_t offset,
                          void *buffer, size_t length);

/**
 * @brief Restore an authoritative stored record into RAM during normal startup.
 * @param[in,out] store Initialized handle; capture must not run concurrently.
 * @param[in] record Complete immutable wire record disjoint from handle/retained RAM.
 * @param[in] length Exact record size; validated before changing retained bytes.
 * @return ESY_OK, NO_MEM, INVALID_ARG, CRC_FAIL, STATE or NOT_SUPPORTED.
 * @note Deliberately replaces a RAM record so an older, uncollected persistent
 * record wins over later reboot faults. Source must remain valid until return.
 */
esy_err_t esy_coredump_restore(esy_coredump_t *store, const void *record,
                             size_t length);

/**
 * @brief Explicitly invalidate retained RAM after collection/authorized discard.
 * @param[in,out] store Initialized, serialized handle; no simultaneous capture.
 * @param[in] record_id Required nonzero identity of the record being discarded.
 * @return ESY_OK, INVALID_ARG, STATE for no/stale record or reused/zero generated
 * identity, a validation error or the identity-source error.
 * @note Validate the exact current record, then obtain its successor identity
 * before invalidation. Any failure preserves the handle and retained record.
 * An empty/already cleared store returns STATE. Does not remove persistent files and must
 * never be inferred from best-effort Logger output. Unknown/corrupt records are
 * not discarded through this identity-checked operation.
 */
esy_err_t esy_coredump_clear(esy_coredump_t *store,
                           const esy_coredump_record_id_t *record_id);

#ifdef __cplusplus
}
#endif
#endif /* ESY_COREDUMP_H */

/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_BIN_SENDER_H
#define ESY_PROTO_BIN_SENDER_H

#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Borrowed ProtoBin payload submission capability, without session driving.
 *
 * All three callbacks are required. Copying the descriptor borrows context;
 * its owner keeps that context alive until every borrower has detached. Calls
 * are externally serialized on one non-ISR owner. They never drive protocol
 * progress or synchronously invoke application data/completion callbacks.
 * One reservation follows acquire -> fill -> commit/cancel. A failed commit
 * retains it, including invalid buffer/length errors. Successful commit consumes
 * it and returns a nonzero transaction ID; it does not promise peer receipt.
 * Contexts may implement direct session submission or a bounded cross-task post.
 */
typedef struct {
  /** @brief Acquire writable payload storage. Outputs are required; failure
   * writes NULL/zero and creates no reservation. BUSY preserves an existing one. */
  esy_err_t (*acquire)(void *context, uint8_t **buffer, size_t *capacity);
  /** @brief Commit the exact acquired pointer and length within its capacity.
   * transaction_id is required, written zero before validation and nonzero on
   * success. Failure retains storage until successful commit or cancel. */
  esy_err_t (*commit)(void *context, const uint8_t *buffer, size_t length,
                      uint32_t *transaction_id);
  /** @brief Release an uncommitted reservation; OK also means none is reserved.
   * Committed payloads are unchanged; failure preserves the reservation. */
  esy_err_t (*cancel)(void *context);
  void *context; /**< Borrowed provider context, interpreted by its callbacks. */
} esy_proto_bin_sender_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_PROTO_BIN_SENDER_H */

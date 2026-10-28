/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_NETWORK_STREAM_H
#define ESY_NETWORK_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Borrowed TCP/TLS byte stream; no framing, QoS, task or connection policy.
 *
 * All callbacks execute synchronously on one externally serialized provider
 * owner, never in ISR context. The provider and context outlive every borrower.
 * send/recv return 0..size or a negative status; zero means temporary lack of
 * capacity/data, never EOF. Accepted send bytes must be copied/retired before
 * return; callers may immediately reuse them. recv never waits for input.
 * Native stack/TLS calls and optional provider progress determine execution
 * latency; this interface does not guarantee a wall-clock or hard realtime bound.
 *
 * send permits a prefix. progress performs one provider iteration; NULL means
 * independently driven. send/recv do not implicitly call progress. Borrowers
 * decide when it runs; synchronous MQTT I/O advances it before each upstream
 * I/O callback. Connection lifecycle, session identity, whole-frame submission
 * and peer acknowledgement evidence are separate provider capabilities.
 * No callback may recursively invoke its borrower or destroy the provider.
 */
typedef struct {
  int32_t (*send)(void *context, const void *data, size_t size); /**< Prefix send. */
  int32_t (*recv)(void *context, void *data, size_t size); /**< Immediate receive. */
  esy_err_t (*progress)(void *context); /**< Optional provider iteration. */
  void *context; /**< Borrowed provider context; provider may allow NULL. */
} esy_network_stream_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_NETWORK_STREAM_H */

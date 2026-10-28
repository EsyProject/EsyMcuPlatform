/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_UTILS_ESY_SHA256_H
#define ESY_COMMON_UTILS_ESY_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Size of a SHA-256 digest in bytes. */
#define ESY_SHA256_DIGEST_BYTES 32u
/** @brief Size of one SHA-256 input block in bytes. */
#define ESY_SHA256_BLOCK_BYTES  64u

/**
 * @brief Caller-owned streaming hash state; first storage may be uninitialized.
 *
 * Initialize before update/final. One externally serialized owner controls each
 * context; do not copy or modify an active context. All calls are synchronous,
 * use no heap and retain no input/output buffers. ISR callers must exclude other
 * access to the same context and budget the input-dependent execution and stack
 * cost; there is no internal wait, lock or background execution.
 */
typedef struct {
  uint32_t state[8];
  uint64_t total_bytes;
  uint8_t block[ESY_SHA256_BLOCK_BYTES];
  size_t block_len;
} esy_sha256_ctx_t;

/**
 * @brief Start or restart a hash, replacing all previous state.
 * @param[out] ctx Context to initialize; NULL is ignored.
 * @note Initialization reads no previous storage bytes and cannot fail. Call
 * this again before reusing a context after final or discarding a partial hash.
 */
void esy_sha256_init(esy_sha256_ctx_t *ctx);
/**
 * @brief Consume input before returning; a zero-length update is a no-op.
 * @param[in,out] ctx Initialized context, not overlapping the input.
 * @param[in] data Input bytes; NULL is permitted only when length is zero.
 * @param length Number of input bytes. Total message length must be below 2^61
 * bytes, as required by the SHA-256 64-bit bit-length encoding.
 * @note NULL ctx or NULL data with nonzero length leaves the context unchanged.
 * Calls on a finalized/uninitialized context or overlapping storage are outside
 * the contract; they cannot be detected without extra state or copies.
 */
void esy_sha256_update(esy_sha256_ctx_t *ctx, const void *data, size_t length);
/**
 * @brief Finish the initialized hash and clear its complete context storage.
 * @param[in,out] ctx Context with all desired input already consumed.
 * @param[out] digest Writable digest storage, disjoint from ctx.
 * @note NULL ctx or digest is ignored without changing either object. After
 * success, init is required before another update/final; final is not repeatable.
 * Context clearing is ordinary memset, not a guaranteed secret-erasure primitive.
 */
void esy_sha256_final(esy_sha256_ctx_t *ctx,
                      uint8_t digest[ESY_SHA256_DIGEST_BYTES]);
/**
 * @brief Hash one message synchronously using temporary stack state.
 * @param[in] data Input bytes; NULL is permitted for an empty message.
 * @param length Input byte count, below 2^61.
 * @param[out] digest Writable digest storage. It may overlap data: output is
 * written only after all input has been consumed.
 * @note NULL digest or NULL data with nonzero length leaves the output unchanged.
 * Distinct calls share no mutable state.
 */
void esy_sha256(const void *data, size_t length,
                uint8_t digest[ESY_SHA256_DIGEST_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_UTILS_ESY_SHA256_H */

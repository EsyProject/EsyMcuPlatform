/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_JSON_H
#define ESY_PROTO_JSON_H

/**
 * @file esy_proto_json.h
 * @brief Allocation-free JSON views, exact integers and bounded generation.
 *
 * Calls are synchronous, with no HAL, OS, clock or transport dependency. One
 * non-ISR owner serializes each iterator/writer; independent objects may run
 * concurrently. Input bytes remain immutable and alive while any view/iterator
 * borrows them. Views must originate from this API and must not be modified.
 * All handle, resource, input and output objects must be disjoint, except views
 * intentionally borrowing input bytes. Strings use explicit byte lengths and
 * may contain decoded NUL bytes. No function allocates memory or invokes I/O.
 * Disjointness and source lifetime are caller preconditions: object bounds and
 * dangling pointers cannot be inferred from the supplied pointer/length pairs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief JSON value category, independent of the parser provider. */
typedef enum {
    ESY_JSON_NULL = 0, /**< JSON null. */
    ESY_JSON_BOOL,     /**< JSON true or false. */
    ESY_JSON_NUMBER,   /**< JSON number preserved as decimal source bytes. */
    ESY_JSON_STRING,   /**< Quoted JSON string, including its escape sequences. */
    ESY_JSON_ARRAY,    /**< JSON array. */
    ESY_JSON_OBJECT    /**< JSON object. */
} esy_json_type_t;

/** @brief Immutable borrowed JSON lexeme, including quotes for strings. */
typedef struct {
    const char *data; /**< Borrowed source bytes; no NUL terminator required. */
    size_t length;   /**< Exact lexeme byte length. */
    esy_json_type_t type; /**< Value category. */
} esy_json_value_t;

/** @brief Iterator storage capacity; not a stable ABI. */
#define ESY_JSON_ITERATOR_STORAGE_BYTES 96u
/** @brief Writer storage capacity; not a stable ABI. */
#define ESY_JSON_WRITER_STORAGE_BYTES 128u

/** @brief Caller-owned iterator. Init permits uninitialized storage. */
typedef struct {
    union {
        max_align_t alignment; /**< Private alignment. */
        uint8_t bytes[ESY_JSON_ITERATOR_STORAGE_BYTES]; /**< Private state. */
    } storage; /**< Private in-place storage. */
} esy_json_iterator_t;

/**
 * @brief Caller-owned writer. Init permits uninitialized storage.
 * @note Do not copy or modify a live handle. Reinit explicitly discards prior
 * output and releases its buffer/workspace borrow; no deinit is necessary.
 */
typedef struct {
    union {
        max_align_t alignment; /**< Private alignment. */
        uint8_t bytes[ESY_JSON_WRITER_STORAGE_BYTES]; /**< Private state. */
    } storage; /**< Private in-place storage. */
} esy_json_writer_t;

/** @brief Writer resources borrowed until reinit or the end of handle use. */
typedef struct {
    char *buffer; /**< Required output buffer, initially overwritten with NUL. */
    size_t buffer_capacity; /**< Capacity including the trailing NUL, at least 1. */
    uint8_t *frame_storage; /**< Required nesting workspace; not read before use. */
    size_t frame_capacity; /**< Workspace bytes, at least 1; one per container. */
} esy_json_writer_resources_t;

/**
 * @brief Validate one complete JSON document and borrow its root lexeme.
 * @param[in] data Required input; a NUL terminator is neither needed nor parsed.
 * @param[in] length Nonzero input length, including optional JSON whitespace.
 * @param[out] value Required root output, unchanged on failure.
 * @return OK, INVALID_ARG for invalid pointers/empty input, FAIL for malformed
 * or truncated JSON, or NO_MEM for the provider nesting limit (default 32).
 * @note Validation covers syntax and exactly one root. Unicode scalar/UTF-8
 * validity is additionally checked when decoding strings or comparing keys.
 * Duplicate keys are permitted by syntax; object_get rejects duplicate matches.
 */
esy_err_t esy_json_parse(const char *data, size_t length, esy_json_value_t *value);

/**
 * @brief Initialize or reset an iterator over an array or object view.
 * @param[out] iterator Required handle, overwritten only on success.
 * @param[in] collection Required borrowed array/object view.
 * @return OK, INVALID_ARG for wrong type/pointers, FAIL or NO_MEM for validation.
 * @note Init may reset an active iterator. It creates no asynchronous borrow.
 */
esy_err_t esy_json_iterator_init(esy_json_iterator_t *iterator,
                                 const esy_json_value_t *collection);

/**
 * @brief Borrow the next direct child; advance only on success.
 * @param[in,out] iterator Initialized iterator.
 * @param[out] key Required object-key view. Arrays return {NULL,0,ESY_JSON_NULL}.
 * @param[out] value Required child view. Both outputs stay unchanged on error.
 * @return OK, NOT_SUPPORTED at end (stable on repeated calls), INVALID_ARG,
 * STATE for a zero/uninitialized-by-init handle, or FAIL for provider failure.
 * @note The caller must initialize before use; arbitrary uninitialized handle
 * bytes are not a valid argument to next. Iteration preserves duplicate keys.
 */
esy_err_t esy_json_iterator_next(esy_json_iterator_t *iterator,
                                 esy_json_value_t *key, esy_json_value_t *value);

/**
 * @brief Find exactly one object member using its decoded UTF-8 key.
 * @param[in] object Required object view.
 * @param[in] key UTF-8 key bytes; NULL is allowed only for zero length.
 * @param[in] key_length Key byte length; empty keys are supported.
 * @param[out] value Required result, unchanged on failure.
 * @return OK, NOT_SUPPORTED if absent, FAIL for duplicate matching decoded keys
 * or malformed Unicode, INVALID_ARG for type/pointer errors, or NO_MEM.
 * @note Matching is case sensitive and escape aware; no Unicode normalization
 * is performed. Unrelated duplicate members remain visible through iteration.
 */
esy_err_t esy_json_object_get(const esy_json_value_t *object, const char *key,
                              size_t key_length, esy_json_value_t *value);

/**
 * @brief Borrow one direct array element by index, in O(index) time.
 * @param[in] array Required array view.
 * @param[in] index Zero-based child index.
 * @param[out] value Required output, unchanged on failure.
 * @return OK, NOT_SUPPORTED if absent, INVALID_ARG, FAIL or NO_MEM.
 */
esy_err_t esy_json_array_get(const esy_json_value_t *array, size_t index,
                             esy_json_value_t *value);

/**
 * @brief Read a boolean without changing output on failure.
 * @param[in] value Required boolean view.
 * @param[out] result Required boolean output.
 * @return OK, INVALID_ARG for pointers/type, or FAIL for an invalid lexeme.
 */
esy_err_t esy_json_as_bool(const esy_json_value_t *value, bool *result);

/**
 * @brief Convert an integer lexeme directly to uint64_t, without floating point.
 * @param[in] value Required number view; signs, fractions and exponents rejected.
 * @param[out] result Required output, unchanged on failure.
 * @return OK, INVALID_ARG for pointers/type, or FAIL for format/overflow.
 */
esy_err_t esy_json_as_u64(const esy_json_value_t *value, uint64_t *result);

/**
 * @brief Convert an integer lexeme directly to int64_t, including INT64_MIN.
 * @param[in] value Required number view; fractions/exponents rejected, -0 allowed.
 * @param[out] result Required output, unchanged on failure.
 * @return OK, INVALID_ARG for pointers/type, or FAIL for format/overflow.
 */
esy_err_t esy_json_as_i64(const esy_json_value_t *value, int64_t *result);

/**
 * @brief Decode a string into caller storage, validating UTF-8 and surrogates.
 * @param[in] value Required quoted string view.
 * @param[out] buffer Destination, or NULL with capacity zero to measure only.
 * @param[in] capacity Destination bytes, including a required final NUL.
 * @param[out] length Required decoded byte count excluding the appended NUL.
 * @return OK, INVALID_ARG, FAIL for malformed Unicode, or NO_MEM.
 * @note Measurement returns OK. NO_MEM reports the required decoded byte count
 * in length and leaves buffer untouched. Other failures preserve both outputs.
 * Embedded decoded NUL is permitted; always use the returned explicit length.
 */
esy_err_t esy_json_string_copy(const esy_json_value_t *value, char *buffer,
                               size_t capacity, size_t *length);

/**
 * @brief Initialize/reset a writer before reading any handle state.
 * @param[out] writer Required handle; arbitrary initial storage is permitted.
 * @param[in] resources Required valid disjoint output buffer and frame workspace.
 * @return OK or INVALID_ARG. Failure leaves handle and resources unchanged.
 * @note The descriptor is copied. Resources are borrowed. Successful init writes
 * buffer[0]=0 and resets all errors and completion state. No heap is used.
 */
esy_err_t esy_json_writer_init(esy_json_writer_t *writer,
                               const esy_json_writer_resources_t *resources);

/**
 * @name Writer operations
 * @brief Emit one root and nested values with automatic separators.
 *
 * An object requires key followed by exactly one value. Array values require no
 * key. end closes the innermost container and rejects a key without a value.
 * Strings/keys accept decoded valid UTF-8; NULL is allowed only for length zero.
 * number accepts one complete JSON number lexeme, including fractions/exponents;
 * integer functions preserve all 64 bits without float conversion.
 *
 * Every operation requires an initialized writer. Returns OK, INVALID_ARG for
 * pointer/UTF-8 errors, STATE for ordering/finished/zero handle, NO_MEM for
 * buffer/depth exhaustion, or FAIL for invalid number syntax. A write failure
 * latches its error until init, except that writes after a successful finish
 * return STATE while preserving the completed result. Bytes may be partial
 * after failure and MUST NOT
 * be published. finish is the only way to obtain a complete successful result.
 * Uninitialized arbitrary handle bytes must not be passed to these functions.
 * @{
 */
/** @brief Begin an object value. @param[in,out] writer Initialized writer. */
esy_err_t esy_json_writer_begin_object(esy_json_writer_t *writer);
/** @brief Begin an array value. @param[in,out] writer Initialized writer. */
esy_err_t esy_json_writer_begin_array(esy_json_writer_t *writer);
/** @brief Close the innermost container. @param[in,out] writer Initialized writer. */
esy_err_t esy_json_writer_end(esy_json_writer_t *writer);
/** @brief Emit an object key. @param[in,out] writer Writer.
 * @param[in] key Decoded UTF-8 bytes. @param[in] length Key byte length. */
esy_err_t esy_json_writer_key(esy_json_writer_t *writer, const char *key, size_t length);
/** @brief Emit a string. @param[in,out] writer Writer.
 * @param[in] text Decoded UTF-8 bytes. @param[in] length String byte length. */
esy_err_t esy_json_writer_string(esy_json_writer_t *writer, const char *text, size_t length);
/** @brief Emit an unsigned integer. @param[in,out] writer Writer.
 * @param[in] value Exact integer. */
esy_err_t esy_json_writer_u64(esy_json_writer_t *writer, uint64_t value);
/** @brief Emit a signed integer. @param[in,out] writer Writer.
 * @param[in] value Exact integer. */
esy_err_t esy_json_writer_i64(esy_json_writer_t *writer, int64_t value);
/** @brief Emit a boolean. @param[in,out] writer Writer.
 * @param[in] value Boolean value. */
esy_err_t esy_json_writer_bool(esy_json_writer_t *writer, bool value);
/** @brief Emit null. @param[in,out] writer Writer. */
esy_err_t esy_json_writer_null(esy_json_writer_t *writer);
/** @brief Emit one number lexeme. @param[in,out] writer Writer.
 * @param[in] number Required decimal bytes. @param[in] length Nonzero length. */
esy_err_t esy_json_writer_number(esy_json_writer_t *writer, const char *number, size_t length);
/** @} */

/**
 * @brief Finish and borrow one complete, NUL-terminated JSON document.
 * @param[in,out] writer Initialized writer; success freezes it until init.
 * @param[out] buffer Required borrowed output pointer, unchanged on failure.
 * @param[out] length Required byte length excluding NUL, unchanged on failure.
 * @return OK (including repeated finish), INVALID_ARG, a latched write error,
 * or STATE if incomplete. An incomplete finish does not latch an error and the
 * caller may continue writing. Output remains valid until writer init/resources
 * are reused. A finished writer rejects further writes with STATE.
 */
esy_err_t esy_json_writer_finish(esy_json_writer_t *writer,
                                 const char **buffer, size_t *length);

#ifdef __cplusplus
}
#endif
#endif /* ESY_PROTO_JSON_H */

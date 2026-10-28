/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_DEFS_H
#define ESY_COMMON_DEFS_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file esy_common_defs.h
 * @brief Common types and generic macros.
 *
 * This header provides:
 * - a unified error type and common error codes;
 * - symbol visibility controls for internal APIs;
 * - core affinity bitmasks for multi-core systems;
 * - small debug helper macros used across the EsyProject codebase.
 */

/* -------------------------------------------------------------------------- */
/* Visibility Control                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @def ESY_INTERNAL
 * @brief Internal symbol visibility modifier.
 *
 * - On GCC/Clang, defaults to `__attribute__((visibility("hidden")))`.
 * - On other toolchains, expands to an empty definition.
 * - Can be used together with `-fvisibility=hidden` to reduce exported symbols.
 */
#ifndef ESY_INTERNAL
#  if defined(__GNUC__)
#    define ESY_INTERNAL __attribute__((visibility("hidden")))
#  else
#    define ESY_INTERNAL
#  endif
#endif

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Sentinel value indicating an unused/unconfigured pin.
 */
#define ESY_PIN_UNUSED 0xFFu

/**
 * @name Core Affinity Masks
 * @brief Bitmasks used to select CPU cores for task/IRQ affinity on multi-core systems.
 * @{
 */
#define ESY_CORE_0   (1u << 0) /**< Core 0 */
#define ESY_CORE_1   (1u << 1) /**< Core 1 */
#define ESY_CORE_ALL 0xFFu     /**< All cores */
///@}

/**
 * @def ESY_MAX_CORE_COUNT
 * @brief Compile-time maximum CPU core count budget used by static middleware
 *        resources.
 *
 * Platforms or applications may override this macro from their build/specs
 * configuration when the target has a different core count. Modules should use
 * runtime core count APIs for actual iteration and this macro only for static
 * storage bounds.
 */
#ifndef ESY_MAX_CORE_COUNT
#define ESY_MAX_CORE_COUNT 2u
#endif

/**
 * @def ESY_ALIGNED_STORAGE
 * @brief Define caller-owned byte storage with default C object alignment.
 *
 * Use this macro for opaque handles, static task stacks, backend private storage,
 * and other fixed-size byte buffers that must be safely cast or passed to lower
 * layers requiring natural C object alignment.
 *
 * @param name       Storage field or variable name.
 * @param size_bytes Storage size in bytes.
 */
#define ESY_ALIGNED_STORAGE(name, size_bytes) \
    union {                                   \
        max_align_t align;                    \
        uint8_t bytes[(size_bytes)];          \
    } name

/* -------------------------------------------------------------------------- */
/* Debug Helpers                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @def DEBUG_SIZEOF
 * @brief Helper macro to force a compile-time check involving `sizeof(T)`.
 *
 * Usage:
 * @code
 * DEBUG_SIZEOF(my_struct_t);
 * @endcode
 *
 * This macro generates a static function that places `sizeof(T)` into a `case` label,
 * forcing the compiler to evaluate `sizeof(T)` at compile time. It is useful for
 * quickly verifying that a type size has not changed unexpectedly.
 *
 * @note
 * This is a compile-time check pattern; it does not print anything at runtime.
 */
#define DEBUG_SIZEOF(T)                                              \
    typedef enum { __esy_enum_zero_##T = 0 } __esy_enum_zero_##T##_t; \
    static void __esy_print_sizeof_##T(void) {                        \
        switch ((__esy_enum_zero_##T##_t)0) {                         \
            case sizeof(T): ;                                         \
        }                                                             \
    }

/* -------------------------------------------------------------------------- */
/* Error Codes                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @typedef esy_err_t
 * @brief Unified error code type (`int32_t`).
 *
 * Convention:
 * - `0` indicates success.
 * - Negative values indicate failures (see error code enum below).
 */
typedef int32_t esy_err_t;

/**
 * @enum
 * @brief Common error code set.
 *
 * All error codes are negative to keep `0` reserved for success.
 */
enum {
    ESY_OK                  = 0,    /**< Success */
    ESY_ERR_FAIL            = -1,   /**< Generic failure */
    ESY_ERR_INVALID_ARG     = -2,   /**< Invalid argument */
    ESY_ERR_NO_MEM          = -3,   /**< Out of memory */
    ESY_ERR_TIMEOUT         = -4,   /**< Timeout */
    ESY_ERR_BUSY            = -5,   /**< Resource busy */
    ESY_ERR_NOT_SUPPORTED   = -6,   /**< Not supported */
    ESY_ERR_NOT_IMPLEMENTED = -7,   /**< Not implemented */
    ESY_ERR_IO              = -8,   /**< I/O error */
    ESY_ERR_CRC_FAIL        = -9,   /**< CRC check failed */
    ESY_ERR_STATE           = -10,  /**< Invalid state / wrong call order */
    ESY_ERR_HASH_FAIL       = -11,  /**< Cryptographic hash check failed */
};

#endif /* ESY_COMMON_DEFS_H */

/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FAULT_DEFS_H
#define ESY_FAULT_DEFS_H

#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

/** @brief Place the fatal call graph in the platform's startup-copied RAM section. */
#if defined(__GNUC__)
#if defined(__XTENSA__)
#define ESY_FAULT_RAM_FUNC __attribute__((noinline, section(".iram1.esy_fault")))
#elif defined(__ARM_ARCH_7EM__)
#define ESY_FAULT_RAM_FUNC __attribute__((noinline, section(".RamFunc.esy_fault")))
#else
#define ESY_FAULT_RAM_FUNC __attribute__((noinline, section(".time_critical.esy_fault")))
#endif
#else
#define ESY_FAULT_RAM_FUNC
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fatal event classifications shared by HAL and OS integration. */
typedef enum {
  ESY_FAULT_REASON_HARDFAULT = 1,
  ESY_FAULT_REASON_STACK_OVERFLOW = 2,
  ESY_FAULT_REASON_ASSERT = 3,
  ESY_FAULT_REASON_SOFTWARE = 4,
  ESY_FAULT_REASON_MEMMANAGE = 5,
  ESY_FAULT_REASON_BUSFAULT = 6,
  ESY_FAULT_REASON_USAGEFAULT = 7,
  ESY_FAULT_REASON_SECUREFAULT = 8
} esy_fault_reason_t;

/** @brief Register mapping recorded in a portable snapshot. */
typedef enum {
  ESY_FAULT_ARCH_ARM_M = 1,
  ESY_FAULT_ARCH_XTENSA = 2
} esy_fault_arch_t;

/** @brief Validity and format flags; unavailable fields are recorded as zero. */
enum {
  ESY_FAULT_FLAG_CORE_REGS_VALID = 1u << 0,
  ESY_FAULT_FLAG_STACK_VALID = 1u << 1,
  ESY_FAULT_FLAG_FP_EXTENDED = 1u << 2,
  ESY_FAULT_FLAG_FRAME_UNSUPPORTED = 1u << 3,
  ESY_FAULT_FLAG_STACKING_ERROR = 1u << 4
};

/** @brief Register slots; unused architecture-specific slots are zero. */
#define ESY_FAULT_REGISTER_COUNT 32u
/** @brief Maximum bounded stack window supported by the initial fault port. */
#define ESY_FAULT_STACK_MAX_BYTES 1024u

/**
 * @brief Borrowed fatal snapshot, valid only during the capture callback.
 *
 * ARM uses r[0..15]=R0..R15 and r[16]=xPSR; r[13] is the reconstructed
 * pre-exception SP when CORE_REGS_VALID is set. Xtensa uses r[0..15]=A0..A15,
 * r[16]=PC, r[17]=PS, r[18]=EXCCAUSE, r[19]=EXCVADDR.
 * On ARM, MSP/PSP and limits are the raw entry values. R4..R11 are always entry
 * values; other r[] entries require CORE_REGS_VALID. Architecture-specific
 * special-register fields are zero when unavailable. stack points into validated SRAM;
 * Software ARM reports may retain the fatal-call LR/PC without claiming a valid
 * hardware-stacked frame; details identify the original assertion/overflow.
 * copy at most stack_length bytes synchronously and never retain the pointer.
 * Stack bytes can still be inconsistent if hardware/another core corrupted RAM.
 */
typedef struct {
  uint32_t r[ESY_FAULT_REGISTER_COUNT]; /**< R0..R15, xPSR. */
  uint32_t exc_return; /**< Raw exception return token. */
  uint32_t msp, psp; /**< Original stack pointers. */
  uint32_t msplim, psplim; /**< Original stack limits. */
  uint32_t control, primask, basepri, faultmask; /**< Original execution controls. */
  uint32_t cfsr, hfsr, dfsr, afsr; /**< Fault status registers. */
  uint32_t mmfar, bfar, shcsr; /**< Fault address/status registers. */
  uint32_t reason, core_id, flags; /**< Event, core and validity flags. */
  uint32_t architecture; /**< esy_fault_arch_t register mapping. */
  uint32_t detail0, detail1; /**< Caller details; addresses are opaque values. */
  const uint8_t *stack; /**< Bounded SRAM window; NULL when unavailable. */
  uint32_t stack_address, stack_length; /**< Window origin and readable bytes. */
} esy_fault_snapshot_t;

/**
 * @brief Capture one fatal snapshot in the port's fatal execution context.
 * @param snapshot Borrowed snapshot; never NULL.
 * @param ctx Borrowed binding context, retained unchanged until system reset.
 * @return Capture status; the port's fatal continuation follows regardless.
 * @warning Callback and all its callees/constants must reside in RAM and use no
 * OS, locks, heap, Logger, Flash, FP instructions or unbounded waits. It may only
 * write its reserved RAM and copy the supplied bounded snapshot/stack. Cortex
 * ports switch to dedicated emergency stacks; the first fatal core invokes it
 * once and other fatal cores park without initiating reset. ESP32 uses the
 * native panic stack and preserves the configured native panic continuation.
 */
typedef esy_err_t (*esy_fault_capture_t)(const esy_fault_snapshot_t *snapshot,
                                        void *ctx);

/** @brief Boot-lifetime binding copied by HAL init; no resource allocation. */
typedef struct {
  esy_fault_capture_t capture; /**< Required RAM-resident capture callback. */
  void *ctx; /**< RAM-resident context, alive and immutable until reset. */
  uint32_t stack_snapshot_bytes; /**< 0..ESY_FAULT_STACK_MAX_BYTES. */
} esy_fault_config_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_FAULT_DEFS_H */

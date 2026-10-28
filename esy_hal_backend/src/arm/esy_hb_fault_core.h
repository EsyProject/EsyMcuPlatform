/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_FAULT_CORE_H
#define ESY_HB_FAULT_CORE_H

#include <stdbool.h>
#include <stddef.h>

#define FAULT_RAM ESY_FAULT_RAM_FUNC __attribute__((used))
#define FAULT_NAKED ESY_FAULT_RAM_FUNC __attribute__((naked, used))
#define FAULT_CORE_STRIDE 4096u
#define FAULT_CORE_SHIFT 12u
#define FAULT_STACK_OFFSET 128u
#define FAULT_EMERGENCY_STACK_BYTES 2048u
#define FAULT_STACK_TOP (FAULT_STACK_OFFSET + FAULT_EMERGENCY_STACK_BYTES)
#define FAULT_RAW_MSP 32u
#define FAULT_RAW_PSP 36u
#define FAULT_RAW_EXC_RETURN 40u
#define FAULT_RAW_MSPLIM 44u
#define FAULT_RAW_PSPLIM 48u
#define FAULT_RAW_CONTROL 52u
#define FAULT_RAW_PRIMASK 56u
#define FAULT_RAW_BASEPRI 60u
#define FAULT_RAW_FAULTMASK 64u
#define FAULT_RAW_IPSR 68u
#define FAULT_RAW_REASON 72u
#define FAULT_RAW_DETAIL0 76u
#define FAULT_RAW_DETAIL1 80u
#define FAULT_BASIC_FRAME_BYTES 32u
#define FAULT_FP_FRAME_BYTES 72u
#define FAULT_STACK_ERROR_MASK 0x00103838u
#define FAULT_SCB_CFSR 0xe000ed28u
#define FAULT_SCB_HFSR 0xe000ed2cu
#define FAULT_SCB_DFSR 0xe000ed30u
#define FAULT_SCB_MMFAR 0xe000ed34u
#define FAULT_SCB_BFAR 0xe000ed38u
#define FAULT_SCB_AFSR 0xe000ed3cu
#define FAULT_SCB_SHCSR 0xe000ed24u

typedef struct {
  uint32_t r4_r11[8];
  uint32_t msp, psp, exc_return, msplim, psplim;
  uint32_t control, primask, basepri, faultmask, ipsr;
  uint32_t reason, detail0, detail1;
} fault_raw_t;

typedef union {
  struct {
    fault_raw_t raw;
    uint8_t reserved[FAULT_STACK_OFFSET - sizeof(fault_raw_t)];
    _Alignas(8) uint8_t emergency_stack[FAULT_EMERGENCY_STACK_BYTES];
    esy_fault_snapshot_t snapshot;
  } state;
  uint8_t stride[FAULT_CORE_STRIDE];
} fault_core_t;

typedef struct {
  fault_core_t cores[FAULT_CORE_COUNT];
  esy_fault_config_t config;
  uint32_t claimed;
  volatile uint32_t *claim_lock;
  bool initialized;
#if defined(FAULT_VECTOR_COUNT)
  _Alignas(512) uint32_t vectors[FAULT_VECTOR_COUNT];
#endif
} esy_hb_fault_ctx_t;

static esy_hb_fault_ctx_t s_ctx;

_Static_assert(sizeof(fault_core_t) == FAULT_CORE_STRIDE, "Fault core stride changed");
_Static_assert(offsetof(fault_core_t, state.emergency_stack) == FAULT_STACK_OFFSET,
               "Fault stack offset changed");
_Static_assert(offsetof(fault_raw_t, detail1) == FAULT_RAW_DETAIL1,
               "Fault register layout changed");
#if FAULT_CLAIM_STYLE == 1
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
               "Fault capture requires native lock-free 32-bit atomics");
#endif

static FAULT_RAM __attribute__((noreturn)) void fault_park(void) {
  for (;;) __asm volatile("wfe" ::: "memory");
}

static FAULT_RAM __attribute__((noreturn)) void fault_reset(void) {
  fault_port_reset();
}

static FAULT_RAM uint32_t fault_status(uint32_t address) {
  return *(const volatile uint32_t *)(uintptr_t)address;
}

static FAULT_RAM bool fault_sram_range(uint32_t address, uint32_t length) {
  return address >= SRAM_BASE && address < SRAM_END && length <= SRAM_END - address;
}

static FAULT_RAM void fault_decode(fault_core_t *core) {
  fault_raw_t *raw = &core->state.raw;
  esy_fault_snapshot_t *snapshot = &core->state.snapshot;
  volatile uint8_t *bytes = (volatile uint8_t *)snapshot;
  for (size_t i = 0u; i < sizeof(*snapshot); ++i) bytes[i] = 0u;
  for (uint32_t i = 0u; i < 8u; ++i) snapshot->r[4u + i] = raw->r4_r11[i];
  snapshot->exc_return = raw->exc_return;
  snapshot->msp = raw->msp;
  snapshot->psp = raw->psp;
  snapshot->msplim = raw->msplim;
  snapshot->psplim = raw->psplim;
  snapshot->control = raw->control;
  snapshot->primask = raw->primask;
  snapshot->basepri = raw->basepri;
  snapshot->faultmask = raw->faultmask;
#if FAULT_HAS_STATUS
  snapshot->cfsr = fault_status(FAULT_SCB_CFSR);
  snapshot->hfsr = fault_status(FAULT_SCB_HFSR);
  snapshot->dfsr = fault_status(FAULT_SCB_DFSR);
  snapshot->afsr = fault_status(FAULT_SCB_AFSR);
  snapshot->mmfar = fault_status(FAULT_SCB_MMFAR);
  snapshot->bfar = fault_status(FAULT_SCB_BFAR);
  snapshot->shcsr = fault_status(FAULT_SCB_SHCSR);
#endif
  snapshot->architecture = ESY_FAULT_ARCH_ARM_M;
  snapshot->core_id = (uint32_t)(core - s_ctx.cores);
  snapshot->detail0 = raw->detail0;
  snapshot->detail1 = raw->detail1;
  snapshot->reason = raw->reason;
  if (raw->reason != 0u) {
    if (raw->reason > ESY_FAULT_REASON_SECUREFAULT) snapshot->reason = ESY_FAULT_REASON_SOFTWARE;
    snapshot->r[14] = raw->exc_return;
    snapshot->r[15] = raw->exc_return;
    return;
  }
  snapshot->reason = raw->ipsr == 4u ? ESY_FAULT_REASON_MEMMANAGE :
                     raw->ipsr == 5u ? ESY_FAULT_REASON_BUSFAULT :
                     raw->ipsr == 6u ? ESY_FAULT_REASON_USAGEFAULT :
                     raw->ipsr == 7u ? ESY_FAULT_REASON_SECUREFAULT :
                                      ESY_FAULT_REASON_HARDFAULT;
  /* Only same-security secure basic/extended frames are decoded. No FP
   * instructions are issued, so pending lazy FP saves are never forced. */
  uint32_t token = raw->exc_return;
  uint32_t basic_token = token | 0x10u;
  if (basic_token != 0xfffffff1u && basic_token != 0xfffffff9u &&
      basic_token != 0xfffffffdu) {
    snapshot->flags = ESY_FAULT_FLAG_FRAME_UNSUPPORTED;
    return;
  }
  if ((snapshot->cfsr & FAULT_STACK_ERROR_MASK) != 0u) {
    snapshot->flags = ESY_FAULT_FLAG_STACKING_ERROR;
    return;
  }
  uint32_t frame = (token & 4u) != 0u ? raw->psp : raw->msp;
  uint32_t fp_bytes = (token & 0x10u) == 0u ? FAULT_FP_FRAME_BYTES : 0u;
  if (fp_bytes != 0u) snapshot->flags |= ESY_FAULT_FLAG_FP_EXTENDED;
  if ((frame & 3u) != 0u ||
      !fault_sram_range(frame, FAULT_BASIC_FRAME_BYTES + fp_bytes)) return;
  const volatile uint32_t *registers = (const volatile uint32_t *)(uintptr_t)frame;
  uint32_t xpsr = registers[7];
  if ((xpsr & (1u << 24u)) == 0u) return;
  uint32_t padding = (xpsr & (1u << 9u)) != 0u ? sizeof(uint32_t) : 0u;
  uint32_t frame_bytes = FAULT_BASIC_FRAME_BYTES + fp_bytes + padding;
  if (!fault_sram_range(frame, frame_bytes)) return;
  for (uint32_t i = 0u; i < 4u; ++i) snapshot->r[i] = registers[i];
  snapshot->r[12] = registers[4];
  snapshot->r[13] = frame + frame_bytes;
  snapshot->r[14] = registers[5];
  snapshot->r[15] = registers[6];
  snapshot->r[16] = xpsr;
  snapshot->flags |= ESY_FAULT_FLAG_CORE_REGS_VALID;
  uint32_t address = snapshot->r[13];
  if (s_ctx.config.stack_snapshot_bytes != 0u && fault_sram_range(address, 1u)) {
    uint32_t length = s_ctx.config.stack_snapshot_bytes;
    if (length > SRAM_END - address) length = SRAM_END - address;
    snapshot->stack = (const uint8_t *)(uintptr_t)address;
    snapshot->stack_address = address;
    snapshot->stack_length = length;
    snapshot->flags |= ESY_FAULT_FLAG_STACK_VALID;
  }
}

static FAULT_RAM __attribute__((noreturn)) void fault_dispatch(fault_core_t *core) {
  /* The port fixes the claim mechanism at compile time. */
#if FAULT_CLAIM_STYLE == 1
  uint32_t expected = 0u;
  /* First fatal wins across ISR/task and both cores. Native M33 exclusive
   * instructions avoid OS/hardware spinlock dependencies in a broken system. */
  if (!__atomic_compare_exchange_n(&s_ctx.claimed, &expected, 1u, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) fault_park();
#elif FAULT_CLAIM_STYLE == 2
  if (s_ctx.claim_lock != NULL && *s_ctx.claim_lock == 0u) fault_park();
  if (s_ctx.claim_lock == NULL) {
    if (s_ctx.claimed != 0u) fault_park();
    s_ctx.claimed = 1u;
  }
#else
  if (s_ctx.claimed != 0u) fault_park();
  s_ctx.claimed = 1u;
#endif
  if (s_ctx.initialized) {
    fault_decode(core);
    (void)s_ctx.config.capture(&core->state.snapshot, s_ctx.config.ctx);
  }
  fault_reset();
}

/* Entry wrappers pass the per-core slot in R3, without using a stack. */
static FAULT_NAKED __attribute__((noreturn)) void fault_save_and_dispatch(void) {
  __asm volatile(
      ".syntax unified\n"
#if defined(__ARM_ARCH_6M__)
      "stmia r3!, {r4-r7}\nsubs r3, #16\n"
      "mov r0, r8\nstr r0, [r3, #16]\n"
      "mov r0, r9\nstr r0, [r3, #20]\n"
      "mov r0, r10\nstr r0, [r3, #24]\n"
      "mov r0, r11\nstr r0, [r3, #28]\n"
#else
      "stmia r3, {r4-r11}\n"
#endif
      "mrs r0, msp\nstr r0, [r3, #32]\n"
      "mrs r0, psp\nstr r0, [r3, #36]\n"
      "mov r0, lr\nstr r0, [r3, #40]\n"
#if defined(__ARM_ARCH_8M_MAIN__)
      "mrs r0, msplim\nstr r0, [r3, #44]\n"
      "mrs r0, psplim\nstr r0, [r3, #48]\n"
#else
      "movs r0, #0\nstr r0, [r3, #44]\nstr r0, [r3, #48]\n"
#endif
      "mrs r0, control\nstr r0, [r3, #52]\n"
      "mrs r0, primask\nstr r0, [r3, #56]\ncpsid i\n"
#if defined(__ARM_ARCH_6M__)
      "movs r0, #0\nstr r0, [r3, #60]\nstr r0, [r3, #64]\n"
#else
      "mrs r0, basepri\nstr r0, [r3, #60]\n"
      "mrs r0, faultmask\nstr r0, [r3, #64]\n"
#endif
      "mrs r0, ipsr\nstr r0, [r3, #68]\n"
#if defined(__ARM_ARCH_8M_MAIN__)
      "movs r0, #0\nmsr msplim, r0\n"
#endif
      "ldr r0, =2176\nadds r0, r3, r0\nmsr msp, r0\n"
#if defined(__ARM_ARCH_8M_MAIN__)
      "addw r0, r3, #128\nmsr msplim, r0\n"
#endif
      "movs r0, #0\nmsr control, r0\nisb\n"
      "mov r0, r3\nb fault_dispatch\n");
}

static FAULT_NAKED void fault_hardware_entry(void) {
  __asm volatile(
      ".syntax unified\n"
#if FAULT_CORE_COUNT > 1
      "ldr r0, =0xd0000000\nldr r0, [r0]\nlsls r0, r0, #12\n"
      "ldr r3, =s_ctx\nadds r3, r3, r0\n"
#else
      "ldr r3, =s_ctx\n"
#endif
      "movs r0, #0\nstr r0, [r3, #72]\nstr r0, [r3, #76]\nstr r0, [r3, #80]\n"
      "b fault_save_and_dispatch\n");
}

static FAULT_NAKED __attribute__((noreturn)) void fault_software_entry(void) {
  __asm volatile(
      ".syntax unified\n"
      "cmp r0, #1\nblo 1f\ncmp r0, #8\nbls 2f\n1: movs r0, #4\n2:\n"
#if FAULT_CORE_COUNT > 1
      "mov r12, r2\nldr r3, =0xd0000000\nldr r3, [r3]\nlsls r3, r3, #12\n"
      "ldr r2, =s_ctx\nadds r3, r3, r2\n"
      "str r0, [r3, #72]\nstr r1, [r3, #76]\nmov r2, r12\nstr r2, [r3, #80]\n"
#else
      "ldr r3, =s_ctx\nstr r0, [r3, #72]\nstr r1, [r3, #76]\nstr r2, [r3, #80]\n"
#endif
      "b fault_save_and_dispatch\n");
}

#endif /* ESY_HB_FAULT_CORE_H */

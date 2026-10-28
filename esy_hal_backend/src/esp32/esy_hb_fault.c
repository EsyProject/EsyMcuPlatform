/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_fault.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_idf_version.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "xtensa_context.h"

#if !CONFIG_IDF_TARGET_ESP32 || !defined(__XTENSA__)
#error "The ESP32 fault port supports the classic Xtensa ESP32 only"
#endif
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 3, 4)
#error "The private ESP32 panic adapter requires ESP-IDF 5.3.4"
#endif
#if !CONFIG_ESP_PANIC_HANDLER_IRAM
#error "The ESP32 fault port requires CONFIG_ESP_PANIC_HANDLER_IRAM=y"
#endif

typedef struct {
  uint32_t valid;
  uint32_t reason;
  uint32_t detail0;
  uint32_t detail1;
} software_fault_t;

typedef struct {
  esy_fault_config_t binding;
  esy_fault_snapshot_t snapshot;
  software_fault_t software[SOC_CPU_CORES_NUM];
  uint32_t captured;
  bool initialized;
} esy_hb_fault_ctx_t;

static DRAM_ATTR esy_hb_fault_ctx_t s_ctx;
static const DRAM_ATTR char k_software_abort[] = "Esy software fatal";

_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), NULL),
               "Fatal admission requires native lock-free 32-bit atomics");

/* GNU ld redirects calls from the native panic entry to this adapter and leaves
 * the SDK implementation available here. The SDK permits a debugger return. */
void __real_esp_panic_handler(panic_info_t *info);
void __wrap_esp_panic_handler(panic_info_t *info);

static IRAM_ATTR bool dram_range(const void *data, size_t length) {
  const uintptr_t address = (uintptr_t)data;
  return address >= (uintptr_t)SOC_DRAM_LOW &&
         address < (uintptr_t)SOC_DRAM_HIGH &&
         length <= (size_t)((uintptr_t)SOC_DRAM_HIGH - address);
}

static IRAM_ATTR void snapshot_clear(esy_fault_snapshot_t *snapshot) {
  volatile uint8_t *bytes = (volatile uint8_t *)snapshot;
  for (size_t index = 0u; index < sizeof(*snapshot); ++index) bytes[index] = 0u;
}

static IRAM_ATTR void snapshot_frame(esy_fault_snapshot_t *snapshot,
                                      const void *address) {
  if (((uintptr_t)address & (sizeof(uint32_t) - 1u)) != 0u ||
      !dram_range(address, sizeof(XtExcFrame))) {
    snapshot->flags = ESY_FAULT_FLAG_FRAME_UNSUPPORTED;
    return;
  }
  const volatile XtExcFrame *frame = address;
  snapshot->r[0] = (uint32_t)frame->a0;
  snapshot->r[1] = (uint32_t)frame->a1;
  snapshot->r[2] = (uint32_t)frame->a2;
  snapshot->r[3] = (uint32_t)frame->a3;
  snapshot->r[4] = (uint32_t)frame->a4;
  snapshot->r[5] = (uint32_t)frame->a5;
  snapshot->r[6] = (uint32_t)frame->a6;
  snapshot->r[7] = (uint32_t)frame->a7;
  snapshot->r[8] = (uint32_t)frame->a8;
  snapshot->r[9] = (uint32_t)frame->a9;
  snapshot->r[10] = (uint32_t)frame->a10;
  snapshot->r[11] = (uint32_t)frame->a11;
  snapshot->r[12] = (uint32_t)frame->a12;
  snapshot->r[13] = (uint32_t)frame->a13;
  snapshot->r[14] = (uint32_t)frame->a14;
  snapshot->r[15] = (uint32_t)frame->a15;
  snapshot->r[16] = (uint32_t)frame->pc;
  snapshot->r[17] = (uint32_t)frame->ps;
  snapshot->r[18] = (uint32_t)frame->exccause;
  snapshot->r[19] = (uint32_t)frame->excvaddr;
  snapshot->flags = ESY_FAULT_FLAG_CORE_REGS_VALID;

  const uintptr_t sp = (uintptr_t)(uint32_t)frame->a1;
  if (s_ctx.binding.stack_snapshot_bytes == 0u || (sp & 15u) != 0u ||
      !dram_range((const void *)sp, 1u)) return;
  size_t available = (uintptr_t)SOC_DRAM_HIGH - sp;
  if (available > s_ctx.binding.stack_snapshot_bytes) {
    available = s_ctx.binding.stack_snapshot_bytes;
  }
  snapshot->stack = (const uint8_t *)sp;
  snapshot->stack_address = (uint32_t)sp;
  snapshot->stack_length = (uint32_t)available;
  snapshot->flags |= ESY_FAULT_FLAG_STACK_VALID;
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info) {
  /* ESP-IDF's architecture entry has already stalled peers and established its
   * RTC watchdog before this call. No task/IPC/Flash API belongs in capture. */
  if (__atomic_load_n(&s_ctx.initialized, __ATOMIC_ACQUIRE) &&
      __atomic_exchange_n(&s_ctx.captured, 1u, __ATOMIC_ACQ_REL) == 0u) {
    esy_fault_snapshot_t *snapshot = &s_ctx.snapshot;
    snapshot_clear(snapshot);
    snapshot->architecture = ESY_FAULT_ARCH_XTENSA;
    snapshot->core_id = (uint32_t)esp_cpu_get_core_id();
    snapshot->reason = g_panic_abort ? ESY_FAULT_REASON_SOFTWARE : ESY_FAULT_REASON_HARDFAULT;
    if (((uintptr_t)info & (sizeof(uint32_t) - 1u)) == 0u &&
        dram_range(info, sizeof(*info))) {
      if (info->core >= 0 && info->core < SOC_CPU_CORES_NUM) {
        snapshot->core_id = (uint32_t)info->core;
      }
      snapshot->detail0 = (uint32_t)info->exception;
      snapshot_frame(snapshot, info->frame);
    } else {
      snapshot->flags = ESY_FAULT_FLAG_FRAME_UNSUPPORTED;
    }
    if (snapshot->core_id < SOC_CPU_CORES_NUM) {
      software_fault_t *software = &s_ctx.software[snapshot->core_id];
      if (__atomic_load_n(&software->valid, __ATOMIC_ACQUIRE) != 0u) {
        snapshot->reason = software->reason;
        snapshot->detail0 = software->detail0;
        snapshot->detail1 = software->detail1;
        __atomic_store_n(&software->valid, 0u, __ATOMIC_RELEASE);
      }
    }
    (void)s_ctx.binding.capture(snapshot, s_ctx.binding.ctx);
    __asm volatile("memw" ::: "memory");
  }
  /* Retain IDF panic output, native core dump, reset policy and debugger flow. */
  __real_esp_panic_handler(info);
}

esy_err_t esy_hb_fault_init(const esy_fault_config_t *config) {
  if (config == NULL || config->capture == NULL ||
      config->stack_snapshot_bytes > ESY_FAULT_STACK_MAX_BYTES) return ESY_ERR_INVALID_ARG;
  if (__atomic_load_n(&s_ctx.initialized, __ATOMIC_ACQUIRE)) return ESY_ERR_STATE;
  if (!esp_ptr_in_iram((const void *)config->capture) ||
      (config->ctx != NULL && !esp_ptr_in_dram(config->ctx))) return ESY_ERR_INVALID_ARG;
  s_ctx.binding = *config;
  __atomic_store_n(&s_ctx.captured, 0u, __ATOMIC_RELAXED);
  /* The one startup owner publishes the copied binding to peer panic readers. */
  __atomic_store_n(&s_ctx.initialized, true, __ATOMIC_RELEASE);
  return ESY_OK;
}

ESY_FAULT_RAM_FUNC __attribute__((noreturn))
void esy_hb_fault_raise(esy_fault_reason_t reason, uint32_t detail0,
                        uint32_t detail1) {
  uint32_t previous;
  __asm volatile("rsil %0, 15" : "=a"(previous) :: "memory");
  (void)previous;
  uint32_t core = (uint32_t)esp_cpu_get_core_id();
  if (core < SOC_CPU_CORES_NUM) {
    software_fault_t *software = &s_ctx.software[core];
    software->reason = (reason >= ESY_FAULT_REASON_HARDFAULT &&
                        reason <= ESY_FAULT_REASON_SECUREFAULT)
                           ? (uint32_t)reason : ESY_FAULT_REASON_SOFTWARE;
    software->detail0 = detail0;
    software->detail1 = detail1;
    __atomic_store_n(&software->valid, 1u, __ATOMIC_RELEASE);
  }
  /* Native abort enters the same exception/peer-stall/panic path as SDK faults. */
  esp_system_abort(k_software_abort);
}

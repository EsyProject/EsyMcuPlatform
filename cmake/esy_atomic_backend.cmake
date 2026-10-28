# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

include("${CMAKE_CURRENT_LIST_DIR}/esy_target_device.cmake")
esy_configure_target_device()
include("${CMAKE_CURRENT_LIST_DIR}/esy_port.cmake")

# Backend capability is derived from the toolchain and supplied SDK targets.
# It is not a consumer feature switch and must not be cached across toolchains.
include(CheckCSourceCompiles)
unset(ESY_NATIVE_ATOMIC_U32_LOCK_FREE)
unset(ESY_NATIVE_ATOMIC_U32_LOCK_FREE CACHE)
check_c_source_compiles([=[
#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
_Static_assert(sizeof(unsigned int) == sizeof(uint32_t), "32-bit unsigned required");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "always-lock-free integers required");
int main(void) { return 0; }
]=] ESY_NATIVE_ATOMIC_U32_LOCK_FREE)

set(ESY_ATOMIC_IRQ_SPINLOCK_BACKEND OFF)
if(NOT ESY_NATIVE_ATOMIC_U32_LOCK_FREE)
    esy_call_port_hook(select_atomic_backend)
    if(NOT ESY_ATOMIC_IRQ_SPINLOCK_BACKEND)
        message(FATAL_ERROR
            "EsyMcuPlatform needs always-lock-free 32-bit atomics or the RP2040 pico_atomic SDK provider")
    endif()
else()
    message(STATUS "EsyMcuPlatform atomics: native always-lock-free 32-bit backend")
endif()

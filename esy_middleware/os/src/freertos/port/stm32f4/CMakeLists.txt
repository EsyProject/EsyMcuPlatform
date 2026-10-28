# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

target_compile_definitions(esy_os PRIVATE ESY_PLATFORM_STM32F4)
add_library(esy_os_stm32f4_interrupts OBJECT esy_os_stm32f4_systick.c)
target_link_libraries(esy_os_stm32f4_interrupts
    PRIVATE esy_os esy_hal esy_common_defs esy_opt_default)
esy_register_interrupt_target(esy_os esy_os_stm32f4_interrupts)

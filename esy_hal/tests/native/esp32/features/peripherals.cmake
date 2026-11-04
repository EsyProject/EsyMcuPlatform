# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

include("${CMAKE_CURRENT_LIST_DIR}/core.cmake")
foreach(native_feature IN ITEMS COMMON_DS GPIO UART SPI I2C PWM TIMER TIMER_CAPTURE
        WATCHDOG FLASH ADC CAN OS_QUEUE OS_MUTEX OS_SEMAPHORE OS_TIMER OS_WORK
        OS_STREAM_BUFFER OS_MSG_BUFFER)
    set(ENABLE_ESY_${native_feature} ON)
endforeach()

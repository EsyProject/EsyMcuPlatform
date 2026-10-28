# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

# esy_middleware/os/src/freertos/CMakeLists.txt

include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/esy_target_device.cmake")
esy_configure_target_device()
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/esy_port.cmake")

if(NOT ENABLE_ESY_SYSTEM)
    message(FATAL_ERROR "esy_os requires ENABLE_ESY_SYSTEM")
endif()
if(ENABLE_ESY_OS_WORK AND (NOT ENABLE_ESY_OS_QUEUE OR NOT ENABLE_ESY_OS_TIMER))
    message(FATAL_ERROR "ENABLE_ESY_OS_WORK requires ENABLE_ESY_OS_QUEUE and ENABLE_ESY_OS_TIMER")
endif()

set(MODULE_SRCS esy_os.c esy_os_task.c esy_os_task_snapshot.c)

if(ENABLE_ESY_OS_QUEUE)
    list(APPEND MODULE_SRCS esy_os_queue.c)
endif()

if(ENABLE_ESY_OS_MUTEX)
    list(APPEND MODULE_SRCS esy_os_mutex.c)
endif()

if(ENABLE_ESY_OS_SEMAPHORE)
    list(APPEND MODULE_SRCS esy_os_semaphore.c)
endif()

if(ENABLE_ESY_OS_TIMER)
    list(APPEND MODULE_SRCS esy_os_timer.c)
endif()

if(ENABLE_ESY_OS_WORK)
    list(APPEND MODULE_SRCS esy_os_work.c)
endif()

if(ENABLE_ESY_OS_STREAM_BUFFER)
    list(APPEND MODULE_SRCS esy_os_stream_buffer.c)
endif()

if(ENABLE_ESY_OS_MSG_BUFFER)
    list(APPEND MODULE_SRCS esy_os_msg_buffer.c)
endif()

target_sources(esy_os PRIVATE ${MODULE_SRCS})

target_link_libraries(esy_os
    PUBLIC esy_common_defs
    PRIVATE
        ${FREE_RTOS_KERNEL_LIB}
        esy_opt_default esy_common esy_hal
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/port/${ESY_BUILD_PORT}/CMakeLists.txt")
    add_subdirectory("port/${ESY_BUILD_PORT}")
endif()

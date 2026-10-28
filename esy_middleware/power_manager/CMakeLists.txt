# Power manager middleware.

add_library(esy_power_manager STATIC)

target_include_directories(esy_power_manager
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(esy_power_manager
    PUBLIC
        esy_common
    PRIVATE
        esy_opt_default
)

add_library(esy_power_hal_backend STATIC)
target_include_directories(esy_power_hal_backend
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

add_library(esy_power_wakeup_timer_adapter STATIC)
target_include_directories(esy_power_wakeup_timer_adapter
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)
target_link_libraries(esy_power_wakeup_timer_adapter
    PUBLIC
        esy_power_manager
    PRIVATE
        esy_common
        esy_opt_default
        esy_hal
)
target_link_libraries(esy_power_hal_backend
    PUBLIC
        esy_power_manager
        esy_hal
    PRIVATE
        esy_common
        esy_opt_default
)

if(ESY_RTOS STREQUAL "FreeRTOS")
    add_library(esy_power_freertos_adapter STATIC)
    target_include_directories(esy_power_freertos_adapter
        PUBLIC
            ${CMAKE_CURRENT_LIST_DIR}/include
    )
    target_link_libraries(esy_power_freertos_adapter
        PUBLIC
            esy_power_manager
            esy_os
        PRIVATE
            esy_common
            esy_opt_default
    )
endif()

add_subdirectory(src)

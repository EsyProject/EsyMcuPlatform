target_sources(esy_power_manager PRIVATE
    esy_power_manager.c
)

target_sources(esy_power_hal_backend PRIVATE
    esy_power_hal_backend.c
)

target_sources(esy_power_wakeup_timer_adapter PRIVATE
    esy_power_wakeup_timer_adapter.c
)

if(TARGET esy_power_freertos_adapter)
    target_sources(esy_power_freertos_adapter PRIVATE
        esy_power_tickless_adapter.c
    )
endif()

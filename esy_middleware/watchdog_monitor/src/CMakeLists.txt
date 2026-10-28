target_sources(esy_watchdog_monitor_core PRIVATE esy_watchdog_monitor_core.c)
if(ESY_RTOS STREQUAL "FreeRTOS")
    target_sources(esy_watchdog_monitor PRIVATE esy_watchdog_monitor.c)
endif()

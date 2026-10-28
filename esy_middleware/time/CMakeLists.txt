# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_time requires target ${dependency}")
    endif()
endforeach()

add_library(esy_time STATIC src/esy_time.c)
target_compile_features(esy_time PUBLIC c_std_11)
target_include_directories(esy_time PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_time PUBLIC esy_common_defs PRIVATE esy_opt_default)

if(ENABLE_ESY_TIME_SNTP)
    add_library(esy_sntp STATIC src/esy_sntp.c)
    target_compile_features(esy_sntp PUBLIC c_std_11)
    target_include_directories(esy_sntp PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(esy_sntp PUBLIC esy_time PRIVATE esy_opt_default)
endif()

if(ENABLE_ESY_TIME_RTC)
    if(NOT ENABLE_ESY_RTC)
        message(FATAL_ERROR "ENABLE_ESY_TIME_RTC requires ENABLE_ESY_RTC")
    endif()
    foreach(dependency esy_hal esy_common_utils)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "esy_time_rtc requires target ${dependency}")
        endif()
    endforeach()
    add_library(esy_time_rtc STATIC src/esy_time_rtc.c)
    target_compile_features(esy_time_rtc PUBLIC c_std_11)
    target_include_directories(esy_time_rtc PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(esy_time_rtc PUBLIC esy_time esy_hal
        PRIVATE esy_common_utils esy_opt_default)
endif()

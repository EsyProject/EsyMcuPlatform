# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

foreach(dependency esy_common_defs esy_opt_default)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "esy_sensor requires target ${dependency}")
    endif()
endforeach()

add_library(esy_sensor STATIC src/esy_sensor.c)
target_compile_features(esy_sensor PUBLIC c_std_11)
target_include_directories(esy_sensor PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(esy_sensor PUBLIC esy_common_defs PRIVATE esy_opt_default)

if(ENABLE_ESY_I2C)
    if(NOT ENABLE_ESY_SYSTEM)
        message(FATAL_ERROR "esy_sensor_i2c requires ENABLE_ESY_SYSTEM")
    endif()
    if(NOT TARGET esy_hal)
        message(FATAL_ERROR "esy_sensor_i2c requires consumer target esy_hal")
    endif()
    add_library(esy_sensor_i2c STATIC
        src/esy_sensor_sht3x.c
        src/esy_sensor_mpu6050.c
        src/esy_sensor_ina219.c
    )
    target_compile_features(esy_sensor_i2c PUBLIC c_std_11)
    target_link_libraries(esy_sensor_i2c PUBLIC esy_sensor esy_hal
        PRIVATE esy_opt_default)
endif()

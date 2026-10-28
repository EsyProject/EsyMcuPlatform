# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-FileCopyrightText: 2026 EsyCloud
#
# SPDX-License-Identifier: Apache-2.0

include("${CMAKE_CURRENT_LIST_DIR}/device_map.cmake")

set(ESY_PLATFORM_COMPILE_OPTIONS
    ${esy_stm32_arch_options}
    $<$<COMPILE_LANGUAGE:ASM>:-x>
    $<$<COMPILE_LANGUAGE:ASM>:assembler-with-cpp>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
set(ESY_PLATFORM_LINK_OPTIONS ${esy_stm32_arch_options})
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_FILE}"
        CACHE FILEPATH "GNU Arm Embedded toolchain" FORCE)
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# This entry also serves as the default CMake toolchain, including try_compile.
# Resolve relative toolchain paths as CMake does: build directory, then source.
# A consumer-selected toolchain is only selected here, never loaded early.
get_filename_component(esy_stm32_entry_path "${CMAKE_CURRENT_LIST_FILE}" REALPATH)
if(IS_ABSOLUTE "${CMAKE_TOOLCHAIN_FILE}")
    get_filename_component(esy_stm32_toolchain_path "${CMAKE_TOOLCHAIN_FILE}" REALPATH)
elseif(EXISTS "${CMAKE_BINARY_DIR}/${CMAKE_TOOLCHAIN_FILE}")
    get_filename_component(esy_stm32_toolchain_path
        "${CMAKE_BINARY_DIR}/${CMAKE_TOOLCHAIN_FILE}" REALPATH)
else()
    get_filename_component(esy_stm32_toolchain_path
        "${CMAKE_SOURCE_DIR}/${CMAKE_TOOLCHAIN_FILE}" REALPATH)
endif()
if(NOT esy_stm32_toolchain_path STREQUAL esy_stm32_entry_path)
    return()
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ESY_TOOLCHAIN_ROOT)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

# Compiler capability probes run before project targets exist. Give them the
# same architecture and ABI as the firmware, preserving caller-supplied flags.
string(JOIN " " esy_stm32_arch_flags ${esy_stm32_arch_options})
foreach(esy_stm32_flag_group IN ITEMS C CXX ASM EXE_LINKER)
    string(FIND "${CMAKE_${esy_stm32_flag_group}_FLAGS_INIT}" "${esy_stm32_arch_flags}"
        esy_stm32_arch_flags_index)
    if(esy_stm32_arch_flags_index EQUAL -1)
        string(APPEND CMAKE_${esy_stm32_flag_group}_FLAGS_INIT " ${esy_stm32_arch_flags}")
    endif()
endforeach()

if(NOT ESY_TOOLCHAIN_ROOT AND DEFINED ENV{ESY_TOOLCHAIN_ROOT})
    set(ESY_TOOLCHAIN_ROOT "$ENV{ESY_TOOLCHAIN_ROOT}")
endif()
set(esy_arm_tool_hints "")
if(ESY_TOOLCHAIN_ROOT)
    list(APPEND esy_arm_tool_hints "${ESY_TOOLCHAIN_ROOT}/bin" "${ESY_TOOLCHAIN_ROOT}")
endif()
if(CMAKE_C_COMPILER AND IS_ABSOLUTE "${CMAKE_C_COMPILER}")
    get_filename_component(esy_arm_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    list(APPEND esy_arm_tool_hints "${esy_arm_compiler_dir}")
endif()

foreach(esy_arm_tool IN ITEMS gcc g++ objcopy size)
    string(MAKE_C_IDENTIFIER "${esy_arm_tool}" esy_arm_tool_id)
    find_program(ESY_ARM_TOOL_${esy_arm_tool_id}
        NAMES "arm-none-eabi-${esy_arm_tool}"
        HINTS ${esy_arm_tool_hints}
        NO_CMAKE_FIND_ROOT_PATH)
    if(NOT ESY_ARM_TOOL_${esy_arm_tool_id})
        message(FATAL_ERROR
            "arm-none-eabi-${esy_arm_tool} was not found. Set ESY_TOOLCHAIN_ROOT or add the GNU Arm toolchain to PATH.")
    endif()
endforeach()

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${ESY_ARM_TOOL_gcc}")
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${ESY_ARM_TOOL_g__}")
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
endif()
if(NOT CMAKE_OBJCOPY)
    set(CMAKE_OBJCOPY "${ESY_ARM_TOOL_objcopy}" CACHE FILEPATH "Object copy tool" FORCE)
endif()
if(NOT CMAKE_SIZE)
    set(CMAKE_SIZE "${ESY_ARM_TOOL_size}" CACHE FILEPATH "Object size tool" FORCE)
endif()
set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

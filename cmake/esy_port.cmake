# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

# Return an entry file without changing the caller's scope. The pre-project
# entry is included directly to preserve the SDK's directory context.
function(esy_get_port_file entry output)
    if(NOT "${ESY_BUILD_PORT}" MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "A resolved ESY_BUILD_PORT is required for platform entries.")
    endif()
    if(NOT "${entry}" MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "Invalid platform entry name: ${entry}")
    endif()
    set(entry_file "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ports/${ESY_BUILD_PORT}/${entry}.cmake")
    if(IS_DIRECTORY "${entry_file}")
        message(FATAL_ERROR "Platform entry must be a file: ${entry_file}")
    endif()
    if(EXISTS "${entry_file}")
        set(${output} "${entry_file}" PARENT_SCOPE)
    else()
        set(${output} "" PARENT_SCOPE)
    endif()
endfunction()

# Loading post_project only defines namespaced hooks. This macro adds no
# scope, so variable outputs and SDK PARENT_SCOPE writes keep their owner.
macro(esy_call_port_hook hook)
    if(NOT "${hook}" MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "Invalid platform build hook name: ${hook}")
    endif()
    esy_get_port_file(post_project esy_port_build_file)
    if(esy_port_build_file)
        include("${esy_port_build_file}")
        if(COMMAND esy_port_${ESY_BUILD_PORT}_${hook})
            cmake_language(CALL esy_port_${ESY_BUILD_PORT}_${hook} ${ARGN})
        endif()
    endif()
endmacro()

# The backend and OS own their vector objects. The platform root aggregates
# these target-local registrations after both owners have been assembled.
function(esy_register_interrupt_target owner interrupt_target)
    if(NOT TARGET "${owner}")
        message(FATAL_ERROR "Interrupt owner target does not exist: ${owner}")
    endif()
    if(NOT TARGET "${interrupt_target}")
        message(FATAL_ERROR "Interrupt object target does not exist: ${interrupt_target}")
    endif()
    get_target_property(interrupt_type "${interrupt_target}" TYPE)
    if(NOT interrupt_type STREQUAL "OBJECT_LIBRARY")
        message(FATAL_ERROR "Interrupt target must be an OBJECT library: ${interrupt_target}")
    endif()
    get_property(interrupt_targets TARGET "${owner}" PROPERTY ESY_INTERRUPT_OBJECT_TARGETS)
    list(APPEND interrupt_targets "${interrupt_target}")
    list(REMOVE_DUPLICATES interrupt_targets)
    set_property(TARGET "${owner}" PROPERTY ESY_INTERRUPT_OBJECT_TARGETS "${interrupt_targets}")
endfunction()

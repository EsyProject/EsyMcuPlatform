# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set(MODULE_NAME esy_common_defs)

add_library(${MODULE_NAME} INTERFACE)
add_library(Esy::common_defs ALIAS ${MODULE_NAME})

set_target_properties(${MODULE_NAME}
    PROPERTIES
        EXPORT_NAME common_defs
)

target_compile_features(${MODULE_NAME} INTERFACE c_std_11)

target_include_directories(${MODULE_NAME}
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/include
)

# Integration supplies a verified backend capability without a hardware dependency.
if(ESY_ATOMIC_IRQ_SPINLOCK_BACKEND)
    target_compile_definitions(${MODULE_NAME} INTERFACE ESY_ATOMIC_IRQ_SPINLOCK_BACKEND=1)
endif()

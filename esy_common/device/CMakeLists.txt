set(MODULE_NAME esy_common_device)

add_library(${MODULE_NAME} STATIC
    src/esy_device.c
)
add_library(Esy::common_device ALIAS ${MODULE_NAME})

set_target_properties(${MODULE_NAME}
    PROPERTIES
        EXPORT_NAME common_device
)

target_compile_features(${MODULE_NAME} PUBLIC c_std_11)

target_include_directories(${MODULE_NAME}
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(${MODULE_NAME}
    PUBLIC
        esy_common_defs
)

if(TARGET esy_opt_default)
    target_link_libraries(${MODULE_NAME}
        PRIVATE
            esy_opt_default
    )
endif()

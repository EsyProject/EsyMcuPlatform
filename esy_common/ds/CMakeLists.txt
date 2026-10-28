set(MODULE_NAME esy_common_ds)

add_library(${MODULE_NAME} STATIC
    src/esy_block_pool.c
    src/esy_block_ring.c
    src/esy_list.c
    src/esy_prio_slot_queue.c
    src/esy_ring_buffer.c
)
add_library(Esy::common_ds ALIAS ${MODULE_NAME})

set_target_properties(${MODULE_NAME}
    PROPERTIES
        EXPORT_NAME common_ds
)

target_compile_features(${MODULE_NAME} PUBLIC c_std_11)

target_include_directories(${MODULE_NAME}
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(${MODULE_NAME}
    PUBLIC
        esy_common_defs
        esy_common_utils
)

if(TARGET esy_opt_default)
    target_link_libraries(${MODULE_NAME}
        PRIVATE
            esy_opt_default
    )
endif()

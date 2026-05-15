set(VDBOARD_MOD_DIR ${CMAKE_CURRENT_LIST_DIR})

add_library(usermod_vdboard INTERFACE)

target_sources(usermod_vdboard INTERFACE
    ${VDBOARD_MOD_DIR}/module.c
    ${VDBOARD_MOD_DIR}/scan.c
    ${VDBOARD_MOD_DIR}/sys.c
)

target_include_directories(usermod_vdboard INTERFACE
    ${VDBOARD_MOD_DIR}
)

target_link_libraries(usermod INTERFACE usermod_vdboard)

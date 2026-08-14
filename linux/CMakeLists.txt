if(NOT AUDIOFX_ENABLE_PIPEWIRE)
    return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
if(NOT PIPEWIRE_LIBRARIES)
    message(FATAL_ERROR "libpipewire-0.3 not found but AUDIOFX_ENABLE_PIPEWIRE is ON")
endif()

add_library(audiofx_pipewire STATIC
    pipewire_backend.cpp
)

target_include_directories(audiofx_pipewire PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_link_libraries(audiofx_pipewire PRIVATE
    audiofx_core
    ${PIPEWIRE_LIBRARIES}
)

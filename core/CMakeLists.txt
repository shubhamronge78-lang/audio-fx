add_library(audiofx_core STATIC
    audio_buffer.cpp
    dsp/biquad_filter.cpp
    dsp/biquad_coefficients.cpp
    dsp/parametric_eq.cpp
)

target_include_directories(audiofx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(audiofx_audio_buffer_test audio_buffer_test.cpp)
target_link_libraries(audiofx_audio_buffer_test PRIVATE audiofx_core)
add_test(NAME AudioBufferTest COMMAND audiofx_audio_buffer_test)

add_executable(audiofx_biquad_filter_test biquad_filter_test.cpp)
target_link_libraries(audiofx_biquad_filter_test PRIVATE audiofx_core)
add_test(NAME BiquadFilterTest COMMAND audiofx_biquad_filter_test)

add_executable(audiofx_biquad_coefficients_test biquad_coefficients_test.cpp)
target_link_libraries(audiofx_biquad_coefficients_test PRIVATE audiofx_core)
add_test(NAME BiquadCoefficientsTest COMMAND audiofx_biquad_coefficients_test)

add_executable(audiofx_parametric_eq_test parametric_eq_test.cpp)
target_link_libraries(audiofx_parametric_eq_test PRIVATE audiofx_core)
add_test(NAME ParametricEQTest COMMAND audiofx_parametric_eq_test)
if(AUDIOFX_ENABLE_PIPEWIRE)
	add_executable(
		pipewire_backend_integration_test
		pipewire_backend_integration_test.cpp
	)

	target_link_libraries(
		pipewire_backend_integration_test
		PRIVATE audiofx_pipewire
	)

	add_test(
		NAME PipeWireBackendIntegrationTest
		COMMAND pipewire_backend_integration_test
	)

	add_executable(
		pipewire_filter_lifecycle_test
		pipewire_filter_lifecycle_test.cpp
	)

	target_link_libraries(
		pipewire_filter_lifecycle_test
		PRIVATE audiofx_pipewire
	)

	add_test(
		NAME PipeWireFilterLifecycleTest
		COMMAND pipewire_filter_lifecycle_test
	)
endif()

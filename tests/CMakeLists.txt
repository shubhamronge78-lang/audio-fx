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

add_executable(processor_passthrough_test processor_passthrough_test.cpp)
target_link_libraries(processor_passthrough_test PRIVATE audiofx_core)
add_test(NAME ProcessorPassThroughTest COMMAND processor_passthrough_test)

add_executable(processor_stereo_test processor_stereo_test.cpp)
target_link_libraries(processor_stereo_test PRIVATE audiofx_core)
add_test(NAME ProcessorStereoTest COMMAND processor_stereo_test)

add_executable(eq_processor_test eq_processor_test.cpp)
target_link_libraries(eq_processor_test PRIVATE audiofx_core)
add_test(NAME EQProcessorTest COMMAND eq_processor_test)

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

	add_executable(
		pipewire_processor_integration_test
		pipewire_processor_integration_test.cpp
	)

	target_link_libraries(
		pipewire_processor_integration_test
		PRIVATE audiofx_pipewire audiofx_core
	)

	add_test(
		NAME PipeWireProcessorIntegrationTest
		COMMAND pipewire_processor_integration_test
	)

          add_executable(
                  pipewire_processor_running_swap_test
                  pipewire_processor_running_swap_test.cpp
          )

          target_link_libraries(
                  pipewire_processor_running_swap_test
                  PRIVATE audiofx_pipewire audiofx_core
          )

          add_test(
                  NAME PipeWireProcessorRunningSwapTest
                  COMMAND pipewire_processor_running_swap_test
          )

endif()

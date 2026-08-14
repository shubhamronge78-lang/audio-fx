cmake_minimum_required(VERSION 3.24)

project(AudioFX
	VERSION 0.1.0
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

option(AUDIOFX_ENABLE_PIPEWIRE
	"Enable Linux/PipeWire backend"
	OFF
)

add_subdirectory(core)
add_subdirectory(tests)

if(AUDIOFX_ENABLE_PIPEWIRE)
	add_subdirectory(linux)
endif()

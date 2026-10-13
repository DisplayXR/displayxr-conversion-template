# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# DisplayXR conversion-provider template: a CPU-only provider plug-in and a
# fake host that drives it. Plain CMake, no dependency beyond a C11 compiler:
#
#   cmake -S . -B build
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure

cmake_minimum_required(VERSION 3.21)
project(displayxr_conversion_template VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# gnu11 on GCC / Clang: POSIX (nanosleep, realpath, dlopen) is visible without feature macros.
set(CMAKE_C_EXTENSIONS ON)

option(EXAMPLE_WERROR "Treat compiler warnings as errors (CI turns this on)" OFF)
option(EXAMPLE_BUILD_FAKEHOST "Build the fake host and register its tests" ON)

# VENDOR TODO: rename the target (and so the binary) to DisplayXR-<Vendor>Conversion.
set(EXAMPLE_PROVIDER DisplayXR-ExampleConversion)

function(example_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /utf-8)
		# The vendored header's tripwires use C11 _Static_assert; the CRT's
		# "unsafe" warnings are noise for this code.
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
		if(EXAMPLE_WERROR)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
		if(EXAMPLE_WERROR)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()
endfunction()

# The provider: a loadable module exporting exactly one symbol,
# xrtConversionPluginNegotiate. Everything else is hidden.
add_library(${EXAMPLE_PROVIDER} MODULE src/example_conversion.c)
target_include_directories(${EXAMPLE_PROVIDER} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(${EXAMPLE_PROVIDER} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
if(APPLE)
	set_target_properties(${EXAMPLE_PROVIDER} PROPERTIES SUFFIX ".dylib")
endif()
if(NOT WIN32)
	find_package(Threads REQUIRED)
	target_link_libraries(${EXAMPLE_PROVIDER} PRIVATE Threads::Threads m)
endif()
if(MSVC)
	# A provider DLL must load in a process whose CRT it does not control: link
	# the CRT statically so the binary carries no MSVC runtime dependency.
	set_property(TARGET ${EXAMPLE_PROVIDER} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
example_warnings(${EXAMPLE_PROVIDER})

install(TARGETS ${EXAMPLE_PROVIDER} LIBRARY DESTINATION lib RUNTIME DESTINATION bin)

if(EXAMPLE_BUILD_FAKEHOST)
	add_executable(example-conversion-fakehost tools/fakehost.c)
	target_include_directories(example-conversion-fakehost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
	if(NOT WIN32)
		target_link_libraries(example-conversion-fakehost PRIVATE ${CMAKE_DL_LIBS} m)
	endif()
	example_warnings(example-conversion-fakehost)
	add_dependencies(example-conversion-fakehost ${EXAMPLE_PROVIDER})

	enable_testing()
	set(_bin $<TARGET_FILE:${EXAMPLE_PROVIDER}>)
	add_test(NAME fakehost.ready COMMAND example-conversion-fakehost ${_bin} --set WARM_MS=50)
	add_test(NAME fakehost.horizontal COMMAND example-conversion-fakehost ${_bin} --set RAMP_DIRECTION=horizontal)
	add_test(NAME fakehost.absent COMMAND example-conversion-fakehost ${_bin} --set DEPENDENCY=absent --expect absent)
	add_test(NAME fakehost.version COMMAND example-conversion-fakehost ${_bin} --set DEPENDENCY=too-old --expect version)
endif()

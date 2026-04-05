cmake_minimum_required(VERSION 3.16)
# Hyphen-free project name (CMake cache vars); binary is still "aud10-suite".
project(aud10_suite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build when the generator has a single configuration.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
find_package(Curses REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

add_executable(aud10_suite
    src/main.cpp
    src/constants.cpp
    src/config.cpp
    src/fx/compressor.cpp
    src/fx/graphic_eq.cpp
    src/fx/reverb.cpp
    src/fx/delay.cpp
    src/fx/deesser.cpp
    src/fx/parametric_eq.cpp
    src/fx/hilo_pass.cpp
    src/fx/pitch_correct.cpp
    src/fx/saturator.cpp
    src/fx/pitch_shift.cpp
)
set_target_properties(aud10_suite PROPERTIES OUTPUT_NAME "aud10-suite")
target_include_directories(aud10_suite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CURSES_INCLUDE_DIRS}
)
target_link_libraries(aud10_suite PRIVATE
    PkgConfig::JACK
    ${CURSES_LIBRARIES}
    pthread
)

# Link-time optimization (LTO / -flto) when the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(ipo_ok)
    set_property(TARGET aud10_suite PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET aud10_suite PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
else()
    message(STATUS "IPO/LTO not available: ${ipo_msg}")
endif()

# Extra Release tuning for GCC and Clang (not MSVC).
# -march=native / -mcpu=native: tune for the CPU building the binary (not portable to older CPUs).
# On x86 this also defines __AVX__ when the host CPU supports it, which enables intrinsics in
# simd_audio.hpp / the FX sources. Set -DAUD10_NATIVE_CPU_TUNE=OFF for a portable baseline (no AVX SIMD).
option(AUD10_NATIVE_CPU_TUNE "Release: -march=native (x86) / -mcpu=native (ARM); enables __AVX__ SIMD on capable CPUs" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(_native_tune "")
    if(AUD10_NATIVE_CPU_TUNE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            set(_native_tune "-march=native;-mtune=native")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64)$")
            if(APPLE)
                set(_native_tune "-mcpu=native")
            else()
                set(_native_tune "-march=native;-mtune=native")
            endif()
        endif()
    endif()
    if(_native_tune)
        target_compile_options(aud10_suite PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:${_native_tune}>
        )
        target_link_options(aud10_suite PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:${_native_tune}>
        )
    endif()

    # Aggressive FP optimizations (audio code may see slightly different rounding / NaN behavior).
    target_compile_options(aud10_suite PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-Ofast;-ffast-math>
    )
    # Match at link time when using LTO.
    target_link_options(aud10_suite PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-ffast-math>
    )
endif()

cmake_minimum_required(VERSION 3.25)

project(koi_engine VERSION 1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Koi Engine v1 targets Windows x64")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Koi Engine v1 requires a 64-bit Windows toolchain")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
enable_testing()

add_executable(koi_core_tests
    tests/koi_core_tests.cpp
)

target_include_directories(koi_core_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(NAME koi_core_tests COMMAND koi_core_tests)

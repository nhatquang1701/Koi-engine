cmake_minimum_required(VERSION 3.31)

project(koi_engine VERSION 1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Koi Engine v1 targets Windows x64")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Koi Engine v1 requires a 64-bit Windows toolchain")
endif()

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "Koi Engine v1 requires an x86-64 compiler target")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT DEFINED CMAKE_CXX26_STANDARD_COMPILE_OPTION)
    set(CMAKE_CXX26_STANDARD_COMPILE_OPTION "/std:c++latest")
endif()

include(CTest)
enable_testing()

add_library(koi_core
    src/koi/move.cpp
    src/koi/move_chooser.cpp
    src/koi/position.cpp
    src/koi/uci_controller.cpp
)

target_include_directories(koi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chess-library
)

add_executable(koi_core_tests
    tests/koi_core_tests.cpp
)

target_link_libraries(koi_core_tests PRIVATE koi_core)

add_test(NAME koi_core_tests COMMAND koi_core_tests)

add_executable(uci_controller_tests
    tests/uci_controller_tests.cpp
)

target_link_libraries(uci_controller_tests PRIVATE koi_core)

add_test(NAME uci_controller_tests COMMAND uci_controller_tests)

add_executable(koi_engine
    src/main.cpp
)

target_link_libraries(koi_engine PRIVATE koi_core)
set_target_properties(koi_engine PROPERTIES OUTPUT_NAME "koi-engine")

find_program(POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)

add_test(
    NAME koi_engine_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/uci_process_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

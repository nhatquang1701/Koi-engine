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

if(MSVC)
    add_compile_options(/permissive-)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT KOI_IPO_SUPPORTED OUTPUT KOI_IPO_ERROR LANGUAGES CXX)
    if(KOI_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "Koi Engine Release IPO/LTO is unavailable: ${KOI_IPO_ERROR}")
    endif()
endif()

include(CTest)
enable_testing()

add_library(koi_core
    src/koi/classical_evaluator.cpp
    src/koi/game_state.cpp
    src/koi/move.cpp
    src/koi/move_chooser.cpp
    src/koi/opening_book.cpp
    src/koi/perft.cpp
    src/koi/position.cpp
    src/koi/search_service.cpp
    src/koi/search_ordering.cpp
    src/koi/static_exchange.cpp
    src/koi/syzygy_tablebase.cpp
    src/koi/strength_suite.cpp
    src/koi/time_manager.cpp
    src/koi/transposition_table.cpp
    src/koi/uci_controller.cpp
)

target_include_directories(koi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(koi_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chess-library
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fathom
)

set_source_files_properties(third_party/fathom/tbprobe.c PROPERTIES LANGUAGE CXX)
target_sources(koi_core PRIVATE third_party/fathom/tbprobe.c)

target_compile_definitions(koi_core PUBLIC
    KOI_ENGINE_BUILD_VERSION="${PROJECT_VERSION}"
)

add_executable(koi_core_tests
    tests/koi_core_tests.cpp
)

target_link_libraries(koi_core_tests PRIVATE koi_core)

add_test(NAME koi_core_tests COMMAND koi_core_tests)

add_executable(syzygy_tablebase_tests
    tests/syzygy_tablebase_tests.cpp
)

target_link_libraries(syzygy_tablebase_tests PRIVATE koi_core)

add_test(NAME syzygy_tablebase_tests COMMAND syzygy_tablebase_tests)

add_executable(koi_rules_tests
    tests/koi_rules_tests.cpp
)

target_link_libraries(koi_rules_tests PRIVATE koi_core)

add_test(NAME koi_rules_tests COMMAND koi_rules_tests)

add_executable(opening_book_tests
    tests/opening_book_tests.cpp
)

target_link_libraries(opening_book_tests PRIVATE koi_core)

add_test(NAME opening_book_tests COMMAND opening_book_tests)

add_executable(uci_controller_tests
    tests/uci_controller_tests.cpp
)

target_link_libraries(uci_controller_tests PRIVATE koi_core)

add_test(NAME uci_controller_tests COMMAND uci_controller_tests)

add_executable(koi_search_tests
    tests/koi_search_tests.cpp
)

target_link_libraries(koi_search_tests PRIVATE koi_core)

add_test(NAME koi_search_tests COMMAND koi_search_tests)

add_executable(search_ordering_tests
    tests/search_ordering_tests.cpp
)

target_link_libraries(search_ordering_tests PRIVATE koi_core)

add_test(NAME search_ordering_tests COMMAND search_ordering_tests)

add_executable(static_exchange_tests
    tests/static_exchange_tests.cpp
)

target_link_libraries(static_exchange_tests PRIVATE koi_core)

add_test(NAME static_exchange_tests COMMAND static_exchange_tests)

add_executable(koi_strength_tests
    tests/koi_strength_tests.cpp
)

target_link_libraries(koi_strength_tests PRIVATE koi_core)

add_test(NAME koi_strength_tests COMMAND koi_strength_tests)

add_executable(perft_tests
    tests/perft_tests.cpp
)

target_link_libraries(perft_tests PRIVATE koi_core)

add_test(NAME perft_tests COMMAND perft_tests)

add_executable(koi_replay_tests
    tests/koi_replay_tests.cpp
)

add_dependencies(koi_replay_tests koi_replay)

add_test(
    NAME koi_replay_tests
    COMMAND koi_replay_tests "$<TARGET_FILE:koi_replay>"
)

add_executable(uci_match_fixture
    tests/uci_match_fixture.cpp
)

add_executable(koi_perft
    tools/perft.cpp
)

target_link_libraries(koi_perft PRIVATE koi_core)

add_executable(koi_bench
    tools/koi_bench.cpp
)

target_link_libraries(koi_bench PRIVATE koi_core)
set_target_properties(koi_bench PROPERTIES OUTPUT_NAME "koi-bench")

add_executable(koi_replay
    tools/koi_replay.cpp
)

target_link_libraries(koi_replay PRIVATE koi_core)
set_target_properties(koi_replay PROPERTIES OUTPUT_NAME "koi-replay")

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

set_tests_properties(koi_engine_process PROPERTIES TIMEOUT 30)

add_test(
    NAME koi_engine_en_croissant_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/en_croissant_uci_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(koi_engine_en_croissant_process PROPERTIES TIMEOUT 30)

add_test(
    NAME koi_benchmark_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/koi_bench_process_test.ps1"
            -BenchPath "$<TARGET_FILE_DIR:koi_engine>/koi-bench.exe"
)

set_tests_properties(koi_benchmark_process PROPERTIES TIMEOUT 300)

add_test(
    NAME koi_uci_match_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/uci_match_process_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(koi_uci_match_process PROPERTIES TIMEOUT 120)

add_test(
    NAME koi_uci_match_clock
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/uci_match_clock_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(koi_uci_match_clock PROPERTIES TIMEOUT 120)

add_test(
    NAME windows_ci_configuration
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ci_configuration_test.ps1"
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import chess"
        RESULT_VARIABLE ELO_ORACLE_PYTHON_CHESS_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(ELO_ORACLE_PYTHON_CHESS_RESULT EQUAL 0)
        add_test(
            NAME elo_oracle_python
            COMMAND "${Python3_EXECUTABLE}" -m unittest tests/elo_oracle_test.py -v
        )
        set_tests_properties(
            elo_oracle_python
            PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        )
    else()
        message(STATUS
            "Skipping elo_oracle_python: python-chess is unavailable. "
            "Install tools/requirements-elo-oracle.txt to register the test."
        )
    endif()
    add_test(
        NAME elo_estimate_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/elo_estimate_test.py -v
    )
    add_test(
        NAME stockfish_match_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/stockfish_match_test.py -v
    )
    add_test(
        NAME elo_openings_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/elo_openings_test.py -v
    )
    set_tests_properties(
        elo_estimate_python stockfish_match_python
        PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
    set_tests_properties(
        elo_openings_python
        PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;KOI_REPLAY_PATH=$<TARGET_FILE:koi_replay>"
    )
    add_test(
        NAME tune_eval_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/tune_eval_test.py -v
    )
    set_tests_properties(
        tune_eval_python
        PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
else()
    message(STATUS
        "Skipping elo_oracle_python: Python 3 interpreter is unavailable."
    )
endif()

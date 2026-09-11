cmake_minimum_required(VERSION 3.31)

project(koi_engine VERSION 1.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Koi Engine v1.1 targets Windows x64")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Koi Engine v1.1 requires a 64-bit Windows toolchain")
endif()

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "Koi Engine v1.1 requires an x86-64 compiler target")
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "Koi Engine v1.1 requires the MSVC x64 toolchain for C++26 named modules; "
        "unsupported compiler ID: ${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT DEFINED CMAKE_CXX26_STANDARD_COMPILE_OPTION)
    set(CMAKE_CXX26_STANDARD_COMPILE_OPTION "/std:c++latest")
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(
        /EHsc
        /permissive-
        "$<$<CONFIG:Release>:/O2>"
        "$<$<CONFIG:Release>:/DNDEBUG>"
        "$<$<CONFIG:Release>:/arch:AVX2>"
    )
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
    src/koi/completion_gate.cpp
    src/koi/cpu_features.cpp
    src/koi/evaluation_features.cpp
    src/koi/game_state.cpp
    src/koi/move.cpp
    src/koi/move_chooser.cpp
    src/koi/nnue.cpp
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

set(KOI_MODULE_SOURCES
    src/koi/modules/types.ixx
    src/koi/modules/position.ixx
    src/koi/modules/eval.ixx
    src/koi/modules/tablebase.ixx
    src/koi/modules/search.ixx
    src/koi/modules/runtime.ixx
    src/koi/modules/koi.ixx
)

# CMake 3.31+ and MSVC are required above, so named modules are a mandatory
# part of the core target rather than an optional experimental build.
target_sources(koi_core PUBLIC
    FILE_SET CXX_MODULES TYPE CXX_MODULES FILES ${KOI_MODULE_SOURCES}
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

option(KOI_BUILD_SHADOW_DIFF
    "Build the optional chess-library differential oracle tests" OFF)

if(KOI_BUILD_SHADOW_DIFF)
    add_executable(koi_shadow_diff_tests
        tests/integration/rules/native_shadow_diff_tests.cpp
    )
    target_link_libraries(koi_shadow_diff_tests PRIVATE koi_core)
    target_include_directories(koi_shadow_diff_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chess-library
    )
    add_test(NAME koi_shadow_diff_tests COMMAND koi_shadow_diff_tests)
endif()

add_executable(koi_core_tests
    tests/unit/core/koi_core_tests.cpp
)

target_link_libraries(koi_core_tests PRIVATE koi_core)

add_test(NAME koi_core_tests COMMAND koi_core_tests)

add_executable(evaluation_boundary_tests
    tests/unit/evaluation/evaluation_boundary_tests.cpp
)

target_link_libraries(evaluation_boundary_tests PRIVATE koi_core)

add_test(NAME evaluation_boundary_tests COMMAND evaluation_boundary_tests)

add_executable(nnue_boundary_tests
    tests/unit/evaluation/nnue_boundary_tests.cpp
)

target_link_libraries(nnue_boundary_tests PRIVATE koi_core)

add_test(NAME nnue_boundary_tests COMMAND nnue_boundary_tests)

add_executable(koi_cpu_features_tests
    tests/unit/runtime/cpu_features_tests.cpp
)

target_link_libraries(koi_cpu_features_tests PRIVATE koi_core)

add_test(NAME koi_cpu_features_tests COMMAND koi_cpu_features_tests)

add_executable(koi_module_tests
    tests/unit/core/koi_module_tests.cpp
)

target_link_libraries(koi_module_tests PRIVATE koi_core)

add_test(NAME koi_module_tests COMMAND koi_module_tests)

add_executable(syzygy_tablebase_tests
    tests/unit/runtime/syzygy_tablebase_tests.cpp
)

target_link_libraries(syzygy_tablebase_tests PRIVATE koi_core)

add_test(NAME syzygy_tablebase_tests COMMAND syzygy_tablebase_tests)

add_executable(koi_rules_tests
    tests/unit/rules/koi_rules_tests.cpp
)

target_link_libraries(koi_rules_tests PRIVATE koi_core)

add_test(NAME koi_rules_tests COMMAND koi_rules_tests)

add_executable(native_rule_state_tests
    tests/unit/rules/native_rule_state_tests.cpp
)

target_link_libraries(native_rule_state_tests PRIVATE koi_core)
target_include_directories(native_rule_state_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/chess-library
)

add_test(NAME native_rule_state_tests COMMAND native_rule_state_tests)

add_executable(opening_book_tests
    tests/unit/runtime/opening_book_tests.cpp
)

target_link_libraries(opening_book_tests PRIVATE koi_core)

add_test(NAME opening_book_tests COMMAND opening_book_tests)

add_executable(uci_controller_tests
    tests/unit/runtime/uci_controller_tests.cpp
)

target_link_libraries(uci_controller_tests PRIVATE koi_core)

add_test(NAME uci_controller_tests COMMAND uci_controller_tests)

add_executable(completion_gate_tests
    tests/unit/runtime/completion_gate_tests.cpp
)

target_link_libraries(completion_gate_tests PRIVATE koi_core)

add_test(NAME completion_gate_tests COMMAND completion_gate_tests)

add_executable(koi_search_tests
    tests/unit/search/koi_search_tests.cpp
)

target_link_libraries(koi_search_tests PRIVATE koi_core)

add_test(NAME koi_search_tests COMMAND koi_search_tests)

add_executable(search_ordering_tests
    tests/unit/search/search_ordering_tests.cpp
)

target_link_libraries(search_ordering_tests PRIVATE koi_core)

add_test(NAME search_ordering_tests COMMAND search_ordering_tests)

add_executable(static_exchange_tests
    tests/unit/search/static_exchange_tests.cpp
)

target_link_libraries(static_exchange_tests PRIVATE koi_core)

add_test(NAME static_exchange_tests COMMAND static_exchange_tests)

add_executable(koi_strength_tests
    tests/unit/search/koi_strength_tests.cpp
)

target_link_libraries(koi_strength_tests PRIVATE koi_core)

add_test(NAME koi_strength_tests COMMAND koi_strength_tests)

add_executable(perft_tests
    tests/unit/rules/perft_tests.cpp
)

target_link_libraries(perft_tests PRIVATE koi_core)

add_test(NAME perft_tests COMMAND perft_tests)

add_executable(koi_replay_tests
    tests/integration/tools/koi_replay_tests.cpp
)

add_dependencies(koi_replay_tests koi_replay)

add_test(
    NAME koi_replay_tests
    COMMAND koi_replay_tests "$<TARGET_FILE:koi_replay>"
)

add_executable(uci_match_fixture
    tests/integration/tools/uci_match_fixture.cpp
)

add_executable(koi_perft
    tools/engine/perft.cpp
)

target_link_libraries(koi_perft PRIVATE koi_core)
set_target_properties(koi_perft PROPERTIES OUTPUT_NAME "koi-perft")

add_executable(koi_bench
    tools/engine/koi_bench.cpp
)

target_link_libraries(koi_bench PRIVATE koi_core)
set_target_properties(koi_bench PROPERTIES OUTPUT_NAME "koi-bench")

add_executable(koi_replay
    tools/engine/koi_replay.cpp
)

target_link_libraries(koi_replay PRIVATE koi_core)
set_target_properties(koi_replay PROPERTIES OUTPUT_NAME "koi-replay")

add_executable(koi_engine
    src/main.cpp
)

target_link_libraries(koi_engine PRIVATE koi_core)
set_target_properties(koi_engine PROPERTIES OUTPUT_NAME "koi-engine")

find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

add_test(
    NAME koi_engine_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/uci/uci_process_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(koi_engine_process PROPERTIES TIMEOUT 60)

add_test(
    NAME koi_engine_en_croissant_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/uci/en_croissant_uci_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(koi_engine_en_croissant_process PROPERTIES TIMEOUT 30)

add_test(
    NAME koi_benchmark_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tools/koi_bench_process_test.ps1"
            -BenchPath "$<TARGET_FILE_DIR:koi_engine>/koi-bench.exe"
)

set_tests_properties(koi_benchmark_process PROPERTIES TIMEOUT 300)

add_test(
    NAME koi_uci_match_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/matches/uci_match_process_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(koi_uci_match_process PROPERTIES TIMEOUT 120)

add_test(
    NAME koi_uci_match_clock
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/matches/uci_match_clock_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(koi_uci_match_clock PROPERTIES TIMEOUT 120)

add_test(
    NAME cutechess_stability_smoke
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/matches/cutechess_stability_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(cutechess_stability_smoke PROPERTIES TIMEOUT 180)

add_test(
    NAME hash_memory_stability
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/runtime/hash_memory_stability_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

set_tests_properties(hash_memory_stability PROPERTIES TIMEOUT 60)

add_test(
    NAME windows_ci_configuration
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/packaging/ci_configuration_test.ps1"
)

add_test(
    NAME install_book_script
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/packaging/install_book_test.ps1"
)

set_tests_properties(install_book_script PROPERTIES TIMEOUT 30)

add_test(
    NAME package_release_layout
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/packaging/package_release_test.ps1"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -PackageScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/build/package_release.ps1"
)

set_tests_properties(package_release_layout PROPERTIES TIMEOUT 60)

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
            COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/measurement/elo_oracle_test.py -v
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
            "Install tools/measurement/requirements-elo-oracle.txt to register the test."
        )
    endif()
    add_test(
        NAME elo_estimate_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/measurement/elo_estimate_test.py -v
    )
    add_test(
        NAME stockfish_match_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/measurement/stockfish_match_test.py -v
    )
    add_test(
        NAME elo_openings_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/measurement/elo_openings_test.py -v
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
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/evaluation/tune_eval_test.py -v
    )
    add_test(
        NAME nnue_training_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/evaluation/nnue_training_test.py -v
    )
    add_test(
        NAME nnue_wrapper_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/evaluation/nnue_wrapper_test.py -v
    )
    add_test(
        NAME measurement_phase_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/measurement/measurement_phase_test.py -v
    )
    add_test(
        NAME measurement_forensics_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/measurement/measurement_forensics_test.py -v
    )
    add_test(
        NAME strength_report_python
        COMMAND "${Python3_EXECUTABLE}" -m unittest tests/python/evaluation/strength_report_test.py -v
    )
    set_tests_properties(
        tune_eval_python
        nnue_training_python nnue_wrapper_python measurement_phase_python measurement_forensics_python
        strength_report_python
        PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
else()
    message(STATUS
        "Skipping elo_oracle_python: Python 3 interpreter is unavailable."
    )
endif()

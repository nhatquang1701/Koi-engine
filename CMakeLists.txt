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
    # The recursive search can legitimately descend through check, singular,
    # and quiet-forcing extensions. A 1 MiB default stack is not enough for a
    # deep tactical line, so reserve a larger stack for the image (worker
    # threads inherit this reserve on Windows).
    add_link_options("/STACK:16777216")
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

# Shared helpers for the C++ test binaries: assertion helpers and the absolute
# location of the repository test data so no test has to walk `__FILE__`.
add_library(koi_test_support INTERFACE)
target_include_directories(koi_test_support INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
)
target_compile_definitions(koi_test_support INTERFACE
    KOI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)

add_library(koi_core
    src/koi/classical_evaluator.cpp
    src/koi/detail/compatibility_mirror.cpp
    src/koi/detail/attack_tables.cpp
    src/koi/detail/evaluation_context.cpp
    src/koi/detail/feature_state.cpp
    src/koi/detail/root_coordinator.cpp
    src/koi/detail/search_context.cpp
    src/koi/detail/search_context_support.cpp
    src/koi/detail/search_ordering_tables.cpp
    src/koi/detail/search_runner.cpp
    src/koi/detail/search_session.cpp
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

# Release and Debug binaries do not deserve the same budgets: the Debug build is
# several times slower and the process tests launch real searches. The scale
# defaults to 3 for Debug trees (and 1 otherwise) so that only genuine hangs
# trip the limits instead of expected slowness; set -DKOI_TIMEOUT_SCALE=N to
# override it explicitly.
set(KOI_TEST_TIMEOUT_SECONDS "600" CACHE STRING
    "Default timeout in seconds for tests without an explicit budget")
set(KOI_TIMEOUT_SCALE "auto" CACHE STRING
    "Multiplier applied to explicit process-test and default timeouts (auto: 3 for Debug, else 1)")
if(KOI_TIMEOUT_SCALE STREQUAL "auto")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(KOI_TIMEOUT_SCALE "3")
    else()
        set(KOI_TIMEOUT_SCALE "1")
    endif()
endif()

function(koi_set_timeout test_name release_seconds)
    math(EXPR KOI_SCALED_TIMEOUT "${release_seconds} * ${KOI_TIMEOUT_SCALE}")
    set_tests_properties(${test_name} PROPERTIES TIMEOUT ${KOI_SCALED_TIMEOUT})
endfunction()

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

add_executable(evaluation_architecture_tests
    tests/unit/evaluation/evaluation_architecture_tests.cpp
)

target_link_libraries(evaluation_architecture_tests PRIVATE koi_core)

add_test(NAME evaluation_architecture_tests COMMAND evaluation_architecture_tests)

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

# The search suite is by far the largest target. Run it as independent shards so
# a multi-core host finishes it in parallel, and so each shard stays well inside
# the per-test timeout even in the slower Debug configuration.
foreach(KOI_SEARCH_SHARD_INDEX RANGE 0 3)
    math(EXPR KOI_SEARCH_SHARD_LABEL "${KOI_SEARCH_SHARD_INDEX} + 1")
    add_test(
        NAME koi_search_tests_${KOI_SEARCH_SHARD_LABEL}of4
        COMMAND koi_search_tests --shard=${KOI_SEARCH_SHARD_INDEX}/4
    )
endforeach()

add_executable(search_ordering_tests
    tests/unit/search/search_ordering_tests.cpp
)

target_link_libraries(search_ordering_tests PRIVATE koi_core)

add_test(NAME search_ordering_tests COMMAND search_ordering_tests)

add_executable(search_architecture_tests
    tests/unit/search/search_architecture_tests.cpp
)

target_link_libraries(search_architecture_tests PRIVATE koi_core)

add_test(NAME search_architecture_tests COMMAND search_architecture_tests)

add_executable(search_policy_tests
    tests/unit/search/search_policy_tests.cpp
)

target_link_libraries(search_policy_tests PRIVATE koi_core)

add_test(NAME search_policy_tests COMMAND search_policy_tests)

add_executable(search_runtime_tests
    tests/unit/search/search_runtime_tests.cpp
)

target_link_libraries(search_runtime_tests PRIVATE koi_core)

add_test(NAME search_runtime_tests COMMAND search_runtime_tests)

add_executable(static_exchange_tests
    tests/unit/search/static_exchange_tests.cpp
)

target_link_libraries(static_exchange_tests PRIVATE koi_core)

add_test(NAME static_exchange_tests COMMAND static_exchange_tests)

add_executable(time_manager_tests
    tests/unit/search/time_manager_tests.cpp
)

target_link_libraries(time_manager_tests PRIVATE koi_core)

add_test(NAME time_manager_tests COMMAND time_manager_tests)

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

# Every C++ test binary shares the assertion/fixture helpers. The optional
# shadow-diff oracle joins the list only when it is part of the build.
set(KOI_TEST_SUPPORT_TARGETS
    koi_core_tests
    evaluation_boundary_tests
    evaluation_architecture_tests
    nnue_boundary_tests
    koi_cpu_features_tests
    koi_module_tests
    syzygy_tablebase_tests
    koi_rules_tests
    native_rule_state_tests
    opening_book_tests
    uci_controller_tests
    completion_gate_tests
    koi_search_tests
    search_ordering_tests
    search_architecture_tests
    search_policy_tests
    search_runtime_tests
    static_exchange_tests
    time_manager_tests
    koi_strength_tests
    perft_tests
    koi_replay_tests
)

if(TARGET koi_shadow_diff_tests)
    list(APPEND KOI_TEST_SUPPORT_TARGETS koi_shadow_diff_tests)
endif()

foreach(KOI_TEST_TARGET IN LISTS KOI_TEST_SUPPORT_TARGETS)
    target_link_libraries(${KOI_TEST_TARGET} PRIVATE koi_test_support)
endforeach()

find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

add_test(
    NAME koi_engine_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/uci/uci_process_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(koi_engine_process 60)

add_test(
    NAME koi_engine_en_croissant_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/uci/en_croissant_uci_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(koi_engine_en_croissant_process 30)

add_test(
    NAME koi_engine_time_safety_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/uci/time_safety_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(koi_engine_time_safety_process 180)

add_test(
    NAME koi_benchmark_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tools/koi_bench_process_test.ps1"
            -BenchPath "$<TARGET_FILE:koi_bench>"
)

koi_set_timeout(koi_benchmark_process 300)

add_test(
    NAME koi_uci_match_process
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/matches/uci_match_process_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(koi_uci_match_process 120)

add_test(
    NAME koi_stockfish_strength_option
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/matches/stockfish_strength_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(koi_stockfish_strength_option 60)

add_test(
    NAME koi_uci_match_clock
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/matches/uci_match_clock_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(koi_uci_match_clock 120)

add_test(
    NAME cutechess_stability_smoke
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/matches/cutechess_stability_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(cutechess_stability_smoke 180)

add_test(
    NAME hash_memory_stability
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/runtime/hash_memory_stability_test.ps1"
            -EnginePath "$<TARGET_FILE:koi_engine>"
)

koi_set_timeout(hash_memory_stability 60)

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

koi_set_timeout(install_book_script 30)

add_test(
    NAME package_release_layout
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/packaging/package_release_test.ps1"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -PackageScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/build/package_release.ps1"
)

koi_set_timeout(package_release_layout 60)

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
        "Skipping all Python tests: the Python 3 interpreter is unavailable."
    )
endif()

# --- Labels and scheduling hints ---------------------------------------------
# Labels let a developer (and CI) select focused subsets with `ctest -L`; the
# PROCESSORS hints keep the heavy search/process tests from oversubscribing a
# `ctest -j` run, and RUN_SERIAL protects the one test that writes into the
# build tree next to the engine binary.
set_tests_properties(
    koi_core_tests evaluation_boundary_tests evaluation_architecture_tests
    nnue_boundary_tests koi_cpu_features_tests koi_module_tests syzygy_tablebase_tests
    koi_rules_tests native_rule_state_tests opening_book_tests completion_gate_tests
    search_ordering_tests search_architecture_tests search_policy_tests
    search_runtime_tests static_exchange_tests time_manager_tests perft_tests
    PROPERTIES LABELS "unit"
)
set_tests_properties(uci_controller_tests PROPERTIES LABELS "unit;runtime")
set_tests_properties(
    koi_search_tests_1of4 koi_search_tests_2of4 koi_search_tests_3of4 koi_search_tests_4of4
    PROPERTIES LABELS "unit;search;heavy"
)
set_tests_properties(
    koi_search_tests_1of4 koi_search_tests_2of4 koi_search_tests_3of4 koi_search_tests_4of4
    koi_strength_tests
    PROPERTIES PROCESSORS 2
)
set_tests_properties(koi_strength_tests PROPERTIES LABELS "unit;search;heavy")
set_tests_properties(koi_replay_tests PROPERTIES LABELS "integration;tools")
set_tests_properties(koi_engine_process koi_engine_en_croissant_process
    PROPERTIES LABELS "process;uci")
set_tests_properties(koi_engine_process PROPERTIES RUN_SERIAL TRUE)
set_tests_properties(koi_engine_time_safety_process
    PROPERTIES LABELS "process;uci;heavy" PROCESSORS 2)
set_tests_properties(koi_benchmark_process
    PROPERTIES LABELS "process;tools;heavy" PROCESSORS 2)
set_tests_properties(koi_uci_match_process koi_uci_match_clock
    PROPERTIES LABELS "process;matches")
set_tests_properties(koi_uci_match_clock PROPERTIES PROCESSORS 2)
set_tests_properties(koi_stockfish_strength_option PROPERTIES LABELS "process;matches")
set_tests_properties(cutechess_stability_smoke
    PROPERTIES LABELS "process;matches;heavy" PROCESSORS 2)
set_tests_properties(hash_memory_stability PROPERTIES LABELS "process;runtime")
set_tests_properties(windows_ci_configuration install_book_script package_release_layout
    PROPERTIES LABELS "process;packaging")

if(TARGET koi_shadow_diff_tests)
    set_tests_properties(koi_shadow_diff_tests PROPERTIES LABELS "integration;rules")
endif()

if(Python3_Interpreter_FOUND)
    set_tests_properties(
        elo_estimate_python stockfish_match_python elo_openings_python
        tune_eval_python nnue_training_python nnue_wrapper_python
        measurement_phase_python measurement_forensics_python strength_report_python
        PROPERTIES LABELS "python"
    )
    if(TEST elo_oracle_python)
        set_tests_properties(elo_oracle_python PROPERTIES LABELS "python;measurement")
    endif()
endif()

# Bound every registered test. A pathological search, an unresponsive child
# process, or a stalled network call must fail the test instead of hanging the
# whole suite. Tests that need a different budget set an explicit TIMEOUT above;
# this loop only fills in the ones that would otherwise run unbounded.
get_property(KOI_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
foreach(KOI_TEST_NAME IN LISTS KOI_REGISTERED_TESTS)
    get_test_property(${KOI_TEST_NAME} TIMEOUT KOI_TEST_TIMEOUT)
    if(KOI_TEST_TIMEOUT STREQUAL "NOTFOUND")
        math(EXPR KOI_DEFAULT_TIMEOUT "${KOI_TEST_TIMEOUT_SECONDS} * ${KOI_TIMEOUT_SCALE}")
        set_tests_properties(${KOI_TEST_NAME} PROPERTIES TIMEOUT ${KOI_DEFAULT_TIMEOUT})
    endif()
endforeach()

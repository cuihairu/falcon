cmake_minimum_required(VERSION 3.15)

project(falcon-cli-tests)

# ============================================================================
# Find GoogleTest
# ============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping falcon-cli tests")
    return()
endif()

# ============================================================================
# Unit Tests
# ============================================================================

add_executable(falcon_cli_tests
    unit/arg_parser_test.cpp
    unit/config_loader_test.cpp
    ../src/arg_parser.cpp
    ../src/config_loader.cpp
)

target_link_libraries(falcon_cli_tests
    PRIVATE
        Falcon::core
        Falcon::protocols
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_cli_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libfalcon-core/include
)

target_compile_features(falcon_cli_tests PRIVATE cxx_std_17)

# Link nlohmann_json if available
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(falcon_cli_tests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(falcon_cli_tests PRIVATE FALCON_USE_JSON)
endif()

# Register with CTest
include(GoogleTest)
gtest_discover_tests(falcon_cli_tests)

# ============================================================================
# Additional unit tests (coverage). Appended registrations only.
# ============================================================================

target_sources(falcon_cli_tests PRIVATE
    unit/arg_parser_coverage_test.cpp
    unit/config_loader_coverage_test.cpp
    unit/terminal_test.cpp
)

# ============================================================================
# Integration tests (POSIX only): execute the real falcon-cli binary.
#
# FALCON_CLI_BIN is injected as a compile definition using a generator
# expression, which is supported by target_compile_definitions(). The test
# spawns the binary via fork()+execve so the --coverage counters of main.cpp
# are recorded too (children exit on their own; never SIGKILLed).
# ============================================================================

if(NOT WIN32 AND NOT CMAKE_CROSSCOMPILING)
    add_executable(falcon_cli_integration_tests
        integration/cli_main_integration_test.cpp
    )

    target_compile_definitions(falcon_cli_integration_tests PRIVATE
        FALCON_CLI_BIN="$<TARGET_FILE:falcon-cli>"
    )

    target_include_directories(falcon_cli_integration_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_link_libraries(falcon_cli_integration_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main
    )

    target_compile_features(falcon_cli_integration_tests PRIVATE cxx_std_17)

    # Make sure the CLI binary is built before the integration tests run.
    add_dependencies(falcon_cli_integration_tests falcon-cli)

    gtest_discover_tests(falcon_cli_integration_tests
        PROPERTIES TIMEOUT 120
    )
endif()

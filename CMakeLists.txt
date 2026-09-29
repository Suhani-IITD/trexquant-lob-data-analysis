cmake_minimum_required(VERSION 3.24)
# Local matching simulation; testing is a separate CTest workflow.
project(deterministic_exchange VERSION 0.5.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake --preset debug")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(EXCHANGE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(EXCHANGE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(EXCHANGE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(EXCHANGE_ENABLE_TSAN "Enable ThreadSanitizer (separate build)" OFF)
option(EXCHANGE_ENABLE_CLANG_TIDY "Run clang-tidy when compiling project targets" OFF)
option(EXCHANGE_REUSE_FILL_BUFFER "Reuse engine fill-planning scratch capacity" ON)
option(EXCHANGE_ENABLE_CALLGRIND "Instrument benchmark measured regions for Linux Callgrind" OFF)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/StaticAnalysis.cmake)
include(CTest)

configure_file(cmake/build_config.hpp.in generated/exchange/build_config.hpp @ONLY)

add_library(exchange_core STATIC
    src/analysis/csv.cpp
    src/analysis/market_analysis.cpp
    src/core/build_info.cpp
    src/core/checked_arithmetic.cpp
    src/core/config.cpp
    src/book/reference_book.cpp
    src/engine/reference_engine.cpp)
add_library(exchange::core ALIAS exchange_core)
target_compile_features(exchange_core PUBLIC cxx_std_20)
target_include_directories(exchange_core
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_compile_definitions(exchange_core PRIVATE EXCHANGE_BUILD_TYPE="$<CONFIG>")
if(EXCHANGE_REUSE_FILL_BUFFER)
    # Public because the option changes the engine's class layout.
    target_compile_definitions(exchange_core PUBLIC EXCHANGE_REUSE_FILL_BUFFER=1)
endif()
exchange_set_warnings(exchange_core)
exchange_enable_sanitizers(exchange_core)
exchange_enable_static_analysis(exchange_core)

add_executable(exchange_info apps/exchange_info/main.cpp)
target_link_libraries(exchange_info PRIVATE exchange::core)
exchange_set_warnings(exchange_info)
exchange_enable_sanitizers(exchange_info)
exchange_enable_static_analysis(exchange_info)

add_executable(scenario_runner apps/scenario_runner/main.cpp)
target_link_libraries(scenario_runner PRIVATE exchange::core)
exchange_set_warnings(scenario_runner)
exchange_enable_sanitizers(scenario_runner)
exchange_enable_static_analysis(scenario_runner)

add_executable(benchmark_driver apps/benchmark_driver/main.cpp)
target_link_libraries(benchmark_driver PRIVATE exchange::core)
exchange_set_warnings(benchmark_driver)
exchange_enable_sanitizers(benchmark_driver)
exchange_enable_static_analysis(benchmark_driver)
if(EXCHANGE_ENABLE_CALLGRIND)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Callgrind profiling requires Linux/WSL")
    endif()
    find_path(CALLGRIND_INCLUDE_DIR valgrind/callgrind.h REQUIRED)
    target_include_directories(benchmark_driver SYSTEM PRIVATE "${CALLGRIND_INCLUDE_DIR}")
    target_compile_definitions(benchmark_driver PRIVATE EXCHANGE_ENABLE_CALLGRIND=1)
endif()

# Allocation replacements are isolated from normal benchmarks and production apps.
add_executable(allocation_driver apps/benchmark_driver/main.cpp apps/benchmark_driver/allocation_tracking.cpp)
target_compile_definitions(allocation_driver PRIVATE EXCHANGE_ALLOCATION_DIAGNOSTICS=1)
target_link_libraries(allocation_driver PRIVATE exchange::core)
exchange_set_warnings(allocation_driver)
exchange_enable_sanitizers(allocation_driver)
exchange_enable_static_analysis(allocation_driver)

add_executable(analysis_demo apps/analysis_demo/main.cpp)
target_link_libraries(analysis_demo PRIVATE exchange::core)
exchange_set_warnings(analysis_demo)
exchange_enable_sanitizers(analysis_demo)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "Exchange ${PROJECT_VERSION}: M2 matching semantics and market-data analysis")

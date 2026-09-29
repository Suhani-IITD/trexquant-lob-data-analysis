# Small workload checks run in all configurations; timings are not test assertions.
add_test(NAME benchmark.workloads COMMAND benchmark_driver --workload all --operations 20 --trials 2 --book-size 3)
add_test(NAME benchmark.allocations COMMAND allocation_driver --workload all --operations 20 --trials 1 --book-size 3)
set_tests_properties(benchmark.allocations PROPERTIES LABELS "benchmark;smoke" TIMEOUT 30)
set_tests_properties(benchmark.workloads PROPERTIES LABELS "benchmark;smoke" TIMEOUT 30)
add_test(NAME benchmark.invalid_count COMMAND benchmark_driver --operations 0)
set_tests_properties(benchmark.invalid_count PROPERTIES WILL_FAIL TRUE LABELS "benchmark;smoke" TIMEOUT 15)

add_executable(exchange_foundation_tests foundation_tests.cpp)
target_link_libraries(exchange_foundation_tests PRIVATE exchange::core)
target_compile_definitions(exchange_foundation_tests PRIVATE
    EXCHANGE_EXPECTED_VERSION="${PROJECT_VERSION}")
exchange_set_warnings(exchange_foundation_tests)
exchange_enable_sanitizers(exchange_foundation_tests)
exchange_enable_static_analysis(exchange_foundation_tests)

foreach(suite IN ITEMS types arithmetic metadata)
    add_test(NAME foundation.${suite} COMMAND exchange_foundation_tests ${suite})
    set_tests_properties(foundation.${suite} PROPERTIES LABELS "foundation;unit" TIMEOUT 15)
endforeach()

add_executable(exchange_engine_tests engine_tests.cpp)
target_link_libraries(exchange_engine_tests PRIVATE exchange::core)
exchange_set_warnings(exchange_engine_tests)
exchange_enable_sanitizers(exchange_engine_tests)
exchange_enable_static_analysis(exchange_engine_tests)
foreach(suite IN ITEMS config fifo price_priority cancel validation generations capacity instruments ingress generated)
    add_test(NAME engine.${suite} COMMAND exchange_engine_tests ${suite})
    set_tests_properties(engine.${suite} PROPERTIES LABELS "engine;unit" TIMEOUT 30)
endforeach()

foreach(case IN ITEMS default version help unknown extra)
    add_test(NAME cli.${case} COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:exchange_info>"
        "-DCASE=${case}"
        "-DVERSION=${PROJECT_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli.cmake")
    set_tests_properties(cli.${case} PROPERTIES LABELS "foundation;smoke" TIMEOUT 15)
endforeach()

# Phase 2: retain Phase 1 fixtures and add executable IOC/replacement examples.
foreach(scenario IN ITEMS fifo_cancel rejections fifo_replace market_ioc)
    add_test(NAME scenario.${scenario} COMMAND scenario_runner
        --input "${PROJECT_SOURCE_DIR}/fixtures/${scenario}.txt")
    set_tests_properties(scenario.${scenario} PROPERTIES LABELS "engine;scenario" TIMEOUT 15)
endforeach()
add_test(NAME scenario.malformed COMMAND ${CMAKE_COMMAND}
    "-DPROGRAM=$<TARGET_FILE:scenario_runner>"
    "-DINPUT=${PROJECT_SOURCE_DIR}/fixtures/malformed.txt"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_scenario_failure.cmake")
set_tests_properties(scenario.malformed PROPERTIES LABELS "engine;scenario" TIMEOUT 15)

# Phase 2: separate suites make the new semantics and allocator-failure evidence
# easy to run without mixing their implementation into the Phase 1 regression file.
add_executable(exchange_phase2_tests phase2_tests.cpp)
target_link_libraries(exchange_phase2_tests PRIVATE exchange::core)
exchange_set_warnings(exchange_phase2_tests)
exchange_enable_sanitizers(exchange_phase2_tests)
exchange_enable_static_analysis(exchange_phase2_tests)
foreach(suite IN ITEMS ioc validation replace_priority replace_crossing acceptance replace_rejections capacity generated)
    add_test(NAME phase2.${suite} COMMAND exchange_phase2_tests ${suite})
    set_tests_properties(phase2.${suite} PROPERTIES LABELS "engine;phase2" TIMEOUT 30)
endforeach()

add_executable(exchange_allocation_failure_tests allocation_failure_tests.cpp)
target_link_libraries(exchange_allocation_failure_tests PRIVATE exchange::core)
exchange_set_warnings(exchange_allocation_failure_tests)
exchange_enable_sanitizers(exchange_allocation_failure_tests)
exchange_enable_static_analysis(exchange_allocation_failure_tests)
add_test(NAME phase2.allocation_failures COMMAND exchange_allocation_failure_tests)
set_tests_properties(phase2.allocation_failures PROPERTIES LABELS "engine;phase2" TIMEOUT 30)


add_executable(exchange_output_reuse_tests output_reuse_tests.cpp)
target_link_libraries(exchange_output_reuse_tests PRIVATE exchange::core)
exchange_set_warnings(exchange_output_reuse_tests)
exchange_enable_sanitizers(exchange_output_reuse_tests)
add_test(NAME engine.output_reuse COMMAND exchange_output_reuse_tests)
add_test(NAME benchmark.output_reuse COMMAND benchmark_driver --workload all --operations 20 --trials 1 --book-size 3 --output reuse)

add_executable(exchange_analysis_tests analysis_tests.cpp)
target_link_libraries(exchange_analysis_tests PRIVATE exchange::core)
exchange_set_warnings(exchange_analysis_tests)
exchange_enable_sanitizers(exchange_analysis_tests)
add_test(NAME analysis.metrics COMMAND exchange_analysis_tests)
add_test(NAME analysis.demo COMMAND ${CMAKE_COMMAND}
    "-DPROGRAM=$<TARGET_FILE:analysis_demo>"
    "-DEXPECTED=${PROJECT_SOURCE_DIR}/fixtures/analysis_expected.csv"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_analysis_demo.cmake")

foreach(case IN ITEMS every_command cadence rejection invalid_interval missing_output same_file output_failure)
    add_test(NAME analysis.scenario_${case} COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:scenario_runner>"
        "-DINPUT=${PROJECT_SOURCE_DIR}/fixtures/analysis_scenario.txt"
        "-DEXPECTED=${PROJECT_SOURCE_DIR}/fixtures/analysis_expected.csv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/analysis-${case}"
        "-DCASE=${case}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_scenario_analysis.cmake")
    set_tests_properties(analysis.scenario_${case} PROPERTIES LABELS "analysis;scenario" TIMEOUT 15)
endforeach()

add_test(NAME benchmark.diverse_analysis COMMAND benchmark_driver --workload diverse --operations 1000 --book-size 100 --trials 1 --output reuse --analysis snapshots --observe-every 37)
set_tests_properties(benchmark.diverse_analysis PROPERTIES LABELS "benchmark;analysis" TIMEOUT 30)
add_test(NAME benchmark.diverse_depth COMMAND benchmark_driver --workload diverse --operations 1000 --book-size 100 --trials 1 --output reuse --analysis depth --observe-every 37)
set_tests_properties(benchmark.diverse_depth PROPERTIES LABELS "benchmark;analysis" TIMEOUT 30)

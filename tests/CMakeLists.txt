# SlickTrader - Tests
#
# Regression coverage for two bugs that only reproduce across a module boundary:
#   - a strategy plugin reading its own BrokerStatusRegistry instead of the host's
#   - a strategy adopting another strategy's orders off the shared event dispatch

find_package(GTest CONFIG REQUIRED)

# Built exactly like a real strategy plugin (SHARED, statically linked against
# slick-trader-core) so it reproduces the private-singleton problem under test.
add_library(probe_plugin SHARED probe_plugin.cpp)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(probe_plugin PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(probe_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(probe_plugin PRIVATE slick-trader-core)
set_target_properties(probe_plugin PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)
if(MSVC)
    target_compile_options(probe_plugin PRIVATE /FS)
endif()

add_executable(slick-trader-tests
    test_main.cpp
    test_broker_status_injection.cpp
    test_strategy_ownership.cpp
)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(slick-trader-tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(slick-trader-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(slick-trader-tests PRIVATE
    slick-trader-core
    GTest::gtest
)
target_compile_definitions(slick-trader-tests PRIVATE
    SLICK_TRADER_TEST_PLUGIN_PATH="$<TARGET_FILE:probe_plugin>"
)
add_dependencies(slick-trader-tests probe_plugin)

if(MSVC)
    target_compile_options(slick-trader-tests PRIVATE /FS)
endif()

include(GoogleTest)
gtest_discover_tests(slick-trader-tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60
)

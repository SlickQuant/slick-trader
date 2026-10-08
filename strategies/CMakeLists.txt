# SlickTrader - Strategy Plugins
#
# Each strategy is built as a shared library (.so/.dll) loaded at runtime by
# StrategyManager. This directory is only added when SLICK_TRADER_BUILD_STRATEGIES
# is ON (see the top-level CMakeLists.txt), so dependencies needed solely by a
# plugin are declared here rather than at the root.

# Declare a strategy plugin.
#
#   slick_trader_add_strategy(<name>
#       [SOURCES <file>...]   # defaults to <name>.cpp
#       [LINK <target>...]    # extra link targets beyond slick-trader-core
#   )
#
# Produces ${CMAKE_BINARY_DIR}/strategies/<name>.dll (or lib<name>.so), with no
# "lib" prefix on any platform because StrategyManager matches plugins by file
# name.
function(slick_trader_add_strategy name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "slick_trader_add_strategy(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(sources ${ARG_SOURCES})
    if(NOT sources)
        set(sources ${name}.cpp)
    endif()

    add_library(${name} SHARED ${sources})

    if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
        set_target_properties(${name} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    endif()

    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(${name} PRIVATE
        slick-trader-core
        ${ARG_LINK}
    )

    set_target_properties(${name} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
    )

    # Synchronize PDB writes on Windows
    if(MSVC)
        target_compile_options(${name} PRIVATE /FS)
    endif()
endfunction()

# slick-stacker - header-only resting-order ladder engine. Required only by the
# order_stacker plugin below, which is why it is fetched here and not at the root.
find_package(slick-stacker 0.2.0 CONFIG QUIET)
if (NOT slick-stacker_FOUND)
    message(STATUS "Fetching slick-stacker...")
    set(SLICK_STACKER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SLICK_STACKER_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(SLICK_STACKER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-stacker
        GIT_REPOSITORY https://github.com/SlickQuant/slick-stacker.git
        GIT_TAG v0.2.0
    )
    FetchContent_MakeAvailable(slick-stacker)
else()
    message(STATUS "Found slick-stacker ${slick-stacker_VERSION}: ${slick-stacker_DIR}")
endif()

# Market Maker - inventory skew, dual-EMA trend detection, book imbalance,
# and configurable fair value modes
slick_trader_add_strategy(market_maker)

# Order Stacker - two-sided resting ladder built on the header-only slick-stacker
# library. Reference example for driving a third-party OMS from StrategyBase.
slick_trader_add_strategy(order_stacker LINK slick::stacker)

# Install Lua/Python strategy examples
install(FILES
    # examples/simple_market_maker.lua
    DESTINATION share/slick-trader/strategies
    OPTIONAL
)

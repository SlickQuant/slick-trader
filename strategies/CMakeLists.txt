# SlickTrader - Strategy Examples

# Each strategy is built as a shared library (.so/.dll) that can be loaded at runtime

# Market Maker - full market making plugin with inventory skew,
# dual-EMA trend detection, book imbalance, and configurable fair value modes
add_library(market_maker SHARED
    market_maker.cpp
)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(market_maker PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(market_maker PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(market_maker PRIVATE
    slick-trader-core
)

set_target_properties(market_maker PROPERTIES
    PREFIX ""
    OUTPUT_NAME "market_maker"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
)

# Synchronize PDB writes on Windows
if(MSVC)
    target_compile_options(market_maker PRIVATE /FS)
endif()

# Install Lua/Python strategy examples
install(FILES
    # examples/simple_market_maker.lua
    DESTINATION share/slick-trader/strategies
    OPTIONAL
)

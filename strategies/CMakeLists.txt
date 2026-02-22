# SlickTrader - Strategy Examples

# Each strategy is built as a shared library (.so/.dll) that can be loaded at runtime

# Simple Strategy Plugin - demonstrates position tracking, PnL, and risk management
add_library(simple_strategy_plugin SHARED
    simple_strategy_plugin.cpp
)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(simple_strategy_plugin PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(simple_strategy_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(simple_strategy_plugin PRIVATE
    slick-trader-core
)

set_target_properties(simple_strategy_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "simple_strategy_plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
)

# Synchronize PDB writes on Windows
if(MSVC)
    target_compile_options(simple_strategy_plugin PRIVATE /FS)
endif()

# Install Lua/Python strategy examples
install(FILES
    # examples/simple_market_maker.lua
    DESTINATION share/slick-trader/strategies
    OPTIONAL
)

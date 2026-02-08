# SlickTrader - Strategy Examples

# Example strategy plugins will be added here
# Each strategy is built as a shared library that can be loaded at runtime

# Example:
# add_library(simple_market_maker SHARED
#     examples/simple_market_maker.cpp
# )
#
# target_include_directories(simple_market_maker PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
# )
#
# target_link_libraries(simple_market_maker PRIVATE
#     slick-trader-core
# )
#
# set_target_properties(simple_market_maker PROPERTIES
#     PREFIX ""
#     LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
# )

# Install Lua/Python strategy examples
install(FILES
    # examples/simple_market_maker.lua
    DESTINATION share/slick-trader/strategies
    OPTIONAL
)

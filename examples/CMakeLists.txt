# Example Trading System executable
add_executable(example_trading_system
    example_trading_system.cpp
)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(example_trading_system PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(example_trading_system PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(example_trading_system PRIVATE
    slick-trader-core
    slick-trader-brokers
    slick::logger
    nlohmann_json::nlohmann_json
)

# Copy config.json to build directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.json
    ${CMAKE_CURRENT_BINARY_DIR}/config.json
    COPYONLY
)

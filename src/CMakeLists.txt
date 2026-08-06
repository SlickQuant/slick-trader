# SlickTrader - Main library and executable

# Core library (includes web server)
add_library(slick-trader-core STATIC
    core/trading_engine.cpp
    core/position_tracker.cpp
    core/pnl_calculator.cpp
    core/risk_manager.cpp
    core/strategy_loader.cpp
    core/strategy_manager.cpp
    web/web_server.cpp
)

target_include_directories(slick-trader-core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(slick-trader-core PUBLIC
    slick::logger
    nlohmann_json::nlohmann_json
    slick::orderbook
    slick::stream_buffer_multiplexer
    unofficial::uwebsockets::uwebsockets
)

# Broker implementations library
add_library(slick-trader-brokers STATIC
    core/broker_factory.cpp
    core/broker_status.cpp
    brokers/coinbase/coinbase_broker.cpp
    brokers/coinbase/coinbase_md_connector.cpp
    brokers/coinbase/coinbase_order_connector.cpp
    brokers/hyperliquid/hyperliquid_broker.cpp
    brokers/hyperliquid/hyperliquid_md_connector.cpp
    brokers/hyperliquid/hyperliquid_order_connector.cpp
)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(slick-trader-core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    set_target_properties(slick-trader-brokers PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(slick-trader-brokers PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(slick-trader-brokers PUBLIC
    slick-trader-core
    slick::coinbase-advanced-cpp
    hyperliquid
)

# Enable precompiled headers for faster compilation of Coinbase connectors
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(slick-trader-brokers PRIVATE
        brokers/coinbase/coinbase_pch.hpp
    )
endif()

# Synchronize PDB writes to avoid locking errors on Windows parallel builds
if(MSVC)
    target_compile_options(slick-trader-brokers PRIVATE /FS)
    target_compile_options(slick-trader-core PRIVATE /FS)
endif()

# # Strategy library
# add_library(slick-trader-strategies STATIC
#     strategy/strategy_loader.cpp
# )

# if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
#     set_target_properties(slick-trader-strategies PROPERTIES CXX_SCAN_FOR_MODULES OFF)
# endif()

# target_include_directories(slick-trader-strategies PUBLIC
#     ${CMAKE_SOURCE_DIR}/include
# )

# target_link_libraries(slick-trader-strategies PUBLIC
#     slick-trader-core
#     ${CMAKE_DL_LIBS}
# )

# Main executable
add_executable(slick-trader
    main.cpp
)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(slick-trader PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_link_libraries(slick-trader PRIVATE
    slick-trader-core
    slick-trader-brokers
    # slick-trader-strategies
    ${CMAKE_DL_LIBS}
)

# Install targets
install(TARGETS slick-trader
    RUNTIME DESTINATION bin
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/slick-trader
    DESTINATION include
)

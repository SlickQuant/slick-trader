# SlickTrader - Main library and executable

# Core library (includes web server)
add_library(slick-trader-core STATIC
    core/broker_registry.cpp
    core/broker_status.cpp
    core/trading_engine.cpp
    core/position_store.cpp
    core/position_tracker.cpp
    core/pnl_calculator.cpp
    core/risk_manager.cpp
    core/strategy_loader.cpp
    core/strategy_manager.cpp
    utils/mapped_file.cpp
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

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(slick-trader-core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

# Broker adapters: one OBJECT library each, registered with BrokerRegistry - see
# cmake/slick_trader_broker.cmake. Also builds any SLICK_TRADER_EXTERNAL_BROKERS.
add_subdirectory(brokers)

# Synchronize PDB writes to avoid locking errors on Windows parallel builds
if(MSVC)
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
    # slick-trader-strategies
    ${CMAKE_DL_LIBS}
)
slick_trader_link_brokers(slick-trader)

# Install targets
install(TARGETS slick-trader
    RUNTIME DESTINATION bin
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/slick-trader
    DESTINATION include
)

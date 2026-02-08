# SlickTrader - Main library and executable

# Core library
add_library(slick-trader-core STATIC
    core/trading_engine.cpp
)

target_include_directories(slick-trader-core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(slick-trader-core PUBLIC
    slick::logger
    nlohmann_json::nlohmann_json
    slick::orderbook
)

# Broker implementations library
add_library(slick-trader-brokers STATIC
    brokers/coinbase/coinbase_broker.cpp
    brokers/coinbase/coinbase_md_connector.cpp
    brokers/coinbase/coinbase_order_connector.cpp
)

target_include_directories(slick-trader-brokers PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(slick-trader-brokers PUBLIC
    slick-trader-core
    slick::coinbase-advanced-cpp
)

# Enable precompiled headers for faster compilation of Coinbase connectors
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(slick-trader-brokers PRIVATE
        brokers/coinbase/coinbase_pch.h
    )
endif()

# Synchronize PDB writes to avoid locking errors on Windows parallel builds
if(MSVC)
    target_compile_options(slick-trader-brokers PRIVATE /FS)
    target_compile_options(slick-trader-core PRIVATE /FS)
endif()

# Strategy library (to be implemented)
# add_library(slick-trader-strategies STATIC
#     strategies/cpp_strategy.cpp
#     strategies/strategy_loader.cpp
# )
#
# target_include_directories(slick-trader-strategies PUBLIC
#     ${CMAKE_SOURCE_DIR}/include
# )
#
# target_link_libraries(slick-trader-strategies PUBLIC
#     slick-trader-core
# )

# Web server library (to be implemented)
# add_library(slick-trader-web STATIC
#     web/web_server.cpp
#     web/rest_api_handler.cpp
#     web/websocket_handler.cpp
# )
#
# target_include_directories(slick-trader-web PUBLIC
#     ${CMAKE_SOURCE_DIR}/include
# )
#
# target_link_libraries(slick-trader-web PUBLIC
#     slick-trader-core
#     unofficial::uwebsockets::uwebsockets
# )

# Main executable
add_executable(slick-trader
    main.cpp
)

target_link_libraries(slick-trader PRIVATE
    slick-trader-core
    slick-trader-brokers
    ${CMAKE_DL_LIBS}
)

# Install targets
install(TARGETS slick-trader
    RUNTIME DESTINATION bin
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/slick-trader
    DESTINATION include
)

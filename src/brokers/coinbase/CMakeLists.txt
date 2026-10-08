slick_trader_add_broker(coinbase
    SOURCES
        coinbase_broker.cpp
        coinbase_md_connector.cpp
        coinbase_order_connector.cpp
    DEPENDS
        slick::coinbase-advanced-cpp
)

# The Coinbase SDK headers are template-heavy; precompile them once.
target_precompile_headers(slick_trader_broker_coinbase PRIVATE coinbase_pch.hpp)

# Resolve the SDK only when SLICK_TRADER_ENABLE_COINBASE is ON.
# GLOBAL lets slick-trader and slick-trader-tests link the SDK from other directories.
find_package(coinbase-advanced-cpp ${COINBASE_ADVANCED_CPP_MIN_VERSION} CONFIG QUIET GLOBAL)
if (NOT coinbase-advanced-cpp_FOUND)
    message(STATUS "Fetching coinbase-advanced-cpp...")
    set(BUILD_COINBASE_ADVANCED_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        coinbase-advanced-cpp
        GIT_REPOSITORY https://github.com/SlickQuant/coinbase-advanced-cpp.git
        GIT_TAG v${COINBASE_ADVANCED_CPP_MIN_VERSION}
    )
    FetchContent_MakeAvailable(coinbase-advanced-cpp)
else()
    message(STATUS "Found coinbase-advanced-cpp ${coinbase-advanced-cpp_VERSION}: ${coinbase-advanced-cpp_DIR}")
endif()
slick_trader_use_slick_net()

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

# Resolve the SDK only when SLICK_TRADER_ENABLE_HYPERLIQUID is ON.
# GLOBAL lets slick-trader and slick-trader-tests link the SDK from other directories.
find_package(hyperliquid ${HYPERLIQUID_MIN_VERSION} CONFIG QUIET GLOBAL)
if (NOT hyperliquid_FOUND)
    message(STATUS "Fetching hyperliquid-cpp...")
    FetchContent_Declare(
        hyperliquid-cpp
        GIT_REPOSITORY https://github.com/SlickQuant/hyperliquid-cpp.git
        GIT_TAG v${HYPERLIQUID_MIN_VERSION}
    )
    FetchContent_MakeAvailable(hyperliquid-cpp)
else()
    message(STATUS "Found hyperliquid ${hyperliquid_VERSION}: ${hyperliquid_DIR}")
endif()
slick_trader_use_slick_net()

slick_trader_add_broker(hyperliquid
    SOURCES
        hyperliquid_broker.cpp
        hyperliquid_md_connector.cpp
        hyperliquid_order_connector.cpp
    DEPENDS
        hyperliquid
)

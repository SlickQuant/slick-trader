cmake_minimum_required(VERSION 3.28)

project(SlickTrader 
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN "Disable C++ module dependency scanning for faster MSVC builds" ON)
option(SLICK_TRADER_FAST_FETCHCONTENT "Avoid FetchContent update checks on every configure" ON)
# Example strategy plugins are for developing against this repo, not for consuming it. Default them
# off when SlickTrader is pulled in as a subproject, so a dependent neither builds them nor pays for
# the dependencies only they need (slick-stacker, declared in strategies/CMakeLists.txt).
option(SLICK_TRADER_BUILD_STRATEGIES "Build the example strategy plugins" ${PROJECT_IS_TOP_LEVEL})
option(SLICK_TRADER_SUPPRESS_VS_REGEN "Disable ZERO_CHECK auto-regeneration for Visual Studio generators" ON)
option(SLICK_TRADER_ENABLE_LUA_STRATEGY "Enable Lua strategy support auto-detection" OFF)
option(SLICK_TRADER_ENABLE_PYTHON_STRATEGY "Enable Python strategy support auto-detection" ON)
option(SLICK_TRADER_BUILD_TESTS "Build the unit/regression tests" ${PROJECT_IS_TOP_LEVEL})

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

# Windows version definition
if(MSVC)
    add_definitions(-D_WIN32_WINNT=0x0601)

    if(SLICK_TRADER_SUPPRESS_VS_REGEN AND CMAKE_GENERATOR MATCHES "Visual Studio")
        # Avoid expensive CMake re-check on each build invocation.
        set(CMAKE_SUPPRESS_REGENERATION ON)
    endif()
endif()

if (CMAKE_BUILD_TYPE MATCHES Debug)
    add_definitions(-DDEBUG)
endif()

if (CMAKE_BUILD_TYPE MATCHES Release)
    add_definitions(-DNDEBUG)
endif()

include(FetchContent)

if(SLICK_TRADER_FAST_FETCHCONTENT)
    # Keep already-fetched dependency sources but skip remote update checks on each configure.
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
    set(FETCHCONTENT_UPDATES_DISCONNECTED_SLICK-LOGGER ON CACHE BOOL "" FORCE)
    set(FETCHCONTENT_UPDATES_DISCONNECTED_SLICK-NET ON CACHE BOOL "" FORCE)
    set(FETCHCONTENT_UPDATES_DISCONNECTED_SLICK-ORDERBOOK ON CACHE BOOL "" FORCE)
    set(FETCHCONTENT_UPDATES_DISCONNECTED_SLICK-STACKER ON CACHE BOOL "" FORCE)
    set(FETCHCONTENT_UPDATES_DISCONNECTED_COINBASE-ADVANCED-CPP ON CACHE BOOL "" FORCE)
endif()

set(SLICK_LOGGER_ENABLE_SOURCE_LOCATION OFF CACHE BOOL "" FORCE)
find_package(slick-logger 2.2.0 CONFIG QUIET)
if (NOT slick-logger_FOUND)
    message(STATUS "Fetching slick-logger...")
    # Disable examples, tests, and benchmarks for slick_logger
    set(BUILD_SLICK_LOGGER_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_LOGGER_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_LOGGER_BENCHMARKS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-logger
        GIT_REPOSITORY https://github.com/SlickQuant/slick-logger.git
        GIT_TAG v2.2.0
    )
    FetchContent_MakeAvailable(slick-logger)
else()
    message(STATUS "Found slick-logger ${slick-logger_VERSION}: ${slick-logger_DIR}")
endif()

# Add slick-orderbook dependency
find_package(slick-orderbook 1.1.0 CONFIG QUIET)
if (NOT slick-orderbook_FOUND)
    message(STATUS "Fetching slick-orderbook...")
    set(BUILD_SLICK_ORDERBOOK_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_ORDERBOOK_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_ORDERBOOK_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-orderbook
        GIT_REPOSITORY https://github.com/SlickQuant/slick-orderbook.git
        GIT_TAG v1.1.0
    )
    FetchContent_MakeAvailable(slick-orderbook)
else()
    message(STATUS "Found slick-orderbook ${slick-orderbook_VERSION}: ${slick-orderbook_DIR}")
endif()

# NOTE: slick-stacker is NOT declared here. It is needed only by the order_stacker strategy
# plugin, so it is declared in strategies/CMakeLists.txt and fetched only when
# SLICK_TRADER_BUILD_STRATEGIES is ON.

# Add coinbase-advanced-cpp dependency
find_package(coinbase-advanced-cpp 2.0.1 CONFIG QUIET)
if (NOT coinbase-advanced-cpp_FOUND)
    message(STATUS "Fetching coinbase-advanced-cpp...")
    set(BUILD_COINBASE_ADVANCED_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        coinbase-advanced-cpp
        GIT_REPOSITORY https://github.com/SlickQuant/coinbase-advanced-cpp.git
        GIT_TAG v2.0.1
    )
    FetchContent_MakeAvailable(coinbase-advanced-cpp)
else()
    message(STATUS "Found coinbase-advanced-cpp ${coinbase-advanced-cpp_VERSION}: ${coinbase-advanced-cpp_DIR}")
endif()

find_package(hyperliquid 0.4.0 CONFIG QUIET)
if (NOT hyperliquid_FOUND)
    message(STATUS "Fetching hyperliquid-cpp...")
    FetchContent_Declare(
        hyperliquid-cpp
        GIT_REPOSITORY https://github.com/SlickQuant/hyperliquid-cpp.git
        GIT_TAG v0.4.0
    )
    FetchContent_MakeAvailable(hyperliquid-cpp)
else()
    message(STATUS "Found hyperliquid ${hyperliquid_VERSION}: ${hyperliquid_DIR}")
endif()

# slick-net should be found or fetched by coinbase/hyperliquid
find_package(slick-net CONFIG QUIET)
if (slick-net_FOUND)
    message(STATUS "Found slick-net ${slick-net_VERSION}: ${slick-net_DIR}")
endif()

find_package(slick-shm CONFIG QUIET)
if (slick-shm_FOUND)
    message(STATUS "Found slick-shm ${slick-shm_VERSION}: ${slick-shm_DIR}")
endif()

find_package(slick-queue CONFIG QUIET)
if (slick-queue_FOUND)
    message(STATUS "Found slick-queue ${slick-queue_VERSION}: ${slick-queue_DIR}")
endif()

find_package(slick-stream-buffer CONFIG QUIET)
if (slick-stream-buffer_FOUND)
    message(STATUS "Found slick-stream-buffer ${slick-stream-buffer_VERSION}: ${slick-stream-buffer_DIR}")
endif()

find_package(slick-stream-buffer-multiplexer CONFIG QUIET)
if (slick-stream-buffer-multiplexer_FOUND)
    message(STATUS "Found slick-stream-buffer-multiplexer ${slick-stream-buffer-multiplexer_VERSION}: ${slick-stream-buffer-multiplexer_DIR}")
endif()

find_package(slick-dynamic_buffer CONFIG QUIET)
if (slick-dynamic_buffer_FOUND)
    message(STATUS "Found slick-dynamic_buffer ${slick-dynamic_buffer_VERSION}: ${slick-dynamic_buffer_DIR}")
endif()

# Add Alpaca SDK (assuming GitHub repo exists)
# Note: If alpaca SDK is not available via FetchContent, it can be integrated manually
# find_package(alpaca-cpp CONFIG QUIET)
# if (NOT alpaca-cpp_FOUND)
#     message(STATUS "Fetching alpaca-cpp...")
#     FetchContent_Declare(
#         alpaca-cpp
#         GIT_REPOSITORY https://github.com/alpacahq/alpaca-trade-api-cpp.git
#         GIT_TAG master
#     )
#     FetchContent_MakeAvailable(alpaca-cpp)
# endif()

# Required dependencies (prefer vcpkg installation)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

# Optional Lua support for script-based strategies
if(SLICK_TRADER_ENABLE_LUA_STRATEGY)
    find_package(Lua 5.4 QUIET)
    if(Lua_FOUND)
        message(STATUS "Lua found - enabling Lua strategy support")
        add_definitions(-DSLICK_TRADER_LUA_SUPPORT)
    else()
        message(STATUS "Lua strategy detection enabled, but Lua not found - Lua strategy support disabled")
    endif()
else()
    message(STATUS "Lua strategy detection disabled")
endif()

# Optional Python support for script-based strategies
if(SLICK_TRADER_ENABLE_PYTHON_STRATEGY)
    find_package(Python3 COMPONENTS Development QUIET)
    if(Python3_FOUND)
        message(STATUS "Python3 found - enabling Python strategy support")
        add_definitions(-DSLICK_TRADER_PYTHON_SUPPORT)
    else()
        message(STATUS "Python3 strategy detection enabled, but Python3 not found - Python strategy support disabled")
    endif()
else()
    message(STATUS "Python strategy detection disabled")
endif()

# Dynamic loading support
if(UNIX AND NOT APPLE)
    set(CMAKE_DL_LIBS ${CMAKE_DL_LIBS} dl)
elseif(UNIX AND APPLE)
    # macOS handles dlopen without explicit linking
    set(CMAKE_DL_LIBS "")
endif()

add_subdirectory(src)
if(SLICK_TRADER_BUILD_STRATEGIES)
    add_subdirectory(strategies)
else()
    message(STATUS "Strategy plugins disabled (SLICK_TRADER_BUILD_STRATEGIES=OFF)")
endif()
add_subdirectory(web)
# add_subdirectory(examples)

# Tests
if(SLICK_TRADER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
else()
    message(STATUS "Tests disabled (SLICK_TRADER_BUILD_TESTS=OFF)")
endif()

cmake_minimum_required(VERSION 3.28)

project(SlickTrader 
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN "Disable C++ module dependency scanning for faster MSVC builds" ON)
option(SLICK_TRADER_FAST_FETCHCONTENT "Avoid FetchContent update checks on every configure" ON)
option(SLICK_TRADER_SUPPRESS_VS_REGEN "Disable ZERO_CHECK auto-regeneration for Visual Studio generators" ON)
option(SLICK_TRADER_ENABLE_LUA_STRATEGY "Enable Lua strategy support auto-detection" OFF)
option(SLICK_TRADER_ENABLE_PYTHON_STRATEGY "Enable Python strategy support auto-detection" ON)

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
    set(FETCHCONTENT_UPDATES_DISCONNECTED_COINBASE-ADVANCED-CPP ON CACHE BOOL "" FORCE)
endif()

find_package(slick-logger 1.0.6 CONFIG QUIET)
if (NOT slick-logger_FOUND)
    message(STATUS "Fetching slick-logger...")
    # Disable examples, tests, and benchmarks for slick_logger
    set(BUILD_SLICK_LOGGER_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_LOGGER_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_LOGGER_BENCHMARKS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-logger
        GIT_REPOSITORY https://github.com/SlickQuant/slick-logger.git
        GIT_TAG v1.0.6
    )
    FetchContent_MakeAvailable(slick-logger)
else()
    message(STATUS "Found slick-logger: ${slick-logger_VERSION}")
endif()

# Add slick-orderbook dependency
find_package(slick-orderbook 1.0.1 CONFIG QUIET)
if (NOT slick-orderbook_FOUND)
    message(STATUS "Fetching slick-orderbook...")
    set(BUILD_SLICK_ORDERBOOK_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_ORDERBOOK_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_ORDERBOOK_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-orderbook
        GIT_REPOSITORY https://github.com/SlickQuant/slick-orderbook.git
        GIT_TAG v1.0.1
    )
    FetchContent_MakeAvailable(slick-orderbook)
else()
    message(STATUS "Found slick-orderbook: ${slick-orderbook_VERSION}")
endif()

# Add coinbase-advanced-cpp dependency
find_package(coinbase-advanced-cpp 0.2.2 CONFIG QUIET)
if (NOT coinbase-advanced-cpp_FOUND)
    message(STATUS "Fetching coinbase-advanced-cpp...")
    set(BUILD_COINBASE_ADVANCED_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        coinbase-advanced-cpp
        GIT_REPOSITORY https://github.com/SlickQuant/coinbase-advanced-cpp.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(coinbase-advanced-cpp)
else()
    message(STATUS "Found coinbase-advanced-cpp: ${coinbase-advanced-cpp_VERSION}")
endif()

find_package(slick-net 2.0.0 CONFIG QUIET)
if (NOT slick-net_FOUND)
    message(STATUS "Fetching slick-net...")
    FetchContent_Declare(
        slick-net
        GIT_REPOSITORY https://github.com/SlickQuant/slick-net.git
        GIT_TAG v2.0.0
    )
    FetchContent_MakeAvailable(slick-net)
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
add_subdirectory(strategies)
add_subdirectory(web)
add_subdirectory(examples)

# Tests
# option(BUILD_EXCH_SIMULATOR_TESTING "Build tests" ON)
# if(BUILD_EXCH_SIMULATOR_TESTING)
#   find_package(GTest CONFIG REQUIRED)
#   enable_testing()
#   add_subdirectory(tests)
# endif()

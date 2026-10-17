add_subdirectory(coinbase)
add_subdirectory(hyperliquid)

# Out-of-tree brokers: directories that call slick_trader_add_broker() exactly like
# the in-tree ones, kept in their own repositories. Each is built into the binary
# and its TESTS into slick-trader-tests. Set it from the command line or a
# git-ignored CMakeUserPresets.json, so no local path is ever committed:
#
#   cmake -S . -B build -DSLICK_TRADER_EXTERNAL_BROKERS="/path/to/broker-a;/path/to/broker-b"
set(SLICK_TRADER_EXTERNAL_BROKERS "" CACHE STRING "Out-of-tree broker directories, ;-separated")
foreach(dir IN LISTS SLICK_TRADER_EXTERNAL_BROKERS)
    if(NOT EXISTS "${dir}/CMakeLists.txt")
        message(FATAL_ERROR "SLICK_TRADER_EXTERNAL_BROKERS: no CMakeLists.txt in '${dir}'")
    endif()
    get_filename_component(_slick_trader_external_name "${dir}" NAME)
    add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/external_brokers/${_slick_trader_external_name}")

    # The version a broker's own project() declares. One that declares none inherits
    # slick-trader's, which says nothing about the broker, so nothing is shown for it.
    get_directory_property(_slick_trader_broker_project DIRECTORY "${dir}" DEFINITION PROJECT_NAME)
    get_directory_property(_slick_trader_broker_version DIRECTORY "${dir}" DEFINITION PROJECT_VERSION)
    if(_slick_trader_broker_project STREQUAL PROJECT_NAME OR NOT _slick_trader_broker_version)
        set(_slick_trader_broker_version "")
    else()
        set(_slick_trader_broker_version " ${_slick_trader_broker_version}")
    endif()
    message(STATUS "slick-trader external broker ${_slick_trader_external_name}${_slick_trader_broker_version}: ${dir}")
endforeach()

get_property(_slick_trader_brokers GLOBAL PROPERTY SLICK_TRADER_ENABLED_BROKERS)
message(STATUS "slick-trader brokers: ${_slick_trader_brokers}")

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
endforeach()

get_property(_slick_trader_brokers GLOBAL PROPERTY SLICK_TRADER_ENABLED_BROKERS)
message(STATUS "slick-trader brokers: ${_slick_trader_brokers}")

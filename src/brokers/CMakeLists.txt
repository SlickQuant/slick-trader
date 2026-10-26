if(SLICK_TRADER_ENABLE_COINBASE)
    add_subdirectory(coinbase)
endif()
if(SLICK_TRADER_ENABLE_HYPERLIQUID)
    add_subdirectory(hyperliquid)
endif()

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
    message(STATUS "Adding slick-trader external broker: ${dir}")

    # Brokers registered so far, to report what this directory adds and flag one that
    # adds none (a path to the wrong repository, say).
    get_property(_slick_trader_before GLOBAL PROPERTY SLICK_TRADER_ENABLED_BROKERS)
    list(LENGTH _slick_trader_before _slick_trader_before_count)

    # Indent the broker's own output (its dependency lookups) under the line above.
    list(APPEND CMAKE_MESSAGE_INDENT "  ")
    add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/external_brokers/${_slick_trader_external_name}")
    list(POP_BACK CMAKE_MESSAGE_INDENT)

    # The version a broker's own project() declares. One that declares none inherits
    # slick-trader's, which says nothing about the broker, so nothing is shown for it.
    get_directory_property(_slick_trader_broker_project DIRECTORY "${dir}" DEFINITION PROJECT_NAME)
    get_directory_property(_slick_trader_broker_version DIRECTORY "${dir}" DEFINITION PROJECT_VERSION)
    if(_slick_trader_broker_project STREQUAL PROJECT_NAME OR NOT _slick_trader_broker_version)
        set(_slick_trader_broker_version "")
    else()
        set(_slick_trader_broker_version " ${_slick_trader_broker_version}")
    endif()

    # Compare lengths first: list(SUBLIST) fails when the begin index equals the length.
    get_property(_slick_trader_after GLOBAL PROPERTY SLICK_TRADER_ENABLED_BROKERS)
    list(LENGTH _slick_trader_after _slick_trader_after_count)
    if(_slick_trader_after_count GREATER _slick_trader_before_count)
        list(SUBLIST _slick_trader_after ${_slick_trader_before_count} -1 _slick_trader_added)
        message(STATUS "slick-trader external broker ${_slick_trader_external_name}${_slick_trader_broker_version} registers: "
            "${_slick_trader_added}")
    else()
        message(WARNING "SLICK_TRADER_EXTERNAL_BROKERS: '${dir}' registered no broker - "
            "its CMakeLists.txt must call slick_trader_add_broker()")
    endif()
endforeach()

get_property(_slick_trader_brokers GLOBAL PROPERTY SLICK_TRADER_ENABLED_BROKERS)
if(_slick_trader_brokers)
    message(STATUS "slick-trader brokers: ${_slick_trader_brokers}")
else()
    message(STATUS "slick-trader brokers: none")
endif()

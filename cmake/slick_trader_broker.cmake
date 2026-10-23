# Broker adapters, each built as its own OBJECT library.
#
#   slick_trader_add_broker(coinbase
#       SOURCES coinbase_broker.cpp coinbase_md_connector.cpp ...
#       DEPENDS slick::coinbase-advanced-cpp
#       TESTS   test_coinbase_something.cpp)
#
# DEPENDS is linked PUBLIC, so a broker's SDK reaches whatever links the broker
# (slick-trader and slick-trader-tests) without either naming it. TESTS are
# compiled into slick-trader-tests, so a broker's suite travels with the broker -
# which is what lets an out-of-tree broker (SLICK_TRADER_EXTERNAL_BROKERS) bring
# its tests along. Relative paths resolve against the calling directory.
#
# The target is an OBJECT library on purpose. Each broker registers itself with
# BrokerRegistry from a namespace-scope initialiser (SLICK_TRADER_REGISTER_BROKER)
# that nothing in the core ever names - out of a *static* library the linker drops
# that object as unreferenced, and the broker silently vanishes from the registry.
# Object files are handed to the linker individually, so the initialiser survives.
function(slick_trader_add_broker name)
    cmake_parse_arguments(STB "" "" "SOURCES;DEPENDS;TESTS" ${ARGN})
    set(target slick_trader_broker_${name})

    add_library(${target} OBJECT ${STB_SOURCES})
    target_link_libraries(${target} PUBLIC slick-trader-core ${STB_DEPENDS})

    if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
        set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    endif()
    if(MSVC)
        # Synchronize PDB writes to avoid locking errors on parallel builds.
        target_compile_options(${target} PRIVATE /FS)
    endif()

    # Lets a consumer - chiefly the test binary - check for a broker only when it
    # is actually built.
    string(TOUPPER ${name} name_uc)
    target_compile_definitions(${target} INTERFACE SLICK_TRADER_HAS_${name_uc}=1)

    set_property(GLOBAL APPEND PROPERTY SLICK_TRADER_ENABLED_BROKERS ${name})

    foreach(test_source IN LISTS STB_TESTS)
        cmake_path(ABSOLUTE_PATH test_source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_property(GLOBAL APPEND PROPERTY SLICK_TRADER_BROKER_TESTS "${test_source}")
    endforeach()
endfunction()

# Brokers using slick-net call this to route its logs through the engine's logger.
# Find slick-net if the broker's SDK has not already provided it. With no caller,
# the build has no slick-net dependency.
macro(slick_trader_use_slick_net)
    get_property(_slick_trader_uses_slick_net GLOBAL PROPERTY SLICK_TRADER_USES_SLICK_NET)
    if(NOT _slick_trader_uses_slick_net)
        if(NOT TARGET slick::net)
            find_package(slick-net CONFIG REQUIRED GLOBAL)
            message(STATUS "Found slick-net ${slick-net_VERSION}: ${slick-net_DIR}")
        endif()
        # Link here so CMake can resolve SDK targets scoped to this broker's directory.
        target_link_libraries(slick-trader-core PRIVATE slick::net)
        set_property(GLOBAL PROPERTY SLICK_TRADER_USES_SLICK_NET ON)
    endif()
endmacro()

# Links every broker into `target`. One function for the executable and the test
# binary, so the two cannot end up with different broker sets.
function(slick_trader_link_brokers target)
    get_property(brokers GLOBAL PROPERTY SLICK_TRADER_ENABLED_BROKERS)
    foreach(broker IN LISTS brokers)
        target_link_libraries(${target} PRIVATE slick_trader_broker_${broker})
    endforeach()
endfunction()

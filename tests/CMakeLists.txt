# One executable per test translation unit: a test that corrupts global state or
# terminates the process cannot hide a failure in another file.

# White-box tests may include the internal canonical codec header, so the
# library's private source directory is on the test include path.  It is never
# installed and never exported.
add_library(mc_test_support INTERFACE)
target_include_directories(mc_test_support INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(mc_test_support INTERFACE ${CMAKE_DL_LIBS})

function(mc_link_first_party target)
    target_link_libraries(${target} PRIVATE $<BUILD_INTERFACE:mc_first_party_warnings>)
    if(TARGET mc_sanitizers)
        target_link_libraries(${target} PRIVATE $<BUILD_INTERFACE:mc_sanitizers>)
    endif()
endfunction()

function(mc_add_test name source label)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        SummonDCCP::MaintenanceCoordinator
        mc_test_support)
    mc_link_first_party(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${label}")
endfunction()

# ---------------------------------------------------------------------------
# Unit tests: value semantics, canonical encoding, error precedence, engine.
# ---------------------------------------------------------------------------

mc_add_test(unit_foundation unit/foundation_test.cpp unit)
mc_add_test(unit_model unit/model_test.cpp unit)
mc_add_test(unit_engine unit/engine_test.cpp unit)
mc_add_test(unit_adversarial unit/adversarial_test.cpp unit)
mc_add_test(unit_property unit/property_test.cpp unit)

# ---------------------------------------------------------------------------
# System tests: real child processes, real crashes, real locks, real files.
# ---------------------------------------------------------------------------

# The probe is a test-only helper process.  It is built next to the system tests
# so that they can locate it from their own module path.
add_executable(mc_probe helpers/mc_probe.cpp)
target_link_libraries(mc_probe PRIVATE
    SummonDCCP::MaintenanceCoordinator
    mc_test_support)
mc_link_first_party(mc_probe)

function(mc_add_system_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        SummonDCCP::MaintenanceCoordinator
        mc_test_support)
    mc_link_first_party(${name})
    add_dependencies(${name} mc_probe)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "system")
endfunction()

mc_add_system_test(system_durability system/durability_test.cpp)
mc_add_system_test(system_concurrency system/concurrency_test.cpp)

# The command line proof only exists when the tool is part of the build.
if(MC_BUILD_CLI)
    mc_add_system_test(system_cli_e2e system/cli_e2e_test.cpp)
    add_dependencies(system_cli_e2e maintenance-coordinator)
endif()

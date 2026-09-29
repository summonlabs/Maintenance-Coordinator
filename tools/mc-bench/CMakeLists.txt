# The benchmark is built only when explicitly requested, and is never part of
# the test suite: a benchmark that runs in CI is a flaky test with extra steps.

add_executable(maintenance-coordinator-bench benchmark.cpp)
target_link_libraries(maintenance-coordinator-bench PRIVATE
    SummonDCCP::MaintenanceCoordinator
    $<BUILD_INTERFACE:mc_first_party_warnings>)

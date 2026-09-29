# The command line surface.  It is a thin, strict adapter over the library: it
# parses arguments and JSON, calls exactly one coordinator command, and renders
# the result.  It owns no policy of its own.

add_executable(maintenance-coordinator main.cpp)

target_link_libraries(maintenance-coordinator PRIVATE
    SummonDCCP::MaintenanceCoordinator
    $<BUILD_INTERFACE:mc_first_party_warnings>)

if(TARGET mc_sanitizers)
    target_link_libraries(maintenance-coordinator PRIVATE $<BUILD_INTERFACE:mc_sanitizers>)
endif()

install(TARGETS maintenance-coordinator
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

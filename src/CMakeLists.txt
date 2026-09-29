# The Maintenance Coordinator runtime library.
#
# Deliberately a static library: the repository is dependency-light and the
# downstream contract is the exported target, not a stable ABI.  Consumers link
# SummonDCCP::MaintenanceCoordinator through find_package.

# Sources are listed explicitly so that the build never picks up a file that
# was left behind by an experiment.
set(MC_LIBRARY_SOURCES
    status.cpp
    ident.cpp
    digest.cpp
    time.cpp
    json.cpp
    scope.cpp
    facility.cpp
    approval.cpp
    obligation.cpp
    plan.cpp
    serialize.cpp
    store.cpp
    engine.cpp
    engine_gates.cpp
    engine_reports.cpp
)

add_library(maintenance_coordinator STATIC ${MC_LIBRARY_SOURCES})

add_library(SummonDCCP::MaintenanceCoordinator ALIAS maintenance_coordinator)

target_include_directories(maintenance_coordinator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

# The warning interface, the sanitizer runtime and the internal include path are
# build-tree concerns.  They are wrapped in BUILD_INTERFACE so that nothing
# non-exported leaks into the exported target and an out-of-tree consumer links
# only the library itself.
target_link_libraries(maintenance_coordinator PRIVATE $<BUILD_INTERFACE:mc_first_party_warnings>)
target_compile_features(maintenance_coordinator PUBLIC cxx_std_20)

set_target_properties(maintenance_coordinator PROPERTIES
    OUTPUT_NAME "maintenance_coordinator"
    # The exported name is the one the documentation and every consumer uses.
    # Without it the installed package would provide the internal target name
    # and SummonDCCP::MaintenanceCoordinator would only exist in this build tree.
    EXPORT_NAME "MaintenanceCoordinator"
    POSITION_INDEPENDENT_CODE ON)

if(TARGET mc_sanitizers)
    target_link_libraries(maintenance_coordinator PRIVATE $<BUILD_INTERFACE:mc_sanitizers>)
endif()

# The include directory reaches the exported interface through the
# INSTALL_INTERFACE generator expression above, so it is deliberately not
# repeated here: a duplicated directory is not wrong, but an exported interface
# should list each directory once.
install(TARGETS maintenance_coordinator
    EXPORT MaintenanceCoordinatorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

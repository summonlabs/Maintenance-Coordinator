# Downstream consumer of the installed Maintenance Coordinator package.
#
# This is an INDEPENDENT out-of-tree CMake project.  It has its own project()
# call and is never added to the main build: it is configured on its own,
# against an installed prefix, so it proves that the exported package is usable
# by a consumer that has no access to this repository's source or build tree.
#
# There is deliberately nothing here but the package contract:
#   * no include-path hacks,
#   * no relative reference back into the source tree,
#   * no installation rule (this project only consumes).

cmake_minimum_required(VERSION 3.20)

project(DownstreamConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The one and only route to the library.  CONFIG mode: the installed
# MaintenanceCoordinatorConfig.cmake is what describes the target, and it
# defines the contract target SummonDCCP::MaintenanceCoordinator.
find_package(MaintenanceCoordinator CONFIG REQUIRED)

if(NOT TARGET SummonDCCP::MaintenanceCoordinator)
    message(FATAL_ERROR
        "find_package(MaintenanceCoordinator) did not provide SummonDCCP::MaintenanceCoordinator")
endif()

add_executable(mc_downstream_consumer consumer.cpp)

target_link_libraries(mc_downstream_consumer PRIVATE SummonDCCP::MaintenanceCoordinator)

target_compile_features(mc_downstream_consumer PRIVATE cxx_std_20)

# The consumer holds itself to the same first-party strictness the library uses.
# These flags belong to this project; nothing is inherited from the package.
if(MSVC)
    target_compile_options(mc_downstream_consumer PRIVATE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /WX)
    target_compile_definitions(mc_downstream_consumer PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# No install() rule: a downstream consumer is not a redistributable.

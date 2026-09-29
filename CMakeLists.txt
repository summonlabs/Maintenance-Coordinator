cmake_minimum_required(VERSION 3.20)

project(MaintenanceCoordinator
    VERSION 1.0.0
    DESCRIPTION "DCCP physical fleet lifecycle - facility maintenance window coordination"
    HOMEPAGE_URL "https://github.com/summonlabs/Maintenance-Coordinator"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language and correctness baseline
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(MC_BUILD_TESTS "Build the Maintenance Coordinator test suite" ${BUILD_TESTING})
option(MC_BUILD_CLI "Build the maintenance-coordinator command line tool" ON)
option(MC_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(MC_ENABLE_ASAN "Enable AddressSanitizer for first-party targets" OFF)
option(MC_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for first-party targets (GCC/Clang)" OFF)
option(MC_BUILD_BENCHMARK "Build the completed-operation benchmark" OFF)

# ---------------------------------------------------------------------------
# First-party warning interface, applied to library, CLI and tests alike.
# ---------------------------------------------------------------------------

add_library(mc_first_party_warnings INTERFACE)
add_library(SummonDCCP::InternalFirstPartyWarnings ALIAS mc_first_party_warnings)

if(MSVC)
    target_compile_options(mc_first_party_warnings INTERFACE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /EHsc
        /MP)
    if(MC_WARNINGS_AS_ERRORS)
        target_compile_options(mc_first_party_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(mc_first_party_warnings INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(mc_first_party_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wold-style-cast
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wcast-qual
        -Wdouble-promotion
        -Wformat=2
        -Wundef)
    if(MC_WARNINGS_AS_ERRORS)
        target_compile_options(mc_first_party_warnings INTERFACE -Werror)
    endif()
endif()

# Sanitizers.  MSVC provides /fsanitize=address on x64; GCC and Clang provide
# the full set.  An unsupported combination is a hard error rather than a
# silent no-op, because a sanitizer run that quietly did nothing would be a
# false proof.
set(MC_ASAN_ACTIVE OFF)
set(MC_UBSAN_ACTIVE OFF)

if(MC_ENABLE_ASAN)
    if(MSVC)
        add_library(mc_sanitizers INTERFACE)
        target_compile_options(mc_sanitizers INTERFACE /fsanitize=address /Zi)
        target_link_options(mc_sanitizers INTERFACE /INCREMENTAL:NO)
        set(MC_ASAN_ACTIVE ON)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_library(mc_sanitizers INTERFACE)
        target_compile_options(mc_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(mc_sanitizers INTERFACE -fsanitize=address)
        set(MC_ASAN_ACTIVE ON)
    else()
        message(FATAL_ERROR "MC_ENABLE_ASAN is not supported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(MC_ENABLE_UBSAN)
    if(MSVC)
        message(FATAL_ERROR "MC_ENABLE_UBSAN requires a GCC or Clang toolchain; MSVC has no UBSan runtime.")
    endif()
    if(NOT TARGET mc_sanitizers)
        add_library(mc_sanitizers INTERFACE)
    endif()
    target_compile_options(mc_sanitizers INTERFACE
        -fsanitize=undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer)
    target_link_options(mc_sanitizers INTERFACE -fsanitize=undefined -fno-sanitize-recover=all)
    set(MC_UBSAN_ACTIVE ON)
endif()

add_subdirectory(src)

if(MC_BUILD_CLI)
    add_subdirectory(tools/maintenance-coordinator)
endif()

if(MC_BUILD_BENCHMARK)
    add_subdirectory(tools/mc-bench)
endif()

if(MC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export / package
# ---------------------------------------------------------------------------

install(EXPORT MaintenanceCoordinatorTargets
    FILE MaintenanceCoordinatorTargets.cmake
    NAMESPACE SummonDCCP::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MaintenanceCoordinator)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MaintenanceCoordinatorConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MaintenanceCoordinatorConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MaintenanceCoordinator
    NO_SET_AND_CHECK_MACRO)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MaintenanceCoordinatorConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MaintenanceCoordinatorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MaintenanceCoordinatorConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MaintenanceCoordinator)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/MaintenanceCoordinator)

# CPack source and binary packages, so the repository can be packaged with no
# external tooling.
set(CPACK_PACKAGE_NAME "MaintenanceCoordinator")
set(CPACK_PACKAGE_VENDOR "Summon Software Labs")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CONTACT "Summon Software Labs")
set(CPACK_SOURCE_GENERATOR "ZIP")
set(CPACK_SOURCE_IGNORE_FILES "/[.]git/;/build/;/[.]cache/;~$")
if(MSVC)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ;ZIP")
endif()
include(CPack)

message(STATUS "MaintenanceCoordinator ${PROJECT_VERSION}")
message(STATUS "  build type ............ ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler .............. ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  warnings as errors .... ${MC_WARNINGS_AS_ERRORS}")
message(STATUS "  address sanitizer ..... ${MC_ASAN_ACTIVE}")
message(STATUS "  undefined sanitizer ... ${MC_UBSAN_ACTIVE}")
message(STATUS "  tests ................. ${MC_BUILD_TESTS}")
message(STATUS "  cli ................... ${MC_BUILD_CLI}")
message(STATUS "  benchmark ............. ${MC_BUILD_BENCHMARK}")

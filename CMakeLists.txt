# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.12)
project(libeco VERSION 0.1.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported; create a build directory and run cmake from there")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "libeco requires Linux")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link with the C driver, without implicit C++ standard-library dependencies.
list(REMOVE_ITEM CMAKE_CXX_IMPLICIT_LINK_LIBRARIES stdc++ c++ c++abi)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

configure_file(include/eco/version.h.in generated/eco/version.h @ONLY)

add_library(eco_objects OBJECT
    src/coroutine.cpp
    src/scheduler.cpp
    src/io.cpp
    src/timer.cpp
    src/mutex.cpp
    src/cond.cpp
    src/process.cpp
)

set_target_properties(eco_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(eco_objects PRIVATE _GNU_SOURCE)
target_compile_options(eco_objects PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)

target_include_directories(eco_objects PRIVATE include src ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_library(eco_shared SHARED $<TARGET_OBJECTS:eco_objects>)
add_library(eco_static STATIC $<TARGET_OBJECTS:eco_objects>)
set_target_properties(eco_shared eco_static PROPERTIES LINKER_LANGUAGE C)

set_target_properties(eco_shared eco_static PROPERTIES OUTPUT_NAME eco)

set_target_properties(eco_shared PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    LINK_FLAGS "-Wl,-z,defs"
)

foreach(target eco_shared eco_static)
    target_compile_features(${target} PUBLIC cxx_std_20)

    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
endforeach()

option(ECO_BUILD_EXAMPLES "Build examples" ON)

if(ECO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS eco_shared eco_static EXPORT ecoTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/eco.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/${PROJECT_NAME})

install(FILES
    include/eco/coroutine.h
    include/eco/mutex.h
    include/eco/cond.h
    include/eco/io.h
    include/eco/process.h
    include/eco/time.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated/eco/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eco
)

install(EXPORT ecoTargets NAMESPACE eco:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eco)

configure_package_config_file(cmake/ecoConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ecoConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eco
)

write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ecoConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ecoConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ecoConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eco)

file(RELATIVE_PATH ECO_PC_PREFIX "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig"
    "${CMAKE_INSTALL_PREFIX}")
file(RELATIVE_PATH ECO_PC_INCLUDE "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig"
    "${CMAKE_INSTALL_FULL_INCLUDEDIR}")

configure_file(cmake/libeco.pc.in libeco.pc @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libeco.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

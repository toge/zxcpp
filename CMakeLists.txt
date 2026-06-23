cmake_minimum_required(VERSION 3.25)
project(zxcpp VERSION 0.0.1 LANGUAGES CXX)

option(BUILD_TEST "Enable tests" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

## add warning
if (CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g3")
endif()

if (MSVC)
    if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    endif()
endif()

## optimization
if (CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native")
endif()
if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Ox")
endif()

## other flag
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(zxc CONFIG REQUIRED)

add_library(zxcpp INTERFACE)
add_library(zxcpp::zxcpp ALIAS zxcpp)

target_include_directories(zxcpp
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(zxcpp INTERFACE zxc::zxc_lib)
target_compile_features(zxcpp INTERFACE cxx_std_23)

set(ZXCPP_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/zxcpp")

install(TARGETS zxcpp EXPORT zxcppTargets)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/zxcpp.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT zxcppTargets
        FILE zxcppTargets.cmake
        NAMESPACE zxcpp::
        DESTINATION ${ZXCPP_INSTALL_CMAKEDIR})

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zxcppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zxcppConfig.cmake"
    INSTALL_DESTINATION ${ZXCPP_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/zxcppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/zxcppConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/zxcppConfigVersion.cmake"
        DESTINATION ${ZXCPP_INSTALL_CMAKEDIR})

if(BUILD_TEST)
    enable_testing()
    add_subdirectory(test)
endif()

cmake_minimum_required(VERSION 3.21)

project(ModernCppConverter
    VERSION 1.2.0
    DESCRIPTION "Rule-based legacy C++ to modern C++ converter"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(ENABLE_CLANG_EXPERIMENTS "Build optional Clang-backed analysis experiments" OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)

if(ENABLE_CLANG_EXPERIMENTS)
    enable_language(C)
    find_package(LLVM CONFIG QUIET)
    if(NOT LLVM_FOUND)
        message(FATAL_ERROR "ENABLE_CLANG_EXPERIMENTS=ON requires LLVM development CMake packages. Install LLVM/Clang or set LLVM_DIR/CMAKE_PREFIX_PATH, or configure with ENABLE_CLANG_EXPERIMENTS=OFF.")
    endif()
    find_package(Clang CONFIG QUIET)
    if(NOT Clang_FOUND)
        message(FATAL_ERROR "ENABLE_CLANG_EXPERIMENTS=ON requires Clang development CMake packages. Install Clang tooling or set Clang_DIR/CMAKE_PREFIX_PATH, or configure with ENABLE_CLANG_EXPERIMENTS=OFF.")
    endif()
    message(STATUS "Clang experiments enabled with LLVM ${LLVM_PACKAGE_VERSION}")
endif()

get_property(MODERN_CPP_CONVERTER_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT MODERN_CPP_CONVERTER_IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(MODERNCPP_APP_VERSION "1.2.0-rc1" CACHE STRING "ModernCppConverter release version")
set(MODERNCPP_RELEASE_CHANNEL "rc" CACHE STRING "ModernCppConverter release channel")
string(TIMESTAMP MODERNCPP_BUILD_DATE "%Y-%m-%d" UTC)
find_package(Git QUIET)
set(MODERNCPP_GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE MODERNCPP_GIT_COMMIT_FROM_GIT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(MODERNCPP_GIT_COMMIT_FROM_GIT)
        set(MODERNCPP_GIT_COMMIT "${MODERNCPP_GIT_COMMIT_FROM_GIT}")
    endif()
endif()
if(ENABLE_CLANG_EXPERIMENTS)
    set(MODERNCPP_CLANG_ENABLED_VALUE 1)
else()
    set(MODERNCPP_CLANG_ENABLED_VALUE 0)
endif()

add_library(ModernCppConverterWarnings INTERFACE)
target_compile_options(ModernCppConverterWarnings
    INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

function(modern_cpp_converter_use_root_output target_name)
    set_target_properties(${target_name}
        PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
            ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
            LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
            ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
            LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
            ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
            LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
    )
endfunction()

include(src/CMakeLists.txt)

enable_testing()
add_subdirectory(tests)

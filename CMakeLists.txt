cmake_minimum_required(VERSION 3.21)

project(seamless-input
    VERSION 1.0.0
    DESCRIPTION "Seamless input between operating systems"
    LANGUAGES C CXX
)

# C++ configuration

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# FetchContent configuration

include(FetchContent)

set(FETCHCONTENT_BASE_DIR
    "${CMAKE_BINARY_DIR}/_deps"
    CACHE PATH "" FORCE
)

# Boost (Asio only)

set(BOOST_VERSION_TAG 1.86.0)

# Only build/configure asio and its transitive dependencies
set(BOOST_INCLUDE_LIBRARIES asio CACHE STRING "" FORCE)
set(BOOST_ENABLE_MPI OFF CACHE BOOL "" FORCE)
set(BOOST_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    Boost
    URL https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION_TAG}/boost-${BOOST_VERSION_TAG}-cmake.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# wxWidgets configuration

set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxBUILD_MONOLITHIC ON CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    wxWidgets
    GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
    GIT_TAG v3.2.8
    GIT_SHALLOW TRUE
)

# Fetch both dependencies
FetchContent_MakeAvailable(Boost wxWidgets)

# Resolve targets

if(NOT TARGET Boost::asio)
    message(FATAL_ERROR "Boost::asio target was not created.")
endif()

if(TARGET wx::mono)
    set(WX_TARGET wx::mono)
elseif(TARGET wxmono)
    set(WX_TARGET wxmono)
elseif(TARGET wxcore AND TARGET wxbase)
    set(WX_TARGET wxcore wxbase)
else()
    message(FATAL_ERROR
        "No wxWidgets target was created (looked for wx::mono, wxmono, wxcore+wxbase)."
    )
endif()

message(STATUS "Using wxWidgets target(s): ${WX_TARGET}")

find_package(Threads REQUIRED)

# Source discovery

file(GLOB_RECURSE PROJECT_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

file(GLOB_RECURSE PROJECT_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
)

if(NOT PROJECT_SOURCES)
    message(FATAL_ERROR
        "No source files found in src/"
    )
endif()

# Application target

add_executable(${PROJECT_NAME}
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Linking

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${WX_TARGET}
        Boost::asio
        Threads::Threads
)

# Windows configuration

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    # Target Windows 10+ for Asio
    target_compile_definitions(${PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0A00)
endif()

# Compiler warnings

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
    )
else()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Status information

message(STATUS "====================================")
message(STATUS "Project:       ${PROJECT_NAME}")
message(STATUS "Version:       ${PROJECT_VERSION}")
message(STATUS "C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "Build Dir:     ${CMAKE_BINARY_DIR}")
message(STATUS "wxWidgets:     3.2.8 (${WX_TARGET})")
message(STATUS "Boost:         ${BOOST_VERSION_TAG} (asio)")
message(STATUS "Sources:       ${PROJECT_SOURCES}")
message(STATUS "====================================")

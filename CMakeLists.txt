cmake_minimum_required(VERSION 3.21)

project(seamless-input
    VERSION 1.0.0
    DESCRIPTION "Seamless input sharing across LAN"
    LANGUAGES C CXX
)

# --------------------------------------------------
# C++ configuration
# --------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --------------------------------------------------
# miniaudio (single header, lives in src/miniaudio/)
# --------------------------------------------------

set(MINIAUDIO_VERSION 0.11.21)
set(MINIAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/miniaudio")

if(NOT EXISTS "${MINIAUDIO_DIR}/miniaudio.h")
    message(STATUS "Downloading miniaudio ${MINIAUDIO_VERSION} into src/miniaudio/ ...")
    file(MAKE_DIRECTORY "${MINIAUDIO_DIR}")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VERSION}/miniaudio.h"
        "${MINIAUDIO_DIR}/miniaudio.h"
        STATUS _ma_status
        TLS_VERIFY ON
    )
    list(GET _ma_status 0 _ma_code)
    if(NOT _ma_code EQUAL 0)
        file(REMOVE "${MINIAUDIO_DIR}/miniaudio.h")
        message(FATAL_ERROR
            "Could not download miniaudio.h (${_ma_status}). "
            "Download it manually to ${MINIAUDIO_DIR}/miniaudio.h")
    endif()
endif()

# --------------------------------------------------
# Dependencies: downloaded by default; -DSI_SYSTEM_DEPS=ON uses the system's Boost headers and
# wxWidgets instead (faster dev builds: apt install libwxgtk3.2-dev libboost-dev)
# --------------------------------------------------

option(SI_SYSTEM_DEPS "Use system Boost/wxWidgets instead of downloading them" OFF)

include(FetchContent)

set(FETCHCONTENT_BASE_DIR
    "${CMAKE_BINARY_DIR}/_deps"
    CACHE PATH "" FORCE
)

if(SI_SYSTEM_DEPS)
    find_package(Boost REQUIRED)
    add_library(Boost::asio INTERFACE IMPORTED)
    target_link_libraries(Boost::asio INTERFACE Boost::headers)
    find_package(wxWidgets REQUIRED COMPONENTS core base)
    add_library(wx_system INTERFACE)
    target_include_directories(wx_system INTERFACE ${wxWidgets_INCLUDE_DIRS})
    target_compile_definitions(wx_system INTERFACE ${wxWidgets_DEFINITIONS})
    target_link_libraries(wx_system INTERFACE ${wxWidgets_LIBRARIES})
    set(WX_TARGET wx_system)
else()
# --------------------------------------------------
    # Boost (Asio only)
    # --------------------------------------------------
    
    set(BOOST_VERSION_TAG 1.86.0)
    
    set(BOOST_INCLUDE_LIBRARIES asio CACHE STRING "" FORCE)
    set(BOOST_ENABLE_MPI OFF CACHE BOOL "" FORCE)
    set(BOOST_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
    
    FetchContent_Declare(
        Boost
        URL https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION_TAG}/boost-${BOOST_VERSION_TAG}-cmake.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    
    # --------------------------------------------------
    # wxWidgets configuration
    # --------------------------------------------------
    
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
    
    FetchContent_MakeAvailable(Boost wxWidgets)
    
    # --------------------------------------------------
    # Resolve targets
    # --------------------------------------------------
    
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
endif()

find_package(Threads REQUIRED)

# --------------------------------------------------
# Source discovery
# --------------------------------------------------

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
    message(FATAL_ERROR "No source files found in src/")
endif()

# miniaudio's implementation is third-party code: no warnings for that file
set_source_files_properties("${MINIAUDIO_DIR}/miniaudio_impl.cpp" PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>"
)

# Monocypher (Ed25519 machine identity, vendored in src/monocypher/) is third-party C code: no warnings either
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/monocypher/monocypher.c" PROPERTIES
    COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>"
)

# --------------------------------------------------
# Targets
#   si_core                  engine: network, input, audio, identity, Core, IPC, service management
#   seamless-input-daemon    headless background process (no wxWidgets)
#   seamless-input           the window; talks to the daemon over local IPC
# --------------------------------------------------

set(GUI_MAIN    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
set(DAEMON_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/daemon_main.cpp")
set(CORE_SOURCES ${PROJECT_SOURCES})
list(REMOVE_ITEM CORE_SOURCES "${GUI_MAIN}" "${DAEMON_MAIN}")

add_library(si_core STATIC ${CORE_SOURCES} ${PROJECT_HEADERS})

target_include_directories(si_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: keeps miniaudio.h's own warnings out of our build output
target_include_directories(si_core SYSTEM PUBLIC "${MINIAUDIO_DIR}")
target_compile_definitions(si_core PUBLIC SI_VERSION="${PROJECT_VERSION}")
target_link_libraries(si_core PUBLIC Boost::asio Threads::Threads)

# miniaudio loads audio backends at runtime (dlopen) on Linux/BSD
if(UNIX AND NOT APPLE)
    target_link_libraries(si_core PUBLIC ${CMAKE_DL_LIBS} m)
endif()

# --------------------------------------------------
# Keyboard / mouse sharing backends
#   Windows : SetWindowsHookEx + SendInput   (user32, nothing to install)
#   X11     : XInput2 + XGrab + XTest        (libX11, libXi, libXtst - optional)
#   Wayland : evdev + uinput                 (kernel headers only)
# --------------------------------------------------

if(UNIX AND NOT APPLE)
    find_package(X11)
    if(TARGET X11::X11 AND TARGET X11::Xi AND TARGET X11::Xtst)
        target_link_libraries(si_core PUBLIC X11::X11 X11::Xi X11::Xtst)
        target_compile_definitions(si_core PUBLIC SI_HAVE_X11=1)
        set(INPUT_X11_STATUS "yes")
        # optional: lets the daemon read the monitor layout without a window
        if(TARGET X11::Xrandr)
            target_link_libraries(si_core PUBLIC X11::Xrandr)
            target_compile_definitions(si_core PUBLIC SI_HAVE_XRANDR=1)
        else()
            message(STATUS "libXrandr not found: the daemon will use the whole X screen as one monitor")
        endif()
    else()
        message(WARNING "libX11/libXi/libXtst development files not found: building WITHOUT the X11 input backend (install libxtst-dev and libxi-dev)")
        set(INPUT_X11_STATUS "no")
    endif()
endif()

if(WIN32)
    target_compile_definitions(si_core PUBLIC _WIN32_WINNT=0x0A00)
    target_link_libraries(si_core PUBLIC ws2_32 mswsock iphlpapi dwmapi uxtheme advapi32 wtsapi32 userenv shell32 ole32 uuid)
endif()

# --- daemon (console subsystem: run it in a terminal to see its log; always started hidden otherwise)
add_executable(seamless-input-daemon "${DAEMON_MAIN}")
target_link_libraries(seamless-input-daemon PRIVATE si_core)

# --- GUI
add_executable(${PROJECT_NAME} "${GUI_MAIN}")
target_link_libraries(${PROJECT_NAME} PRIVATE si_core ${WX_TARGET})
add_dependencies(${PROJECT_NAME} seamless-input-daemon)   # the GUI starts the daemon: build both

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# DPI awareness: without it Windows reports scaled (virtualized) monitor sizes
if(MSVC)
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
    target_sources(seamless-input-daemon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
endif()

install(TARGETS ${PROJECT_NAME} seamless-input-daemon RUNTIME DESTINATION bin)

# --------------------------------------------------
# Compiler warnings
# --------------------------------------------------

foreach(_t si_core seamless-input-daemon ${PROJECT_NAME})
    if(MSVC)
        target_compile_options(${_t} PRIVATE /W4)
    else()
        target_compile_options(${_t} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# --------------------------------------------------
# Status information
# --------------------------------------------------

message(STATUS "====================================")
message(STATUS "Project:       ${PROJECT_NAME}")
message(STATUS "Version:       ${PROJECT_VERSION}")
message(STATUS "C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "wxWidgets:     3.2.8 (${WX_TARGET})")
message(STATUS "Boost:         ${BOOST_VERSION_TAG} (asio)")
message(STATUS "miniaudio:     ${MINIAUDIO_VERSION}")
if(UNIX AND NOT APPLE)
    message(STATUS "Input:         evdev/uinput (Wayland), X11 backend: ${INPUT_X11_STATUS}")
endif()
message(STATUS "====================================")

#-------------------------------------------------------------
# CMakeLists.txt
#
# Builds UmbraOpenRGBPlugin - an OpenRGB Plugin API v4
# plugin adding support for the AsiaHorse UMBRA ARGB Hub
#
# Two independent build options:
#   UMBRA_BUILD_PLUGIN  (default ON)  the OpenRGB plugin DLL.
#                                     Requires OPENRGB_SOURCE_DIR, Qt and hidapi.
#   UMBRA_BUILD_TESTS   (default ON)  pure protocol unit tests.
#                                     Standalone: no OpenRGB/Qt/hidapi needed.
#
# Protocol tests only:
#   cmake -B build && cmake --build build && ctest --test-dir build
#
# This file is part of the UmbraOpenRGBPlugin project.
# SPDX-License-Identifier: GPL-2.0-or-later
#-------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

project(UmbraOpenRGBPlugin VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

set(UMBRA_PLUGIN_VERSION ${PROJECT_VERSION})

option(UMBRA_BUILD_PLUGIN "Build the OpenRGB plugin"          ON)
option(UMBRA_BUILD_TESTS  "Build the UmbraProtocol unit tests" ON)

#-------------------------------------------------------------
# Unit tests for the pure protocol layer.
# No hardware, no Qt, no hidapi, no OpenRGB required.
#-------------------------------------------------------------
if(UMBRA_BUILD_TESTS)
    enable_testing()

    add_executable(umbra_protocol_test
        tests/umbra_protocol_test.cpp
        src/UmbraProtocol.cpp
    )

    target_include_directories(umbra_protocol_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    add_test(NAME umbra_protocol COMMAND umbra_protocol_test)
endif()

if(NOT UMBRA_BUILD_PLUGIN)
    message(STATUS "UmbraOpenRGBPlugin: building protocol tests only")
    return()
endif()

#-------------------------------------------------------------
# OpenRGB source tree
#
# The plugin compiles against the headers of the exact OpenRGB
# version it will be loaded by, and links in the framework's
# RGBController.cpp (those symbols are not exported from the
# OpenRGB executable to plugins). Point this at your OpenRGB
# source checkout matching your OpenRGB build (e.g. tag
# release_candidate_1.0rc3.1).
#
#   cmake -DOPENRGB_SOURCE_DIR=D:/src/OpenRGB ..
#-------------------------------------------------------------
set(OPENRGB_SOURCE_DIR "" CACHE PATH "Path to the OpenRGB source tree matching your OpenRGB build")

if(NOT EXISTS "${OPENRGB_SOURCE_DIR}/RGBController/RGBController.cpp")
    message(FATAL_ERROR
        "OPENRGB_SOURCE_DIR is not set or does not point to an OpenRGB source tree.\n"
        "Set it to a checkout of the same OpenRGB version you are building/running,\n"
        "e.g.:  cmake -DOPENRGB_SOURCE_DIR=/path/to/OpenRGB ..\n"
        "(or configure with -DUMBRA_BUILD_PLUGIN=OFF to build only the protocol tests)")
endif()

#-------------------------------------------------------------
# Qt
#
# OpenRGB 1.0rc3 ships Qt5 and Qt objects (QWidget/QMenu/plugin
# metadata) cross the DLL boundary, so the plugin MUST use the
# same Qt major version as the host application. Qt5 is forced
# unless you explicitly opt into Qt6 for a host built with Qt6.
#-------------------------------------------------------------
option(UMBRA_PLUGIN_FORCE_QT6 "Link against Qt6 instead of Qt5 (only for OpenRGB hosts built with Qt6)" OFF)

if(UMBRA_PLUGIN_FORCE_QT6)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    set(UMBRA_QT_MAJOR 6)
else()
    find_package(Qt5 REQUIRED COMPONENTS Widgets)
    set(UMBRA_QT_MAJOR 5)
endif()

message(STATUS "Using Qt${UMBRA_QT_MAJOR}")

#-------------------------------------------------------------
# hidapi
#
# By default hidapi is fetched from upstream and statically
# linked into the plugin DLL so nothing extra needs to be
# deployed next to it.
#-------------------------------------------------------------
option(USE_SYSTEM_HIDAPI "Link against a system-installed hidapi instead of fetching one" OFF)

if(USE_SYSTEM_HIDAPI)
    find_package(hidapi REQUIRED)
    set(HIDAPI_TARGET hidapi::hidapi)
else()
    #---------------------------------------------------------
    # Statically link hidapi into the plugin DLL so nothing
    # extra has to be deployed next to it
    #---------------------------------------------------------
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    include(FetchContent)
    FetchContent_Declare(
        hidapi
        GIT_REPOSITORY https://github.com/libusb/hidapi.git
        GIT_TAG        hidapi-0.14.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(hidapi)

    if(TARGET hidapi::hidapi)
        set(HIDAPI_TARGET hidapi::hidapi)
    elseif(TARGET hidapi_winapi)
        set(HIDAPI_TARGET hidapi_winapi)
    elseif(TARGET hidapi-hidraw)
        set(HIDAPI_TARGET hidapi-hidraw)
    elseif(TARGET hidapi-libusb)
        set(HIDAPI_TARGET hidapi-libusb)
    else()
        message(FATAL_ERROR "hidapi target not found after FetchContent")
    endif()
endif()

message(STATUS "Using hidapi target: ${HIDAPI_TARGET}")

#-------------------------------------------------------------
# Plugin sources
#-------------------------------------------------------------
set(UMBRA_PLUGIN_SOURCES
    src/UmbraPlugin.h
    src/UmbraPlugin.cpp
    src/UmbraWidget.h
    src/UmbraWidget.cpp
    src/UmbraProtocol.h
    src/UmbraProtocol.cpp
    src/UmbraController.h
    src/UmbraController.cpp
    src/RGBController_Umbra.h
    src/RGBController_Umbra.cpp

    #---------------------------------------------------------
    # OpenRGB framework sources compiled into the plugin.
    # Plugins cannot resolve these symbols from the OpenRGB
    # binary, so each plugin carries its own copy of the base
    # class implementation. It must come from the same source
    # tree/version as the host application.
    #---------------------------------------------------------
    "${OPENRGB_SOURCE_DIR}/RGBController/RGBController.cpp"
)

add_library(UmbraOpenRGBPlugin SHARED ${UMBRA_PLUGIN_SOURCES})

target_include_directories(UmbraOpenRGBPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${OPENRGB_SOURCE_DIR}"
    "${OPENRGB_SOURCE_DIR}/RGBController"
    "${OPENRGB_SOURCE_DIR}/i2c_smbus"
)

target_compile_definitions(UmbraOpenRGBPlugin PRIVATE
    UMBRA_PLUGIN_VERSION="${UMBRA_PLUGIN_VERSION}"
)

target_link_libraries(UmbraOpenRGBPlugin PRIVATE
    Qt${UMBRA_QT_MAJOR}::Widgets
    ${HIDAPI_TARGET}
)

#-------------------------------------------------------------
# Toolchain notes
#
# STL objects (std::string/std::vector) cross the plugin API
# boundary in both directions, which makes it a C++ ABI
# boundary. Build the plugin with the SAME toolchain that was
# used for your OpenRGB binary:
#
#   Official Windows releases (MSYS2 MinGW64 GCC):
#     Those packages ship NO libstdc++-6.dll/libgcc_s/libwinpthread
#     next to OpenRGB.exe - the host itself links them statically.
#     This build mirrors that exactly: libstdc++, libgcc and
#     winpthread are statically linked into the plugin as well,
#     so the only runtime dependencies are system DLLs and the
#     Qt libraries already present in the host folder.
#     Cross-module new/delete still routes through the shared
#     CRT allocator (msvcrt/ucrtbase), so by-value std::string
#     handover stays safe even with two embedded libstdc++
#     copies in one process.
#
#   Self-built OpenRGB:
#     Use the exact compiler/CRT combination of that build.
#     If it links dynamic runtimes, remove the -static flags
#     below so both modules share one libstdc++ instance.
#-------------------------------------------------------------
if(MINGW)
    target_link_options(UmbraOpenRGBPlugin PRIVATE
        -static-libgcc
        -static-libstdc++
    )
    #---------------------------------------------------------
    # Pull every object from the static winpthreads archive.
    # References can originate in libstdc++/libgcc archives
    # processed after ours; whole-archive guarantees their
    # definitions are already present, so the driver's
    # trailing dynamic -lwinpthread contributes no import.
    #---------------------------------------------------------
    target_link_libraries(UmbraOpenRGBPlugin PRIVATE
        "-Wl,-Bstatic,--whole-archive"
        "-lwinpthread"
        "-Wl,--no-whole-archive,-Bdynamic"
    )
endif()

#-------------------------------------------------------------
# Optional deployment helper:
#   cmake -DCOPY_PLUGIN_TO=... .
# copies the built plugin into an existing plugins directory
#-------------------------------------------------------------
set(COPY_PLUGIN_TO "" CACHE PATH "Optional directory to copy the built plugin into")

if(COPY_PLUGIN_TO)
    add_custom_command(TARGET UmbraOpenRGBPlugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:UmbraOpenRGBPlugin> "${COPY_PLUGIN_TO}"
        COMMENT "Copying plugin to ${COPY_PLUGIN_TO}")
endif()

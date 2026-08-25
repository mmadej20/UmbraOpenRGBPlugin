#-------------------------------------------------------------
# CMakeLists.txt
#
# Builds UmbraOpenRGBPlugin - an OpenRGB Plugin API v4
# plugin adding support for the AsiaHorse UMBRA ARGB Hub
#
# This file is part of the UmbraOpenRGBPlugin project.
# SPDX-License-Identifier: GPL-2.0-or-later
#-------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

project(UmbraOpenRGBPlugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

set(UMBRA_PLUGIN_VERSION ${PROJECT_VERSION})

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
        "e.g.:  cmake -DOPENRGB_SOURCE_DIR=/path/to/OpenRGB ..")
endif()

#-------------------------------------------------------------
# Qt - OpenRGB 1.0rc3 uses Qt5; Qt6 is supported as well
#-------------------------------------------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

message(STATUS "Using Qt${QT_VERSION_MAJOR}")

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
    "${OPENRGB_SOURCE_DIR}/i2c_smbus"
)

target_compile_definitions(UmbraOpenRGBPlugin PRIVATE
    UMBRA_PLUGIN_VERSION="${UMBRA_PLUGIN_VERSION}"
)

target_link_libraries(UmbraOpenRGBPlugin PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    ${HIDAPI_TARGET}
)

#-------------------------------------------------------------
# Toolchain notes
#
# The plugin exchanges STL objects (std::string/std::vector)
# with the host across the API boundary. Build it with the
# SAME toolchain family as your OpenRGB build:
#   - official Windows releases / MSYS2 builds -> MinGW64 GCC
#     (do NOT pass -static-libstdc++/-static-libgcc, both
#     binaries must share libstdc++-6.dll and its heap)
#   - self-built MSVC OpenRGB                  -> same MSVC toolset
#-------------------------------------------------------------

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

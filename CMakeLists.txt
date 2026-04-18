cmake_minimum_required(VERSION 3.16...4.0)
project(qarma VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Prefer Qt6, fall back to Qt5
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
if(NOT Qt6_FOUND)
    find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
    set(QT_VERSION_MAJOR 5)
else()
    set(QT_VERSION_MAJOR 6)
endif()

# Optional D-Bus support
option(QARMA_DISABLE_DBUS "Build without D-Bus support" OFF)

if(NOT QARMA_DISABLE_DBUS)
    if(QT_VERSION_MAJOR EQUAL 6)
        find_package(Qt6 REQUIRED COMPONENTS DBus)
    else()
        find_package(Qt5 REQUIRED COMPONENTS DBus)
    endif()
endif()

# Qt5-only: x11extras on Linux
if(QT_VERSION_MAJOR EQUAL 5 AND UNIX AND NOT APPLE)
    find_package(Qt5 REQUIRED COMPONENTS X11Extras)
endif()

add_executable(qarma
    Qarma.cpp
    Qarma.h
)

# Core Qt link
if(QT_VERSION_MAJOR EQUAL 6)
    target_link_libraries(qarma PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
else()
    target_link_libraries(qarma PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
endif()

# D-Bus
if(NOT QARMA_DISABLE_DBUS)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(qarma PRIVATE Qt6::DBus)
    else()
        target_link_libraries(qarma PRIVATE Qt5::DBus)
    endif()
else()
    target_compile_definitions(qarma PRIVATE QARMA_NO_DBUS)
endif()

# Linux/Unix-specific: X11
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(qarma PRIVATE ${X11_LIBRARIES})
    target_compile_definitions(qarma PRIVATE WS_X11)

    if(QT_VERSION_MAJOR EQUAL 5)
        target_link_libraries(qarma PRIVATE Qt5::X11Extras)
    endif()
endif()

# Install
include(GNUInstallDirs)
install(TARGETS qarma DESTINATION ${CMAKE_INSTALL_BINDIR})

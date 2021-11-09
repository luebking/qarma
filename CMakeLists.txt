project(Qarma)

cmake_minimum_required(VERSION 2.8.12 FATAL_ERROR)

set(CMAKE_AUTOMOC TRUE)

include(GNUInstallDirs)

find_package(Katie)
if(NOT KATIE_FOUND)
    find_package(Qt4)
endif()

add_definitions(${QT_DEFINITIONS})
include_directories(${QT_INCLUDES})

set(QARMA_SOURCES
    Qarma.cpp
    Qarma.h
)

add_executable(qarma ${QARMA_SOURCES})

target_link_libraries(qarma ${QT_QTCORE_LIBRARY} ${QT_QTGUI_LIBRARY} ${QT_QTDBUS_LIBRARY})

install(
    TARGETS qarma
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
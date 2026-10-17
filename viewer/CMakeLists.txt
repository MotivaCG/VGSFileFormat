# SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
# VGS Viewer is supplied under the VGS Decoder licence (../decoder/LICENSE.md).

cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/viewerversion.h" VGS_VIEWER_VERSION_LINE
    REGEX "#define[ \t]+VIEWER_VERSION_NAME")
string(REGEX MATCH "\"v([0-9]+\\.[0-9]+\\.[0-9]+)\"" VGS_VIEWER_VERSION_MATCH "${VGS_VIEWER_VERSION_LINE}")
if(NOT VGS_VIEWER_VERSION_MATCH)
    message(FATAL_ERROR "VIEWER_VERSION_NAME must contain vMAJOR.MINOR.PATCH in viewerversion.h")
endif()
set(VGS_VIEWER_VERSION "${CMAKE_MATCH_1}")
project(VGSViewer VERSION ${VGS_VIEWER_VERSION} LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/viewerversion.h")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Multimedia)
set(VGS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "VGSFileFormat sources")
set(VGS_EDITOR_DIR "${VGS_ROOT}/editor")

# Compile only the reader. Neither the authoring code nor its signing key is linked.
add_library(viewer_decoder STATIC
    ${VGS_ROOT}/decoder/src/vgsdecoder.cpp
    ${VGS_ROOT}/core/src/vgscontainer.cpp
    ${VGS_ROOT}/core/src/vgsframe.cpp
    ${VGS_ROOT}/core/src/mgscodec.cpp
    ${VGS_ROOT}/core/crypto/vgscrypto.cpp
    ${VGS_ROOT}/core/crypto/tweetnacl.c)
target_include_directories(viewer_decoder PUBLIC ${VGS_ROOT}/decoder/include PRIVATE
    ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto ${VGS_ROOT}/core/src)

# Share the display implementation with the editor without linking editor_support.
set(VGS_VIEWER_UI_SOURCES editortheme.cpp displayscaling.cpp viewport.cpp viewcube.cpp
    rangeslider.cpp audiopreview.cpp captureworker.cpp previewmemory.cpp isolation.cpp loopmorph.cpp pruning.cpp project.cpp
    modifiers.cpp capturesettings.cpp dependencies/mint/mintfile.cpp
    dependencies/mint/mintskinrecovery.cpp)
list(TRANSFORM VGS_VIEWER_UI_SOURCES PREPEND "${VGS_EDITOR_DIR}/")
qt_add_executable(VGSViewer WIN32 viewermain.cpp viewerwindow.cpp viewerwindow.h
    viewerversion.h ${VGS_VIEWER_UI_SOURCES} ${VGS_EDITOR_DIR}/resources.qrc)
target_compile_definitions(VGSViewer PRIVATE VGS_VIEWER)
target_include_directories(VGSViewer PRIVATE . ${VGS_EDITOR_DIR}
    ${VGS_EDITOR_DIR}/dependencies/mint ${VGS_EDITOR_DIR}/dependencies/eigen
    ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto ${VGS_ROOT}/core/src)
target_link_libraries(VGSViewer PRIVATE viewer_decoder Qt6::Widgets Qt6::OpenGLWidgets
    Qt6::OpenGL Qt6::Multimedia)
if(WIN32)
    string(REPLACE "." "," VGS_VIEWER_VERSION_COMMAS "${VGS_VIEWER_VERSION},0")
    configure_file(appicon.rc.in ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc @ONLY)
    target_sources(VGSViewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc)
    target_link_libraries(VGSViewer PRIVATE user32)
endif()
if(MSVC)
    target_compile_options(viewer_decoder PRIVATE /bigobj /utf-8)
    target_compile_options(VGSViewer PRIVATE /utf-8)
endif()

option(VIEWER_BUILD_TESTS "Check that the viewer contains no signing key" ON)
if(VIEWER_BUILD_TESTS)
    enable_testing()
    add_executable(viewer_keyleak ${VGS_ROOT}/tests/keyleak.cpp)
    target_include_directories(viewer_keyleak PRIVATE ${VGS_ROOT}/core/crypto)
    add_test(NAME keyleak_viewer COMMAND viewer_keyleak --absent $<TARGET_FILE:VGSViewer>)
endif()

# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
#
# Proprietary software owned by Víctor M. Feliz.
# ScanMeNow and The4DScanner are licensed for internal use only.
# No ownership, sale, redistribution, sublicensing or modification rights
# are granted. All other rights remain reserved to the copyright holder.
# See LICENSE.md for the limited use grant and applicable terms.
# Other uses require prior written authorisation, subject to mandatory law.

cmake_minimum_required(VERSION 3.21)
project(VGSEditor VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Multimedia)
set(VGS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "VGSFileFormat sources")
add_library(editor_decoder STATIC
    ${VGS_ROOT}/decoder/src/vgsdecoder.cpp
    ${VGS_ROOT}/core/src/vgscontainer.cpp
    ${VGS_ROOT}/core/src/vgsframe.cpp
    ${VGS_ROOT}/core/src/mgscodec.cpp
    ${VGS_ROOT}/core/crypto/vgscrypto.cpp
    ${VGS_ROOT}/core/crypto/tweetnacl.c)
target_include_directories(editor_decoder PUBLIC ${VGS_ROOT}/decoder/include PRIVATE
    ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto ${VGS_ROOT}/core/src)
add_library(editor_authoring STATIC ${VGS_ROOT}/core/src/vgsencode.cpp ${VGS_ROOT}/core/crypto/vgssign.cpp)
target_compile_definitions(editor_authoring PRIVATE VGS_AUTHORING)
target_include_directories(editor_authoring PUBLIC ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto PRIVATE ${VGS_ROOT}/core/src)
target_link_libraries(editor_authoring PUBLIC editor_decoder)
add_library(editor_support STATIC editortheme.cpp displayscaling.cpp isolation.cpp pruning.cpp audiomix.cpp mintwriter.cpp modifiers.cpp nativeexport.cpp exportcapture.cpp exporttask.cpp dependencies/mint/mintskinrecovery.cpp project.cpp capturesettings.cpp captureworker.cpp filehistory.cpp presetstore.cpp dependencies/mint/mintfile.cpp)
target_include_directories(editor_support PUBLIC . dependencies/mint dependencies/eigen)
target_link_libraries(editor_support PUBLIC Qt6::Widgets editor_decoder editor_authoring)
if(WIN32)
    target_link_libraries(editor_support PRIVATE user32)
endif()
qt_add_executable(VGSEditor main.cpp audiopreview.cpp audiopreview.h animationpanel.cpp animationpanel.h modifierpanel.cpp modifierpanel.h taskqueuedialog.cpp taskqueuedialog.h mainwindow.cpp viewport.cpp viewcube.cpp capturesettingsdialog.cpp mainwindow.h viewport.h viewcube.h capturesettingsdialog.h rangeslider.cpp rangeslider.h resources.qrc)
if(WIN32)
    # The version is set once, in licensemanagement.h ("v1.0.0").
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/licensemanagement.h VGS_EDITOR_VERSION_LINE REGEX "#define[ \t]+VERSION_NAME")
    string(REGEX REPLACE ".*\"v([0-9.]+)\".*" "\\1" VGS_EDITOR_VERSION "${VGS_EDITOR_VERSION_LINE}")
    string(REPLACE "." "," VGS_EDITOR_VERSION_COMMAS "${VGS_EDITOR_VERSION},0")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/licensemanagement.h)
    configure_file(appicon.rc.in ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc @ONLY)
    target_sources(VGSEditor PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc)
endif()
target_link_libraries(VGSEditor PRIVATE editor_support Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Multimedia)
if(MSVC)
    target_compile_options(editor_decoder PRIVATE /bigobj)
    target_compile_options(editor_support PRIVATE /utf-8)
    target_compile_options(VGSEditor PRIVATE /utf-8)
endif()
# Beside the program: the built-in presets, and the ffmpeg exports mix and cut the
# soundtrack with (tools/ffmpeg.exe). ffmpeg is not in the repository; point VGS_FFMPEG at
# one. Without it exports still work, keeping one audio track as delivered.
set(VGS_FFMPEG "D:/Trabajos/THE4DSCANNER/GraciaConverter/Gracia4DGSConverter/build/Desktop_Qt_6_8_0_MSVC2022_64bit-Release/release/tools/ffmpeg.exe" CACHE FILEPATH "ffmpeg copied beside the editor")
add_custom_command(TARGET VGSEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/presets $<TARGET_FILE_DIR:VGSEditor>/presets)
if(EXISTS "${VGS_FFMPEG}")
    add_custom_command(TARGET VGSEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:VGSEditor>/tools
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VGS_FFMPEG} $<TARGET_FILE_DIR:VGSEditor>/tools/)
else()
    message(WARNING "VGS_FFMPEG not found (${VGS_FFMPEG}): exports will not mix or cut audio.")
endif()
# VGS Viewer: opens and plays .vgs/.pgs, nothing else. It compiles the shared display sources
# itself and links the decoder only - never editor_support or editor_authoring, which carry
# the encoder and the signing key (the keyleak_viewer check in tests/ guards that).
set(VGS_UI_SOURCES editortheme.cpp displayscaling.cpp viewport.cpp viewcube.cpp rangeslider.cpp audiopreview.cpp
    captureworker.cpp isolation.cpp pruning.cpp project.cpp modifiers.cpp capturesettings.cpp
    dependencies/mint/mintfile.cpp dependencies/mint/mintskinrecovery.cpp)
qt_add_executable(VGSViewer WIN32 viewer/viewermain.cpp viewer/viewerwindow.cpp viewer/viewerwindow.h viewer/viewerversion.h ${VGS_UI_SOURCES} resources.qrc)
target_compile_definitions(VGSViewer PRIVATE VGS_VIEWER)
target_include_directories(VGSViewer PRIVATE . viewer dependencies/mint dependencies/eigen ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto ${VGS_ROOT}/core/src)
target_link_libraries(VGSViewer PRIVATE editor_decoder Qt6::Widgets Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Multimedia)
if(WIN32)
    target_sources(VGSViewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc)
    target_link_libraries(VGSViewer PRIVATE user32)
endif()
if(MSVC)
    target_compile_options(VGSViewer PRIVATE /utf-8)
endif()
option(EDITOR_BUILD_TESTS "Build project and decoder integration tests" ON)
if(EDITOR_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(mainwindow_tests tests/mainwindow_tests.cpp audiopreview.cpp animationpanel.cpp animationpanel.h modifierpanel.cpp modifierpanel.h taskqueuedialog.cpp taskqueuedialog.h mainwindow.cpp viewport.cpp viewcube.cpp capturesettingsdialog.cpp rangeslider.cpp resources.qrc)
    target_link_libraries(mainwindow_tests PRIVATE editor_support Qt6::Test Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Multimedia)
    if(MSVC)
        target_compile_options(mainwindow_tests PRIVATE /utf-8)
    endif()
    add_test(NAME mainwindow_tests COMMAND mainwindow_tests)
    qt_add_executable(export_tests tests/export_tests.cpp rangeslider.cpp rangeslider.h)
    target_link_libraries(export_tests PRIVATE editor_support Qt6::Test)
    add_test(NAME export_tests COMMAND export_tests)
    qt_add_executable(editor_tests tests/editor_tests.cpp)
    target_link_libraries(editor_tests PRIVATE editor_support Qt6::Test)
    add_test(NAME editor_tests COMMAND editor_tests)
    qt_add_executable(viewport_tests tests/viewport_tests.cpp viewport.cpp viewport.h viewcube.cpp viewcube.h)
    target_link_libraries(viewport_tests PRIVATE editor_support Qt6::Test Qt6::OpenGLWidgets Qt6::OpenGL)
    if(MSVC)
        target_compile_options(viewport_tests PRIVATE /utf-8)
    endif()
    add_test(NAME viewport_tests COMMAND viewport_tests)
endif()

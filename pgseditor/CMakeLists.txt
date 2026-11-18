# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
# PGS Editor is supplied under the VGS Editor licence (../editor/LICENSE.md).
#
# PGS Editor: static captures (one 3DGS .ply): normals, a mesh made with gs2mesh, lights, and
# .ply / .pgs export. It compiles the VGS editor's sources it shares (viewport, export, normals)
# from ../editor, as the viewer does, and loads gs2mesh (../editor/dependencies/gs2mesh_cpp, a
# submodule) at run time.
#
#   cmake -S . -B build/cmake-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/Qt6.8.1/6.8.0/msvc2022_64
#   cmake --build build/cmake-msvc --config Release

cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/pgsversion.h" PGS_EDITOR_VERSION_LINE REGEX "#define[ \t]+PGS_EDITOR_VERSION_NAME")
string(REGEX MATCH "\"v([0-9]+\\.[0-9]+\\.[0-9]+)\"" PGS_EDITOR_VERSION_MATCH "${PGS_EDITOR_VERSION_LINE}")
if(NOT PGS_EDITOR_VERSION_MATCH)
    message(FATAL_ERROR "PGS_EDITOR_VERSION_NAME must contain vMAJOR.MINOR.PATCH in pgsversion.h")
endif()
set(PGS_EDITOR_VERSION "${CMAKE_MATCH_1}")
project(PGSEditor VERSION ${PGS_EDITOR_VERSION} LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pgsversion.h")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Multimedia Network)
find_package(Threads REQUIRED)
set(VGS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "VGSFileFormat sources")
set(VGS_EDITOR_DIR "${VGS_ROOT}/editor")
include("${VGS_ROOT}/cmake/VGSEntropy.cmake")

# gs2mesh: its header to call it, and where a build run in place finds its DLLs (the
# submodule's install). Deployed copies carry them in a gs2mesh folder beside the exe.
set(GS2MESH_SOURCE "${VGS_EDITOR_DIR}/dependencies/gs2mesh_cpp" CACHE PATH "gs2mesh sources (submodule)")
set(GS2MESH_RUNTIME "${GS2MESH_SOURCE}/INSTALL_WIN64/bin" CACHE PATH "gs2mesh.dll and its libraries")
if(NOT EXISTS "${GS2MESH_SOURCE}/include/gs2mesh.h")
    message(FATAL_ERROR "gs2mesh is missing: git submodule update --init editor/dependencies/gs2mesh_cpp")
endif()
# The ONNX models gs2mesh uses with photos, for builds run where they were made (the deployed
# editor carries them in gs2mesh/models). They are not in the repository: point these at them.
set(PGS_STEREO_MODEL "D:/Trabajos/THE4DSCANNER/3dgs2mesh/FoundationStereo/pretrained_models/onnx/foundation_stereo_vits_640x1280.onnx"
    CACHE FILEPATH "FoundationStereo .onnx for real-photo stereo")
set(PGS_MOGE_MODEL "D:/Trabajos/ScanMeNow/RaspberrySoft/SMNForge/build/Desktop_Qt_6_8_0_MSVC2022_64bit-Profile/release/onnx/depth/moge-2-vitl-dynamic.onnx"
    CACHE FILEPATH "MoGe-2 .onnx for the faces' relief (SMNForge's onnx/depth)")
if(NOT EXISTS "${GS2MESH_RUNTIME}/gs2mesh.dll")
    message(WARNING "gs2mesh.dll not found in ${GS2MESH_RUNTIME}: build and install gs2mesh (see its README) to make meshes.")
endif()

# The container, the decoder and the authoring side, as the editor builds them.
add_library(pgs_decoder STATIC
    ${VGS_ENTROPY_SOURCES}
    ${VGS_ROOT}/decoder/src/vgsdecoder.cpp
    ${VGS_ROOT}/core/src/vgscontainer.cpp
    ${VGS_ROOT}/core/src/vgsframe.cpp
    ${VGS_ROOT}/core/src/mgscodec.cpp
    ${VGS_ROOT}/core/crypto/vgscrypto.cpp
    ${VGS_ROOT}/core/crypto/tweetnacl.c)
vgs_link_entropy(pgs_decoder)
target_include_directories(pgs_decoder PUBLIC ${VGS_ROOT}/decoder/include PRIVATE
    ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto ${VGS_ROOT}/core/src)
add_library(pgs_authoring STATIC ${VGS_ROOT}/core/src/vgsencode.cpp ${VGS_ROOT}/core/crypto/vgssign.cpp)
target_compile_definitions(pgs_authoring PRIVATE VGS_AUTHORING)
target_include_directories(pgs_authoring PUBLIC ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto PRIVATE ${VGS_ROOT}/core/src)
target_link_libraries(pgs_authoring PUBLIC pgs_decoder PRIVATE Threads::Threads)

# The editor's sources the PGS editor shares: its export, normals and display support.
set(PGS_SHARED_SOURCES editortheme.cpp displayscaling.cpp previewmemory.cpp plysequencespec.cpp plysequence.cpp isolation.cpp loopmorph.cpp
    pruning.cpp audiomix.cpp mintwriter.cpp modifiers.cpp nativeexport.cpp gaussiannormals.cpp normalrigs.cpp exportcapture.cpp
    dependencies/mint/mintskinrecovery.cpp project.cpp capturesettings.cpp captureworker.cpp packedchunk.cpp packedsource.cpp
    playbackahead.cpp dependencies/mint/mintfile.cpp plyexport.cpp dependencies/plyseq/plyfit.cpp)
list(TRANSFORM PGS_SHARED_SOURCES PREPEND "${VGS_EDITOR_DIR}/")
add_library(pgs_support STATIC ${PGS_SHARED_SOURCES}
    pgsply.cpp pgscapture.cpp colmapfinder.cpp gs2meshrunner.cpp meshio.cpp meshtangents.cpp meshsculpt.cpp pgsexport.cpp pgsproject.cpp pgsautomation.cpp pgsqueue.cpp
    ${VGS_EDITOR_DIR}/viewport.cpp ${VGS_EDITOR_DIR}/viewcube.cpp)
target_include_directories(pgs_support PUBLIC . ${VGS_EDITOR_DIR} ${VGS_EDITOR_DIR}/dependencies/mint ${VGS_EDITOR_DIR}/dependencies/eigen
    ${GS2MESH_SOURCE}/include PRIVATE ${GS2MESH_SOURCE}/dependencies/win64/mikktspace)
target_compile_definitions(pgs_support PUBLIC PGS_GS2MESH_DIR="${GS2MESH_RUNTIME}" PGS_STEREO_MODEL="${PGS_STEREO_MODEL}" PGS_MOGE_MODEL="${PGS_MOGE_MODEL}")
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(pgs_support PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(pgs_support PRIVATE EIGEN_DONT_PARALLELIZE)
endif()
target_link_libraries(pgs_support PUBLIC Qt6::Widgets Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Network pgs_decoder pgs_authoring)
if(WIN32)
    target_link_libraries(pgs_support PRIVATE user32)
endif()

qt_add_executable(PGSEditor WIN32 pgsmain.cpp pgswindow.cpp pgswindowautomation.cpp pgswindow.h ${VGS_EDITOR_DIR}/resources.qrc)
target_link_libraries(PGSEditor PRIVATE pgs_support Qt6::Multimedia)
if(WIN32)
    string(REPLACE "." "," PGS_EDITOR_VERSION_COMMAS "${PGS_EDITOR_VERSION},0")
    configure_file(appicon.rc.in ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc @ONLY)
    target_sources(PGSEditor PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc)
endif()
if(MSVC)
    target_compile_options(pgs_decoder PRIVATE /bigobj)
    target_compile_options(pgs_support PRIVATE /utf-8 /bigobj)
    target_compile_options(PGSEditor PRIVATE /utf-8)
endif()

option(PGS_BUILD_TESTS "Build the PGS editor's tests" ON)
if(PGS_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(pgseditor_tests tests/pgseditor_tests.cpp pgswindow.cpp pgswindowautomation.cpp pgswindow.h ${VGS_EDITOR_DIR}/resources.qrc)
    target_link_libraries(pgseditor_tests PRIVATE pgs_support Qt6::Multimedia Qt6::Test)
    if(MSVC)
        target_compile_options(pgseditor_tests PRIVATE /utf-8)
    endif()
    add_test(NAME pgseditor_tests COMMAND pgseditor_tests)

    # Signed static captures consumed by the UE importer acceptance script.
    add_executable(pgs_unreal_fixture tests/unreal_fixture.cpp)
    target_include_directories(pgs_unreal_fixture PRIVATE ${VGS_ROOT}/plugins/vgsbridge/include)
    target_link_libraries(pgs_unreal_fixture PRIVATE pgs_support)
endif()

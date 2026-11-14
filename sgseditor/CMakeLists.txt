# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
# SGS Editor is supplied under the VGS Editor licence (../editor/LICENSE.md).
#
# SGS Editor: static captures (one 3DGS .ply): normals, a mesh made with gs2mesh, lights, and
# .ply / .sgs export. It compiles the VGS editor's sources it shares (viewport, export, normals)
# from ../editor, as the viewer does, and loads gs2mesh (../editor/dependencies/gs2mesh_cpp, a
# submodule) at run time.
#
#   cmake -S . -B build/cmake-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/Qt6.8.1/6.8.0/msvc2022_64
#   cmake --build build/cmake-msvc --config Release

cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sgsversion.h" SGS_EDITOR_VERSION_LINE REGEX "#define[ \t]+SGS_EDITOR_VERSION_NAME")
string(REGEX MATCH "\"v([0-9]+\\.[0-9]+\\.[0-9]+)\"" SGS_EDITOR_VERSION_MATCH "${SGS_EDITOR_VERSION_LINE}")
if(NOT SGS_EDITOR_VERSION_MATCH)
    message(FATAL_ERROR "SGS_EDITOR_VERSION_NAME must contain vMAJOR.MINOR.PATCH in sgsversion.h")
endif()
set(SGS_EDITOR_VERSION "${CMAKE_MATCH_1}")
project(SGSEditor VERSION ${SGS_EDITOR_VERSION} LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sgsversion.h")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Multimedia)
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
set(SGS_STEREO_MODEL "D:/Trabajos/THE4DSCANNER/3dgs2mesh/FoundationStereo/pretrained_models/onnx/foundation_stereo_vits_640x1280.onnx"
    CACHE FILEPATH "FoundationStereo .onnx for real-photo stereo")
set(SGS_MOGE_MODEL "D:/Trabajos/ScanMeNow/RaspberrySoft/SMNForge/build/Desktop_Qt_6_8_0_MSVC2022_64bit-Profile/release/onnx/depth/moge-2-vitl-dynamic.onnx"
    CACHE FILEPATH "MoGe-2 .onnx for the faces' relief (SMNForge's onnx/depth)")
if(NOT EXISTS "${GS2MESH_RUNTIME}/gs2mesh.dll")
    message(WARNING "gs2mesh.dll not found in ${GS2MESH_RUNTIME}: build and install gs2mesh (see its README) to make meshes.")
endif()

# The container, the decoder and the authoring side, as the editor builds them.
add_library(sgs_decoder STATIC
    ${VGS_ENTROPY_SOURCES}
    ${VGS_ROOT}/decoder/src/vgsdecoder.cpp
    ${VGS_ROOT}/core/src/vgscontainer.cpp
    ${VGS_ROOT}/core/src/vgsframe.cpp
    ${VGS_ROOT}/core/src/mgscodec.cpp
    ${VGS_ROOT}/core/crypto/vgscrypto.cpp
    ${VGS_ROOT}/core/crypto/tweetnacl.c)
vgs_link_entropy(sgs_decoder)
target_include_directories(sgs_decoder PUBLIC ${VGS_ROOT}/decoder/include PRIVATE
    ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto ${VGS_ROOT}/core/src)
add_library(sgs_authoring STATIC ${VGS_ROOT}/core/src/vgsencode.cpp ${VGS_ROOT}/core/crypto/vgssign.cpp)
target_compile_definitions(sgs_authoring PRIVATE VGS_AUTHORING)
target_include_directories(sgs_authoring PUBLIC ${VGS_ROOT}/core/include/vgs ${VGS_ROOT}/core/crypto PRIVATE ${VGS_ROOT}/core/src)
target_link_libraries(sgs_authoring PUBLIC sgs_decoder PRIVATE Threads::Threads)

# The editor's sources the SGS editor shares: its export, normals and display support.
set(SGS_SHARED_SOURCES editortheme.cpp displayscaling.cpp previewmemory.cpp plysequencespec.cpp plysequence.cpp isolation.cpp loopmorph.cpp
    pruning.cpp audiomix.cpp mintwriter.cpp modifiers.cpp nativeexport.cpp gaussiannormals.cpp normalrigs.cpp exportcapture.cpp
    dependencies/mint/mintskinrecovery.cpp project.cpp capturesettings.cpp captureworker.cpp packedchunk.cpp packedsource.cpp
    playbackahead.cpp dependencies/mint/mintfile.cpp plyexport.cpp dependencies/plyseq/plyfit.cpp)
list(TRANSFORM SGS_SHARED_SOURCES PREPEND "${VGS_EDITOR_DIR}/")
add_library(sgs_support STATIC ${SGS_SHARED_SOURCES}
    sgsply.cpp sgscapture.cpp colmapfinder.cpp gs2meshrunner.cpp meshio.cpp sgsexport.cpp sgsproject.cpp
    ${VGS_EDITOR_DIR}/viewport.cpp ${VGS_EDITOR_DIR}/viewcube.cpp)
target_include_directories(sgs_support PUBLIC . ${VGS_EDITOR_DIR} ${VGS_EDITOR_DIR}/dependencies/mint ${VGS_EDITOR_DIR}/dependencies/eigen
    ${GS2MESH_SOURCE}/include)
target_compile_definitions(sgs_support PUBLIC SGS_GS2MESH_DIR="${GS2MESH_RUNTIME}" SGS_STEREO_MODEL="${SGS_STEREO_MODEL}" SGS_MOGE_MODEL="${SGS_MOGE_MODEL}")
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(sgs_support PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(sgs_support PRIVATE EIGEN_DONT_PARALLELIZE)
endif()
target_link_libraries(sgs_support PUBLIC Qt6::Widgets Qt6::OpenGLWidgets Qt6::OpenGL sgs_decoder sgs_authoring)
if(WIN32)
    target_link_libraries(sgs_support PRIVATE user32)
endif()

qt_add_executable(SGSEditor WIN32 sgsmain.cpp sgswindow.cpp sgswindow.h ${VGS_EDITOR_DIR}/resources.qrc)
target_link_libraries(SGSEditor PRIVATE sgs_support Qt6::Multimedia)
if(WIN32)
    string(REPLACE "." "," SGS_EDITOR_VERSION_COMMAS "${SGS_EDITOR_VERSION},0")
    configure_file(appicon.rc.in ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc @ONLY)
    target_sources(SGSEditor PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/appicon.rc)
endif()
if(MSVC)
    target_compile_options(sgs_decoder PRIVATE /bigobj)
    target_compile_options(sgs_support PRIVATE /utf-8 /bigobj)
    target_compile_options(SGSEditor PRIVATE /utf-8)
endif()

option(SGS_BUILD_TESTS "Build the SGS editor's tests" ON)
if(SGS_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(sgseditor_tests tests/sgseditor_tests.cpp sgswindow.cpp sgswindow.h ${VGS_EDITOR_DIR}/resources.qrc)
    target_link_libraries(sgseditor_tests PRIVATE sgs_support Qt6::Multimedia Qt6::Test)
    if(MSVC)
        target_compile_options(sgseditor_tests PRIVATE /utf-8)
    endif()
    add_test(NAME sgseditor_tests COMMAND sgseditor_tests)
endif()

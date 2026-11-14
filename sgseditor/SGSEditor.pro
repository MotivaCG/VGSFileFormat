# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
# SGS Editor is supplied under the VGS Editor licence (../editor/LICENSE.md).
#
# SGS Editor: static captures (one 3DGS .ply): normals, a mesh made with gs2mesh, lights, and
# .ply / .sgs export. It compiles the VGS editor's sources it shares from ../editor, as the
# viewer does; gs2mesh (../editor/dependencies/gs2mesh_cpp, a submodule) is loaded at run time.

!versionAtLeast(QT_VERSION, 6.8.0) {
    error("SGS Editor requires Qt 6.8 or newer. Select the Desktop Qt 6.8.0 MSVC2022 64-bit kit.")
}
QT += core gui widgets opengl openglwidgets multimedia
CONFIG += c++17
TARGET = SGSEditor
DEFINES += VGS_AUTHORING
msvc: DEFINES += EIGEN_DONT_PARALLELIZE
EDITOR = $$clean_path($$PWD/../editor)
VGS_ROOT = $$clean_path($$PWD/..)
include($$VGS_ROOT/cmake/VGSEntropy.pri)

# gs2mesh: its header to call it, and where a build run in place finds its DLLs (the
# submodule's install). Deployed copies carry them in a gs2mesh folder beside the exe.
GS2MESH_SOURCE = $$EDITOR/dependencies/gs2mesh_cpp
!exists($$GS2MESH_SOURCE/include/gs2mesh.h) {
    error("gs2mesh is missing: git submodule update --init editor/dependencies/gs2mesh_cpp")
}
DEFINES += SGS_GS2MESH_DIR=\\\"$$GS2MESH_SOURCE/INSTALL_WIN64/bin\\\"
# The ONNX models gs2mesh uses with photos, for builds run where they were made (the deployed
# editor carries them in gs2mesh/models). Not in the repository: point these at them.
isEmpty(SGS_STEREO_MODEL): SGS_STEREO_MODEL = D:/Trabajos/THE4DSCANNER/3dgs2mesh/FoundationStereo/pretrained_models/onnx/foundation_stereo_vits_640x1280.onnx
isEmpty(SGS_MOGE_MODEL): SGS_MOGE_MODEL = D:/Trabajos/ScanMeNow/RaspberrySoft/SMNForge/build/Desktop_Qt_6_8_0_MSVC2022_64bit-Profile/release/onnx/depth/moge-2-vitl-dynamic.onnx
DEFINES += SGS_STEREO_MODEL=\\\"$$SGS_STEREO_MODEL\\\" SGS_MOGE_MODEL=\\\"$$SGS_MOGE_MODEL\\\"

INCLUDEPATH += $$PWD $$EDITOR $$EDITOR/dependencies/mint $$EDITOR/dependencies/eigen $$GS2MESH_SOURCE/include \
    $$VGS_ROOT/decoder/include $$VGS_ROOT/core/include/vgs $$VGS_ROOT/core/crypto $$VGS_ROOT/core/src

SOURCES += $$PWD/sgsmain.cpp $$PWD/sgswindow.cpp $$PWD/sgsply.cpp $$PWD/sgscapture.cpp $$PWD/colmapfinder.cpp \
    $$PWD/gs2meshrunner.cpp $$PWD/meshio.cpp $$PWD/sgsexport.cpp $$PWD/sgsproject.cpp
HEADERS += $$PWD/sgswindow.h $$PWD/sgsply.h $$PWD/sgscapture.h $$PWD/colmapfinder.h $$PWD/gs2meshrunner.h \
    $$PWD/meshio.h $$PWD/sgsexport.h $$PWD/sgsproject.h $$PWD/sgsversion.h

# The editor's sources the SGS editor shares: its viewport, export, normals and display support.
SOURCES += $$EDITOR/viewport.cpp $$EDITOR/viewcube.cpp $$EDITOR/editortheme.cpp $$EDITOR/displayscaling.cpp \
    $$EDITOR/previewmemory.cpp $$EDITOR/plysequencespec.cpp $$EDITOR/plysequence.cpp $$EDITOR/plyexport.cpp \
    $$EDITOR/dependencies/plyseq/plyfit.cpp $$EDITOR/isolation.cpp $$EDITOR/loopmorph.cpp $$EDITOR/pruning.cpp \
    $$EDITOR/audiomix.cpp $$EDITOR/mintwriter.cpp $$EDITOR/modifiers.cpp $$EDITOR/nativeexport.cpp \
    $$EDITOR/gaussiannormals.cpp $$EDITOR/normalrigs.cpp $$EDITOR/exportcapture.cpp $$EDITOR/project.cpp \
    $$EDITOR/capturesettings.cpp $$EDITOR/captureworker.cpp $$EDITOR/packedchunk.cpp $$EDITOR/packedsource.cpp \
    $$EDITOR/playbackahead.cpp $$EDITOR/dependencies/mint/mintfile.cpp $$EDITOR/dependencies/mint/mintskinrecovery.cpp \
    $$VGS_ROOT/decoder/src/vgsdecoder.cpp $$VGS_ROOT/core/src/vgscontainer.cpp $$VGS_ROOT/core/src/vgsframe.cpp \
    $$VGS_ROOT/core/src/mgscodec.cpp $$VGS_ROOT/core/src/vgsencode.cpp $$VGS_ROOT/core/crypto/vgscrypto.cpp \
    $$VGS_ROOT/core/crypto/vgssign.cpp $$VGS_ROOT/core/crypto/tweetnacl.c
HEADERS += $$EDITOR/viewport.h $$EDITOR/viewcube.h $$EDITOR/captureworker.h
RESOURCES += $$EDITOR/resources.qrc
msvc: QMAKE_CXXFLAGS += /utf-8 /bigobj /openmp
win32: LIBS += -lopengl32 -luser32
win32: RC_ICONS = $$EDITOR/assets/gracia/logo.ico

# The version is set once, in sgsversion.h.
VGS_HEADER_LINES = $$cat($$PWD/sgsversion.h, lines)
VGS_NAME_LINE = $$find(VGS_HEADER_LINES, SGS_EDITOR_VERSION_NAME)
VERSION = $$replace(VGS_NAME_LINE, ^.*v([0-9.]+).*$, \\1).0
QMAKE_TARGET_PRODUCT = SGS Editor
QMAKE_TARGET_DESCRIPTION = SGS Editor
QMAKE_TARGET_COMPANY = Victor M. Feliz
QMAKE_TARGET_COPYRIGHT = Copyright (c) 2026 Victor M. Feliz. All rights reserved.
DISTFILES += $$PWD/README.md $$PWD/deploy.ps1 $$PWD/installer/SGSEditor_InnoSetup.iss

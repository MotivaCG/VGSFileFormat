# SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
# Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#
# VGS Viewer: opens and plays .vgs/.pgs, nothing else. It compiles the editor's display
# sources itself and the decoder only - never the export, encoder or signing sources, which
# carry the private key.

QT += core gui widgets opengl openglwidgets multimedia
CONFIG += c++17
TARGET = VGSViewer
DEFINES += VGS_VIEWER
EDITOR = $$clean_path($$PWD/..)
VGS_ROOT = $$clean_path($$PWD/../..)
INCLUDEPATH += $$PWD $$EDITOR $$EDITOR/dependencies/mint $$EDITOR/dependencies/eigen \
    $$VGS_ROOT/decoder/include $$VGS_ROOT/core/include/vgs $$VGS_ROOT/core/crypto $$VGS_ROOT/core/src

SOURCES += $$PWD/viewermain.cpp $$PWD/viewerwindow.cpp \
    $$EDITOR/editortheme.cpp $$EDITOR/displayscaling.cpp $$EDITOR/viewport.cpp $$EDITOR/viewcube.cpp \
    $$EDITOR/rangeslider.cpp $$EDITOR/audiopreview.cpp $$EDITOR/captureworker.cpp $$EDITOR/isolation.cpp \
    $$EDITOR/pruning.cpp $$EDITOR/project.cpp $$EDITOR/modifiers.cpp $$EDITOR/capturesettings.cpp \
    $$EDITOR/dependencies/mint/mintfile.cpp $$EDITOR/dependencies/mint/mintskinrecovery.cpp \
    $$VGS_ROOT/decoder/src/vgsdecoder.cpp $$VGS_ROOT/core/src/vgscontainer.cpp $$VGS_ROOT/core/src/vgsframe.cpp \
    $$VGS_ROOT/core/src/mgscodec.cpp $$VGS_ROOT/core/crypto/vgscrypto.cpp $$VGS_ROOT/core/crypto/tweetnacl.c
HEADERS += $$PWD/viewerwindow.h $$PWD/viewerversion.h \
    $$EDITOR/viewport.h $$EDITOR/viewcube.h $$EDITOR/rangeslider.h $$EDITOR/audiopreview.h $$EDITOR/captureworker.h
RESOURCES += $$EDITOR/resources.qrc
msvc: QMAKE_CXXFLAGS += /utf-8 /bigobj
win32: LIBS += -lopengl32 -luser32
win32: RC_ICONS = $$EDITOR/assets/gracia/logo.ico

# The version is set once, in viewerversion.h.
VGS_HEADER_LINES = $$cat($$PWD/viewerversion.h, lines)
VGS_NAME_LINE = $$find(VGS_HEADER_LINES, VIEWER_VERSION_NAME)
VERSION = $$replace(VGS_NAME_LINE, ^.*v([0-9.]+).*$, \\1).0
QMAKE_TARGET_PRODUCT = VGS Viewer
QMAKE_TARGET_DESCRIPTION = VGS Viewer
QMAKE_TARGET_COMPANY = Victor M. Feliz
QMAKE_TARGET_COPYRIGHT = Copyright (c) 2026 Victor M. Feliz. All rights reserved.

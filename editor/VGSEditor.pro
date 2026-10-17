# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
#
# Proprietary software owned by Víctor M. Feliz.
# ScanMeNow and The4DScanner are licensed for internal use only.
# No ownership, sale, redistribution, sublicensing or modification rights
# are granted. All other rights remain reserved to the copyright holder.
# See LICENSE.md for the limited use grant and applicable terms.
# Other uses require prior written authorisation, subject to mandatory law.

!versionAtLeast(QT_VERSION, 6.8.0) {
    error("VGS Editor requires Qt 6.8 or newer. Select the Desktop Qt 6.8.0 MSVC2022 64-bit kit; Qt 5 cannot build this project.")
}
QT += core gui widgets opengl openglwidgets multimedia network
SOURCES += $$PWD/editorautomation.cpp $$PWD/mainwindowautomation.cpp
HEADERS += $$PWD/editorautomation.h
HEADERS += $$PWD/pointvisibility.h
CONFIG += c++17
DEFINES += VGS_AUTHORING
TARGET = VGSEditor
SOURCES += $$PWD/displayscaling.cpp
SOURCES += $$PWD/previewmemory.cpp
HEADERS += $$PWD/previewmemory.h
HEADERS += $$PWD/displayscaling.h \
    licensemanagement.h
SOURCES += $$PWD/editortheme.cpp
HEADERS += $$PWD/editortheme.h
VGS_ROOT = $$clean_path($$PWD/..)
# Resolve local headers when regenerating the incremental build dependencies,
# including the shared cache information used by inspection and the modifier UI.
DEPENDPATH += $$PWD
INCLUDEPATH += $$PWD/dependencies/eigen $$PWD $$PWD/dependencies/mint $$VGS_ROOT/decoder/include \
    $$VGS_ROOT/core/include/vgs $$VGS_ROOT/core/crypto $$VGS_ROOT/core/src
SOURCES += $$PWD/main.cpp $$PWD/mainwindow.cpp $$PWD/inspection.cpp $$PWD/viewport.cpp $$PWD/viewcube.cpp $$PWD/captureworker.cpp $$PWD/project.cpp $$PWD/filehistory.cpp $$PWD/presetstore.cpp $$PWD/capturesettings.cpp $$PWD/capturesettingsdialog.cpp \
    $$PWD/dependencies/mint/mintfile.cpp \
    $$VGS_ROOT/decoder/src/vgsdecoder.cpp \
    $$VGS_ROOT/core/src/vgscontainer.cpp $$VGS_ROOT/core/src/vgsframe.cpp \
    $$VGS_ROOT/core/src/mgscodec.cpp $$VGS_ROOT/core/crypto/vgscrypto.cpp \
    $$VGS_ROOT/core/crypto/tweetnacl.c
HEADERS += $$PWD/mainwindow.h $$PWD/viewport.h $$PWD/viewcube.h $$PWD/captureworker.h $$PWD/project.h $$PWD/filehistory.h $$PWD/presetstore.h $$PWD/capturesettings.h $$PWD/capturesettingsdialog.h $$PWD/dependencies/mint/mintfile.h
msvc: QMAKE_CXXFLAGS += /utf-8 /bigobj
win32: LIBS += -lopengl32 -luser32
RESOURCES += $$PWD/resources.qrc
win32: RC_ICONS = $$PWD/assets/gracia/logo.ico
# The version is set once, in licensemanagement.h ("v1.0.0"): the exe's version
# information takes it from there, as the window title, About and the installer do.
VGS_HEADER_LINES = $$cat($$PWD/licensemanagement.h, lines)
VGS_NAME_LINE = $$find(VGS_HEADER_LINES, VERSION_NAME)
VERSION = $$replace(VGS_NAME_LINE, ^.*v([0-9.]+).*$, \\1).0
QMAKE_TARGET_PRODUCT = VGS Editor
QMAKE_TARGET_DESCRIPTION = VGS Editor
QMAKE_TARGET_COMPANY = Victor M. Feliz
QMAKE_TARGET_COPYRIGHT = Copyright (c) 2026 Victor M. Feliz. All rights reserved.
DISTFILES += $$PWD/README.md $$PWD/dependencies/mint/README.md

SOURCES += $$PWD/rangeslider.cpp $$PWD/exportcapture.cpp $$PWD/dependencies/mint/mintskinrecovery.cpp $$VGS_ROOT/core/src/vgsencode.cpp $$VGS_ROOT/core/crypto/vgssign.cpp
HEADERS += $$PWD/rangeslider.h $$PWD/exportcapture.h $$PWD/dependencies/mint/mintskinrecovery.h

SOURCES += $$PWD/nativeexport.cpp $$PWD/gaussiannormals.cpp $$PWD/normalrigs.cpp
HEADERS += $$PWD/nativeexport.h $$PWD/gaussiannormals.h

SOURCES += $$PWD/modifiers.cpp $$PWD/audiomix.cpp
HEADERS += $$PWD/audiomix.h
SOURCES += $$PWD/audiopreview.cpp $$PWD/isolation.cpp $$PWD/loopmorph.cpp $$PWD/pruning.cpp $$PWD/animationpanel.cpp
HEADERS += $$PWD/audiopreview.h $$PWD/isolation.h $$PWD/loopmorph.h $$PWD/pruning.h $$PWD/animationpanel.h
SOURCES += $$PWD/mintwriter.cpp
HEADERS += $$PWD/mintwriter.h

SOURCES += $$PWD/modifierpanel.cpp
HEADERS += $$PWD/modifierpanel.h
SOURCES += $$PWD/exporttask.cpp $$PWD/taskqueuedialog.cpp
HEADERS += $$PWD/exporttask.h $$PWD/taskqueuedialog.h

# Beside the program: the built-in presets, and the ffmpeg exports mix the soundtrack with.
VGS_FFMPEG = D:/Trabajos/THE4DSCANNER/GraciaConverter/Gracia4DGSConverter/build/Desktop_Qt_6_8_0_MSVC2022_64bit-Release/release/tools/ffmpeg.exe
CONFIG(debug, debug|release): VGS_OUT = $$OUT_PWD/debug
else: VGS_OUT = $$OUT_PWD/release
builtinPresets.files = $$files($$PWD/presets/*.preset)
builtinPresets.path = $$VGS_OUT/presets
COPIES += builtinPresets
mcpAdapter.files = $$PWD/mcp/server.py $$PWD/mcp/http_transport.py $$PWD/mcp/connections.py $$PWD/mcp/workflows.md $$PWD/mcp/tools.md $$PWD/mcp/requirements.txt $$PWD/mcp/install.ps1 $$PWD/mcp/README.md
mcpAdapter.path = $$VGS_OUT/mcp
COPIES += mcpAdapter
exists($$VGS_FFMPEG) {
    ffmpegTool.files = $$VGS_FFMPEG
    ffmpegTool.path = $$VGS_OUT/tools
    COPIES += ffmpegTool
}

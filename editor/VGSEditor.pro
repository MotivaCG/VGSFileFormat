QT += core gui widgets opengl openglwidgets
CONFIG += c++17
DEFINES += VGS_AUTHORING
TARGET = VGSEditor
VGS_ROOT = $$clean_path($$PWD/..)
INCLUDEPATH += $$PWD/dependencies/eigen $$PWD $$PWD/dependencies/mint $$VGS_ROOT/decoder/include \
    $$VGS_ROOT/core/include/vgs $$VGS_ROOT/core/crypto $$VGS_ROOT/core/src
SOURCES += $$PWD/main.cpp $$PWD/mainwindow.cpp $$PWD/viewport.cpp $$PWD/viewcube.cpp $$PWD/captureworker.cpp $$PWD/project.cpp $$PWD/filehistory.cpp $$PWD/presetstore.cpp $$PWD/capturesettings.cpp $$PWD/capturesettingsdialog.cpp \
    $$PWD/dependencies/mint/mintfile.cpp \
    $$VGS_ROOT/decoder/src/vgsdecoder.cpp \
    $$VGS_ROOT/core/src/vgscontainer.cpp $$VGS_ROOT/core/src/vgsframe.cpp \
    $$VGS_ROOT/core/src/mgscodec.cpp $$VGS_ROOT/core/crypto/vgscrypto.cpp \
    $$VGS_ROOT/core/crypto/tweetnacl.c
HEADERS += $$PWD/mainwindow.h $$PWD/viewport.h $$PWD/viewcube.h $$PWD/captureworker.h $$PWD/project.h $$PWD/filehistory.h $$PWD/presetstore.h $$PWD/capturesettings.h $$PWD/capturesettingsdialog.h $$PWD/dependencies/mint/mintfile.h
msvc: QMAKE_CXXFLAGS += /utf-8 /bigobj
win32: LIBS += -lopengl32
RESOURCES += $$PWD/resources.qrc
win32: RC_ICONS = $$PWD/assets/gracia/logo.ico
DISTFILES += $$PWD/README.md $$PWD/dependencies/mint/README.md

SOURCES += $$PWD/rangeslider.cpp $$PWD/exportcapture.cpp $$PWD/dependencies/mint/mintskinrecovery.cpp $$VGS_ROOT/core/src/vgsencode.cpp $$VGS_ROOT/core/crypto/vgssign.cpp
HEADERS += $$PWD/rangeslider.h $$PWD/exportcapture.h $$PWD/dependencies/mint/mintskinrecovery.h

SOURCES += $$PWD/nativeexport.cpp
HEADERS += $$PWD/nativeexport.h

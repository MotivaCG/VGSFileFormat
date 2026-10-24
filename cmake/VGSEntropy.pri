# Shared entropy dependencies for the standalone Qt/qmake editor and viewer.
INCLUDEPATH += $$VGS_ROOT/core/third_party/fse
DEFINES += FSEU16_MAX_SYMBOL_VALUE=4095
# FSE's U16 template includes these .c files with different macros. They must
# also compile separately; qmake otherwise removes them from SOURCES.
CONFIG += compile_included_sources
SOURCES += $$VGS_ROOT/core/src/pcodec_bridge.cpp \
    $$VGS_ROOT/core/third_party/fse/fseU16.c \
    $$VGS_ROOT/core/third_party/fse/fse_compress.c \
    $$VGS_ROOT/core/third_party/fse/fse_decompress.c \
    $$VGS_ROOT/core/third_party/fse/entropy_common.c \
    $$VGS_ROOT/core/third_party/fse/hist.c \
    $$VGS_ROOT/core/third_party/fse/huf_compress.c \
    $$VGS_ROOT/core/third_party/fse/huf_decompress.c \
    $$VGS_ROOT/core/third_party/fse/debug.c

CONFIG(debug, debug|release): VGS_PCO_CONFIG = Debug
else: VGS_PCO_CONFIG = Release
VGS_PCO_BUILD = $$clean_path($$OUT_PWD/build_vgs_pcodec/$$VGS_PCO_CONFIG)
win32-msvc {
    VGS_PCO_GENERATOR = NMake Makefiles
    VGS_PCO_LIBRARY = $$VGS_PCO_BUILD/pcodec/$$VGS_PCO_CONFIG/vgs_pcodec.lib
    VGS_PCO_SYSTEM_LIBS = -luserenv -lws2_32 -lbcrypt -lntdll
} else:unix {
    VGS_PCO_GENERATOR = Unix Makefiles
    VGS_PCO_LIBRARY = $$VGS_PCO_BUILD/pcodec/$$VGS_PCO_CONFIG/vgs_pcodec.a
    unix:!macx: VGS_PCO_SYSTEM_LIBS = -lpthread -ldl -lm
} else {
    error("VGS Pcodec qmake builds support MSVC and Unix kits; use CMake for other toolchains.")
}
isEmpty(VGS_CMAKE): VGS_CMAKE = cmake

# Use the same offline Cargo builder as the SDK. Build this prerequisite before
# linking; Qt Creator does not need Cargo on PATH (CMake locates the user install).
vgs_pcodec.target = $$VGS_PCO_LIBRARY
vgs_pcodec.depends = $$files($$VGS_ROOT/core/pcodec/*.rs, true) \
    $$VGS_ROOT/core/pcodec/Cargo.toml $$VGS_ROOT/core/pcodec/Cargo.lock \
    $$files($$VGS_ROOT/core/third_party/pcodec/*.rs, true) \
    $$files($$VGS_ROOT/core/third_party/pcodec/Cargo.toml, true) \
    $$VGS_ROOT/cmake/VGSEntropy.cmake
vgs_pcodec.commands = $$shell_quote($$VGS_CMAKE) -S $$shell_quote($$VGS_ROOT) \
    -B $$shell_quote($$VGS_PCO_BUILD) -G $$shell_quote($$VGS_PCO_GENERATOR) \
    -DCMAKE_BUILD_TYPE=$$VGS_PCO_CONFIG -DVGS_BUILD_TOOLS=OFF -DVGS_BUILD_TESTS=OFF \
    -DVGS_BUILD_PLUGINS=OFF -DVGS_BUILD_EDITOR=OFF -DVGS_BUILD_VIEWER=OFF \
    $$escape_expand(\\n\\t) \
    $$shell_quote($$VGS_CMAKE) --build $$shell_quote($$VGS_PCO_BUILD) \
    --config $$VGS_PCO_CONFIG --target vgs_pcodec_build
QMAKE_EXTRA_TARGETS += vgs_pcodec
PRE_TARGETDEPS += $$VGS_PCO_LIBRARY
LIBS += $$quote($$VGS_PCO_LIBRARY) $$VGS_PCO_SYSTEM_LIBS

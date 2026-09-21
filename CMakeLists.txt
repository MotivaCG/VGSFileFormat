cmake_minimum_required(VERSION 3.16)
project(VGSFileFormat VERSION 2.0.0 LANGUAGES C CXX)

# The VFGS container: an encoder, a decoder, and the codec they share.
#
# Native build, both libraries, installed into the build directory:
#
#   cmake -S . -B build_win64 -G "Visual Studio 17 2022" -A x64
#   cmake --build build_win64 --config Release
#   cmake --install build_win64 --config Release      -> build_win64/INSTALL
#
# The libraries are static by default. -DVGS_BUILD_SHARED=ON builds DLLs instead, which
# export the C interface and nothing else; README.md says when that is the one to want.
#
# WebAssembly decoder, same sources through Emscripten:
#
#   emcmake cmake -S . -B build_wasm -DVGS_BUILD_WASM=ON
#   cmake --build build_wasm
#
# There are two build directories because a native library and a WebAssembly module come
# from two different toolchains, and one configure step can only hold one. Everything
# else - sources, options, install rules - is this one project. BUILD.md walks through it.
#
# The libraries are deliberately not symmetrical. The encoder compiles the container
# writer and the private signing key; the decoder compiles neither, so a decoder shipped
# to a browser or to a customer cannot write a capture and holds nothing that would let
# it forge one. core/CMakeLists.txt is where that line is drawn.

option(VGS_BUILD_TOOLS "Build the command line encoder and the decoder samples" ON)
option(VGS_BUILD_TESTS "Build the test programs" ON)
option(VGS_BUILD_WASM "Build the WebAssembly decoder instead of the native libraries" OFF)
option(VGS_BUILD_SHARED "Build the libraries as DLLs exporting the C interface only" OFF)

# What the decoder keeps decoded while playing, as a starting point. Empty means the
# built-in default for the target, which differs between a native build and a browser;
# see CachePolicy in vgsdecoder.h. A caller can always override it with setCachePolicy.
set(VGS_CACHE_BEHIND "" CACHE STRING "Decoded chunks kept behind the one being played")
set(VGS_CACHE_AHEAD "" CACHE STRING "Decoded chunks kept ahead of it")
set(VGS_CACHE_MAX_BYTES "" CACHE STRING "Ceiling on the decoded chunks held, in bytes")

function(vgs_apply_cache_defaults target)
  foreach(name BEHIND AHEAD MAX_BYTES)
    if(NOT VGS_CACHE_${name} STREQUAL "")
      target_compile_definitions(${target} PRIVATE VGS_CACHE_${name}=${VGS_CACHE_${name}})
    endif()
  endforeach()
endfunction()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Nothing is exported unless it says so. A static build is unaffected; a shared build then
# exports exactly the functions marked in vgsencoder_c.h and vgsdecoder_c.h, and none of
# the C++ API, the codec or the crypto. Never turn on WINDOWS_EXPORT_ALL_SYMBOLS here: it
# would export the lot and make every internal a supported entry point by accident.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

if(VGS_BUILD_SHARED)
  set(VGS_LIBRARY_TYPE SHARED)
else()
  set(VGS_LIBRARY_TYPE STATIC)
endif()

# Everything lands in INSTALL inside the build directory unless the caller says
# otherwise, so a plain build gives something immediately usable without arguing about
# system directories. Inside the build directory rather than beside the sources, because
# a native build, a WebAssembly build and a 32-bit build are three different sets of
# artefacts and each should have its own place to land.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/INSTALL" CACHE PATH "Install location" FORCE)
endif()

include(GNUInstallDirs)

# The two libraries install into separate trees, so the decoder half can be handed to
# somebody without the encoder coming with it - and without having to trust that they will
# not look in the wrong directory. Each tree is a complete install on its own: headers,
# library, package files, tools.
#
#   INSTALL/decoder/{bin,include,lib,share,web}
#   INSTALL/encoder/{bin,include,lib}
#
# They are also install components, so a decoder-only tree can be produced straight into
# wherever it is going:
#
#   cmake --install build_win64 --config Release --component decoder --prefix D:/ship
set(VGS_DECODER_PREFIX decoder)
set(VGS_ENCODER_PREFIX encoder)

# One place for the flags, applied directly rather than through an interface target: a
# static library that links one would carry it into its exported target set, and a
# consumer of the installed package should need nothing but the library itself.
function(vgs_apply_settings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()

add_subdirectory(core)
# The encoder is native only. A WebAssembly build is a reader in a browser, and building
# the authoring library there would mean compiling the private key for a target whose
# whole job is to be downloaded.
if(NOT VGS_BUILD_WASM)
  add_subdirectory(encoder)
endif()
add_subdirectory(decoder)

if(VGS_BUILD_TESTS AND NOT VGS_BUILD_WASM)
  enable_testing()
  add_subdirectory(tests)
endif()

# The decoder: the codec's shared half and the API over it.
#
# What is missing here is the point. There is no VGS_AUTHORING, no vgsencode.cpp and no
# vgssign.cpp, so this library cannot write a capture and does not contain the private
# key that would make one valid. It is the half that ships.

vgs_core_quiet_vendor_sources()

set(VGS_DECODER_SOURCES
    ${VGS_CORE_SHARED_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vgsdecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vgsdecoder_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vgsdecoder/vgsdecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vgsdecoder/vgsdecoder_c.h)

if(VGS_BUILD_WASM)
  # The same sources through Emscripten. wasm/ adds the module target and its glue; no
  # static library is built or installed, because nothing native links a wasm build.
  add_subdirectory(wasm)
  return()
endif()

add_library(vgsdecoder ${VGS_LIBRARY_TYPE} ${VGS_DECODER_SOURCES})
add_library(VGS::Decoder ALIAS vgsdecoder)

if(VGS_BUILD_SHARED)
  # The macro is PUBLIC so that a consumer's include of the C header sees dllimport; the
  # build-side one is PRIVATE, which is what flips the same macro to dllexport in here.
  target_compile_definitions(vgsdecoder PUBLIC VGS_DECODER_SHARED PRIVATE VGS_DECODER_BUILD)
  set_target_properties(vgsdecoder PROPERTIES
      VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

target_include_directories(vgsdecoder
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${VGS_CORE_INCLUDE_DIRS})
# EXPORT_NAME is what makes the installed package offer VGS::Decoder; the alias above
# only exists inside this build tree, so without it a consumer would have to write
# VGS::vgsdecoder instead.
set_target_properties(vgsdecoder PROPERTIES DEBUG_POSTFIX d EXPORT_NAME Decoder)
vgs_apply_settings(vgsdecoder)
vgs_apply_cache_defaults(vgsdecoder)

if(VGS_BUILD_SHARED)
  # The samples and the tests use the C++ API, which a DLL does not export. They link a
  # static copy of the same sources rather than being rewritten against the C interface:
  # what they are there to demonstrate is the C++ API.
  add_library(vgsdecoder_internal STATIC EXCLUDE_FROM_ALL ${VGS_DECODER_SOURCES})
  target_include_directories(vgsdecoder_internal
      PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
      PRIVATE ${VGS_CORE_INCLUDE_DIRS})
  vgs_apply_settings(vgsdecoder_internal)
  vgs_apply_cache_defaults(vgsdecoder_internal)
  set(VGS_DECODER_LINK vgsdecoder_internal)
else()
  set(VGS_DECODER_LINK vgsdecoder)
endif()
set(VGS_DECODER_LINK ${VGS_DECODER_LINK} PARENT_SCOPE)

if(VGS_BUILD_TOOLS)
  foreach(sample vgsinfo vgsplay vgsdump vgsexport vgspagecost)
    add_executable(${sample} examples/${sample}.cpp ${VGS_ICON})
    target_link_libraries(${sample} PRIVATE ${VGS_DECODER_LINK})
    vgs_apply_settings(${sample})
    install(TARGETS ${sample} RUNTIME DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_BINDIR} COMPONENT decoder)
  endforeach()
endif()

# The terms and the readme install with the library rather than only being added by the
# packaging script: an install is what a consumer gets, and one without a licence is one
# nobody can lawfully act on.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md
        DESTINATION ${VGS_DECODER_PREFIX}
        COMPONENT decoder)
# The readme shows the mark, so it goes too rather than leaving a broken image.
if(EXISTS "${CMAKE_SOURCE_DIR}/docs/logo.png")
  install(FILES ${CMAKE_SOURCE_DIR}/docs/logo.png
          DESTINATION ${VGS_DECODER_PREFIX}
          COMPONENT decoder)
endif()

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/DISTRIBUTION.md
        DESTINATION ${VGS_DECODER_PREFIX}
        RENAME README.md
        COMPONENT decoder)

# The samples' source ships with the library. A consumer integrating this gets working
# code for every part of the API, buildable on the spot against what was just installed,
# rather than a header and the hope that the comments are enough.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/vgsinfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/vgsplay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/vgsdump.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/vgsexport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/vgspagecost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/plywriter.h
    DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_DATADIR}/vgsdecoder/examples
        COMPONENT decoder)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.standalone.txt
        DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_DATADIR}/vgsdecoder/examples
        RENAME CMakeLists.txt
        COMPONENT decoder)

install(TARGETS vgsdecoder EXPORT VGSDecoderTargets
        ARCHIVE DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_BINDIR}
        COMPONENT decoder)
install(DIRECTORY include/ DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT decoder)
install(EXPORT VGSDecoderTargets
        FILE VGSDecoderTargets.cmake
        NAMESPACE VGS::
        DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/VGSDecoder
        COMPONENT decoder)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/VGSDecoderConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/VGSDecoderConfig.cmake
    INSTALL_DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/VGSDecoder)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/VGSDecoderConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/VGSDecoderConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/VGSDecoderConfigVersion.cmake
    DESTINATION ${VGS_DECODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/VGSDecoder
        COMPONENT decoder)

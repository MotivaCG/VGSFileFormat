# The encoder: the codec's shared half, the authoring half, and the API over them.
#
# This is the only target that defines VGS_AUTHORING, and so the only one that may
# include vgskeys.h - the header refuses to compile anywhere else.

vgs_core_quiet_vendor_sources()

add_library(vgsencoder ${VGS_LIBRARY_TYPE}
    ${VGS_CORE_SHARED_SOURCES}
    ${VGS_CORE_AUTHORING_SOURCES}
    src/vgsencoder.cpp
    src/vgsencoder_c.cpp
    include/vgsencoder/vgsencoder.h
    include/vgsencoder/vgsencoder_c.h)
add_library(VGS::Encoder ALIAS vgsencoder)

target_compile_definitions(vgsencoder PRIVATE VGS_AUTHORING=1)
if(VGS_BUILD_SHARED)
  # The macro is PUBLIC so that a consumer's include of the C header sees dllimport; the
  # build-side one is PRIVATE, which is what flips the same macro to dllexport in here.
  target_compile_definitions(vgsencoder PUBLIC VGS_ENCODER_SHARED PRIVATE VGS_ENCODER_BUILD)
  set_target_properties(vgsencoder PROPERTIES
      VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

target_include_directories(vgsencoder
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${VGS_CORE_INCLUDE_DIRS})
# EXPORT_NAME is what makes the installed package offer VGS::Encoder; the alias above
# only exists inside this build tree, so without it a consumer would have to write
# VGS::vgsencoder instead.
set_target_properties(vgsencoder PROPERTIES DEBUG_POSTFIX d EXPORT_NAME Encoder)
vgs_apply_settings(vgsencoder)

if(VGS_BUILD_SHARED)
  # Same as the decoder: vgsencode is written against the C++ API, which a DLL does not
  # export, so it links a static copy of the same sources.
  add_library(vgsencoder_internal STATIC EXCLUDE_FROM_ALL
      ${VGS_CORE_SHARED_SOURCES} ${VGS_CORE_AUTHORING_SOURCES}
      src/vgsencoder.cpp src/vgsencoder_c.cpp)
  target_compile_definitions(vgsencoder_internal PRIVATE VGS_AUTHORING=1)
  target_include_directories(vgsencoder_internal
      PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
      PRIVATE ${VGS_CORE_INCLUDE_DIRS})
  vgs_apply_settings(vgsencoder_internal)
  set(VGS_ENCODER_LINK vgsencoder_internal)
else()
  set(VGS_ENCODER_LINK vgsencoder)
endif()
set(VGS_ENCODER_LINK ${VGS_ENCODER_LINK} PARENT_SCOPE)

if(VGS_BUILD_TOOLS)
  add_executable(vgsencode tools/vgsencode.cpp)
  target_link_libraries(vgsencode PRIVATE ${VGS_ENCODER_LINK})
  vgs_apply_settings(vgsencode)
  install(TARGETS vgsencode RUNTIME DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_BINDIR} COMPONENT encoder)
endif()

install(TARGETS vgsencoder EXPORT VGSEncoderTargets
        ARCHIVE DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_BINDIR}
        COMPONENT encoder)
install(DIRECTORY include/ DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT encoder)
install(EXPORT VGSEncoderTargets
        FILE VGSEncoderTargets.cmake
        NAMESPACE VGS::
        DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/VGSEncoder
        COMPONENT encoder)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/VGSEncoderConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/VGSEncoderConfig.cmake
    INSTALL_DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/VGSEncoder)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/VGSEncoderConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/VGSEncoderConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/VGSEncoderConfigVersion.cmake
    DESTINATION ${VGS_ENCODER_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/VGSEncoder
        COMPONENT encoder)

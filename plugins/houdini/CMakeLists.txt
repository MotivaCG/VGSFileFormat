# The Houdini plugin: a VGS Capture object and SOP, compiled against the HDK, packaged as a
# Houdini package.
#
#   <build>/plugins/houdini/<config>/package/vgs.json        the package file
#   <build>/plugins/houdini/<config>/package/vgs/            the plugin: dso/, config/, help/
#   <build>/plugins/houdini/<config>/vgs-houdini-<version>-h<houdini>.zip   both, to install
#
# Installing is unzipping into the packages folder of the user's Houdini preferences
# ($HOUDINI_USER_PREF_DIR/packages), or anywhere HOUDINI_PACKAGE_DIR points.
#
# An HDK plugin is built for one Houdini version and loads in that one only, so this needs
# the Houdini it is for: VGS_HOUDINI_ROOT, or HFS in the environment, or else the newest
# Houdini installed (from SideFX's registry entries, or the default install folders).
# Without one, the plugin is skipped.

set(VGS_HOUDINI_VERSION 1.0.0)

set(VGS_HOUDINI_ROOT "" CACHE PATH
    "Houdini installation to build the Houdini plugin against (the folder holding toolkit/); empty to find one")

# A Houdini is a folder with the HDK's CMake package in it; the Houdini Server and the
# Launcher are installed beside it under similar names and are not.
function(vgs_is_houdini folder result)
  if(folder AND EXISTS "${folder}/toolkit/cmake/HoudiniConfig.cmake")
    set(${result} TRUE PARENT_SCOPE)
  else()
    set(${result} FALSE PARENT_SCOPE)
  endif()
endfunction()

set(houdini_root "${VGS_HOUDINI_ROOT}")
vgs_is_houdini("${houdini_root}" found)
if(NOT found AND DEFINED ENV{HFS})
  set(houdini_root "$ENV{HFS}")
  vgs_is_houdini("${houdini_root}" found)
endif()
if(NOT found)
  # Every Houdini installed, as "version|folder", newest first. SideFX's installer lists
  # them in the registry, wherever they went; the default folders catch the rest.
  set(candidates "")
  if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    cmake_host_system_information(RESULT versions QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Side Effects Software/Houdini" VALUE_NAMES)
    foreach(version IN LISTS versions)
      if(version MATCHES "^[0-9]+\\.")
        cmake_host_system_information(RESULT folder QUERY WINDOWS_REGISTRY
            "HKLM/SOFTWARE/Side Effects Software/Houdini" VALUE "${version}")
        file(TO_CMAKE_PATH "${folder}" folder)
        string(REGEX REPLACE "/+$" "" folder "${folder}")
        list(APPEND candidates "${version}|${folder}")
      endif()
    endforeach()
  endif()
  file(GLOB folders "C:/Program Files/Side Effects Software/Houdini *" "/opt/hfs*"
                    "/Applications/Houdini/Houdini*/Frameworks/Houdini.framework/Versions/Current/Resources")
  foreach(folder IN LISTS folders)
    if(folder MATCHES "([0-9]+\\.[0-9]+\\.[0-9]+)")
      list(APPEND candidates "${CMAKE_MATCH_1}|${folder}")
    endif()
  endforeach()
  list(SORT candidates COMPARE NATURAL ORDER DESCENDING)
  foreach(candidate IN LISTS candidates)
    string(REGEX REPLACE "^[^|]*\\|" "" folder "${candidate}")
    vgs_is_houdini("${folder}" found)
    if(found)
      set(houdini_root "${folder}")
      break()
    endif()
  endforeach()
endif()
if(NOT found)
  message(STATUS "Houdini plugin skipped: no Houdini found; set VGS_HOUDINI_ROOT to build it")
  return()
endif()
set(VGS_HOUDINI_ROOT "${houdini_root}")

find_package(Houdini REQUIRED PATHS "${VGS_HOUDINI_ROOT}/toolkit/cmake" NO_DEFAULT_PATH)
set(houdini_version "${Houdini_VERSION_MAJOR}.${Houdini_VERSION_MINOR}")
math(EXPR houdini_next_minor "${Houdini_VERSION_MINOR} + 1")
set(houdini_next "${Houdini_VERSION_MAJOR}.${houdini_next_minor}")
message(STATUS "Houdini plugin: building against Houdini ${Houdini_VERSION} in ${VGS_HOUDINI_ROOT}")

# The decoder and the player, compiled with this project's settings rather than the HDK's,
# into a library of their own. They share Houdini's C runtime, the DLL one: the plugin
# lives in Houdini's process and hands nothing across it but plain arrays, but mixing
# runtimes within one DLL does not link.
vgs_core_quiet_vendor_sources()
add_library(vgshoudini_player STATIC
    ${VGS_CORE_SHARED_SOURCES}
    ${CMAKE_SOURCE_DIR}/decoder/src/vgsdecoder.cpp
    ${CMAKE_SOURCE_DIR}/plugins/vgsblender/src/vgsblender.cpp)
target_include_directories(vgshoudini_player
    PUBLIC ${CMAKE_SOURCE_DIR}/plugins/vgsblender/include
    PRIVATE ${CMAKE_SOURCE_DIR}/decoder/include ${VGS_CORE_INCLUDE_DIRS})
target_compile_definitions(vgshoudini_player PUBLIC VGSBLENDER_STATIC)
set_target_properties(vgshoudini_player PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(vgshoudini_player PUBLIC Threads::Threads)
vgs_apply_settings(vgshoudini_player)
vgs_apply_cache_defaults(vgshoudini_player)

set(stage ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/package)

add_library(vgs_houdini SHARED
    src/plugin.cpp
    src/OBJ_VGSCapture.cpp
    src/OBJ_VGSCapture.h
    src/SOP_VGSCapture.cpp
    src/SOP_VGSCapture.h
    src/VGS_Parameters.cpp
    src/VGS_Parameters.h)
target_link_libraries(vgs_houdini PRIVATE vgshoudini_player Houdini)
# The HDK builds against the release runtime in every configuration.
set_target_properties(vgs_houdini PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
houdini_configure_target(vgs_houdini INSTDIR ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/dso)

# The package: the plugin, its icons, its help and its File > Import entry.
set(package_dir ${CMAKE_CURRENT_SOURCE_DIR}/package)
# The node icon: the project's logo_trans.png cropped to the mark, 128 pixels. PNG rather
# than logo.svg, whose clip paths not every SVG renderer draws.
set(logo ${package_dir}/icon.png)
set(zip ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/vgs-houdini-${VGS_HOUDINI_VERSION}-h${houdini_version}.zip)
configure_file(${package_dir}/vgs.json.in ${CMAKE_CURRENT_BINARY_DIR}/vgs.json @ONLY)

add_custom_target(vgs_houdini_package ALL
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${stage}/vgs/dso ${stage}/vgs/config/Icons
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/vgs.json ${stage}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${package_dir}/vgs ${stage}/vgs
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:vgs_houdini> ${stage}/vgs/dso
    COMMAND ${CMAKE_COMMAND} -E copy ${logo} ${stage}/vgs/config/Icons/SOP_vgs_capture.png
    COMMAND ${CMAKE_COMMAND} -E copy ${logo} ${stage}/vgs/config/Icons/OBJ_vgs_capture.png
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage}
            ${CMAKE_COMMAND} -E tar cf ${zip} --format=zip -- vgs.json vgs
    DEPENDS vgs_houdini ${logo} ${package_dir}/vgs.json.in
    COMMENT "Packaging the Houdini plugin ${VGS_HOUDINI_VERSION} for Houdini ${houdini_version}"
    VERBATIM)
add_dependencies(vgs_houdini_package vgs_houdini)

#   cmake --install build_win64 --config Release --component houdini --prefix D:/ship
install(FILES ${zip} ${CMAKE_CURRENT_SOURCE_DIR}/INSTALL.txt DESTINATION houdini COMPONENT houdini)

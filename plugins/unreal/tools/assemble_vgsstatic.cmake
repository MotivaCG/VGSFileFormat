# Assembles the VGSStatic plugin: its own files, plus what it shares with the VGS plugin,
# copied from there and renamed so that both plugins can be enabled in one project.
#
#   cmake -DVGS=<plugins/unreal/VGS> -DSTATIC=<plugins/unreal/VGSStatic> -DEDITOR=<editor>
#         -DSTAGE=<output plugin folder> -P assemble_vgsstatic.cmake
#
# Shared, from the VGS plugin (edit them there, never in the assembled copy):
#   Source/VGSSplatRenderer         -> Source/VGSStaticSplatRenderer   (renamed)
#   Shaders/Private/SplatRenderer   -> Shaders/Private/SplatRenderer   (renamed)
#   Shaders/Shared                  -> Shaders/Shared                  (renamed)
#   Resources/InverseToneCurve      -> Resources/InverseToneCurve
#   Source/VGSRuntime/Private/VGSSplatLayout.h -> Source/VGSStaticRuntime/Public (plain C++)
# From the VGS editor, for estimating normals at import:
#   editor/gaussiannormals.h, .cpp, normalrigs.cpp, dependencies/nanoflann
#     -> Source/VGSStaticEditor/Private/Normals (the .cpp files as .inl, built by VGSStaticNormals.cpp)
#
# The renaming is what makes the two plugins' modules, classes, global shader types, shader
# paths, console variables and stats distinct: VGSSplat -> VGSStaticSplat, VGSSPLAT ->
# VGSSTATICSPLAT, /Plugin/VGS/ -> /Plugin/VGSStatic/, the VGSShaders module -> VGSStaticShaders.
# The assembly fails if a name that would collide is left.

foreach(var VGS STATIC EDITOR STAGE)
  if(NOT ${var})
    message(FATAL_ERROR "assemble_vgsstatic.cmake: -D${var}= is required")
  endif()
endforeach()

file(REMOVE_RECURSE ${STAGE}/Source ${STAGE}/Shaders ${STAGE}/Resources/InverseToneCurve)
file(MAKE_DIRECTORY ${STAGE})
file(COPY ${STATIC}/VGSStatic.uplugin DESTINATION ${STAGE})
file(COPY ${STATIC}/Source DESTINATION ${STAGE})

function(rename_text text out)
  string(REPLACE "VGSSPLAT" "VGSSTATICSPLAT" text "${text}")
  string(REPLACE "VGSSplat" "VGSStaticSplat" text "${text}")
  string(REPLACE "/Plugin/VGS/" "/Plugin/VGSStatic/" text "${text}")
  string(REPLACE "\"VGSShaders\"" "\"VGSStaticShaders\"" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Copies a shared folder renamed, with a first line saying where it comes from.
function(copy_renamed from to label)
  file(GLOB_RECURSE files RELATIVE ${from} ${from}/*)
  foreach(rel ${files})
    file(READ ${from}/${rel} text)
    rename_text("${text}" text)
    # Nothing of the VGS plugin's names may be left in what is shared.
    string(REGEX MATCH "VGSSplat|VGSSPLAT|/Plugin/VGS/|\"VGSShaders\"" left "${text}")
    if(left)
      message(FATAL_ERROR "assemble_vgsstatic.cmake: ${from}/${rel} still says ${left} once renamed")
    endif()
    string(REPLACE "VGSSplat" "VGSStaticSplat" target_rel "${rel}")
    if(rel MATCHES "\\.(h|cpp|cs|usf|ush)$")
      set(text "// VGSStatic: assembled from plugins/unreal/VGS/${label}/${rel} by plugins/unreal/tools/assemble_vgsstatic.cmake; edit it there.\n${text}")
    endif()
    file(WRITE ${to}/${target_rel} "${text}")
  endforeach()
endfunction()

copy_renamed(${VGS}/Source/VGSSplatRenderer ${STAGE}/Source/VGSStaticSplatRenderer Source/VGSSplatRenderer)
copy_renamed(${VGS}/Shaders/Private/SplatRenderer ${STAGE}/Shaders/Private/SplatRenderer Shaders/Private/SplatRenderer)
copy_renamed(${VGS}/Shaders/Shared ${STAGE}/Shaders/Shared Shaders/Shared)
file(COPY ${VGS}/Resources/InverseToneCurve DESTINATION ${STAGE}/Resources)
file(COPY ${VGS}/Source/VGSRuntime/Private/VGSSplatLayout.h DESTINATION ${STAGE}/Source/VGSStaticRuntime/Public)

set(normals ${STAGE}/Source/VGSStaticEditor/Private/Normals)
file(COPY ${EDITOR}/gaussiannormals.h DESTINATION ${normals})
file(COPY_FILE ${EDITOR}/gaussiannormals.cpp ${normals}/gaussiannormals.inl)
file(COPY_FILE ${EDITOR}/normalrigs.cpp ${normals}/normalrigs.inl)
file(COPY ${EDITOR}/dependencies/nanoflann/nanoflann.hpp ${EDITOR}/dependencies/nanoflann/LICENSE.txt
     DESTINATION ${normals}/dependencies/nanoflann)

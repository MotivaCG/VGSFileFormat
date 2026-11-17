# Assembles the SGS plugin: its own files, plus what it shares with the VGS plugin,
# copied from there and renamed so that both plugins can be enabled in one project.
#
#   cmake -DVGS=<plugins/unreal/VGS> -DSTATIC=<plugins/unreal/SGS> -DEDITOR=<editor>
#         -DSTAGE=<output plugin folder> -P assemble_sgs.cmake
#
# Shared, from the VGS plugin (edit them there, never in the assembled copy):
#   Source/VGSSplatRenderer         -> Source/SGSSplatRenderer   (renamed)
#   Shaders/Private/SplatRenderer   -> Shaders/Private/SplatRenderer   (renamed)
#   Shaders/Shared                  -> Shaders/Shared                  (renamed)
#   Resources/InverseToneCurve      -> Resources/InverseToneCurve
#   Content/Icons (the actor's icon) -> Content/Icons
#   Source/VGSRuntime/Private/VGSSplatLayout.h -> Source/SGSRuntime/Public (plain C++)
# From the VGS editor, for estimating normals at import:
#   editor/gaussiannormals.h, .cpp, normalrigs.cpp, dependencies/nanoflann
#     -> Source/SGSEditor/Private/Normals (the .cpp files as .inl, built by SGSNormals.cpp)
#
# The renaming is what makes the two plugins' modules, classes, global shader types, shader
# paths, console variables and stats distinct: VGSSplat -> SGSSplat, VGSSPLAT ->
# SGSSPLAT, /Plugin/VGS/ -> /Plugin/SGS/, the VGSShaders module -> SGSShaders.
# The assembly fails if a name that would collide is left.

foreach(var VGS STATIC EDITOR STAGE)
  if(NOT ${var})
    message(FATAL_ERROR "assemble_sgs.cmake: -D${var}= is required")
  endif()
endforeach()

file(REMOVE_RECURSE ${STAGE}/Source ${STAGE}/Shaders ${STAGE}/Resources/InverseToneCurve ${STAGE}/Content/Icons)
file(MAKE_DIRECTORY ${STAGE})
file(COPY ${STATIC}/SGS.uplugin DESTINATION ${STAGE})
file(COPY ${STATIC}/Source DESTINATION ${STAGE})

function(rename_text text out)
  string(REPLACE "VGSSPLAT" "SGSSPLAT" text "${text}")
  string(REPLACE "VGSSplat" "SGSSplat" text "${text}")
  string(REPLACE "/Plugin/VGS/" "/Plugin/SGS/" text "${text}")
  string(REPLACE "\"VGSShaders\"" "\"SGSShaders\"" text "${text}")
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
      message(FATAL_ERROR "assemble_sgs.cmake: ${from}/${rel} still says ${left} once renamed")
    endif()
    string(REPLACE "VGSSplat" "SGSSplat" target_rel "${rel}")
    if(rel MATCHES "\\.(h|cpp|cs|usf|ush)$")
      set(text "// SGS: assembled from plugins/unreal/VGS/${label}/${rel} by plugins/unreal/tools/assemble_sgs.cmake; edit it there.\n${text}")
    endif()
    file(WRITE ${to}/${target_rel} "${text}")
  endforeach()
endfunction()

copy_renamed(${VGS}/Source/VGSSplatRenderer ${STAGE}/Source/SGSSplatRenderer Source/VGSSplatRenderer)
copy_renamed(${VGS}/Shaders/Private/SplatRenderer ${STAGE}/Shaders/Private/SplatRenderer Shaders/Private/SplatRenderer)
copy_renamed(${VGS}/Shaders/Shared ${STAGE}/Shaders/Shared Shaders/Shared)
file(COPY ${VGS}/Resources/InverseToneCurve DESTINATION ${STAGE}/Resources)
if(EXISTS ${STATIC}/Content/Icons)
  file(COPY ${STATIC}/Content/Icons DESTINATION ${STAGE}/Content)
endif()
file(COPY ${STATIC}/Resources DESTINATION ${STAGE})
if(EXISTS ${STATIC}/Content/Materials)
  file(COPY ${STATIC}/Content/Materials DESTINATION ${STAGE}/Content)
endif()
file(COPY ${VGS}/Source/VGSRuntime/Private/VGSSplatLayout.h DESTINATION ${STAGE}/Source/SGSRuntime/Public)

set(normals ${STAGE}/Source/SGSEditor/Private/Normals)
file(COPY ${EDITOR}/gaussiannormals.h DESTINATION ${normals})
file(COPY_FILE ${EDITOR}/gaussiannormals.cpp ${normals}/gaussiannormals.inl)
file(COPY_FILE ${EDITOR}/normalrigs.cpp ${normals}/normalrigs.inl)
file(COPY ${EDITOR}/dependencies/nanoflann/nanoflann.hpp ${EDITOR}/dependencies/nanoflann/LICENSE.txt
     DESTINATION ${normals}/dependencies/nanoflann)

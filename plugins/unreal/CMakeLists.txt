# The Unreal Engine plugin, assembled into the folder a project's Plugins directory takes:
#
#   <build>/plugins/unreal/<config>/VGS/                  the plugin, ready to copy or link
#   <build>/plugins/unreal/<config>/vgs-unreal-<version>.zip  the same, to unzip into Plugins/
#
# CMake does not build the plugin - Unreal does, when the project that holds it is built.
# What this step adds to the sources is what they need from the rest of the tree: the
# bridge's header to compile against, and vgsbridge.dll with its licence to load at run
# time. VGSRuntime.Build.cs looks for both inside the plugin, so the plugin is used from
# here and not from plugins/unreal/VGS directly.
#
# Only the plugin's own files go in the zip; whatever Unreal builds into the folder
# (Binaries/Win64, Intermediate) stays out of it. The staged Source/ is replaced on every
# build, and so is Shaders/, so a file removed from the tree does not linger there.
#
# Resources/InverseToneCurve holds the data of Epic's inverse tone curve LUTs, which the
# renderer copy builds its textures from (EPIC_RENDERER_COPY.md); the icon is added beside it.
#
# Content/ holds the editor's assets (the actor's icon), saved by Unreal 5.9 and committed as
# they are. A running editor keeps vgsbridge.dll open, so it is only replaced when it changed.
#
# The version comes from VGS.uplugin, the one place to bump it.

set(plugin VGS)
set(plugin_dir ${CMAKE_CURRENT_SOURCE_DIR}/${plugin})

file(STRINGS ${plugin_dir}/VGS.uplugin version_line REGEX "\"VersionName\"")
string(REGEX REPLACE ".*\"VersionName\" *: *\"([^\"]+)\".*" "\\1" plugin_version "${version_line}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${plugin_dir}/VGS.uplugin)

file(GLOB_RECURSE plugin_sources CONFIGURE_DEPENDS ${plugin_dir}/Source/* ${plugin_dir}/Shaders/* ${plugin_dir}/Content/* ${plugin_dir}/Resources/*)

set(out ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>)
set(stage ${out}/${plugin})
set(zip ${out}/vgs-unreal-${plugin_version}.zip)
set(bridge_header ${CMAKE_SOURCE_DIR}/plugins/vgsbridge/include/vgsbridge.h)
set(decoder_license ${CMAKE_SOURCE_DIR}/decoder/LICENSE.md)
set(icon ${CMAKE_SOURCE_DIR}/logo.png)
set(third_party_bin ${stage}/Binaries/ThirdParty/vgsbridge/Win64)

add_custom_target(vgs_unreal_plugin ALL
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${stage}/Source
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${stage}/Shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory ${stage}/Source/ThirdParty/vgsbridge/include
    COMMAND ${CMAKE_COMMAND} -E make_directory ${third_party_bin}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${stage}/Resources
    COMMAND ${CMAKE_COMMAND} -E copy ${plugin_dir}/VGS.uplugin ${stage}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${plugin_dir}/Source ${stage}/Source
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${plugin_dir}/Shaders ${stage}/Shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${plugin_dir}/Content ${stage}/Content
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${plugin_dir}/Resources ${stage}/Resources
    COMMAND ${CMAKE_COMMAND} -E copy ${bridge_header} ${stage}/Source/ThirdParty/vgsbridge/include
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vgsbridge> ${third_party_bin}
    COMMAND ${CMAKE_COMMAND} -E copy ${decoder_license} ${third_party_bin}/LICENSE.md
    COMMAND ${CMAKE_COMMAND} -E copy ${icon} ${stage}/Resources/Icon128.png
    COMMAND ${CMAKE_COMMAND} -E chdir ${out}
            ${CMAKE_COMMAND} -E tar cf ${zip} --format=zip --
            ${plugin}/VGS.uplugin ${plugin}/Source ${plugin}/Shaders ${plugin}/Content ${plugin}/Resources ${plugin}/Binaries/ThirdParty
    DEPENDS vgsbridge ${plugin_dir}/VGS.uplugin ${plugin_sources} ${bridge_header} ${decoder_license} ${icon}
    COMMENT "Assembling the Unreal plugin ${plugin} ${plugin_version}"
    VERBATIM)
add_dependencies(vgs_unreal_plugin vgsbridge)

#   cmake --install build_win64 --config Release --component unreal --prefix D:/ship
install(FILES ${zip} DESTINATION unreal COMPONENT unreal)

# ---- VGSStatic: static Gaussian splat characters from PLY files ---------------------------
#
#   <build>/plugins/unreal/<config>/VGSStatic/                    the plugin
#   <build>/plugins/unreal/<config>/vgsstatic-unreal-<version>.zip
#
# Shares the renderer copy, its shaders and LUTs, and the float layout with the VGS plugin,
# copied from plugins/unreal/VGS and renamed (tools/assemble_vgsstatic.cmake), so the two can
# be enabled in one project; and the VGS editor's normals estimate, for PLYs without normals.
# It needs no vgsbridge.

set(static_dir ${CMAKE_CURRENT_SOURCE_DIR}/VGSStatic)
file(STRINGS ${static_dir}/VGSStatic.uplugin static_version_line REGEX "\"VersionName\"")
string(REGEX REPLACE ".*\"VersionName\" *: *\"([^\"]+)\".*" "\\1" static_version "${static_version_line}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${static_dir}/VGSStatic.uplugin)
file(GLOB_RECURSE static_sources CONFIGURE_DEPENDS ${static_dir}/Source/*)
set(static_stage ${out}/VGSStatic)
set(static_zip ${out}/vgsstatic-unreal-${static_version}.zip)
set(editor_dir ${CMAKE_SOURCE_DIR}/editor)

add_custom_target(vgs_static_unreal_plugin ALL
    COMMAND ${CMAKE_COMMAND} -DVGS=${plugin_dir} -DSTATIC=${static_dir} -DEDITOR=${editor_dir} -DSTAGE=${static_stage}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/assemble_vgsstatic.cmake
    COMMAND ${CMAKE_COMMAND} -E copy ${icon} ${static_stage}/Resources/Icon128.png
    COMMAND ${CMAKE_COMMAND} -E chdir ${out}
            ${CMAKE_COMMAND} -E tar cf ${static_zip} --format=zip --
            VGSStatic/VGSStatic.uplugin VGSStatic/Source VGSStatic/Shaders VGSStatic/Resources
    DEPENDS ${static_dir}/VGSStatic.uplugin ${static_sources} ${plugin_sources}
            ${editor_dir}/gaussiannormals.h ${editor_dir}/gaussiannormals.cpp ${editor_dir}/normalrigs.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/assemble_vgsstatic.cmake ${icon}
    COMMENT "Assembling the Unreal plugin VGSStatic ${static_version}"
    VERBATIM)

install(FILES ${static_zip} DESTINATION unreal COMPONENT unreal)

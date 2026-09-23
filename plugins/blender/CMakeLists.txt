# The Blender add-on, packaged as the .zip Blender installs: Edit > Preferences > Get
# Extensions > Install from Disk.
#
#   <build>/plugins/blender/<config>/vgs/                  the add-on, unpacked
#   <build>/plugins/blender/<config>/vgs-<version>.zip     the same, to install
#
# The unpacked folder is there for development: point Blender at it (a junction into the
# user extensions directory does it) and a rebuild is picked up on restart.
#
# The version comes from blender_manifest.toml, which is what Blender reads, so there is
# one place to bump it.

set(addon vgs)
set(addon_dir ${CMAKE_CURRENT_SOURCE_DIR}/${addon})

file(STRINGS ${addon_dir}/blender_manifest.toml version_line REGEX "^version *=")
string(REGEX REPLACE "^version *= *\"([^\"]+)\".*" "\\1" addon_version "${version_line}")

set(addon_files
    blender_manifest.toml
    LICENSE.txt
    __init__.py
    native.py
    playback.py
    properties.py
    operators.py
    interface.py
    logo.png)
set(addon_sources "")
foreach(file ${addon_files})
  list(APPEND addon_sources ${addon_dir}/${file})
endforeach()

set(stage ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${addon})
set(zip ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${addon}-${addon_version}.zip)

# The manifest sits at the root of the zip, not in a folder, which is what Blender expects
# of an extension package.
add_custom_target(vgs_blender_addon ALL
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${stage}/bin
    COMMAND ${CMAKE_COMMAND} -E copy ${addon_sources} ${stage}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:vgsblender> ${stage}/bin
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage}
            ${CMAKE_COMMAND} -E tar cf ${zip} --format=zip --
            ${addon_files} bin/$<TARGET_FILE_NAME:vgsblender>
    DEPENDS vgsblender ${addon_sources}
    COMMENT "Packaging the Blender add-on ${addon} ${addon_version}"
    VERBATIM)
add_dependencies(vgs_blender_addon vgsblender)

# Beside INSTALL/decoder and INSTALL/encoder, as its own component:
#
#   cmake --install build_win64 --config Release --component blender --prefix D:/ship
install(FILES ${zip} DESTINATION blender COMPONENT blender)

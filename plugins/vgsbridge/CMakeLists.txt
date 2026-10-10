# vgsbridge: the native component host integrations load to play captures - the decoder,
# and a thread per capture that decodes ahead of the playhead. It is the boundary between
# the closed VGS code and integrations that must stay free software, such as the Blender
# add-on (GPL); include/vgsbridge.h says why it is shaped the way it is.
#
# It compiles the decoder's sources itself rather than linking libvgsdecoder, for one
# reason: the C runtime. This library is loaded into somebody else's process, and a
# machine without the Visual C++ redistributable should still be able to use it, so it
# links the runtime statically - which the decoder library, built against the shared
# runtime, cannot be mixed with. What it compiles is the decoder's list and nothing
# else, so no authoring code and no signing key end up in here either.

vgs_core_quiet_vendor_sources()

add_library(vgsbridge SHARED
    ${VGS_CORE_SHARED_SOURCES}
    ${CMAKE_SOURCE_DIR}/decoder/src/vgsdecoder.cpp
    ${CMAKE_SOURCE_DIR}/decoder/include/vgsdecoder/vgsdecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vgsbridge.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vgsbridge.cpp)

target_include_directories(vgsbridge
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/decoder/include ${VGS_CORE_INCLUDE_DIRS})
target_compile_definitions(vgsbridge PRIVATE VGSBRIDGE_BUILD)

# vgsbridge.dll, not libvgsbridge.so: integrations look for one name on every platform.
set_target_properties(vgsbridge PROPERTIES
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Threads REQUIRED)
target_link_libraries(vgsbridge PRIVATE Threads::Threads)

vgs_apply_settings(vgsbridge)
vgs_apply_cache_defaults(vgsbridge)

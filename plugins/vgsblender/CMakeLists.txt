# The native half of the Blender add-on: the decoder, and a thread per capture that
# decodes ahead of the playhead.
#
# It compiles the decoder's sources itself rather than linking libvgsdecoder, for one
# reason: the C runtime. This library is loaded into somebody else's process, and a
# machine without the Visual C++ redistributable should still be able to use it, so it
# links the runtime statically - which the decoder library, built against the shared
# runtime, cannot be mixed with. What it compiles is the decoder's list and nothing
# else, so no authoring code and no signing key end up in here either.

vgs_core_quiet_vendor_sources()

add_library(vgsblender SHARED
    ${VGS_CORE_SHARED_SOURCES}
    ${CMAKE_SOURCE_DIR}/decoder/src/vgsdecoder.cpp
    ${CMAKE_SOURCE_DIR}/decoder/include/vgsdecoder/vgsdecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vgsblender.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vgsblender.cpp)

target_include_directories(vgsblender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/decoder/include ${VGS_CORE_INCLUDE_DIRS})
target_compile_definitions(vgsblender PRIVATE VGSBLENDER_BUILD)

# vgsblender.dll, not libvgsblender.so: the add-on looks for one name on every platform.
set_target_properties(vgsblender PROPERTIES
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Threads REQUIRED)
target_link_libraries(vgsblender PRIVATE Threads::Threads)

vgs_apply_settings(vgsblender)
vgs_apply_cache_defaults(vgsblender)

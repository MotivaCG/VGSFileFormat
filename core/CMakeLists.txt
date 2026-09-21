# The codec itself. This directory builds nothing: it hands the other two a list of
# sources each, so that what a library contains is a decision made here, once.
#
# The split is the point. Anything that writes a capture - the container writer, the MINT
# import, the signing key - is in the authoring list, and only the encoder compiles it.
# The decoder gets the shared list and nothing else, so a library that ships to a browser
# or to a customer cannot produce a file and holds no private key to produce it with.
#
# mgscodec.cpp is in the shared list although it codes in both directions: it is the rANS
# attribute codec, symmetric by construction and impractical to cut in half. It matters
# less than it looks, because a capture is only valid once signed, and the key for that is
# on the other side of this line.

set(core ${CMAKE_CURRENT_SOURCE_DIR})
set(VGS_CORE_TWEETNACL ${core}/crypto/tweetnacl.c PARENT_SCOPE)
set(VGS_CORE_TWEETNACL ${core}/crypto/tweetnacl.c)

set(VGS_CORE_INCLUDE_DIRS
    ${core}/include/vgs
    ${core}/crypto
    ${core}/src
    PARENT_SCOPE)

# Read a capture, check that it is genuine, decode its frames.
set(VGS_CORE_SHARED_SOURCES
    ${core}/src/vgscontainer.cpp
    ${core}/src/vgsframe.cpp
    ${core}/src/mgscodec.cpp
    ${core}/src/vgsinternal.h
    ${core}/include/vgs/vgscodec.h
    ${core}/include/vgs/vgsframe.h
    ${core}/include/vgs/mgscodec.h
    ${core}/crypto/vgscrypto.cpp
    ${core}/crypto/vgscrypto.h
    ${core}/crypto/vgspublickey.h
    ${core}/crypto/tweetnacl.c
    ${core}/crypto/tweetnacl.h
    PARENT_SCOPE)

# Write one. Compiled only where VGS_AUTHORING is defined; vgskeys.h enforces that.
set(VGS_CORE_AUTHORING_SOURCES
    ${core}/src/vgsencode.cpp
    ${core}/crypto/vgssign.cpp
    ${core}/crypto/vgssign.h
    ${core}/crypto/vgskeys.h
    PARENT_SCOPE)

# TweetNaCl is the reference implementation, vendored unchanged. It is C, and it is not
# ours to tidy, so it builds as C with its own warnings quiet. Source properties are per
# directory, so each library calls this for itself rather than inheriting anything.
function(vgs_core_quiet_vendor_sources)
  set_source_files_properties(${VGS_CORE_TWEETNACL} PROPERTIES LANGUAGE C)
  if(MSVC)
    set_source_files_properties(${VGS_CORE_TWEETNACL} PROPERTIES COMPILE_OPTIONS "/W0")
  else()
    set_source_files_properties(${VGS_CORE_TWEETNACL} PROPERTIES COMPILE_OPTIONS "-w")
  endif()
endfunction()

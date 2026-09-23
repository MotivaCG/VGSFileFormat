# Integrations with host applications, built on the decoder.
#
#   vgsblender/   the native half: a library that plays captures ahead of a host's playhead
#   blender/      the Blender add-on that loads it, and the .zip that installs both
#
# The line between the two is the licence line. The add-on is Python written against
# Blender's API and is GPL, as everything written against that API is; the library holds
# the decoder and ships under the decoder's own terms. plugins/README.md says more.

add_subdirectory(vgsblender)
add_subdirectory(blender)

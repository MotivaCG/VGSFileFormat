# The test suite.
#
# Two kinds. The ones that need a capture to work on are skipped unless one is supplied,
# because a repository cannot carry a 300 MB fixture:
#
#   cmake -S . -B build_win64 -DVGS_TEST_MINT=D:/captures/boxing.mint
#   ctest --test-dir build_win64 -C Release --output-on-failure
#
# The ones that do not need a fixture always run, and the most important of them is one of
# those: keyleak reads the built artefacts and fails if the signing key is inside anything
# the decoder ships. That check has to run on every build, not only on the machines that
# happen to have a capture lying about.

set(VGS_TEST_MINT "" CACHE FILEPATH "A .mint capture for the tests that need one")

# ---- always ----------------------------------------------------------------------------

add_executable(vgs_keyleak keyleak.cpp)
target_include_directories(vgs_keyleak PRIVATE ${VGS_CORE_INCLUDE_DIRS})
vgs_apply_settings(vgs_keyleak)

# The decoder must not contain the key; the encoder must, or the test is looking for the
# wrong bytes and proves nothing. Generator expressions because the path of a built
# artefact is only known at build time on a multi-config generator.
add_test(NAME keyleak_decoder
         COMMAND vgs_keyleak --absent
                 $<TARGET_FILE:vgsdecoder>
                 $<TARGET_FILE:vgsinfo>
                 $<TARGET_FILE:vgsplay>
                 $<TARGET_FILE:vgspagecost>)
add_test(NAME keyleak_encoder
         COMMAND vgs_keyleak --present
                 $<TARGET_FILE:vgsencoder>
                 $<TARGET_FILE:vgsencode>)

# ---- with a capture ---------------------------------------------------------------------

if(VGS_TEST_MINT STREQUAL "")
  message(STATUS "VGS_TEST_MINT is not set: the tests that need a capture will be skipped")
  return()
endif()

if(NOT EXISTS "${VGS_TEST_MINT}")
  message(FATAL_ERROR "VGS_TEST_MINT is set to ${VGS_TEST_MINT}, which does not exist")
endif()

set(fixture ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.vgs)

add_executable(vgs_roundtrip roundtrip.cpp)
target_link_libraries(vgs_roundtrip PRIVATE ${VGS_ENCODER_LINK} ${VGS_DECODER_LINK})
vgs_apply_settings(vgs_roundtrip)

add_executable(vgs_tamper tampertest.cpp)
target_link_libraries(vgs_tamper PRIVATE ${VGS_DECODER_LINK})
vgs_apply_settings(vgs_tamper)

# roundtrip writes the capture the others read, so it goes first and they wait for it.
add_test(NAME roundtrip COMMAND vgs_roundtrip ${VGS_TEST_MINT} ${fixture})
set_tests_properties(roundtrip PROPERTIES FIXTURES_SETUP capture)

add_test(NAME tamper COMMAND vgs_tamper ${fixture})
set_tests_properties(tamper PROPERTIES FIXTURES_REQUIRED capture)

# The tools are part of the delivery, so they are run rather than only built: a sample
# that does not survive its own capture is not a sample anybody should copy from.
add_test(NAME tool_info COMMAND vgsinfo ${fixture})
# vgsplay exercises both delivery modes in one run, which is why it is worth running as
# a test: the packed path is not touched by anything else in this suite.
add_test(NAME tool_play COMMAND vgsplay ${fixture})
add_test(NAME tool_dump COMMAND vgsdump ${fixture} 0.5 ${CMAKE_CURRENT_BINARY_DIR}/frame.ply)
# vgspagecost walks every chunk at every detail level, which makes it the one run in
# this suite that goes through Detail::Positions via prepare() on a whole capture.
add_test(NAME tool_pagecost COMMAND vgspagecost ${fixture} --rounds 1)
set_tests_properties(tool_info tool_play tool_dump tool_pagecost PROPERTIES FIXTURES_REQUIRED capture)

# A .pgs of the same source must decode to the same thing: the two extensions are one
# container and a reader is not allowed to need to know which it has.
add_test(NAME plain_encode
         COMMAND vgsencode ${VGS_TEST_MINT} ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.pgs
                 --plain --quiet)
add_test(NAME plain_info COMMAND vgsinfo ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.pgs)
set_tests_properties(plain_encode PROPERTIES FIXTURES_SETUP plain)
set_tests_properties(plain_info PROPERTIES FIXTURES_REQUIRED plain)

# The web read-ahead's byte bookkeeping, which decides whether a capture is downloaded
# once or twice and is the kind of thing only a test can keep honest. It needs no
# capture, no WebAssembly and no browser, so it runs wherever node does and is simply
# skipped where it does not.
find_program(VGS_NODE NAMES node node.exe)
if(VGS_NODE)
  add_test(NAME web_readahead
           COMMAND ${VGS_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/spans.mjs)
endif()

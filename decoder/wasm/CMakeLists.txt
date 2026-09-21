# The WebAssembly decoder. Only reached under Emscripten, from the top level with
# -DVGS_BUILD_WASM=ON.
#
# The module is the same decoder sources the native library is built from, plus the thin
# entry points in vgswasm.cpp. Nothing about the format is reimplemented for the browser.

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "VGS_BUILD_WASM needs the Emscripten toolchain: run emcmake cmake")
endif()

# The native C interface is left out: vgswasm.cpp is this build's C interface, and the two
# use the same names for the same ideas - vgs_open, vgs_set_time, vgs_bounds - because both
# are the decoder seen from C. Linking them together would be two definitions of each.
set(VGS_WASM_SOURCES ${VGS_DECODER_SOURCES})
list(FILTER VGS_WASM_SOURCES EXCLUDE REGEX "vgsdecoder_c\.(cpp|h)$")

add_executable(vgsdecoder_wasm ${VGS_WASM_SOURCES} vgswasm.cpp)
target_include_directories(vgsdecoder_wasm
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include ${VGS_CORE_INCLUDE_DIRS})

# Native WebAssembly exceptions rather than the JavaScript emulation: the codec reports
# every failure by throwing, so this is on the path that decides whether a file is
# genuine, and the emulated form costs both size and speed on it.
#
# Position independent code is turned off again here. It is on for the project because a
# static library may end up in a shared object, which does not apply to a WebAssembly
# module and costs a few percent on the decode loop.
#
# Link time optimisation matters more than usual for this module: the hot path runs from
# the container reader through the entropy decoder into the frame evaluator, three
# translation units deep, and nothing inlines across them without it.
set_target_properties(vgsdecoder_wasm PROPERTIES POSITION_INDEPENDENT_CODE OFF)
vgs_apply_cache_defaults(vgsdecoder_wasm)
# No -msimd128. It was measured: decompression did not move outside the noise and the
# module grew by a quarter, because the compiler cannot vectorise what dominates here.
# An entropy decoder is serial by construction - each symbol depends on the state the
# previous one left - so the gain would have to come from the format carrying several
# independent streams to decode in step, not from a compiler flag.
target_compile_options(vgsdecoder_wasm PRIVATE -O3 -flto -fwasm-exceptions)

set(VGS_WASM_EXPORTS
    _malloc,_free
    ,_vgs_error,_vgs_staging
    ,_vgs_head_reserve,_vgs_structural_size,_vgs_open,_vgs_close,_vgs_prime_reserve
    ,_vgs_string,_vgs_tag_count,_vgs_tag,_vgs_number,_vgs_bounds
    ,_vgs_chunk,_vgs_chunk_at,_vgs_chunk_bounds
    ,_vgs_set_time,_vgs_prepare,_vgs_prepared_fraction
    ,_vgs_set_output,_vgs_output,_vgs_chunk_layout,_vgs_instant,_vgs_frame,_vgs_frame_sh_coefficients,_vgs_frame_chunk
    ,_vgs_positions_at,_vgs_positions_data
    ,_vgs_set_cache_policy,_vgs_is_chunk_cached,_vgs_cached_chunk_count,_vgs_cached_bytes,_vgs_release_cache
    ,_vgs_has_payload,_vgs_payload_format,_vgs_payload_offset,_vgs_payload_size,_vgs_payload)
string(REPLACE ";" "" VGS_WASM_EXPORTS "${VGS_WASM_EXPORTS}")

# SINGLE_FILE embeds the .wasm in the .js as base64. It costs about a third in transfer
# size, and it buys a module that loads from anywhere without the server having been told
# what application/wasm is - which has bitten this project before, and which a customer
# dropping the viewer on their own hosting cannot be asked to get right.
# Web and worker only, by default. The node branch of Emscripten's glue imports node's
# own modules, and every web bundler follows that import and warns about it on every
# build - in a module a web project is meant to drop in and forget. Turn VGS_WASM_NODE on
# to run this module under node instead.
option(VGS_WASM_NODE "Also build the WebAssembly decoder for node" OFF)
set(VGS_WASM_ENVIRONMENT "web,worker")
if(VGS_WASM_NODE)
  set(VGS_WASM_ENVIRONMENT "web,worker,node")
endif()

target_link_options(vgsdecoder_wasm PRIVATE
    -O3 -flto -fwasm-exceptions
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createVgsDecoder -sSINGLE_FILE=1
    -sENVIRONMENT=${VGS_WASM_ENVIRONMENT}
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=2147483648
    -sEXPORTED_FUNCTIONS=${VGS_WASM_EXPORTS}
    -sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,HEAPF32,HEAPF64)

set_target_properties(vgsdecoder_wasm PROPERTIES OUTPUT_NAME vgsdecoder SUFFIX .js)

# The module and the JavaScript that drives it install together: neither is any use on its
# own, and a web project wants both dropped into the same folder.
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/vgsdecoder.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/vgs.mjs
    ${CMAKE_CURRENT_SOURCE_DIR}/js/vgssource.mjs
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    DESTINATION ${VGS_DECODER_PREFIX}/web)

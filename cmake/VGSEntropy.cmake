# Pinned, offline Rust/Pcodec build. Native consumers receive a prebuilt staticlib;
# only building VGS itself requires Rust. FSE sources join each codec target.
include_guard(GLOBAL)
include(GNUInstallDirs)
get_filename_component(VGS_ENTROPY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VGS_ENTROPY_SOURCES
  "${VGS_ENTROPY_ROOT}/core/src/pcodec_bridge.cpp"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/fseU16.c"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/fse_compress.c"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/fse_decompress.c"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/entropy_common.c"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/hist.c"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/huf_compress.c"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/huf_decompress.c"
  "${VGS_ENTROPY_ROOT}/core/third_party/fse/debug.c")
find_program(VGS_CARGO NAMES cargo HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin" REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VGS_RUST_TARGET "" CACHE STRING "Rust target triple for native cross compilation")
option(VGS_WASM_SIMD128 "Use portable SIMD128 in Pcodec (no AVX2)" ON)
set(pco_target_args)
set(pco_target_subdir "")
set(pco_flags "-C embed-bitcode=no")
if(EMSCRIPTEN)
  set(pco_target wasm32-unknown-unknown)
  if(VGS_WASM_SIMD128)
    string(APPEND pco_flags " -C target-feature=+simd128")
  else()
    string(APPEND pco_flags " -C target-feature=-simd128")
  endif()
elseif(VGS_RUST_TARGET)
  set(pco_target "${VGS_RUST_TARGET}")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(pco_target i686-pc-windows-msvc)
elseif(WIN32 AND CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
  set(pco_target aarch64-pc-windows-msvc)
elseif(CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "Set VGS_RUST_TARGET for native cross compilation")
endif()
if(pco_target)
  set(pco_target_args --target "${pco_target}")
  set(pco_target_subdir "${pco_target}/")
endif()
if(WIN32 AND NOT EMSCRIPTEN)
  set(pco_filename vgs_pcodec.lib)
else()
  set(pco_filename libvgs_pcodec.a)
endif()
set(pco_root "${CMAKE_BINARY_DIR}/pcodec")
file(WRITE "${pco_root}/cargo-config.toml"
  "[source.crates-io]\nreplace-with = 'vendored-sources'\n[source.vendored-sources]\ndirectory = '${VGS_ENTROPY_ROOT}/core/third_party/rust-vendor'\n")
file(GLOB_RECURSE pco_sources CONFIGURE_DEPENDS
  "${VGS_ENTROPY_ROOT}/core/pcodec/*.rs" "${VGS_ENTROPY_ROOT}/core/pcodec/Cargo.*"
  "${VGS_ENTROPY_ROOT}/core/third_party/pcodec/*.rs" "${VGS_ENTROPY_ROOT}/core/third_party/pcodec/Cargo.toml"
  "${VGS_ENTROPY_ROOT}/core/third_party/pcodec/*/Cargo.toml")
set(pco_output "${pco_root}/$<CONFIG>/vgs_pcodec${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(pco_raw "${pco_root}/$<CONFIG>/${pco_target_subdir}release/${pco_filename}")
if(EMSCRIPTEN)
  get_filename_component(ems_upstream "${EMSCRIPTEN_ROOT_PATH}/.." ABSOLUTE)
  find_program(VGS_LLVM_AR NAMES llvm-ar HINTS "${ems_upstream}/bin" REQUIRED)
  set(pco_finish "${Python3_EXECUTABLE}" "${VGS_ENTROPY_ROOT}/cmake/strip_rust_features.py"
      "${pco_raw}" "${pco_output}" "${VGS_LLVM_AR}")
else()
  set(pco_finish "${CMAKE_COMMAND}" -E copy_if_different "${pco_raw}" "${pco_output}")
endif()
add_custom_command(OUTPUT "${pco_output}"
  COMMAND "${CMAKE_COMMAND}" -E env "RUSTFLAGS=${pco_flags}" "PCO_DISMISS_BUILD_WARNINGS=1"
      "CARGO_PROFILE_RELEASE_DEBUG=$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>,$<BOOL:${VGS_WASM_DEBUG_INFO}>>,2,0>"
      "${VGS_CARGO}" --config "${pco_root}/cargo-config.toml" build --offline --locked
      --manifest-path "${VGS_ENTROPY_ROOT}/core/pcodec/Cargo.toml" --release --lib
      ${pco_target_args} --target-dir "${pco_root}/$<CONFIG>"
  COMMAND ${pco_finish}
  DEPENDS ${pco_sources} "${VGS_ENTROPY_ROOT}/cmake/strip_rust_features.py"
  VERBATIM)
add_custom_target(vgs_pcodec_build DEPENDS "${pco_output}")
add_library(vgs_pcodec STATIC IMPORTED GLOBAL)
set_target_properties(vgs_pcodec PROPERTIES IMPORTED_LOCATION "${pco_root}/vgs_pcodec${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(pco_configs ${CMAKE_CONFIGURATION_TYPES} "${CMAKE_BUILD_TYPE}" Debug Release RelWithDebInfo MinSizeRel)
foreach(config IN LISTS pco_configs)
  if(config)
    string(TOUPPER "${config}" upper)
    set_property(TARGET vgs_pcodec PROPERTY "IMPORTED_LOCATION_${upper}" "${pco_root}/${config}/vgs_pcodec${CMAKE_STATIC_LIBRARY_SUFFIX}")
  endif()
endforeach()
add_dependencies(vgs_pcodec vgs_pcodec_build)
if(WIN32 AND NOT EMSCRIPTEN)
  set_property(TARGET vgs_pcodec PROPERTY INTERFACE_LINK_LIBRARIES userenv ws2_32 bcrypt ntdll)
elseif(NOT EMSCRIPTEN)
  find_package(Threads REQUIRED)
  set_property(TARGET vgs_pcodec PROPERTY INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS};m")
endif()
function(vgs_link_entropy target)
  target_include_directories(${target} PRIVATE "${VGS_ENTROPY_ROOT}/core/third_party/fse")
  target_compile_definitions(${target} PRIVATE FSEU16_MAX_SYMBOL_VALUE=4095)
  target_link_libraries(${target} PRIVATE "$<BUILD_INTERFACE:vgs_pcodec>" "$<INSTALL_INTERFACE:VGS::Pcodec>")
endfunction()
if(NOT EMSCRIPTEN)
  foreach(component decoder encoder)
    if(component STREQUAL "decoder")
      set(pco_package VGSDecoder)
    else()
      set(pco_package VGSEncoder)
    endif()
    install(FILES "${pco_output}" DESTINATION "${component}/${CMAKE_INSTALL_LIBDIR}"
        RENAME "${CMAKE_STATIC_LIBRARY_PREFIX}vgs_pcodec${CMAKE_STATIC_LIBRARY_SUFFIX}" COMPONENT ${component})
    configure_file("${VGS_ENTROPY_ROOT}/cmake/VGSPcodecConfig.cmake.in"
        "${CMAKE_BINARY_DIR}/VGSPcodecConfig.cmake" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/VGSPcodecConfig.cmake"
        DESTINATION "${component}/${CMAKE_INSTALL_LIBDIR}/cmake/${pco_package}" COMPONENT ${component})
  endforeach()
endif()
foreach(component decoder encoder)
  install(FILES "${VGS_ENTROPY_ROOT}/core/third_party/NOTICES.txt"
      DESTINATION "${component}" RENAME THIRD_PARTY_NOTICES.txt COMPONENT ${component})
endforeach()

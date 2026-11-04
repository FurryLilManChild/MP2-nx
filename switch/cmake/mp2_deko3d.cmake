# mp2-nx deko3d phase 0 (docs/deko3d-plan.md): the toolchain the deko3d renderer will need, linked into
# the NRO and exercised by a smoke test (switch/source/deko/dk_smoke.cpp) while the game draws with Dawn
# on NVK. Included from the Switch block of Echoes' CMakeLists.txt (switch/patches/echoes-*.patch).
#
#   deko3d 0.5.0 and uam 1.1.0 (zlib, with SwitchWaker's PATCHES.md): from MP2NX_SWITCHWAKER_DIR, a
#     SwitchWaker checkout at 084291f (switch/fetch_sources.sh), built here for the console
#   uam's Mesa and NVK's Mesa (libvulkan.a) share symbol names: the NRO links a copy of uamlib whose
#     clashing symbols are renamed (switch/deko/aurora_deko3d_uam_rename.cmake, Alek's, MIT)
#   Tint's GLSL writer: built with Dawn by switch/build_deps.sh (TINT_BUILD_GLSL_WRITER=ON, the two
#     dawn-switch-tint-*.patch options), its archives in $MP2NX_PREFIX/lib/tint
#   switch/deko/shader_translate.cpp: SwitchWaker's WGSL -> GLSL translation (MIT, via Alek)
# On by default in the deko3d test builds (MP2NX_APP_NAME other than mp2-nx), off for releases.
if(MP2NX_APP_NAME STREQUAL "mp2-nx")
  set(_dk_default OFF)
else()
  set(_dk_default ON)
endif()
option(MP2NX_DEKO3D "link the deko3d toolchain and its smoke test (phase 0)" ${_dk_default})
if(NOT MP2NX_DEKO3D)
  return()
endif()
set(MP2NX_SWITCHWAKER_DIR "/work/deps/switchwaker" CACHE PATH "SwitchWaker checkout at 084291f")
set(MP2NX_DAWN_SRC "/work/deps/dawn" CACHE PATH "Dawn's source tree (Tint's headers)")
set(_dk_sw "${MP2NX_SWITCHWAKER_DIR}/switch")
if(NOT EXISTS "${_dk_sw}/deko3d/CMakeLists.txt" OR NOT EXISTS "${_dk_sw}/uam/CMakeLists.txt")
  message(FATAL_ERROR "MP2NX_DEKO3D: ${MP2NX_SWITCHWAKER_DIR} is not a SwitchWaker checkout (switch/fetch_sources.sh)")
endif()
file(GLOB _dk_tint_libs "${MP2NX_PREFIX}/lib/tint/*.a")
if(NOT _dk_tint_libs OR NOT EXISTS "${MP2NX_PREFIX}/lib/tint/libtint_lang_glsl_writer.a")
  message(FATAL_ERROR "MP2NX_DEKO3D: Tint's GLSL writer is not in ${MP2NX_PREFIX}/lib/tint (switch/build_deps.sh)")
endif()
if(NOT EXISTS "${MP2NX_VULKAN_ROOT}/lib/libvulkan.a")
  message(FATAL_ERROR "MP2NX_DEKO3D: ${MP2NX_VULKAN_ROOT}/lib/libvulkan.a (NVK's Mesa) is needed to rename uam's")
endif()

# deko3d and uam for the console
set(NX_ROOT "${DEVKITPRO}/libnx")  # uam's CMakeLists: the console build (no host threads, no tool)
add_subdirectory("${_dk_sw}/deko3d" "${CMAKE_BINARY_DIR}/mp2nx_deko3d/deko3d" EXCLUDE_FROM_ALL)
add_subdirectory("${_dk_sw}/uam" "${CMAKE_BINARY_DIR}/mp2nx_deko3d/uam" EXCLUDE_FROM_ALL)
target_compile_options(uamlib PRIVATE -USWITCH)  # SWITCH is a token of uam's GLSL parser
set(_dk_uam_renamed "${CMAKE_BINARY_DIR}/mp2nx_deko3d/libuam_renamed.a")
add_custom_command(OUTPUT "${_dk_uam_renamed}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:uamlib>" "${_dk_uam_renamed}"
  COMMAND "${CMAKE_COMMAND}" "-DUAM=${_dk_uam_renamed}" "-DOTHER=${MP2NX_VULKAN_ROOT}/lib/libvulkan.a"
          "-DNM=${CMAKE_NM}" "-DOBJCOPY=${CMAKE_OBJCOPY}" "-DOUT=${CMAKE_BINARY_DIR}/mp2nx_deko3d/uam_renamed.txt"
          -P "${MP2NX_ROOT}/switch/deko/aurora_deko3d_uam_rename.cmake"
  DEPENDS uamlib "$<TARGET_FILE:uamlib>" "${MP2NX_ROOT}/switch/deko/aurora_deko3d_uam_rename.cmake"
  COMMENT "uam: renaming the symbols NVK's Mesa also defines"
  VERBATIM)
add_custom_target(mp2nx_uam_renamed DEPENDS "${_dk_uam_renamed}")
add_library(mp2nx_uam STATIC IMPORTED GLOBAL)
set_target_properties(mp2nx_uam PROPERTIES IMPORTED_LOCATION "${_dk_uam_renamed}"
  INTERFACE_INCLUDE_DIRECTORIES "${_dk_sw}/uam")
add_dependencies(mp2nx_uam mp2nx_uam_renamed)

# The translation and the smoke test
add_library(mp2nx_deko STATIC
  "${MP2NX_ROOT}/switch/deko/shader_translate.cpp"
  "${MP2NX_ROOT}/switch/source/deko/dk_smoke.cpp")
target_include_directories(mp2nx_deko PRIVATE "${MP2NX_ROOT}/switch/deko" "${_dk_sw}/uam"
  "${MP2NX_DAWN_SRC}" "${MP2NX_DAWN_SRC}/include" "${MP2NX_PREFIX}/include")
# Tint's build flags, as its targets export them (shader_translate.cpp reads its headers)
target_compile_definitions(mp2nx_deko PRIVATE TINT_BUILD_GLSL_WRITER=1 TINT_BUILD_WGSL_READER=1
  TINT_BUILD_WGSL_WRITER=1 TINT_BUILD_SPV_READER=1 TINT_BUILD_SPV_WRITER=1 TINT_BUILD_HLSL_WRITER=0
  TINT_BUILD_MSL_WRITER=0 TINT_BUILD_GLSL_VALIDATOR=0 TINT_BUILD_NULL_WRITER=1 TINT_BUILD_IR_BINARY=0
  TINT_BUILD_TINTD=0 TINT_BUILD_IS_LINUX=0 TINT_BUILD_IS_MAC=0 TINT_BUILD_IS_WIN=0)
set_target_properties(mp2nx_deko PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(mp2nx_deko PRIVATE "MP2NX_APP_NAME=\"${MP2NX_APP_NAME}\"")
add_dependencies(mp2nx_deko mp2nx_uam_renamed)
# Tint's archives also carry objects the monolithic Dawn has (the same build): one group, rescanned
target_link_libraries(mp2nx_deko PUBLIC mp2nx_uam deko3d_release "$<LINK_GROUP:RESCAN,${_dk_tint_libs}>")
message(STATUS "mp2-nx: deko3d phase 0 toolchain (deko3d, uam, Tint GLSL) from ${MP2NX_SWITCHWAKER_DIR}")

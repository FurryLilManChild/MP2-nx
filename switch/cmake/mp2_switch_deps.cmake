# mp2-nx: injected ahead of Echoes Unbound's top-level CMakeLists.txt with
# CMAKE_PROJECT_TOP_LEVEL_INCLUDES, so the upstream build is used as it is. It decides,
# before Aurora asks, where Dawn, SDL3, nod and the Vulkan driver come from on the Switch.
include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  return()
endif()

if(NOT DEVKITPRO)
  set(DEVKITPRO "$ENV{DEVKITPRO}")
endif()
if(NOT DEVKITPRO)
  set(DEVKITPRO /opt/devkitpro)
endif()

set(MP2NX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "mp2-nx checkout")
set(MP2NX_PREFIX "" CACHE PATH "Prefix with Dawn and SDL3 built for the Switch (switch/build_deps.sh)")
list(PREPEND CMAKE_PREFIX_PATH "${MP2NX_PREFIX}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${MP2NX_PREFIX}")

# Dawn and SDL3 are prebuilt into MP2NX_PREFIX (switch/build_deps.sh); nod is the shim below.
set(AURORA_DAWN_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_BUNDLED_DEPS ON CACHE BOOL "" FORCE)
set(MP_BUNDLED_DEPS ON CACHE BOOL "" FORCE)
set(MP_EMBED_RESOURCES ON CACHE BOOL "" FORCE)

# The REL module scoping step (tools/scope_module.py) partially links AArch64 objects.
set(MP_LD_BFD "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-ld" CACHE FILEPATH "" FORCE)
set(MP_OBJCOPY "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-objcopy" CACHE FILEPATH "" FORCE)
set(MP_NM "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-nm" CACHE FILEPATH "" FORCE)

# GNU ld has no RESCAN link feature declared by devkitA64's toolchain. Echoes links
# aurora_core and aurora_gx through one, and the Vulkan driver's archive needs one too.
set(CMAKE_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group" CACHE INTERNAL "" FORCE)
set(CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE CACHE INTERNAL "" FORCE)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE CACHE INTERNAL "" FORCE)

# newlib hides POSIX extensions (fdopen, strdup, ...) under strict C++; Alek's Switch
# toolchain sets the same.
add_compile_definitions(_DEFAULT_SOURCE)
# Stand-ins for the Linux headers Horizon lacks (dlfcn.h, sys/syscall.h, sys/un.h).
include_directories(BEFORE SYSTEM "${MP2NX_ROOT}/switch/compat")

# The NVK Vulkan driver (Mesa for Horizon), from the build image's portlibs.
set(MP2NX_VULKAN_ROOT "${DEVKITPRO}/portlibs/switch" CACHE PATH "Mesa for Horizon (NVK) prefix")
if(NOT TARGET mp2nx::vulkan)
  add_library(mp2nx::vulkan INTERFACE IMPORTED GLOBAL)
  set_target_properties(mp2nx::vulkan PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MP2NX_VULKAN_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "$<LINK_GROUP:RESCAN,${MP2NX_VULKAN_ROOT}/lib/libvulkan.a,expat,zstd,z,nx,stdc++,m>"
    INTERFACE_LINK_OPTIONS "-Wl,-u,vk_icdGetInstanceProcAddr")
endif()

# nod is Rust; Horizon builds no Rust. Alek's shim implements nod's C entry points over
# iso/gcm and, through disc_container.cpp, rvz/wia/gcz (CC0, from the Twilight Princess
# Switch port). Aurora skips its own nod provider when nod::nod already exists.
# The app's name: the NRO, its title and its SD folder (switch/include/mp2nx_app.h). "mp2-nx", the one release
# track since v0.5.0 (the deko3d test builds were "mp2-nx-deko" before that).
# The release version: the NRO's title info and the Switch menu's System page (MP2NX_VERSION, switch_menu.cpp).
set(MP2NX_VERSION "0.5.0")
set(MP2NX_APP_NAME "mp2-nx" CACHE STRING "the NRO and SD folder name (sdmc:/switch/<name>/)")
# (set only on the targets that use it, so a name change rebuilds those and not the whole game)
set(_mp2nx_disc "${MP2NX_ROOT}/switch/source/disc")
add_library(mp2nx_nod STATIC
  ${_mp2nx_disc}/iso_nod_shim.cpp
  ${_mp2nx_disc}/disc_container.cpp
  ${_mp2nx_disc}/wii_crypto.cpp
  ${_mp2nx_disc}/wii_crypto_switch.cpp
  ${_mp2nx_disc}/disc_header.cpp)
target_include_directories(mp2nx_nod
  PUBLIC "${MP2NX_ROOT}/switch/third_party/nod-c"
  PRIVATE "${MP2NX_ROOT}/switch/include" "${DEVKITPRO}/portlibs/switch/include")
target_compile_definitions(mp2nx_nod PRIVATE DUSK_WII_KEY_DIR="sdmc:/switch/${MP2NX_APP_NAME}")
target_compile_features(mp2nx_nod PUBLIC cxx_std_20)
# Created before the platform module turns PIC on for new targets; the NRO is a PIE, and
# position-dependent tables in .rodata would need relocating at run time.
set_target_properties(mp2nx_nod PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(mp2nx_nod PUBLIC
  ${DEVKITPRO}/portlibs/switch/lib/libzstd.a ${DEVKITPRO}/portlibs/switch/lib/libbz2.a
  ${DEVKITPRO}/portlibs/switch/lib/liblzma.a ${DEVKITPRO}/portlibs/switch/lib/libz.a)
add_library(nod::nod ALIAS mp2nx_nod)

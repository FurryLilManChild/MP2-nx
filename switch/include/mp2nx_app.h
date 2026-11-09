#pragma once
// mp2-nx: the app's name and its folder on the SD card. The CMake option MP2NX_APP_NAME sets both
// (switch/cmake/mp2_switch_deps.cmake): "mp2-nx", the one release track since v0.5.0. Another name (a side by
// side build) gets its own folder for logs, saves and caches.
#ifndef MP2NX_APP_NAME
#define MP2NX_APP_NAME "mp2-nx"
#endif
#ifndef MP2NX_VERSION
#define MP2NX_VERSION "0.5.0" // (CMake passes MP2NX_VERSION from switch/cmake/mp2_switch_deps.cmake)
#endif
#define MP2NX_APP_DIR "sdmc:/switch/" MP2NX_APP_NAME
// The release's folder: a build under another name without a disc of its own reads the disc from there.
#define MP2NX_MAIN_DIR "sdmc:/switch/mp2-nx"

#pragma once
// mp2-nx: the app's name and its folder on the SD card. The CMake option MP2NX_APP_NAME sets both
// (switch/cmake/mp2_switch_deps.cmake): "mp2-nx" for releases, "mp2-nx-deko" for the deko3d test builds,
// so the two can sit side by side on one SD card with their own logs, saves and caches.
#ifndef MP2NX_APP_NAME
#define MP2NX_APP_NAME "mp2-nx"
#endif
#define MP2NX_APP_DIR "sdmc:/switch/" MP2NX_APP_NAME
// The release's folder: a test build without a disc of its own reads the disc from there.
#define MP2NX_MAIN_DIR "sdmc:/switch/mp2-nx"

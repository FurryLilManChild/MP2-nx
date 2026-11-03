#!/usr/bin/env bash
# Builds Dawn and SDL3 for the Switch into $PREFIX, inside the build image
# (ghcr.io/danfromtico/switch-dev-legacy-vulkan). Sources:
#   DAWN_SRC: encounter/dawn 1155e0ed (Aurora's pin) with switch/patches/dawn-*.patch and
#             tools/fetch_dawn_dependencies.py already run
#   SDL_SRC:  devkitPro/SDL c329016c (the Switch branch Alek's port pins)
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAWN_SRC="${DAWN_SRC:-/work/deps/dawn}"
SDL_SRC="${SDL_SRC:-/work/deps/sdl}"
OUT="${OUT:-/work/out}"
PREFIX="${PREFIX:-$OUT/prefix}"
JOBS="${JOBS:-$(nproc)}"
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
export CFLAGS="-D_DEFAULT_SOURCE" CXXFLAGS="-D_DEFAULT_SOURCE"
TC=/opt/devkitpro/cmake/Switch.cmake

if [ ! -f "$PREFIX/lib/libwebgpu_dawn.a" ]; then
  cmake -S "$DAWN_SRC" -B "$OUT/build-dawn" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DDAWN_ENABLE_VULKAN=ON -DDAWN_ENABLE_NULL=ON -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF \
    -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_WEBGPU_ON_WEBGPU=OFF \
    -DDAWN_PLATFORM_SWITCH=ON -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF -DDAWN_USE_GLFW=OFF \
    -DDAWN_SUPPORTS_GLFW_FOR_WINDOWING=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF \
    -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_ENABLE_INSTALL=ON -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC \
    -DDAWN_FETCH_DEPENDENCIES=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_IR_BINARY=OFF \
    -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_HLSL_WRITER=OFF -DTINT_BUILD_MSL_WRITER=OFF -DABSL_PROPAGATE_CXX_STD=ON
  ninja -C "$OUT/build-dawn" -j"$JOBS" webgpu_dawn
  # Dawn's install step builds every target (some do not build for Horizon); only the monolithic
  # library and the headers are needed. The library is stripped of debug info: 1.7 GB to 40 MB.
  mkdir -p "$PREFIX/lib/cmake/Dawn" "$PREFIX/include"
  /opt/devkitpro/devkitA64/bin/aarch64-none-elf-strip --strip-debug \
    -o "$PREFIX/lib/libwebgpu_dawn.a" "$OUT/build-dawn/src/dawn/native/libwebgpu_dawn.a"
  cp -r "$DAWN_SRC/include/." "$PREFIX/include/"
  cp -r "$OUT/build-dawn/gen/include/." "$PREFIX/include/"
  cp "$REPO/switch/cmake/DawnConfig.cmake" "$PREFIX/lib/cmake/Dawn/DawnConfig.cmake"
fi

if [ ! -f "$PREFIX/lib/libSDL3.a" ]; then
  cmake -DSDL_SOURCE_DIR="$SDL_SRC" -DOVERLAY_DIR="$REPO/switch/sdl" -DOVERLAY_ID=mp2nx \
    -P "$REPO/switch/cmake/SwitchSdlOverlay.cmake"
  cmake -S "$SDL_SRC" -B "$OUT/build-sdl" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
  ninja -C "$OUT/build-sdl" -j"$JOBS"
  ninja -C "$OUT/build-sdl" install
fi
echo "deps ready in $PREFIX"

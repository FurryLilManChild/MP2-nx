#!/usr/bin/env bash
# mp2-nx Switch build, run on a Linux machine with Docker (the cloud workspace).
#   1. switch/fetch_sources.sh   sources into ../deps (once)
#   2. switch/build_deps.sh      Dawn and SDL3 for the Switch into ../out/prefix (once, slow)
#   3. this script               patches Echoes Unbound, configures and builds out/build-switch/mp2-nx.nro
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(cd "$REPO/.." && pwd)"
IMAGE="${IMAGE:-ghcr.io/danfromtico/switch-dev-legacy-vulkan:latest}"
JOBS="${JOBS:-$(nproc)}"

# Upstream stays on its pinned commit; the Switch changes are applied idempotently.
apply() { # dir patch
  if git -C "$1" apply --reverse --check "$2" 2>/dev/null; then return 0; fi
  git -C "$1" apply "$2"
}
apply "$REPO/extern/echoes" "$REPO/switch/patches/echoes-switch.patch"
apply "$REPO/extern/echoes/extern/aurora" "$REPO/switch/patches/aurora-switch.patch"

docker run --rm -v "$WORK:/work" "$IMAGE" bash -c "
  set -e; unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
  cmake -C /work/mp2-nx/switch/cmake/deps_cache.cmake -S /work/mp2-nx/extern/echoes -B /work/out/build-switch -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=/work/mp2-nx/switch/cmake/mp2_switch_deps.cmake \
    -DMP2NX_PREFIX=/work/out/prefix -DCMAKE_PREFIX_PATH=/work/out/prefix -DMP_SDK_HEADERS_ONLY=OFF
  ninja -C /work/out/build-switch -j$JOBS metroid_prime2_port_nro
"

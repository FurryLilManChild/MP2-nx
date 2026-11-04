#!/usr/bin/env bash
# Fetches the sources the Switch build needs into $DEPS (default ../deps next to the repo),
# with git for GitHub (the cloud workspace cannot download GitHub archive tarballs) and
# plain downloads elsewhere. The versions are Aurora's pins (extern/aurora/extern/CMakeLists.txt).
set -euo pipefail
DEPS="${DEPS:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/deps}"
mkdir -p "$DEPS"; cd "$DEPS"
gitpin() { # dir url ref (an existing checkout is left alone)
  [ -e "$1/.mp2nx-ok" ] && return 0
  if [ -e "$1" ]; then echo "keeping existing $DEPS/$1"; touch "$1/.mp2nx-ok"; return 0; fi
  git init -q "$1"; git -C "$1" remote add origin "$2"
  git -C "$1" fetch -q --depth 1 origin "$3"; git -C "$1" checkout -q FETCH_HEAD; touch "$1/.mp2nx-ok"
}
fetchx() { # dir url (an existing folder is left alone)
  [ -e "$1/.mp2nx-ok" ] && return 0
  if [ -e "$1" ]; then echo "keeping existing $DEPS/$1"; touch "$1/.mp2nx-ok"; return 0; fi
  rm -rf "$1.tmp"; mkdir -p "$1.tmp"; curl -sSL "$2" -o "$1.archive"
  case "$2" in *.zip) unzip -q "$1.archive" -d "$1.tmp";; *) tar xf "$1.archive" -C "$1.tmp";; esac
  mv "$1.tmp"/* "$1"; rmdir "$1.tmp"; rm -f "$1.archive"; touch "$1/.mp2nx-ok"
}
gitpin dawn     https://github.com/encounter/dawn 1155e0ed531126f33a1279afa029349651ca1c93
gitpin sdl      https://github.com/devkitPro/SDL c329016c0e338ab4397ad930f83330c9fe058348
gitpin abseil-cpp https://github.com/abseil/abseil-cpp refs/tags/20240722.0
gitpin xxhash   https://github.com/Cyan4973/xxHash refs/tags/v0.8.3
gitpin fmt      https://github.com/fmtlib/fmt refs/tags/12.1.0
gitpin zlib-ng  https://github.com/zlib-ng/zlib-ng refs/tags/2.3.3
gitpin libpng   https://github.com/pnggroup/libpng refs/tags/v1.6.58
gitpin imgui    https://github.com/ocornut/imgui refs/tags/v1.91.9b-docking
gitpin rmlui    https://github.com/mikke89/RmlUi 0ae381e00d7426762bb5ed897973366358b16642
gitpin tracy    https://github.com/wolfpld/tracy refs/tags/v0.14.1
fetchx freetype https://files.twilitrealm.dev/freetype-2.14.3.tar.gz
fetchx sqlite3  https://sqlite.org/2026/sqlite-amalgamation-3510300.zip
fetchx zstd     https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
if [ ! -e tracy/.mp2nx-patched ]; then
  git -C tracy apply "$(dirname "${BASH_SOURCE[0]}")/patches/tracy-horizon.patch" && touch tracy/.mp2nx-patched
fi
# Dawn: its third-party sources and the two Switch patches from Alek's port.
if [ ! -e dawn/.mp2nx-patched ]; then
  (cd dawn && python3 tools/fetch_dawn_dependencies.py)
  git -C dawn apply "$(dirname "${BASH_SOURCE[0]}")/patches/dawn-optional-layers.patch"
  git -C dawn apply "$(dirname "${BASH_SOURCE[0]}")/patches/dawn-worker-stack.patch"
  touch dawn/.mp2nx-patched
fi
# deko3d (docs/deko3d-plan.md): deko3d 0.5.0 and uam 1.1.0 with their patches come from SwitchWaker
# (centollOS, MIT; deko3d and uam zlib), and Tint's GLSL writer gets SwitchWaker's two options.
gitpin switchwaker https://github.com/centollOS/SwitchWaker 084291f791d40b0ce2d389659bad0acab375d5f3
if [ ! -e dawn/.mp2nx-tint-patched ]; then
  git -C dawn apply "$(dirname "${BASH_SOURCE[0]}")/patches/dawn-switch-tint-depth-zero-to-one.patch"
  git -C dawn apply "$(dirname "${BASH_SOURCE[0]}")/patches/dawn-switch-tint-position-y-up.patch"
  touch dawn/.mp2nx-tint-patched
fi
echo "sources ready in $DEPS"

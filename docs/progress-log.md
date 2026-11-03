# Progress log

Newest first.

## 2026-11-03: resumed, Switch build bring-up (cloud builds, per the owner)

- Renderer plan: deko3d is the goal (mkdd-nx 1.8.0 measured about 42 to 59.9 fps going from OpenGL to deko3d), but
  Echoes Unbound's Aurora is the heavily extended one from the Metroid Prime port (HDR, bloom, volumetric fog, PBR,
  extra GPU readbacks; about 30,000 lines). Alek's deko3d bridge (from SwitchWaker) skips anything that is not a
  standard Aurora draw, and Echoes uses some of that (scan peek, visor flare depth snapshot, EFB CPU copy). So:
  Stage 1 is Dawn on NVK with Echoes' Aurora unchanged; Stage 2 adds deko3d and extends it for those passes.
- Aurora bases found by tree comparison: Echoes' is upstream 011e331 (2026-09-17) plus its port work; Alek's is
  3227d76 (2026-09-29) plus Switch and deko3d. A full merge left 30 files in conflict; Alek's changes are tangled with
  his own features (Flip Grip, rumble and motion APIs, a reworked SQLite cache layer). Took the minimal route instead:
  `switch/patches/aurora-switch.patch` (Horizon surface, docked size, big-stack and core-pinned threads, one 8 MiB
  shader compile thread, Switch texture cache budget).
- Dawn: encounter/dawn 1155e0ed (the same base as Odran's fork; his extra commits are OpenGL only) with Alek's two
  patches, built standalone into `out/prefix` (`switch/build_deps.sh`). SDL3: devkitPro's Switch branch c329016c with
  Alek's video and joystick overlay.
- Disc: Alek's nod shim and RVZ/WIA/GCZ reader (`switch/source/disc`, CC0) stand in for the Rust nod crate.
- REL arena: Echoes needs its module arena below 4 GiB; `switch/source/platform/low_arena.cpp` maps heap pages there
  with svcMapProcessCodeMemory and svcSetProcessMemoryPermission (untested, top suspect for an early crash).
- The cloud workspace cannot download GitHub archive tarballs, so `switch/fetch_sources.sh` clones Aurora's pinned
  dependencies and `switch/cmake/deps_cache.cmake` points FetchContent at them. Aurora uses Dawn's Horizon-patched
  Abseil. Mistake logged: the first fetch script deleted and re-cloned `deps/dawn` mid-build; it now never deletes.
- Compile fixes so far: stand-ins for dlfcn.h, sys/syscall.h, sys/un.h (`switch/compat`), arpa/inet.h for
  port_console.cpp, SQLite without WAL or mmap and with the unix-none VFS as default.

## 2026-10-29: paused (pick up here)

- Paused by the owner until Sonic Adventure or Sonic Heroes is finished.
- Done: CMake, Ninja and patch installed in MSYS2. devkitPro core updated (`pacman -Syu`), but the Switch portlibs
  were not installed yet: the core update stopped pacman before the install step.
- Next on resume: the owner runs, in a new PowerShell window,
  `C:\devkitPro\msys2\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm switch-dev switch-libzstd switch-bzip2 switch-liblzma switch-freetype switch-harfbuzz switch-libpng switch-curl switch-libexpat"`
  (names checked against his dkp-libs.db). Then verify the .a files, pin an Echoes Unbound release (0.6.2 was
  current), study the Twilight Princess Switch layer, and write the mp2-nx build script.

## 2026-10-29: kickoff

- Chose the route: Echoes Unbound (decomp-based native PC port on Aurora) plus an Aurora/Dawn Switch layer on NVK,
  following the Twilight Princess and Melee Switch ports. Disc: GameCube USA (G2ME01).
- PC baseline: Echoes Unbound 0.6.1 for Windows booted the owner's `.rvz`, played Temple Grounds through about eight rooms,
  a Dark Aether portal round trip and a save station, and quit cleanly (D3D12 on his Ryzen Z1 Extreme).
  Notes for Switch: 500 pipelines compiled in about 39 s (200 to 370 ms each, 8 threads), no bundled initial pipeline
  cache; a 320x184 texture re-uploaded every frame (Aurora "possible texture leak" warning); about 800 MB of distinct
  textures in Aurora's object cache.
- NVK SDK: extracted from ghcr.io/danfromtico/switch-dev-legacy-vulkan (layer sha256:90586b29...) straight onto
  The owner's PC at `C:\MP2-nx\sdk\` (provenance in `sdk\PROVENANCE.txt`).
- The owner's PC has devkitPro (devkitA64 GCC 16.1, libnx, SDL2, zlib) and MSYS2 (git, python) but no CMake, Ninja or
  patch in MSYS2, and none of the portlibs Aurora's Switch build needs (zstd, bzip2, xz, freetype, harfbuzz, libpng,
  curl, expat).

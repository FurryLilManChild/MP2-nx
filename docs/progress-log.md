# Progress log

Newest first.

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

# Progress log

Newest first.

## 2026-11-03: switch-test4 result, test5 (button remapping)

- Test4: the caches persist (`271 pipelines ready 35702 ms after startup`, from test3's list); 64 new pipelines in
  new areas. Card: no errors; the game created a new MetroidPrime2 save and the owner confirmed the save station saved.
  Save state slot 1 saved (`saved slot 1 (Temple Grounds - Hive Chamber A)`). Menu: the owner says it feels great.
- Alek audit (aurora-aleks and his Dusklight Switch repo, CC0): the findings, HIGH first, are the test6 list: compile
  thread boost and CPU FastLoad during loads, quick_check on every cache open (18.5 s measured by Alek on a big Dawn
  cache), pipeline memory (Echoes keeps every precompiled pipeline; trim needs a Horizon memory source), clean exit
  through AM (`__nx_applet_exit_mode`, avoids 2001-0106 and "the software was closed"), journal_mode TRUNCATE,
  cache shutdown lock, no prune/VACUUM on Switch, shader module reuse, buffered logging, audio sample frames 4096,
  abort wrap, texture budget 640 MB, NRO icon, rumble and gyro over libnx, suspend clock, a seed pipeline list read in
  memory (not Echoes' merge-every-start).
- Test5: Buttons page in the Switch menu (Fire, Jump, Morph Ball, Missile, Lock-on, Free aim, Map, Pause, Spring
  Ball): capture through SDL, swap with the action that had the button, written to the Default profile of
  controls.toml the way the F1 Remap page does; Reset buttons. Plus: hidInitializeTouchScreen (touch did nothing),
  render worker pinned to Affinity::Render, CardGciFolder skips unreadable or short .gci files, the io.cpp rename
  special case dropped (rename_wrap covers it; it logged a harmless ENOENT).

## 2026-11-03: switch-test3 result, test4 (saves, Switch menu, save states)

- Test3: both caches opened (`SQLite cache init succeeded`, 100 pipelines compiled and stored), so the SQLite VFS fix
  works; the second launch is the real cache test. The owner: the menu opens and closes with + and - but neither the pad
  nor the touch screen does anything in it.
- Saving still failed: the card is a GCI folder (`user/USA/Card A`), not a raw image. Test2 created the
  `MetroidPrime2` file but its data write failed, so test3 found it with no comment (CMemoryCardDriver::IndexFiles:
  kE_FileCorrupted) and tried to delete it. CardGciFolder::deleteFile moves the file into `Card A/_deleted` with
  SDL_RenamePath, which failed three times (`Failed to delete file at idx: 0`), most likely the FAT rename-onto-existing
  refusal again with a copy already in `_deleted`.
- Fix: `switch/source/platform/rename_wrap.cpp`, linked with `-Wl,--wrap=rename`, so every rename in the program
  (std::filesystem::rename, std::rename, SDL_RenamePath; about 20 save paths: card, settings, bindings, save states)
  replaces an existing target: old moved aside, new renamed in, old removed; restored on failure, logged with errno.
  objdump shows no direct `rename` call left outside the wrapper.
- Menu: per the owner, a Switch-native menu replaces Echoes' F1 window instead of fixing its input:
  `switch/source/menu/switch_menu.cpp` (built into mp_port_audio, hooked in debug_ui/window.cpp DrawUI). libnx pad and
  touch input, drawn with ImGui draw lists only (no ImGui nav or focus). Pages: Game (resume, skip cutscenes, Fast
  Morph, Spring Ball, return to title), Save states (8 slots with room and play time, save, load, undo), Controls
  (twin stick, stick aim speed, invert aim, crosshair, lock-on toggle, sticky charge, auto-aim, map invert), Video
  (resolution for handheld and for docked, applied on mode change; dynamic resolution; FPS counter; MSAA; anisotropy;
  aspect; FOV; HUD size; black bars; hide helmet), System. Settings go to Echoes' settings.cfg; the per-mode
  resolution and the FPS counter to `sdmc:/switch/mp2-nx/switch_menu.cfg`. Button remapping is not in it yet.

## 2026-11-03: switch-test2 result, test3 prepared

- Booted to gameplay on hardware: Vulkan on NVK, the REL arena below 4 GiB, the .rvz reader and input all work. The owner:
  smooth at stock clocks. Problems: assets pop in the first time they are drawn (arm cannon, a wall, the whole room on
  first morph ball, the scan visor), fine the second time; the game reports the memory card as corrupted.
- Pop-in cause (mp2-nx.log): both caches fail to open, `Cache database sdmc:/switch/mp2-nx/cache/dawn_cache.db failed
  its check: unable to open database file` (same for pipeline_cache.db). SQLite treats a path without a leading '/' as
  relative and prepends getcwd() to "sdmc:/...". With no caches every pipeline compiles live on the one compile
  thread (200 to 340 ms each) and Aurora skips draws until it is ready (1,917 skipped draws in one context).
- Fix: `switch/source/platform/sqlite_switch.cpp`, adapted from aurora-aleks-nx (MIT): a copy of SQLite's unix-none
  VFS whose xFullPathname passes mount-qualified paths through, made the default, plus syscall shims for libnx file
  semantics (reads past the end clamp to 0 instead of EIO, writes past the end zero-fill the gap, stat of an open file
  reports a plain file). Expected: the second visit to anything is smooth across launches. First-time pop-in stays
  until a starter cache ships (option: seed from the PC build's pipeline_cache.db).
- Memory card cause: `[aurora::card] No file to open: MetroidPrime2`, then `Failed to write 8192 bytes to card`.
  `AtomicFileWriter::commit()` renames the temp file over the existing one, which FAT refuses. Fix in Aurora io.cpp
  (Switch only): move the old file to `.prev`, rename the temp into place (restore `.prev` on failure), remove `.prev`.
- Noted for later (the owner): the F1 overlay menu needs a controller and handheld overhaul. Also seen: two small textures
  (320x224, 320x184) re-uploaded every frame (same on PC), and scan_peek "all 40x64 pixels are zero" lines (unverified).

## 2026-11-03: switch-test1 result

- Boot reached Echoes' settings and folder setup (takeover check passed, 3189 MiB, the .rvz found, MP2_* paths used),
  then `port: uncaught C++ exception in std::terminate` right after the storage report thread. No crash.txt: not a
  CPU exception, the terminate handler logged it and exited.
- Cause (base-skill known issue, missed at bring-up): libnx's pthread_detach returns ENOSYS, so
  `std::thread(...).detach()` in port_storage_report.cpp throws and the joinable std::thread terminates.
- Fix for test2 (built after the owner's go-ahead): `switch/source/platform/pthread_wrap.cpp` wraps pthread_detach (returns
  0) and pthread_create (1 MiB stack when no attributes); the terminate handler now logs the call stack as program
  offsets.

## 2026-11-03: first NRO (switch-test1, boot test)

- Linked: `mp2-nx.nro` 61.6 MB, sha256 44a729b1acd49e945acdeb00ed3d9754d5ea9bd5f355d3540da766cf024cd8b4. Symbols-only
  ELF for addr2line: `C:\MP2-nx\out\elf\mp2-nx-test1-syms.elf.gz`.
- Link fixes: POSIX stubs (pipe, waitpid, execvp, ttyname, geteuid, fchown, flock; `switch/source/platform/posix_stubs.cpp`),
  `nod_partition_is_wii` added to the nod shim, the nod shim built as PIC (its .rodata tables needed run-time
  relocation in a read-only segment: "read-only segment has dynamic relocations").
- Dawn's `ninja install` builds every target and some do not build for Horizon (absl failure_signal_handler), so
  build_deps.sh installs only the stripped monolithic library (1.7 GB to 40 MB), the headers and a DawnConfig.
- Prebuilt Dawn and SDL3 saved to the owner's PC: `C:\MP2-nx\out\cache\mp2nx-prefix-dawn1155e0ed-sdl3.tar.gz`.
- Workspace note: the cloud workspace restarted twice and took dockerd with it; run dockerd with nohup and keep long
  waits in the background. `cmake --fresh` forced full recompiles; the configure no longer uses it.

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
  shader compile thread). Correction (Nov 3, Alek audit): the Switch texture cache budget was listed here but never
  made it into the patch.
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

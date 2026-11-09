# mp2-nx

A native Nintendo Switch port of **Metroid Prime 2: Echoes** (GameCube, USA). Not emulation: the game code is
[Echoes Unbound](https://github.com/Odrannnn/MetroidPrime2Port), a native PC port built on the Echoes
decompilation, and mp2-nx adds the Switch layer: a deko3d renderer (with Vulkan on NVK as the other choice),
Switch controls with twin stick and gyro aiming, saves on the SD card, and a Switch-native port menu.

Unofficial and fan-made, with no affiliation with Nintendo or Retro Studios. **No game files are included.** You
need your own USA GameCube disc (G2ME01) as an image.

## Requirements

- A Switch running custom firmware (Atmosphere) with the homebrew menu.
- Your own disc image: .iso, .gcm, .rvz, .wia, .gcz or .ciso.

## Install

1. Download the zip from the [Releases](../../releases) page and extract it to the root of your SD card. It makes
   `switch/mp2-nx/`.
2. Copy your disc image into `switch/mp2-nx/`.
3. Hold **R** while opening any game to get the homebrew menu with full memory, then start **mp2-nx**. Running it
   from the Album does not give it enough memory.

## Playing

Press **+** and **-** together for the port menu: controls (twin stick aiming, gyro, remapping), video
(resolution, anti-aliasing, field of view and more), save states, and the renderer.

Default buttons: A fire, B jump, X morph ball, Y missile, ZL lock-on, ZR free aim, R map, + pause. With twin stick
aiming on, hold L and press the D-pad to pick a beam (up Power, right Dark, left Light, down Annihilator).

Saves, settings and controls are kept in `switch/mp2-nx/user/`, shader caches in `switch/mp2-nx/cache/`, and logs
in `switch/mp2-nx/logs/`. When reporting a problem, include `mp2-nx.err.log` and `mp2-nx.log` from the logs
folder.

The NRO contains precompiled shaders for the Switch's GPU, so areas draw smoothly the first time. They contain no
game assets.

## Building

The NRO is built on Linux with Docker:

1. `switch/fetch_sources.sh` fetches the pinned sources (Echoes Unbound v0.6.2 is the `extern/echoes` submodule).
2. `switch/build_deps.sh` builds Dawn, SDL3 and the rest for the Switch (once, slow).
3. `switch/build.sh` applies `switch/patches/` to Echoes Unbound and Aurora and builds `mp2-nx.nro`.

Upstream code stays on its pinned commit; every change to it is in `switch/patches/`. `docs/progress-log.md` is
the full development history.

## Credits and licenses

mp2-nx's own code is MIT (`LICENSE`). Files taken from other projects keep their own licenses, stated in each
file's header or in the README next to them:

- `switch/deko/mpl/`: MPL-2.0 (SwitchWakerHD, via SwitchWaker and Alexgg1014's Aurora).
  The source of every changed file is in this repository.
- The rest of `switch/deko/` taken from SwitchWaker and Alexgg1014's Aurora: MIT.
- `switch/third_party/nod-c` and the Switch code adapted from Alexgg1014's Dusklight Switch
  port: CC0 (`switch/LICENSE-dusklight-switch-CC0.md`).
- `switch/sdl/`: SDL (zlib).
- `switch/patches/echoes-switch.patch` and `aurora-switch.patch` change Echoes Unbound (MIT)
  and Aurora (MIT); the patches are under those projects' licenses.

Credits:

- [Echoes Unbound](https://github.com/Odrannnn/MetroidPrime2Port) (MIT) and the Echoes decompilation (CC0).
- Alexgg1014's Dusklight Switch port (CC0) and his Aurora fork (MIT): much of the Switch groundwork.
- [SwitchWaker](https://github.com/centollOS/SwitchWaker) by centollOS (MIT): the deko3d renderer's base, with
  files from [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD) (MPL-2.0, in `switch/deko/mpl/`).
- deko3d and uam by fincs (zlib), Aurora (MIT), Dawn (BSD-3-Clause), SDL3 (zlib), Mesa NVK (MIT), SQLite (public
  domain), Abseil (Apache-2.0), libnx (ISC).

# switch/deko: the deko3d renderer (stage 2, docs/deko3d-plan.md, docs/deko3d-phase1-design.md)

deko3d under Aurora's WebGPU calls: Aurora's device is Dawn's Null device and the WebGPU C functions it calls are
wrapped at link time (switch/cmake/mp2_deko3d.cmake lists them). Chosen at run time in the Switch menu (System >
Renderer); with Vulkan every wrapper forwards to Dawn.

| File | Origin | Licence |
|---|---|---|
| `dk_bridge.hpp`, `dk_buffers.cpp`, `dk_pipelines.cpp`, `dk_encoder.cpp`, `dk_surface.cpp` | mp2-nx | MIT |
| `dk_objects.hpp`, `dk_objects.cpp` | SwitchWaker `switch/deko/aurora/` (084291f) via Alek's Aurora (`lib/deko`); mp2-nx changes marked | MIT |
| `shader_cache.cpp` | Alek's Aurora (`lib/deko`); mp2-nx: named stages of this renderer | MIT |
| `shader_translate.h`, `shader_translate.cpp` | SwitchWaker `switch/deko/` (084291f) via Alek's Aurora; mp2-nx: sampler bindings | MIT (Copyright (c) 2026 Pulpparty and depende3000, and the SwitchWaker contributors) |
| `mpl/*` | SwitchWakerHD (centollOS, df8fbde) via SwitchWaker and Alek's Aurora; mp2-nx changes noted in each header (device.cpp rewritten around the surface; dksh_file: SWDK version 2; shaders.cpp: named records in dksh_local.bin) | MPL-2.0 (file level) |
| `aurora_deko3d_uam_rename.cmake` | Alek's Aurora (`cmake/`) | MIT |
| `README-alek.md` | Alek's `lib/deko/README.md`, for reference | MIT |

Built from a SwitchWaker checkout at 084291f (`MP2NX_SWITCHWAKER_DIR`, fetched by `switch/fetch_sources.sh`):
deko3d 0.5.0 and uam 1.1.0 (devkitPro, zlib; uam's `mesa-imported/` MIT) with the patches in their
`PATCHES.md`, and the two Tint options in `switch/patches/dawn-switch-tint-*.patch` (SwitchWaker, MIT).

`../source/deko/dk_smoke.cpp` is the phase 0 smoke test (no longer built).

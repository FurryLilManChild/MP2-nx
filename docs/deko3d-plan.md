# deko3d plan (stage 2)

Written 2026-11-04, after the v0.1.0 test release (Dawn on NVK). Sources: Alek's Aurora (`aurora-aleks`, lib/deko,
MIT, plus `mpl/` under MPL-2.0), which took the bridge from SwitchWaker (centollOS, MIT, commit 084291f), whose
platform layer comes from SwitchWakerHD (MPL-2.0); deko3d 0.5.0 and uam 1.1.0 (zlib) with SwitchWaker's patches.

## Why

SwitchWaker's console A/B (The Wind Waker, same Aurora design, handheld 1020 MHz CPU / 460 MHz GPU), deko3d against
their previous renderer (OpenGL ES on Mesa, not Vulkan):
- render worker CPU 1.5 to 2.1 ms a frame against 14.5 to 19.7 ms (about 10 times less per draw)
- GPU time 30 to 45 percent lower (no Dawn or Mesa validation, fewer barriers)
- shader warm-up: 3,095 pipelines in 1.1 s from a precompiled DKSH file, against 154.6 s
- what it did not change: scenes bound by GPU fill rate stay so.

Our baseline is Vulkan on NVK, not GL, so the gap will be smaller than theirs; the console tests decide. The
precompiled shader file matters most for us: DKSH is Maxwell code, not tied to a driver build, so a cache built
from the owner's recipe lists can ship in the NRO and remove first-visit shader compiles for everything it covers.

## How the bridge works

Aurora keeps recording frames for WebGPU, with Dawn's Null device as the object model. Twelve wgpu calls are
wrapped (`--wrap`) so every texture, view, sampler and bind group gets a deko3d shadow. Each recorded frame is
replayed as deko3d commands (lib/deko/dk_encode.cpp). Shaders: WGSL -> Tint GLSL -> uam -> DKSH, looked up in
`romfs:/initial_dksh_cache.bin`, then `dksh_local.bin` on the SD card, compiled on the console on a miss. Dawn/NVK
stays available and is the fallback (a boot marker: two starts that never presented go back to Dawn).

## What our Aurora adds (Echoes Unbound's, 011e331 plus port work)

- The frame structures the bridge reads (FramePacket, RenderPass, DrawCommand) and the GX shader binding layout
  match Alek's closely; both use Dawn 1155e0ed, so the two Tint patches should apply.
- Pipelines differ most: ours predates upstream's CompiledPipeline/PipelineOptions, and our cache has prewarm,
  eviction, trim and the seed DB tied to `wgpu::RenderPipeline`. Hardest part of the port.
- Echoes really uses three readbacks the bridge does not handle: scan peek (scan visor, gameplay-critical), the
  EFB CPU copy (visor, camera filter, gun, flare effects) and region depth peek (visor flare).
- Bloom, HDR, volumetric fog, shadows, PBR probes, thermal/x-ray, vfx and water are in our Aurora but never called
  by Echoes (dormant), so the bridge skipping encoder tasks and custom draws costs nothing today.
- Off in deko3d mode at first: MSAA (forced 1x), dynamic resolution (scale locked), GPU timers, the GPU self-test,
  screenshots and photo mode, resident geometry and buffer spill (staging pinned).

## Phases (each ends in a build the owner tests)

0. Build plumbing (1 to 2 sessions): Dawn rebuilt with Tint's GLSL writer and the two Tint patches, tint exported;
   deko3d and uam vendored from SwitchWaker 084291f; uam's symbols renamed against NVK's Mesa; the wrap flags; a
   renderer choice in the Switch menu (restart to apply). Test: boots on Dawn exactly as v0.1.0, and the deko3d
   choice falls back cleanly.
1. Standard GX drawing (3 to 5 sessions): lib/deko ported (RmlUi dropped), the frame, encoding, recording,
   resource cache, texture, gpu, aurora, imgui and gpu_cache hooks, the pipeline cache adapted. The fixed shaders
   (clears, EFB copy conversions, palettes, present) exist only as DKSH records, so they are built first (offline,
   about 25 modules). GX shaders compile on the console at first. Test: title, gameplay, saves; long pop-in on
   first boot until shaders compile; frame rate compared at stock clocks.
2. Echoes' readbacks (2 to 3 sessions): scan peek, EFB CPU copy, region depth peek with age and forced snapshots,
   EFB copy mip chains, then dynamic resolution and present sharpening. Test: scan visor, visor effects, flares.
3. Offline DKSH cache (1 to 2 sessions): SwitchWaker's dksh_cache tool (MPL-2.0) adapted to our generator and DB,
   run on the owner's merged recipe list, shipped as romfs:/initial_dksh_cache.bin (game-derived: outside git). Test:
   first visits without shader compiles for everything in the list.
4. Optional: encoder tasks and custom draws (only if a dormant Prime 1 pass is ever wanted).

Roughly 8 to 12 sessions, plus the owner's console tests.

## Risks

- The pipeline cache rework (prewarm, eviction and trim must free deko3d pipelines safely).
- The module hash and WGSL must match between the console and the cache tool, or the cache misses.
- Tint or uam failures on some of our shader variants: those draws stay skipped until fixed.
- Pinned staging memory: about 79 MiB a slot, 3 slots, against 1.2 to 1.7 GB free in recent logs.
- dst alpha: ours uses dual source or nothing; the bridge's dual-source path is unproven on the console.
- The scan visor readback must be exact.
- Tooling: the dksh_cache tool runs in a Linux container (in the cloud, with the owner's OK each time).

## Licenses

lib/deko/mpl and the dksh_cache tool: MPL-2.0 (file-level: keep the headers, publish changes to those files).
The rest of lib/deko: MIT. deko3d, uam: zlib (altered versions marked). Dawn: BSD-3. No GPL.

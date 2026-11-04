# deko3d phase 1 design

Written 2026-11-04, after the phase 0 console test (docs/progress-log.md). Replaces the "port lib/deko" route of
docs/deko3d-plan.md phase 1; phases 2 and 3 change with it (below).

## Why not port Alek's encoder

Alek's lib/deko (from SwitchWaker) replays Aurora's recorded FramePacket op by op with code that knows each kind of
pass Aurora has (GX draws, clears, EFB copies, conversions, RmlUi). Our Aurora has diverged a long way from his: no
RenderTargetLayout, PipelineOptions or CompiledPipeline; a pipeline cache built on `wgpu::RenderPipeline` (prewarm,
eviction, trim, the seed DB); spill and resident buffers; scan peek, the EFB CPU copy, region depth peek, mip
chains, HDR, probes. Porting the encoder means re-deriving every one of those paths for deko3d by hand.

## What we do instead: deko3d under the WebGPU calls

Aurora talks to the GPU through 108 WebGPU C functions (measured from the link: `nm -u` over Aurora's objects).
In deko3d mode the device is Dawn's Null device (the object model, validation, refcounts, no GPU work), and about 50
of those functions are wrapped (`-Wl,--wrap=`) so each call also does the deko3d work:

- creation and release: buffers (deko3d memory; `GetMappedRange` returns the deko3d CPU pointer, so map-write and
  map-read buffers need no copies), textures, views, samplers (Alek's shadows, dk_objects.cpp), bind groups,
  bind group and pipeline layouts, shader modules (their WGSL kept), render and compute pipelines (built from the
  descriptor: WGSL -> Tint GLSL with the Sequential binding table -> uam -> DKSH, the fixed state from the
  descriptor's wgpu structs as Alek's dk_pipeline.cpp converts them)
- encoding: render passes (targets, load and store ops, clears), pipelines, bind groups with dynamic offsets,
  vertex and index buffers, draws, viewport, scissor, blend constant, stencil reference, immediates (UBO 15),
  copies buffer/texture in all four directions, debug groups; compute passes
- the queue: WriteBuffer and WriteTexture (copy-engine copies from an upload ring, in command order), Submit
- the surface: GetCurrentTexture hands out a texture whose shadow is the current swapchain image; Present presents

The pipeline cache, prewarm, eviction, trim and seed DB are untouched: they keep holding `wgpu::RenderPipeline`s;
the deko3d pipeline is the shadow of that handle. Every pass Aurora has (and the readbacks: scan peek, EFB CPU
copy, depth peek all end in CopyTextureToBuffer plus MapAsync) goes through the same small set of calls, so they
come with phase 1 instead of needing their own phase.

Reused unchanged or nearly (MPL-2.0 files keep their headers; MIT credits kept): Alek's lib/deko/mpl (device,
memory, descriptors, shaders and the uam worker, DKSH files, overlay), dk_objects.cpp (shadows, formats),
dk_pipeline.cpp (state conversion), shader_cache.cpp, dk_runtime.cpp (switch, options, boot marker),
SwitchWaker's shader_translate.

## Order of work

1. Renderer switch: menu "Renderer: Vulkan / deko3d (test)", restart to apply; deko3d mode asks Aurora for the
   Null backend; boot marker (two starts that never presented go back to Vulkan).
2. mpl platform layer and object shadows built into the NRO.
3. Buffers, pipelines, render passes, draws, queue writes, submit, present: the title screen.
4. Copies and mapping: EFB copies, readbacks (scan visor).
5. Compute (two sites), debug groups, timestamps (off at first).
6. Offline DKSH cache from the owner's recipe list (romfs:/initial_dksh_cache.bin, outside git), keyed by the GLSL
   hash as Alek's is, so console and tool agree as long as the WGSL and Tint options match.

## Risks

- CPU cost of the Null device's validation (Alek has it too; measured on the console in the first test).
- Barriers: one full barrier per pass end at first (as Alek's), tuned later.
- Formats deko3d lacks or swizzles (BGRA), depth formats, and copy row pitches.
- The uam compile time per shader (phase 0: about 0.9 s per pipeline): the offline cache is required before the
  build is pleasant to play.

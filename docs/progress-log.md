# Progress log

Newest first.

## 2026-11-04: deko-16b result, deko-17 (group 2 suspects, EFB copy, WebGPU discard rule)

- Screenshots work (21 per run, 640x360 BMPs, no visible freeze). Buffers zeroed at creation: no change.
- In the door room (loop 2) and the white-floored room (loop 3), only group 2 (see-through pipelines with
  id % 6 == 1) changed the fault: the grey strips and the white floor went black. Group 2 hid only three
  pipelines in all three loops: 'GX Pipeline 8b66dcab231efa19' and '50d2b937df92bb30' (no textures) and
  'f04c1df9b8e47a11' (two 64x8 textures and a 51x51 'Resolved Texture', an EFB copy most world pipelines sample).
- Gap found in Aurora's code: discard_dead_stores ends passes with StoreOp Discard; WebGPU (Dawn) then reads that
  target as zeros until it is written again. The bridge ignored store ops and kept the old contents.
- deko-17 cycle: normal; each suspect hidden; small 'Resolved Texture's (up to 128x128) sampled as transparent
  black; the discard rule (a discarded target loaded by the next pass is cleared to zero, sampling one reads the
  empty image); the rest of group 2 hidden. Counts of discarded loads and samples are logged every minute, and
  each screenshot also saves the last small EFB copy (colour and alpha, 4x) as *_efbcopy_WxH.bmp.

## 2026-11-04: deko-15 result, deko-16 (buffers zeroed, see-through groups, screenshots)

- deko-15 in Temple Grounds and the door room. Test 5 (see-through blends hidden, SrcAlpha/OneMinusSrcAlpha)
  removed both the white ground patches and the grey door-room strips, uncovering the correct texture underneath
  (the brushed panels from the Vulkan photo). So the bad pixels are a see-through layer drawn over good surfaces.
- Ruled out: smallest mip (test 2, LOD clamped to 0, no change); unfilled mip levels (test 7 hid 0 draws, no
  "levels never written" diag); leftover depth (now 4 to 5 depth textures cleared, patches unchanged).
- Test 3 (textures with RenderAttachment usage hidden) blanked the screen: Aurora draws the frame into
  'Render texture' and the final blit samples it. A bad test, dropped.
- Quit crash (crash.txt, replay: begin render pass): pc in prepare_draw's vertex buffer loop, far
  0x3200000000006c, after Aurora's "Device lost: Device was destroyed". Shutdown only; not fixed yet.
- New suspect for the boot-to-boot changes: deko3d memory blocks for WebGPU buffers came from recycled heap
  memory and were never zeroed (WebGPU buffers start as zeros). deko-16 zeroes them at creation
  (MP2DK_NO_BUFFER_ZERO=1 turns it off).
- deko-16 cycle: normal, then the see-through pipelines in 6 groups by build order (RPipe::id % 6), one hidden
  per mode; the first draw of each hidden pipeline is logged with its textures.
- Screenshots: halfway through each mode of the first 3 loops, the swapchain image is copied to a buffer and
  written as shots/loopL_testT.bmp (MP2DK_SHOTS=0 turns it off).
- deko-16 was untestable: each screenshot (uncached reads, row-by-row SD writes, all on the present) froze the
  game long enough that the resume prompt could not be answered. deko-16b: CPU-cached copy, half size
  (640x360, 0.7 MB), the SD write on its own thread, and the cycle starts 30 s after the first present.

## 2026-11-04: deko-15 (narrower test cycle, depth wipe fixed)

- The owner's Vulkan photo of the door room: the strips are ribbed metal grates with lighter brushed panels. On
  deko3d they are one flat grey, which looks like a texture's smallest mip level (its average colour) or a
  texture drawn during play that is not ready.
- texture_zero_later no longer skips depth textures, so zero_texture's depth branch (deko-14b) runs.
- Cycle log fixed: "mode N (...) ended: X draws, Y hidden" is printed for the mode that ended.
- New modes (MP2DK_CYCLE=10, 7 modes): normal; full-size textures only (every sampler clamped to LOD 0,
  a second sampler descriptor keyed apart); textures drawn during play hidden (a fragment texture with
  RenderAttachment usage); additive blends hidden (colour dst factor One); see-through blends hidden
  (SrcAlpha, OneMinusSrcAlpha); other blends hidden; textures with unfilled levels hidden.
- Tex::mipsWritten (one bit per level, set by uploads, copies and passes). A texture sampled with levels never
  written is logged once ("diag: ... is sampled with levels never written"), and the unfilled mode logs the
  textures it hides.

## 2026-11-04: deko-14b result (paused here)

- One boot, 8-mode cycle. The log shows "0 depth textures cleared": texture_zero_later (dk_encoder.cpp) still
  returns for depth textures, so zero_texture's depth branch never ran. The leftover-depth theory is untested.
- Per-mode counts are printed one line late (each "hid N" belongs to the previous mode).
- Hidden draws in Temple Grounds: screen-reading blends 0, dual-source 0, constant about 600, all blended about
  235k of 275k (Aurora marks most GX draws blended, so that mode is too broad). Census: 1085 pipelines, 890
  blended, 2 read the destination, 16 dual-source, 530 constant.
- This boot most whitewash was gone (luck of the boot, not a fix). One spot still wrong: grey strips in the
  door room, unchanged in modes 1 to 7, black in mode 8 (a blended layer over black, wrong colour).
- Next (deko-15, waiting on the owner's Vulkan photo of the strips): drop the depth filter in texture_zero_later;
  fix the count labels; replace the blend modes with narrower ones (additive, alpha, draws sampling a texture
  rendered during play such as cube probes) and log the pipeline that draws the strips.

## 2026-11-04: deko-14b

- Anti-aliasing ruled out: the owner's setting was already off (the "asks for 4 samples" log line is a clear
  pipeline built either way; misread). Re-running deko-13 in the same places gave different good and bad spots
  with no settings changed: the fault depends on leftover memory or timing, not a fixed translation error.
- Found: the game's fog volumes (CCubeRenderer::ReallyRenderFogVolume) copy depth into textures and blend with
  destination alpha; the hazy hallway on Vulkan is one. The white patches match its colour.
- deko-14b: depth textures are now cleared to 0 at creation like other textures (they were skipped since
  deko-11; image heap memory is reused). New cycle mode: the GPU's zcull state invalidated at every pass start.
  8 modes, one boot.

## 2026-11-04: deko-13 result, deko-14 (test cycle)

- deko-13: two runs, no crash, about 60 fps. Process memory about 1,700 MiB against about 1,950 MiB on deko-11
  (scenes not identical). The crash.txt in the logs is from 14:33 (deko-4 to deko-7 era): its pc lands on a cmp in
  deko-13, so it is not from this build.
- The diagnostics answered no: one texture sampled before a write ('Render texture', once at start), zero dropped
  copies or uploads. Missing texture data is ruled out.
- The owner's photos: whole surface pieces whitewashed (texture still visible under the white), changing with small
  camera moves, near surfaces too. Points at per-draw brightness or blending, not textures or mips.
- Found while reading: WebGPU starts every render pass with a blend constant of 0; the bridge kept the last value.
- deko-14: the blend constant is reset at every pass start. deko3d_env.txt MP2DK_CYCLE=<seconds> steps through
  7 modes in-game without a restart (shown on screen under the FPS counter, logged at each change): normal, the old
  blend constant, every draw's settings re-sent, and hiding draws whose blending reads the screen, is dual-source,
  uses the constant, or blends at all (the Dear ImGui overlay is never hidden). MP2DK_MODE=<n> holds one mode. A
  blending census is logged with the pipeline report. The owner's dksh_local.bin is bundled as
  romfs:/initial_dksh_cache.bin (not committed).

## 2026-11-04: deko-12 (diagnostics) and prep while the owner was away; deko-13 = both

- deko-13 (built 17:37, not yet tested) is deko-12 plus the Null buffer fix below; deko-12 itself was never run.
- deko-12 (built, not run): the owner's photos from deko-11 show whole surfaces flat white or black (far walls in
  an elevator shaft, pipe rings, plant tips, cave patches) while nearby surfaces of the same kind are right. The log
  has no errors for them. deko-12 logs every texture sampled before anything wrote it ("diag: ... is sampled before
  anything wrote it") and every upload or copy the bridge drops, and adds deko3d_env.txt switches MP2DK_MIP0,
  MP2DK_NO_ZERO and MP2DK_OLD_VIEWS for A/B tests. The owner's run also has AURORA_DUAL_SOURCE=0 (the shot squares that
  come back as the blast fades).
- Dawn's Null device held an uninitialised CPU copy of every buffer (about 270 MiB in deko-10's log) that deko3d
  mode never reads, and refuses buffers past 512 MiB in total. dawn-null-lazy-buffers.patch allocates the copy on
  first use and raises the cap to 3 GiB; libwebgpu_dawn.a is rebuilt with it (the next NRO links it). The Null
  backend is not used by the Vulkan renderer.
- The precompiled shader file needs no new tool: initial_dksh_cache.bin uses the same format as the console's
  dksh_local.bin (shader, module and named records, compiler id checked). The owner's current dksh_local.bin copied to
  out/romfs/initial_dksh_cache.bin (outside the repo, game-derived) goes into the next NRO's romfs.

## 2026-11-04: deko3d phase 1, tests deko-1 to deko-11

The phase 1 renderer (docs/deko3d-phase1-design.md): Aurora's device is Dawn's Null device and its WebGPU calls
are wrapped and replayed into deko3d. Builds are mp2-nx-deko (own folder), chosen in the menu, falling back to
Vulkan after two starts with no picture. Logs from C:\MP2-nx\out\sd\switch\dekologs.
- deko-1: abort at start. Cause: Aurora made an SDL renderer for the Null backend. Fix: in deko3d mode Aurora is
  told the backend is Vulkan (same WGSL, no SDL renderer). Also the portlibs include path (zlib.h).
- deko-2: crash on loading a save; crash.txt was empty. Cause: the crash handler wrote past the end of the file and
  read the base address as 0. Fix: the file is created at its size, the base comes from svcQueryMemory.
- deko-3: null DkImageView in dkCmdBufBindRenderTargets. Cause: passes with empty colour slots. Fix: trailing
  empty slots are left unbound, middle ones get a scratch image.
- deko-4 to deko-7: heap corruption at the scan visor. Robustness turned on for buffers and copies bounds-checked
  (no change); a graveyard for released shadows then logged dead views. Cause: Dawn hands back a cached object for
  an equal descriptor (views, samplers, layouts, modules, pipelines) and the bridge made a second shadow for it,
  freed at the first release. Fix: the existing shadow takes the extra reference.
- deko-8/9: 11 pipelines failed in Tint ("texture missing from texture_builtins_from_uniform list"). Fix:
  dawn-switch-tint-robustness-buffers-only.patch; it only took effect once the monolithic Dawn library, which has
  its own copy of the GLSL writer, was rebuilt from the patched tree.
- deko-10: plays. Scan visor, a boss, a cutscene and a save room, about 12M draws, none skipped, no failed
  pipelines, 55 to 60 fps, no crash. Wrong: normal shots leave white squares on walls (charge shots are right),
  some enemy shadows are white squares, the save room has white areas where Vulkan shows effects.
- deko-11 (fixes from reading Aurora's code; the log had no errors for these): (1) a view's deko3d type now follows
  the view dimension: cube probes (6-layer 2D textures viewed as cubes) and the VFX path's one-layer textures
  viewed as 2D arrays were sampled through a descriptor of the image's type. (2) New textures are zeroed by the next
  submit, ahead of their writes, as Dawn's are: the image heap hands back memory earlier textures used, and some
  textures (the empty PBR lightmap) are never written. Depth textures are left to their passes' clears.
  (3) deko3d_env.txt in the app folder sets variables before Aurora reads them, for A/B tests without a build
  (next suspect if squares remain: dual-source blending, AURORA_DUAL_SOURCE=0).
- Noted: the Null device still allocates about 440 MiB of its own (buffers 270, textures 170) that deko3d mode
  never uses; worth removing later.

## 2026-11-04: deko3d phase 0 result (console)

The owner ran mp2-nx-deko with the smoke test on (logs in C:\MP2-nx\out\sd\switch\dekologs). It booted and played
normally; the test passed: a deko3d device and queue next to Dawn/NVK in 6.9 ms, the test shader, and 80 of 80 GX
shader stages (the first 40 shaders) through Tint GLSL and uam, all loaded as DKSH. No errors, no crash.
- Per shader: Tint GLSL 61 ms, uam 406 ms on average; vertex stages 117 to 1,256 ms in uam (most 500 to 1,200),
  fragment stages 4 to 183 ms. Worst vertex plus fragment 1,360 ms. Whole test 37.6 s.
- The same start's Vulkan prewarm: 1,000 pipelines in 76.8 s (about 77 ms each), single compiles 200 to 340 ms.
  The test ran on core 2 next to that prewarm, so its times are inflated, but uam (GLSL frontend, TGSI, nv50_ir)
  is still several times slower than NVK's compiler, mostly in vertex shaders.
- Consequence: on-console compiles cannot carry deko3d; the offline DKSH cache (plan phase 3) is what makes it
  work, and should come with the first drawing build rather than after it.

## 2026-11-04: deko3d phase 0 (plumbing and smoke test)

- Tint's GLSL writer built with Dawn (TINT_BUILD_GLSL_WRITER=ON; the GLSL validator off, it would pull in
  glslang), with SwitchWaker's two Tint options (depth_zero_to_one, disable_position_y_negation). Both touch only
  the GLSL writer, so the Vulkan path is unchanged. Dawn's monolithic library was rebuilt in the same build so its
  Tint objects match the separate Tint archives (prefix lib/tint, 65 archives, 16 MB stripped).
- deko3d 0.5.0 and uam 1.1.0 from SwitchWaker 084291f build for the console (switch/cmake/mp2_deko3d.cmake). The
  uam rename step found 0 clashing symbols with this NVK libvulkan.a; checked separately with nm (3,589 uam
  symbols, 14,963 NVK symbols, none shared). The NRO links with no duplicate-symbol errors.
- Smoke test (switch/source/deko/dk_smoke.cpp), off unless chosen: System page "deko3d test (next start)" runs it
  once on the next start, on its own thread (8 MiB stack, core 2). It creates a deko3d device next to Dawn/NVK,
  compiles a test shader and the first 40 GX shaders (a tap in Aurora's build_shader) through Tint GLSL and uam,
  loads the DKSH into code memory, and logs "[deko3d test]" timings; a toast shows the summary. A marker file makes
  the next start skip it after a crash or hang. Not yet run on hardware.

## 2026-11-04: v0.1.0 private test release

Test8 confirmed by the owner (Fire on ZR charges and strafes). Packaged as `mp2-nx-v0.1.0-switch.zip` (sha256 974abe98...,
NRO 3c08038e..., commit 761e700): the NRO with the built-in starter shader list (kept, since the release is private),
README.txt, a disc-image note, and the Echoes Unbound (MIT), decomp (CC0) and Dusklight Switch (CC0) licenses.
Release notes in `C:\MP2-nx\out\release\`. This session cannot push tags or create releases; the owner publishes it
(tag v0.1.0 on main). Next: deko3d (stage 2).

## 2026-11-04: test7 result, test8

- Test7 logs: the starter list merged once (1,282 rows, 3,018 uses) and the next start skipped it ("already merged").
  64 new pipelines in play (test6d session: 208), worst context 8,171 skipped draws (was 185,483). Startup prepares
  1,282 to 1,456 in 40 to 52 s in the background. Compile split: build_shader (WGSL and parse) is 15 to 20 percent;
  about 80 percent is Dawn's Tint to SPIR-V plus NVK. Texture memory followed loaded areas (peak 461 MiB, free heap
  1.2 GB); the per-frame uploads go to the content cache, capped at 128 MB: not a leak.
- Control bug (the owner, isolated by remapping): Fire on ZR made Samus stand still while charging, worse when locked on.
  Cause: ZR carried GameCube R's click and its analog (free aim, which roots Samus); the Buttons page swapped the
  click off ZR but not the analog. Fixes: Switch triggers bind only L/R's click (port_input_devices.cpp BindMapping),
  and a remap also clears LAnalog/RAnalog from the button it takes.
- An 83 s "no frame" stall and an 84 s "compile" were the console asleep and at the HOME menu (the owner). Now
  misc_switch.cpp tracks focus changes and resumes through an applet hook (an epoch, plus 3 s after each);
  aurora_is_suspended() reports it, so the watchdog and the harness stay quiet, and a compile that spans a suspension
  is left out of the timings.
- Test8 NRO sha256 3c08038e... in the SD folder. Next (the owner): a private test release, then deko3d.

## 2026-11-04: play-session logs, test7

- Logs (test6d, longer play): boot prepared 575 cached pipelines in 23.6 s (test3: 271 in 35.7 s), so the load boost
  works. New pipelines in gameplay: 208 compiles, median 591 ms, p90 971 ms, max 2083 ms; one area needed 178 new
  (185,483 skipped draws), another 238. Free heap never under 1.69 GB; textures peaked at 104 MiB (the test6c climb did
  not recur). No errors.
- The owner's caches: his Switch cache (1,180 recipes, 2,570 uses) and the PC 0.6.1 one (872, 2,021); 770 of the PC's are
  identical by hash. Same schema (1) and GX config version (15).
- Test7 (NRO sha256 6f27ea16..., `C:\MP2-nx\out\test7\`):
  - Starter list: both merged (1,282 recipes, 3,018 uses, 5.4 MB) as romfs:/initial_pipeline_cache.db. Game-derived,
    so outside the repo (`out/romfs`, CMake MP2NX_ROMFS_DIR); the NRO gets ROMFS only when the folder exists. Merged
    into the SD cache once per seed (its size kept in table mp2nx_seed), not on every start.
  - Backlog boost: 8 or more game-requested pipelines waiting boosts the compile thread until the queue is empty.
  - Timing: each slow compile and the 100/500/1000 summaries now give the build_shader share (WGSL generation and
    CreateShaderModule) apart from the rest (Tint to SPIR-V and NVK).
  - The owner's notes: SDL joystick L/R as shoulders and ZL/ZR as triggers with no MISC copies (SDL rebuilt), Separate menu
    buttons on by default, gyro (libnx axes, accelerometer negated), Spring Ball description, [dock] log off on
    Switch, scan peek diagnostics every 512 instead of 32.
- Plan (the owner): after test7, a private test release as a fallback, then the deko3d work (stage 2).

## 2026-11-03: test6e, gyro left/right again

- Test6d (the owner): gyro left/right still reversed. The test6d change (y and z negated on both sensors) could not change
  it: Echoes' default gyro space is Player (port_gyro_motion.cpp), which takes yaw about the gravity direction from
  the accelerometer, so turning both sensors together cancels out.
- Test6e: gyro axes back to libnx's (test6c had pitch right), accelerometer negated (libnx's sign is opposite to
  SDL's, by inference from the two tests; unverified). Fallback if still wrong: the Local gyro space (yaw = -y, no
  gravity). Also includes the standard menu buttons change.

## 2026-11-03: test6c result, test6d

- Test6c (the owner): rumble works, gyro aim works but left/right is reversed, Exit game returns to HOME cleanly (the
  test6 AM exit works). Regression: audio choppy throughout.
- Audio cause: test6's `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=4096` (Dusklight's crackle fix). Echoes keeps only 64 ms
  queued (platform/ai_dma.cpp kTargetQueuedBytes, 32 kHz) and a 4096-frame device pull is about 85 ms, so every pull
  ran it dry. Removed; audio is as in test5. Lesson: check how this game feeds a subsystem before copying a tuning.
- Gyro: libnx's frame is SDL's turned 180 degrees about x; y and z negated for gyro and accel (one axis alone would
  mirror the frame and break the gravity-based gyro space).
- Also seen in the test6c memory log: textures climbed to 1529 (836 MiB) and free heap to 490 MiB before falling back
  (the trim and Aurora's texture eviction). The per-frame 320x224/320x184 uploads are the likely source; a test7 item.

## 2026-11-03: test6c, Exit game

Test6b plus an "Exit game" row on the Switch menu's Game page (A twice), the F1 window's path: save settings, push
SDL_EVENT_QUIT, so the main loop shuts down and the process ends through AM. Without it the only way out was HOME and
Close, which never runs the test6 exit fix. In `C:\MP2-nx\out\test6c\`.

## 2026-11-03 (overnight): test6b, rumble and gyro

Test6 plus `switch/source/platform/device_switch.cpp` (in `C:\MP2-nx\out\test6b\`, NRO sha256 d81a7387...): a reduced
port of aurora-aleks lib/device_switch.cpp (MIT). Aurora's lib/device.cpp is compiled empty on Switch and this file
gives aurora::device rumble (hidInitializeVibrationDevices for player 1's npad, 160/320 Hz, a worker ending timed
pulses, stopped on focus loss) and gyro/accel (six-axis, revolutions/s to rad/s and g to m/s^2, right Joy-Con of a
detached pair). Player 1's npad by Alek's rule: the one connected alone, else the one with the latest input.
Echoes' ReadGyroRates reads it through `mp2nx_switch_gyro` (SDL has no sensors on Switch). Menu: Gyro aim (Off, Hold,
Always, Hold to pause, Toggle), gyro sensitivity, Rumble (the game's own option). Axis directions are as libnx gives
them (as Alek passes them); unverified on hardware.

## 2026-11-03 (overnight): test6, Alek's fixes

Built clean (NRO sha256 ce2ac6e2...; in `C:\MP2-nx\out\test6\` so test5 stays on the owner's SD folder). From the two
audits (aurora-aleks MIT, Dusklight CC0):
- Exit: `__nx_applet_exit_mode` 1 in userAppExit, our `__libnx_exit`, and `mp2nx_end_process` (ISelfController::Exit,
  then the kernel after 10 s) for the terminate path. Always back to HOME: the REL arena's code mappings and live
  threads would crash hbloader.
- Load boost (`switch/source/platform/load_boost.cpp`, Aurora `aurora_shader_set_compile_boost`): the compile thread
  may use core 0 at priority 0x2C on the title (startup warmup), the CMFGameLoader screen and world transitions;
  CMFGameLoader also sets CPU boost FastLoad. Hooks in src/MetroidPrime/CMFGameLoader.cpp and CWorldTransManager.cpp
  (echoes-switch.patch now covers src/).
- Caches: quick_check only when `<db>.open` survived an unclean exit, never for dawn_cache.db; journal TRUNCATE on
  Switch; Dawn cache_shutdown under the cache lock and marked broken; no Dawn prune or VACUUM on Switch.
- Memory: port_memory_log reads the heap through mallinfo and libnx's fake_heap bounds; the pipeline trim runs on
  Switch with precompiling kept (512 / 256 MiB free).
- Logs fully buffered and flushed every 200 ms by a thread once the menu draws; `--wrap=abort` (log caller, then a
  panic break for an Atmosphere report); `--wrap=fchmod` returns 0; SDL_AUDIO_DEVICE_SAMPLE_FRAMES 4096; texture
  replacement 1024 max and 640 MB on Switch; NRO icon (`switch/res/icon.jpg`, original text art).
- First compile failed: `switch.h` was included inside the glibc block of pipeline_cache.cpp. Fixed.

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

#pragma once
// mp2-nx deko3d renderer (docs/deko3d-phase1-design.md): deko3d under Aurora's WebGPU calls. MIT.
//
// In deko3d mode Aurora's device is Dawn's Null device, which keeps the object model (creation,
// validation, references) and does no GPU work. The WebGPU C functions Aurora calls are wrapped at link
// time (-Wl,--wrap=, switch/cmake/mp2_deko3d.cmake):
//   - objects Dawn still creates get a deko3d shadow: textures, views, samplers, bind groups and their
//     layouts (dk_objects.cpp, from SwitchWaker via Alek), buffers, shader modules, render pipelines
//     (dk_buffers.cpp, dk_pipelines.cpp)
//   - command encoders, passes, command buffers, the queue's writes and submits, buffer mapping and the
//     surface are this renderer's alone and never reach Dawn (dk_encoder.cpp, dk_surface.cpp)
// With the Vulkan renderer (no deko3d device) every wrapper forwards to Dawn unchanged.
#include <deko3d.h>
#include <webgpu/webgpu.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "dk.h"
#include "dk_objects.hpp"
#include "shader_translate.h"

namespace mp2dk {

// deko3d mode is on (set once, before Aurora creates its device)
bool on() noexcept;

// ---- submit serials (dk_encoder.cpp): every queue submit signals a fence; objects remember the
// serial of the last submit that used them
uint64_t next_serial();           // the serial the next submit will have
bool serial_done(uint64_t serial);  // the GPU has finished that submit (no wait)
void serial_wait(uint64_t serial);  // waits for it (submits pending work first if it is the next one)

// ---- buffers (dk_buffers.cpp)
struct Buf {
    std::atomic<int32_t> refs{1};
    WGPUBuffer handle = nullptr;
    uint64_t size = 0;
    uint64_t usage = 0;
    swdk::Block mem;
    bool cached = false;              // CPU-cached (map-read buffers): invalidated before the CPU reads
    bool unmapDawn = false;           // created mapped: Dawn's own mapping ends at the first Unmap
    std::atomic<uint64_t> lastUse{0};  // the serial of the last submit that read or wrote it
    bool ui = false;                   // mp2-nx (deko-14): a Dear ImGui buffer (the overlay is never skipped)
};
Buf* find_buffer(WGPUBuffer b);
void buffer_retain(Buf* b);
void buffer_release(Buf* b);
// pending maps whose submits the GPU has finished: their callbacks are called (any thread)
void maps_poll();
// whether a future came from this renderer's MapAsync, and whether it has completed
bool map_future(uint64_t id, bool* completed);

// ---- shader modules and pipelines (dk_pipelines.cpp)
struct Stage {
    swdk::ShaderEntry* entry = nullptr;
    std::vector<swdk::SlotBinding> bindings;
};
struct RPipe {
    std::atomic<int32_t> refs{1};
    WGPURenderPipeline handle = nullptr;
    bool valid = false;
    Stage vs, fs;
    bool hasFs = false;
    DkPrimitive prim = DkPrimitive_Triangles;
    bool restart = false;
    DkRasterizerState rasterizer{};
    DkColorState color{};
    DkColorWriteState colorWrite{};
    DkBlendState blend[8] = {};
    uint32_t colorCount = 0;
    DkDepthStencilState depthStencil{};
    bool depthBias = false;
    float bias[3] = {};  // dkCmdBufSetDepthBias: constant, clamp, slope
    DkVtxAttribState attribs[16] = {};
    uint32_t attribCount = 0;
    DkVtxBufferState vbufs[8] = {};
    uint32_t vbufCount = 0;
    std::string label;
    // mp2-nx (deko-14 test cycle): what its blending reads (kBlend* in dk_encoder.cpp's cycle)
    uint8_t blendKinds = 0;
    uint32_t id = 0;  // mp2-nx (deko-16): the order it was built in (the test cycle's groups)
};
// mp2-nx (deko-14): RPipe::blendKinds bits
constexpr uint8_t kBlendAny = 1, kBlendDst = 2, kBlendSrc1 = 4, kBlendConst = 8;
// mp2-nx (deko-15): additive (the colour's destination factor is One) and plain see-through
// (SrcAlpha, OneMinusSrcAlpha)
constexpr uint8_t kBlendAdd = 16, kBlendAlpha = 32;
RPipe* find_render_pipeline(WGPURenderPipeline p);
void pipeline_retain(RPipe* p);
void pipeline_release(RPipe* p);
// render worker: both stages are loaded (a compiled one now, within the frame's load budget)
bool pipeline_ready(RPipe* p);

// ---- the surface and the frame (dk_surface.cpp)
// Aurora's webgpu::initialize, before the adapter is requested: the run-time choice (sdmc renderer
// setting, boot marker). True: use the Null backend and this renderer.
bool choose();
// after the device exists: the deko3d device, queue, memory, swapchain, shader caches
void init(uint32_t gxConfigVersion);
// the first present happened (the boot marker is cleared)
void presented();
// the swapchain image slot of the current frame, acquired on first use (-1 outside a surface frame)
int current_image();

// ---- the crash handler's note (switch/source/platform/startup.cpp): what the renderer last started
extern "C" const char* volatile mp2nx_crash_note;
inline void note(const char* what) { mp2nx_crash_note = what; }

// ---- the log
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace mp2dk

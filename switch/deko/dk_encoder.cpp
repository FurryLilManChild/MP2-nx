// mp2-nx deko3d renderer: command encoders, the queue and submits (dk_bridge.hpp). MIT.
//
// Encoders and passes are this renderer's own objects (Dawn never sees them): each WebGPU call appends an
// op to its command encoder, holding a reference to every shadow it names. QueueSubmit replays the
// queue's pending writes and then each command buffer into the deko3d command buffer (swdk::R.cmd), in
// order, ends with a fence of its own (the submit's serial) and sends it to the GPU. A buffer or texture
// remembers the serial of the last submit that used it (mapping waits for it).
//
// Barriers: one Fragments barrier with the texture cache invalidated at every render pass end (as Alek's
// encoder), and Full barriers with the image, shader and L2 caches invalidated around copy-engine work.
// The bound state is cached within a pass and rebound when a pass starts.
//
// Queue writes (WriteBuffer, WriteTexture) are copied at once into an upload ring of deko3d memory and
// become copy-engine copies at the start of the next submit, as WebGPU orders them.
//
// Compute passes are recorded and skipped (logged): Echoes calls none of Aurora's compute work yet
// (docs/deko3d-phase1-design.md).
#include "dk_bridge.hpp"

#include "../include/mp2nx_app.h"

#include <sys/stat.h>

#include <switch.h>

#include <algorithm>
#include <xxhash.h>
#include <cstdio>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <cstring>
#include <deque>
#include <mutex>

extern "C" {
const char* aurora_frame_capture_dir(void);  // Aurora's encoding.cpp (mp2-nx frame capture)
WGPUCommandEncoder __real_wgpuDeviceCreateCommandEncoder(WGPUDevice, WGPUCommandEncoderDescriptor const*);
WGPURenderPassEncoder __real_wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder, WGPURenderPassDescriptor const*);
WGPUComputePassEncoder __real_wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder, WGPUComputePassDescriptor const*);
void __real_wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder, WGPUBuffer, uint64_t, WGPUBuffer, uint64_t, uint64_t);
void __real_wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder, WGPUTexelCopyBufferInfo const*,
                                                  WGPUTexelCopyTextureInfo const*, WGPUExtent3D const*);
void __real_wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder, WGPUTexelCopyTextureInfo const*,
                                                  WGPUTexelCopyBufferInfo const*, WGPUExtent3D const*);
void __real_wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder, WGPUTexelCopyTextureInfo const*,
                                                   WGPUTexelCopyTextureInfo const*, WGPUExtent3D const*);
WGPUCommandBuffer __real_wgpuCommandEncoderFinish(WGPUCommandEncoder, WGPUCommandBufferDescriptor const*);
void __real_wgpuCommandEncoderRelease(WGPUCommandEncoder);
void __real_wgpuCommandEncoderResolveQuerySet(WGPUCommandEncoder, WGPUQuerySet, uint32_t, uint32_t, WGPUBuffer, uint64_t);
void __real_wgpuCommandEncoderWriteTimestamp(WGPUCommandEncoder, WGPUQuerySet, uint32_t);
void __real_wgpuCommandBufferRelease(WGPUCommandBuffer);
void __real_wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder, uint32_t, uint32_t, uint32_t);
void __real_wgpuComputePassEncoderEnd(WGPUComputePassEncoder);
void __real_wgpuComputePassEncoderRelease(WGPUComputePassEncoder);
void __real_wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder, uint32_t, WGPUBindGroup, size_t, uint32_t const*);
void __real_wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder, WGPUComputePipeline);
void __real_wgpuRenderPassEncoderDraw(WGPURenderPassEncoder, uint32_t, uint32_t, uint32_t, uint32_t);
void __real_wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
void __real_wgpuRenderPassEncoderEnd(WGPURenderPassEncoder);
void __real_wgpuRenderPassEncoderPopDebugGroup(WGPURenderPassEncoder);
void __real_wgpuRenderPassEncoderPushDebugGroup(WGPURenderPassEncoder, WGPUStringView);
void __real_wgpuRenderPassEncoderRelease(WGPURenderPassEncoder);
void __real_wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder, uint32_t, WGPUBindGroup, size_t, uint32_t const*);
void __real_wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder, WGPUColor const*);
void __real_wgpuRenderPassEncoderSetImmediates(WGPURenderPassEncoder, uint32_t, void const*, size_t);
void __real_wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder, WGPUBuffer, WGPUIndexFormat, uint64_t, uint64_t);
void __real_wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder, WGPURenderPipeline);
void __real_wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder, uint32_t, uint32_t, uint32_t, uint32_t);
void __real_wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder, uint32_t, WGPUBuffer, uint64_t, uint64_t);
void __real_wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder, float, float, float, float, float, float);
void __real_wgpuQueueSubmit(WGPUQueue, size_t, WGPUCommandBuffer const*);
void __real_wgpuQueueWriteBuffer(WGPUQueue, WGPUBuffer, uint64_t, void const*, size_t);
void __real_wgpuQueueWriteTexture(WGPUQueue, WGPUTexelCopyTextureInfo const*, void const*, size_t,
                                  WGPUTexelCopyBufferLayout const*, WGPUExtent3D const*);
}

namespace swdk {
void frame_open();
void submit(const char* why);
}

namespace mp2dk {
using aurora::gfx::dk::BindGroup;
using aurora::gfx::dk::Samp;
using aurora::gfx::dk::Tex;
using aurora::gfx::dk::View;
namespace dko = aurora::gfx::dk;
using swdk::R;

namespace {

// ---- recorded ops
enum class OpType : uint8_t {
    BeginPass, EndPass, SetPipeline, SetBindGroup, SetVertexBuffer, SetIndexBuffer, Draw, DrawIndexed, Viewport,
    Scissor, BlendConstant, Immediates, CopyB2B, CopyB2T, CopyT2B, CopyT2T, Compute,
};

struct Attachment {
    View* view = nullptr;
    View* resolve = nullptr;
    bool clear = false;
    bool discard = false;  // mp2-nx (deko-17): StoreOp Discard
    WGPUColor clearValue{};
};
struct PassDesc {
    Attachment colors[8];
    uint32_t colorCount = 0;
    View* depth = nullptr;
    bool clearDepth = false, clearStencil = false;
    bool discardDepth = false;  // mp2-nx (deko-17): depth StoreOp Discard
    char label[48] = "";        // mp2-nx (frame capture): Aurora's pass label
    float depthValue = 1.0f;
    uint8_t stencilValue = 0;
};
struct CopyDesc {
    Buf* buf = nullptr;
    Buf* buf2 = nullptr;
    Tex* tex = nullptr;
    Tex* tex2 = nullptr;
    uint64_t offset = 0, offset2 = 0, size = 0;
    uint32_t bytesPerRow = 0, rowsPerImage = 0;
    uint32_t mip = 0, mip2 = 0;
    WGPUOrigin3D origin{}, origin2{};
    WGPUExtent3D extent{};
};

struct Op {
    OpType type;
    void* p = nullptr;
    union {
        uint32_t u[8];
        float f[8];
        int32_t i[8];
    };
    uint64_t q[2] = {};
};

struct Enc {
    std::atomic<int32_t> refs{1};
    std::vector<Op> ops;
    std::vector<PassDesc> passes;
    std::vector<CopyDesc> copies;
    std::vector<uint32_t> words;  // dynamic offsets, immediates
    // what the ops name, released after the replay
    std::vector<View*> views;
    std::vector<BindGroup*> groups;
    std::vector<Buf*> bufs;
    std::vector<RPipe*> pipes;
    std::vector<Tex*> texs;
    bool finished = false;
};

struct PassEnc {
    std::atomic<int32_t> refs{1};
    Enc* enc = nullptr;
    bool compute = false;
};

Enc* enc_of(WGPUCommandEncoder e) { return reinterpret_cast<Enc*>(e); }
PassEnc* pass_of(WGPURenderPassEncoder p) { return reinterpret_cast<PassEnc*>(p); }
PassEnc* cpass_of(WGPUComputePassEncoder p) { return reinterpret_cast<PassEnc*>(p); }

void tex_retain(Tex* t) { dko::retain(t); }
void tex_release(Tex* t) { dko::release(t); }

void enc_free(Enc* e) {
    for (View* v : e->views) dko::release(v);
    for (BindGroup* g : e->groups) dko::release(g);
    for (Buf* b : e->bufs) buffer_release(b);
    for (RPipe* p : e->pipes) pipeline_release(p);
    for (Tex* t : e->texs) tex_release(t);
    delete e;
}
void enc_release(Enc* e) {
    if (e->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) enc_free(e);
}

// ---- serials and fences
constexpr uint32_t kFenceRing = 64;
DkFence g_fences[kFenceRing];
std::mutex g_submitMutex;           // R.cmd, the frame, the fences, the pending queue ops
uint64_t g_nextSerial = 1;          // under g_submitMutex
std::atomic<uint64_t> g_completed{0};

bool fence_done(uint64_t serial) {
    if (serial == 0 || serial <= g_completed.load(std::memory_order_acquire)) return true;
    if (serial >= g_nextSerial) return false;
    if (dkFenceWait(&g_fences[serial % kFenceRing], 0) == DkResult_Success) {
        uint64_t c = g_completed.load(std::memory_order_relaxed);
        while (c < serial && !g_completed.compare_exchange_weak(c, serial)) {
        }
        return true;
    }
    return false;
}

// ---- the upload ring (queue writes)
constexpr uint32_t kRingSize = 48u << 20;
swdk::Block g_ring;
std::mutex g_ringMutex;
uint32_t g_ringHead = 0;
struct RingUse {
    uint32_t begin, end;
    uint64_t id;
    uint64_t serial;  // the submit that copies it (UINT64_MAX until its op is queued)
};
std::deque<RingUse> g_ringUses;
uint64_t g_ringIds = 0;

// queue ops waiting for the next submit (g_submitMutex)
struct QueueOp {
    bool texture = false;
    bool zero = false;  // mp2-nx (deko-11): zero the whole texture (a new one), from g_zero
    Buf* buf = nullptr;
    Tex* tex = nullptr;
    uint64_t dstOffset = 0;
    DkGpuAddr src = 0;
    uint32_t size = 0, rowBytes = 0, rows = 0, mip = 0;
    WGPUOrigin3D origin{};
    WGPUExtent3D extent{};
    swdk::Block own;      // a write too big for the ring: its own block, freed after the submit
    uint64_t ringId = 0;  // its ring use (0: none)
};
std::vector<QueueOp> g_queueOps;
// mp2-nx (deko-11): zeros, the source of new textures' first copy
constexpr uint32_t kZeroSize = 4u << 20;
swdk::Block g_zero;

void submit_locked(const char* why);

// Space in the ring (g_ringMutex held by the caller, released while it waits for the GPU).
DkGpuAddr ring_alloc(std::unique_lock<std::mutex>& ringLock, uint32_t size, uint8_t** cpu, uint64_t* id) {
    size = (size + 255u) & ~255u;
    *cpu = nullptr;
    if (size > kRingSize) return 0;
    for (int attempt = 0; attempt < 256; attempt++) {
        while (!g_ringUses.empty() && g_ringUses.front().serial != UINT64_MAX && fence_done(g_ringUses.front().serial))
            g_ringUses.pop_front();
        int64_t at = -1;
        if (g_ringUses.empty()) {
            at = 0;
        } else {
            const uint32_t oldest = g_ringUses.front().begin, newest = g_ringUses.back().end;
            const bool wrapped = g_ringUses.back().begin < oldest;
            if (!wrapped) {
                if (newest + size <= kRingSize) at = newest;
                else if (size <= oldest) at = 0;
            } else if (newest + size <= oldest) {
                at = newest;
            }
        }
        if (at >= 0) {
            g_ringUses.push_back({uint32_t(at), uint32_t(at) + size, ++g_ringIds, UINT64_MAX});
            *cpu = g_ring.cpu + at;
            *id = g_ringIds;
            return g_ring.gpu + at;
        }
        // full: what is in it must reach the GPU and finish
        uint64_t wait = 0;
        if (!g_ringUses.empty()) wait = g_ringUses.front().serial;
        ringLock.unlock();
        {
            std::lock_guard<std::mutex> lock(g_submitMutex);
            if (!g_queueOps.empty()) submit_locked("the upload ring is full");
        }
        if (wait != 0 && wait != UINT64_MAX) serial_wait(wait);
        else svcSleepThread(1'000'000);
        ringLock.lock();
    }
    log("encoder: the upload ring stayed full: a queue write was dropped");
    return 0;
}

// with g_submitMutex held: the op joins the next submit, and its ring use learns that submit's serial
void queue_push(QueueOp& q) {
    if (q.ringId) {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        for (auto it = g_ringUses.rbegin(); it != g_ringUses.rend(); ++it)
            if (it->id == q.ringId) {
                it->serial = g_nextSerial;
                break;
            }
    }
    g_queueOps.push_back(q);
}

// ---- mp2-nx (deko-14): the test cycle
// deko3d_env.txt MP2DK_CYCLE=<seconds> steps through the modes below, one every <seconds>, round and round,
// without a restart (the mode is read at every draw); MP2DK_MODE=<n> holds one mode instead. The mode is
// shown on screen under the FPS counter (mp2nx_dk_cycle_label, switch_menu.cpp) and logged when it changes.
// The Dear ImGui overlay is never skipped.
// mp2-nx (deko-17): deko-16's screenshots put the white floor and grey strips in see-through group 2, which
// hid only three pipelines (by label hash, kSuspects). One mode hides each; one samples the small EFB copies
// ('Resolved Texture', 51x51 in the world pipelines) as transparent black; one applies WebGPU's rule that a
// target a pass threw away (StoreOp Discard) reads as zeros; one hides the rest of group 2.
enum CycleMode : int {
    kModeNormal, kModeSuspect1, kModeSuspect2, kModeSuspect3, kModeBlankResolved, kModeDiscardRules, kModeRestOfGroup2,
    kModeCount
};
constexpr uint32_t kGroups = 6;
const char* const kSuspects[3] = {"GX Pipeline 8b66dcab231efa19", "GX Pipeline 50d2b937df92bb30",
                                  "GX Pipeline f04c1df9b8e47a11"};
const char* const kModeNames[kModeCount] = {
    "normal",
    "pipeline 8b66dcab hidden",
    "pipeline 50d2b937 hidden",
    "pipeline f04c1df9 hidden",
    "small EFB copies blank",
    "WebGPU discard rule",
    "rest of group 2 hidden",
};
std::atomic<int> g_mode{kModeNormal};
bool g_cycleOn = false;
uint64_t g_cycleSeconds = 0, g_cycleStart = 0;
int g_cycleLeft = 0;  // seconds left in this mode
uint64_t g_modeDraws = 0, g_modeHidden = 0;
char g_cycleLabel[128];
// mp2-nx (deko-16): the test cycle's screenshots (shot_take_locked)
bool g_shotPending = false;
// mp2-nx (deko-17): passes that loaded a target the last pass threw away; samples of such textures
uint64_t g_discardLoads = 0, g_discardDepthLoads = 0;
std::atomic<uint64_t> g_discardSamples{0};
Tex* g_resolvedTex = nullptr;  // the last small EFB copy a draw sampled (retained), for the screenshots
char g_shotName[96];
swdk::Block g_shotBuf;
constexpr uint64_t kShotLoops = 3;

bool g_cycleStarted = false;
void cycle_set(int mode) {
    const int old = g_mode.exchange(mode, std::memory_order_relaxed);
    if (old == mode && g_cycleStarted) return;
    // mp2-nx (deko-15): the counts belong to the mode that ended (deko-14 printed them on the next mode's line)
    if (g_cycleStarted)
        log("test cycle: mode %d (%s) ended: %llu draws, %llu hidden", old + 1, kModeNames[old],
            (unsigned long long)g_modeDraws, (unsigned long long)g_modeHidden);
    g_cycleStarted = true;
    log("test cycle: mode %d of %d, %s", mode + 1, int(kModeCount), kModeNames[mode]);
    g_modeDraws = g_modeHidden = 0;
}

// ---- replay
struct BoundGroup {
    BindGroup* g = nullptr;
    uint32_t dyn[8] = {};
    uint32_t dynCount = 0;
};
struct Replay {
    // pass
    bool inPass = false;
    uint32_t width = 0, height = 0;
    const PassDesc* pass = nullptr;
    // state
    RPipe* pipe = nullptr;
    RPipe* boundPipe = nullptr;
    const DkShader* vs = nullptr;
    const DkShader* fs = nullptr;
    BoundGroup groups[4];
    bool bindingsDirty = true;
    struct {
        Buf* b = nullptr;
        uint64_t offset = 0, size = 0;
    } vb[8], ib;
    WGPUIndexFormat ibFormat = WGPUIndexFormat_Uint16;
    bool vbDirty = true, ibDirty = true;
    DkGpuAddr immediates = 0;
    bool transfer = false;  // the last work was copy-engine work (between Full barriers)
    uint64_t serial = 0;
    // statistics
    uint64_t draws = 0, skippedDraws = 0;
};

struct Totals {
    uint64_t submits = 0, draws = 0, skipped = 0, passes = 0, copies = 0, uploads = 0, uploadBytes = 0, computes = 0;
    uint64_t depthZeroed = 0;  // mp2-nx (deko-14b)
} g_totals;

void mark(Buf* b, Replay& r) {
    if (b) b->lastUse.store(r.serial, std::memory_order_release);
}

constexpr uint32_t kInvalidateAll =
    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache | DkInvalidateFlags_Descriptors;

void transfer_begin(Replay& r) {
    if (!r.transfer) dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    r.transfer = true;
}
void work_begin(Replay& r) {
    if (r.transfer) dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    r.transfer = false;
}

uint32_t level_size(uint32_t size, uint32_t level) { return std::max(size >> level, 1u); }

// a copy that would write past the end of its buffer: skipped (the GPU would write over whatever memory
// follows the buffer's block)
void bad_copy(const char* what, uint64_t need, uint64_t size) {
    static int logged = 0;
    if (logged++ < 16) log("encoder: a copy %s needs %llu bytes of a %llu-byte buffer: skipped", what,
                           (unsigned long long)need, (unsigned long long)size);
}


// mp2-nx (deko-12 diagnostics): a copy into or out of a texture dropped before it reached deko3d
void dropped_copy(const char* what, const Tex* t, uint32_t mip, WGPUOrigin3D o, WGPUExtent3D e) {
    static int logged = 0;
    if (logged++ < 40)
        log("diag: %s dropped: '%s' (%ux%u, %u mips) level %u at %u,%u,%u size %ux%ux%u", what,
            t ? t->label.c_str() : "?", t ? t->width : 0, t ? t->height : 0, t ? t->mips : 0, mip, o.x, o.y, o.z,
            e.width, e.height, e.depthOrArrayLayers);
}

bool is_int_format(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_R8Uint: case WGPUTextureFormat_R8Sint: case WGPUTextureFormat_R16Uint:
    case WGPUTextureFormat_R16Sint: case WGPUTextureFormat_RG8Uint: case WGPUTextureFormat_RG8Sint:
    case WGPUTextureFormat_R32Uint: case WGPUTextureFormat_R32Sint: case WGPUTextureFormat_RG16Uint:
    case WGPUTextureFormat_RG16Sint: case WGPUTextureFormat_RGBA8Uint: case WGPUTextureFormat_RGBA8Sint:
    case WGPUTextureFormat_RG32Uint: case WGPUTextureFormat_RG32Sint: case WGPUTextureFormat_RGBA16Uint:
    case WGPUTextureFormat_RGBA16Sint: case WGPUTextureFormat_RGBA32Uint: case WGPUTextureFormat_RGBA32Sint:
    case WGPUTextureFormat_RGB10A2Uint:
        return true;
    default:
        return false;
    }
}

void copy_image(Replay& r, Tex* src, uint32_t srcMip, WGPUOrigin3D so, Tex* dst, uint32_t dstMip, WGPUOrigin3D dO,
                WGPUExtent3D e) {
    if (!src || !dst || !src->valid || !dst->valid) {
        dropped_copy("a texture copy (an invalid texture)", dst, dstMip, dO, e);
        return;
    }
    uint32_t w = e.width, h = e.height;
    const uint32_t sw = level_size(src->width, srcMip), shh = level_size(src->height, srcMip);
    const uint32_t dw = level_size(dst->width, dstMip), dh = level_size(dst->height, dstMip);
    uint32_t sx = so.x, sy = so.y, dx = dO.x, dy = dO.y;
    if (sx >= sw || sy >= shh || dx >= dw || dy >= dh) {
        dropped_copy("a texture copy (outside the level)", dst, dstMip, dO, e);
        return;
    }
    w = std::min({w, sw - sx, dw - dx});
    h = std::min({h, shh - sy, dh - dy});
    if (w == 0 || h == 0) return;
    if (dst->depth != src->depth) dropped_copy("note: a copy between depth and colour", dst, dstMip, dO, e);
    dst->written = true;
    dst->discarded = false;
    if (dstMip < 32) dst->mipsWritten.fetch_or(1u << dstMip, std::memory_order_relaxed);
    if (src->compressed) {
        w = (w + src->blockW - 1) / src->blockW;
        h = (h + src->blockH - 1) / src->blockH;
        sx /= src->blockW;
        sy /= src->blockH;
        dx /= dst->blockW;
        dy /= dst->blockH;
    }
    transfer_begin(r);
    const DkImageView sv = level_view(src, srcMip), dv = level_view(dst, dstMip);
    const DkImageRect sr = {sx, sy, so.z, w, h, std::max<uint32_t>(e.depthOrArrayLayers, 1)};
    const DkImageRect dr = {dx, dy, dO.z, w, h, std::max<uint32_t>(e.depthOrArrayLayers, 1)};
    dkCmdBufCopyImage(R.cmd, &sv, &sr, &dv, &dr, 0);
    g_totals.copies++;
}

// mp2-nx (frame capture): XXH3 of a level's tight rows (blocks x block bytes) at rowBytes apart
uint64_t level_hash(const uint8_t* p, uint32_t rowBytes, const Tex* t, uint32_t w, uint32_t h) {
    const uint32_t tight = (w + t->blockW - 1) / t->blockW * t->blockBytes, rows = (h + t->blockH - 1) / t->blockH;
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    for (uint32_t y = 0; y < rows; y++) XXH3_64bits_update(st, p + size_t(y) * rowBytes, tight);
    const uint64_t hash = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    return hash ? hash : 1;
}

void buffer_to_image(Replay& r, DkGpuAddr src, uint32_t rowBytes, uint32_t rows, Tex* t, uint32_t mip, WGPUOrigin3D o,
                     WGPUExtent3D e, const uint8_t* cpu = nullptr) {
    if (!t || !t->valid || mip >= t->mips) {
        dropped_copy("an upload (an invalid texture or level)", t, mip, o, e);
        return;
    }
    const uint32_t lw = level_size(t->width, mip), lh = level_size(t->height, mip);
    if (o.x >= lw || o.y >= lh || o.z >= (t->is3d ? level_size(t->layers, mip) : t->layers)) {
        dropped_copy("an upload (outside the level)", t, mip, o, e);
        return;
    }
    const uint32_t w = std::min(e.width, lw - o.x), h = std::min(e.height, lh - o.y);
    if (w == 0 || h == 0) return;
    t->discarded = false;
    if (mip == 0 && o.x == 0 && o.y == 0 && o.z == 0 && w == lw && h == lh && t->layers == 1 && !t->is3d)
        t->uploadHash = cpu && src != g_zero.gpu ? level_hash(cpu, rowBytes, t, w, h) : 0;
    if (src != g_zero.gpu) {
        t->written = true;
        if (mip < 32) t->mipsWritten.fetch_or(1u << mip, std::memory_order_relaxed);
    }
    transfer_begin(r);
    const DkCopyBuf buf = {src, rowBytes, rowBytes * std::max(rows, 1u)};
    const DkImageView view = level_view(t, mip);
    const DkImageRect rect = {o.x, o.y, o.z, w, h, std::max<uint32_t>(e.depthOrArrayLayers, 1)};
    dkCmdBufCopyBufferToImage(R.cmd, &buf, &view, &rect, swdk::C.flipTexture ? uint32_t(DkBlitFlag_FlipY) : 0u);
    g_totals.uploads++;
}

void end_pass(Replay& r) {
    if (!r.inPass) return;
    // resolve targets: the single-sampled colour copied into them
    const PassDesc& p = *r.pass;
    // mp2-nx (deko-17): what this pass threw away (WebGPU: it reads as zeros until written again)
    for (uint32_t i = 0; i < p.colorCount; i++)
        if (p.colors[i].view && p.colors[i].view->tex) p.colors[i].view->tex->discarded = p.colors[i].discard;
    if (p.depth && p.depth->tex) p.depth->tex->discarded = p.discardDepth;
    bool resolved = false;
    for (uint32_t i = 0; i < p.colorCount; i++) {
        const Attachment& a = p.colors[i];
        if (!a.resolve || !a.view || !a.view->tex || !a.resolve->tex) continue;
        if (!resolved) dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        resolved = true;
        const WGPUOrigin3D zero{0, 0, 0};
        copy_image(r, a.view->tex, a.view->baseMip, zero, a.resolve->tex, a.resolve->baseMip, zero,
                   WGPUExtent3D{r.width, r.height, 1});
    }
    if (resolved) {
        dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
        r.transfer = false;
    } else {
        dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    }
    r.inPass = false;
}

// A colour slot without a usable target (WebGPU allows a null view between used ones; a view without a
// deko3d image): deko3d needs an image in every slot it binds, so trailing empty slots are not bound and
// one in the middle gets a scratch image of the pass's size (its writes go nowhere useful).
struct Scratch {
    uint32_t w = 0, h = 0;
    DkImage image{};
    swdk::ImageAlloc mem;
};
std::vector<Scratch> g_scratch;
const DkImage* scratch_image(uint32_t w, uint32_t h) {
    for (Scratch& s : g_scratch)
        if (s.w == w && s.h == h) return &s.image;
    Scratch s;
    s.w = w;
    s.h = h;
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.flags = DkImageFlags_UsageRender;
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = w;
    m.dimensions[1] = h;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    s.mem = swdk::image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&s.image, &layout, s.mem.block, s.mem.offset);
    g_scratch.push_back(s);
    log("encoder: a scratch colour target %ux%u for an empty slot between used ones", w, h);
    return &g_scratch.back().image;
}

const char* why_unusable(const View* v) {
    if (!v) return "no view (an empty slot)";
    if (!v->tex) return "a view without a texture shadow";
    if (!v->tex->valid) return "a texture without a deko3d image (its format)";
    return nullptr;
}

void begin_pass(Replay& r, const PassDesc& p) {
    work_begin(r);
    r.inPass = true;
    r.pass = &p;
    r.width = r.height = 0;
    DkImageView colors[8];
    const DkImageView* colorPtrs[8] = {};
    for (uint32_t i = 0; i < p.colorCount; i++) {
        View* v = p.colors[i].view;
        if (v && dko::alive(v, "a pass's colour target") && v->tex && dko::alive(v->tex, "a colour target's texture") &&
            v->tex->valid) {
            colors[i] = v->view;
            colors[i].pImage = &v->tex->image;
            colors[i].mipLevelCount = 1;
            colorPtrs[i] = &colors[i];
            v->tex->written = true;
            if (v->baseMip < 32) v->tex->mipsWritten.fetch_or(1u << v->baseMip, std::memory_order_relaxed);
            if (!r.width) {
                r.width = level_size(v->tex->width, v->baseMip);
                r.height = level_size(v->tex->height, v->baseMip);
            }
        }
    }
    DkImageView depth;
    const DkImageView* depthPtr = nullptr;
    if (p.depth && dko::alive(p.depth, "a pass's depth target") && p.depth->tex &&
        dko::alive(p.depth->tex, "a depth target's texture") && p.depth->tex->valid) {
        depth = p.depth->view;
        depth.pImage = &p.depth->tex->image;
        depth.mipLevelCount = 1;
        depthPtr = &depth;
        p.depth->tex->written = true;
        if (p.depth->baseMip < 32) p.depth->tex->mipsWritten.fetch_or(1u << p.depth->baseMip, std::memory_order_relaxed);
        if (!r.width) {
            r.width = level_size(p.depth->tex->width, p.depth->baseMip);
            r.height = level_size(p.depth->tex->height, p.depth->baseMip);
        }
    }
    // empty slots: trailing ones are left out, middle ones get a scratch image
    uint32_t bound = p.colorCount;
    while (bound > 0 && !colorPtrs[bound - 1]) bound--;
    for (uint32_t i = 0; i < p.colorCount; i++) {
        if (colorPtrs[i]) continue;
        static int logged = 0;
        if (logged++ < 8) log("encoder: render pass colour slot %u of %u: %s", i, p.colorCount, why_unusable(p.colors[i].view));
        if (i < bound && r.width) {
            dkImageViewDefaults(&colors[i], scratch_image(r.width, r.height));
            colorPtrs[i] = &colors[i];
        }
    }
    if (bound > 0 && !r.width) bound = 0;
    dkCmdBufBindRenderTargets(R.cmd, colorPtrs, bound, depthPtr);
    static const DkViewportSwizzle kSwizzle = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                               DkSwizzle_PositiveW};
    dkCmdBufSetViewportSwizzles(R.cmd, 0, &kSwizzle, 1);
    const DkViewport vp = {0.f, 0.f, float(r.width), float(r.height), 0.f, 1.f};
    const DkScissor sc = {0, 0, r.width, r.height};
    dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
    dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
    // mp2-nx (deko-17): a target the last pass threw away and this one loads (WebGPU: zeros)
    const bool discardRules = g_cycleOn && g_mode.load(std::memory_order_relaxed) == kModeDiscardRules;
    bool lazyColor[8] = {};
    for (uint32_t i = 0; i < bound; i++) {
        const Attachment& a = p.colors[i];
        if (a.clear || !colorPtrs[i] || !a.view || !a.view->tex || !a.view->tex->discarded) continue;
        g_discardLoads++;
        lazyColor[i] = discardRules;
    }
    bool lazyDepth = false;
    if (depthPtr && !p.clearDepth && p.depth->tex->discarded) {
        g_discardLoads++;
        g_discardDepthLoads++;
        lazyDepth = discardRules;
    }
    for (uint32_t i = 0; i < bound; i++) {
        const Attachment& a = p.colors[i];
        if (lazyColor[i]) {
            dkCmdBufClearColorFloat(R.cmd, i, DkColorMask_RGBA, 0.f, 0.f, 0.f, 0.f);
            continue;
        }
        if (!a.clear || !colorPtrs[i] || !a.view || !a.view->tex || !a.view->tex->valid) continue;
        const WGPUColor& c = a.clearValue;
        if (is_int_format(a.view->tex->wformat)) {
            dkCmdBufClearColorUint(R.cmd, i, DkColorMask_RGBA, uint32_t(c.r), uint32_t(c.g), uint32_t(c.b), uint32_t(c.a));
        } else {
            dkCmdBufClearColorFloat(R.cmd, i, DkColorMask_RGBA, float(c.r), float(c.g), float(c.b), float(c.a));
        }
    }
    if (lazyDepth)
        dkCmdBufClearDepthStencil(R.cmd, true, 0.f, 0xFF, p.clearStencil ? p.stencilValue : 0);
    else if (depthPtr && (p.clearDepth || p.clearStencil))
        dkCmdBufClearDepthStencil(R.cmd, p.clearDepth, p.depthValue, p.clearStencil ? 0xFF : 0, p.stencilValue);
    // everything is bound again in a pass
    r.boundPipe = nullptr;
    r.vs = r.fs = nullptr;
    r.bindingsDirty = r.vbDirty = r.ibDirty = true;
    // mp2-nx (deko-14): WebGPU starts every render pass with a blend constant of 0 (deko3d keeps the last)
    dkCmdBufSetBlendConst(R.cmd, 0.f, 0.f, 0.f, 0.f);
    if (r.immediates) {
        dkCmdBufBindUniformBuffer(R.cmd, DkStage_Vertex, swdk::kImmediatesUboSlot, r.immediates, 256);
        dkCmdBufBindUniformBuffer(R.cmd, DkStage_Fragment, swdk::kImmediatesUboSlot, r.immediates, 256);
    }
    g_totals.passes++;
}

void bind_pipeline(Replay& r, RPipe* p) {
    if (r.boundPipe == p) return;
    const DkShader* vs = &p->vs.entry->shader;
    const DkShader* fs = p->hasFs ? &p->fs.entry->shader : nullptr;
    if (vs != r.vs || fs != r.fs) {
        if (swdk::code_take_written()) dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Shader);
        const DkShader* sh[2] = {vs, fs};
        dkCmdBufBindShaders(R.cmd, DkStageFlag_GraphicsMask, sh, fs ? 2 : 1);
        r.vs = vs;
        r.fs = fs;
    }
    dkCmdBufBindRasterizerState(R.cmd, &p->rasterizer);
    dkCmdBufBindColorState(R.cmd, &p->color);
    dkCmdBufBindColorWriteState(R.cmd, &p->colorWrite);
    dkCmdBufBindBlendStates(R.cmd, 0, p->blend, std::max<uint32_t>(p->colorCount, 1));
    dkCmdBufBindDepthStencilState(R.cmd, &p->depthStencil);
    if (p->depthBias) dkCmdBufSetDepthBias(R.cmd, p->bias[0], p->bias[1], p->bias[2]);
    dkCmdBufBindVtxAttribState(R.cmd, p->attribCount ? p->attribs : nullptr, p->attribCount);
    dkCmdBufBindVtxBufferState(R.cmd, p->vbufCount ? p->vbufs : nullptr, p->vbufCount);
    dkCmdBufSetPrimitiveRestart(R.cmd, p->restart, r.ibFormat == WGPUIndexFormat_Uint32 ? 0xFFFFFFFFu : 0xFFFFu);
    r.boundPipe = p;
    r.bindingsDirty = true;
    r.vbDirty = true;
}

void bind_stage(Replay& r, const Stage& s, DkStage stage) {
    for (const swdk::SlotBinding& b : s.bindings) {
        if (b.kind == swdk::SlotBinding::Kind::Immediates) continue;  // bound at the pass start
        if (b.group >= 4 || !r.groups[b.group].g) continue;
        if (!dko::alive(r.groups[b.group].g, "a draw's bind group")) continue;
        const BoundGroup& bg = r.groups[b.group];
        const BindGroup::Entry* e = dko::bind_group_entry(bg.g, b.binding);
        if (b.kind == swdk::SlotBinding::Kind::Sampler) {
            View* view = e ? e->view : nullptr;
            if (view && !dko::alive(view, "a bind group's texture view")) view = nullptr;
            Samp* samp = nullptr;
            if (b.samplerGroup < 4 && r.groups[b.samplerGroup].g) {
                const BindGroup::Entry* se = dko::bind_group_entry(r.groups[b.samplerGroup].g, b.samplerBinding);
                if (se && dko::alive(se->sampler, "a bind group's sampler")) samp = se->sampler;
            }
            dkCmdBufBindTexture(R.cmd, stage, b.slot, dko::texture_handle(view, samp));
            // mp2-nx (deko-17): the small EFB copy this frame uses, for the screenshots
            if (g_cycleOn && view && view->tex && view->tex->smallResolved == 1 && view->tex != g_resolvedTex) {
                tex_retain(view->tex);
                if (g_resolvedTex) tex_release(g_resolvedTex);
                g_resolvedTex = view->tex;
            }
            continue;
        }
        if (!e || !e->buf || !e->buf->mem) continue;
        uint64_t offset = e->offset;
        if (e->dyn >= 0 && uint32_t(e->dyn) < bg.dynCount) offset += bg.dyn[e->dyn];
        uint64_t size = e->size == WGPU_WHOLE_SIZE ? e->buf->size - std::min(e->buf->size, offset) : e->size;
        mark(e->buf, r);
        if (b.kind == swdk::SlotBinding::Kind::Uniform) {
            size = std::min<uint64_t>(size, 0x10000);
            dkCmdBufBindUniformBuffer(R.cmd, stage, b.slot, e->buf->mem.gpu + offset, uint32_t((size + 255) & ~255ull));
        } else {
            dkCmdBufBindStorageBuffer(R.cmd, stage, b.slot, e->buf->mem.gpu + offset, uint32_t(size));
        }
    }
}

// mp2-nx (deko-15): whether a draw's fragment stage samples a texture that matches (the views bound for it)
template <class F> bool samples_any(const Replay& r, F&& match) {
    if (!r.pipe->hasFs) return false;
    for (const swdk::SlotBinding& b : r.pipe->fs.bindings) {
        if (b.kind != swdk::SlotBinding::Kind::Sampler || b.group >= 4 || !r.groups[b.group].g) continue;
        if (!dko::alive(r.groups[b.group].g, "a draw's bind group (test cycle)")) continue;
        const BindGroup::Entry* e = dko::bind_group_entry(r.groups[b.group].g, b.binding);
        View* v = e ? e->view : nullptr;
        if (!v || v->magic != dko::kAlive || !v->tex || v->tex->magic != dko::kAlive) continue;
        if (match(v)) return true;
    }
    return false;
}

// mp2-nx (deko-16): a draw the test cycle's mode hides (never the Dear ImGui overlay); the first draws of each
// pipeline hidden are logged with the textures they sample (what the group draws)
std::vector<uint32_t> g_loggedPipes;
bool cycle_hides(const Replay& r) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != kModeSuspect1 && mode != kModeSuspect2 && mode != kModeSuspect3 && mode != kModeRestOfGroup2)
        return false;
    if (r.vb[0].b && r.vb[0].b->ui) return false;
    RPipe* p = r.pipe;
    if (p->tag < 0) {
        p->tag = 0;
        for (int i = 0; i < 3; i++)
            if (p->label.find(kSuspects[i]) != std::string::npos) p->tag = int8_t(i + 1);
    }
    if (mode != kModeRestOfGroup2) return p->tag == mode - kModeSuspect1 + 1;
    if (p->tag != 0 || !(p->blendKinds & kBlendAlpha) || p->id % kGroups != 1) return false;
    if (g_loggedPipes.size() < 400 &&
        std::find(g_loggedPipes.begin(), g_loggedPipes.end(), r.pipe->id) == g_loggedPipes.end()) {
        g_loggedPipes.push_back(r.pipe->id);
        char tex[256] = "";
        size_t n = 0;
        samples_any(r, [&](const View* v) {
            if (n < sizeof tex - 1)
                n += snprintf(tex + n, sizeof tex - n, " '%s' %ux%u", v->tex->label.c_str(), v->tex->width,
                              v->tex->height);
            return false;
        });
        log("test cycle: mode %d hides pipeline %u '%s', textures:%s", mode + 1, r.pipe->id,
            r.pipe->label.c_str(), n ? tex : " none");
    }
    return true;
}

bool prepare_draw(Replay& r) {
    if (!r.inPass || !r.pipe) return false;
    if (!pipeline_ready(r.pipe)) {
        r.skippedDraws++;
        return false;
    }
    if (g_cycleOn) {
        g_modeDraws++;
        if (cycle_hides(r)) {
            g_modeHidden++;
            return false;
        }
    }
    bind_pipeline(r, r.pipe);
    if (r.bindingsDirty) {
        bind_stage(r, r.pipe->vs, DkStage_Vertex);
        if (r.pipe->hasFs) bind_stage(r, r.pipe->fs, DkStage_Fragment);
        r.bindingsDirty = false;
    }
    if (r.vbDirty) {
        for (uint32_t i = 0; i < r.pipe->vbufCount; i++) {
            if (!r.vb[i].b || !r.vb[i].b->mem) continue;
            const uint64_t size = r.vb[i].size == WGPU_WHOLE_SIZE ? r.vb[i].b->size - r.vb[i].offset : r.vb[i].size;
            dkCmdBufBindVtxBuffer(R.cmd, i, r.vb[i].b->mem.gpu + r.vb[i].offset, uint32_t(size));
            mark(r.vb[i].b, r);
        }
        r.vbDirty = false;
    }
    return true;
}

const char* const kOpNotes[] = {
    "replay: begin render pass", "replay: end render pass", "replay: set pipeline", "replay: set bind group",
    "replay: set vertex buffer", "replay: set index buffer", "replay: draw", "replay: draw indexed",
    "replay: viewport", "replay: scissor", "replay: blend constant", "replay: immediates",
    "replay: copy buffer to buffer", "replay: copy buffer to texture", "replay: copy texture to buffer",
    "replay: copy texture to texture", "replay: compute",
};

// ---- mp2-nx (frame capture): while Aurora records a capture (aurora_frame_capture_dir, encoding.cpp), every
// pass and draw replayed is listed in draws.txt in its folder, written when the capture ends.
std::string g_capText, g_capDir;
// (the snapshots, below)
constexpr uint32_t kSnapW = 256, kSnapH = 144, kSnapCols = 16, kSnapRows = 4, kSnapMax = 64;
constexpr uint32_t kSnapFirst = 40;  // every one of the frame's first 40 full-size draws, then every Nth
uint32_t g_fullDraws = 0, g_fullDrawsLast = 0, g_snapStride = 1, g_snapSeen = 0;
// Texture dumps: every texture a captured draw samples (its view's first level, as stored: BC1 blocks or
// texels), copied at that draw into one 24 MiB buffer and written as texNNN_WxH_fmtXX.raw; the draw lines
// name them (#NNN) with the descriptor slot.
struct Dump {
    const Tex* tex;
    uint32_t id, w, h, rowBytes, rows, offset;
    unsigned fmt;
    uint64_t expect;  // the texture's uploadHash when dumped (level 0 only; 0 unknown)
    std::string label;
    uint32_t slot;
};
std::vector<Dump> g_dumps;
uint32_t g_dumpUsed = 0;
constexpr uint32_t kDumpBytes = 24u << 20;
swdk::Block g_dumpBuf;
uint32_t g_capPass = 0, g_capDraw = 0;
float g_capViewport[4] = {}, g_capScissor[4] = {};

const char* cap_dir() {
    const char* d = aurora_frame_capture_dir();
    if (d && g_capDir.empty()) {
        g_capDir = d;
        g_capPass = 0;
        g_snapSeen = 0;
        g_snapStride = std::max<uint32_t>(
            1, (std::max(g_fullDrawsLast, kSnapFirst) - kSnapFirst + (kSnapMax - kSnapFirst) - 1) / (kSnapMax - kSnapFirst));
        g_dumps.clear();
        g_dumpUsed = 0;
        g_capText = "deko3d draws of the frame capture\n"
                    "blend: on/off, colour src dst op, alpha src dst op (deko3d DkBlendFactor/DkBlendOp numbers)\n"
                    "depth: test write compare (DkCompareOp)\n";
        char line[96];
        snprintf(line, sizeof line, "snapshots: the first %u full-size draws, then every %u (%u in the frame before)\n\n",
                 kSnapFirst, g_snapStride, g_fullDrawsLast);
        g_capText += line;
    }
    return d;
}

void cap_pass(const PassDesc& p) {
    if (!cap_dir()) return;
    g_capDraw = 0;
    char line[512];
    int n = snprintf(line, sizeof line, "\n== pass %u '%s'", g_capPass++, p.label);
    for (uint32_t i = 0; i < p.colorCount; i++) {
        const View* v = p.colors[i].view;
        if (v && v->tex)
            n += snprintf(line + n, sizeof line - n, " colour%u '%s' %ux%u fmt 0x%X %s%s", i, v->tex->label.c_str(),
                          v->tex->width, v->tex->height, unsigned(v->tex->wformat), p.colors[i].clear ? "clear" : "load",
                          p.colors[i].discard ? " discard" : "");
    }
    if (p.depth && p.depth->tex)
        snprintf(line + n, sizeof line - n, " depth '%s' %ux%u %s%s", p.depth->tex->label.c_str(), p.depth->tex->width,
                 p.depth->tex->height, p.clearDepth ? "clear" : "load", p.discardDepth ? " discard" : "");
    g_capText += line;
    g_capText += "\n";
}

// Snapshots: after draws into a full-size target (the EFB), the 2D engine shrinks the target into a 256x144
// cell of an atlas (16 x 4 cells); at the end the atlas is written as draws_atlas.pam, the cell number on the
// draw's line (snap N: column N % 16, row N / 16). Every draw was too much: deko3d aborted in the capture
// frame's present (signOffGpfifoEntry; Atmosphere report 01794020303), so the snapshots are spread over the
// frame: every Nth full-size draw, N from the frame before (g_fullDrawsLast), 60 at most.
DkMemBlock g_snapMem = nullptr;
DkImage g_snapImg;
uint32_t g_snapUsed = 0;
bool g_snapFailed = false;

bool snap_init() {
    if (g_snapMem) return true;
    if (g_snapFailed) return false;
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.flags = DkImageFlags_Usage2DEngine;
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = kSnapW * kSnapCols;
    m.dimensions[1] = kSnapH * kSnapRows;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    const uint32_t align = std::max<uint32_t>(dkImageLayoutGetAlignment(&layout), DK_MEMBLOCK_ALIGNMENT);
    const uint32_t size = (uint32_t(dkImageLayoutGetSize(&layout)) + align - 1) & ~(align - 1);
    DkMemBlockMaker bm;
    dkMemBlockMakerDefaults(&bm, R.device, size);
    bm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_snapMem = dkMemBlockCreate(&bm);
    if (!g_snapMem) {
        g_snapFailed = true;
        log("frame capture: no memory for the draw snapshots (%u KiB)", size >> 10);
        return false;
    }
    dkImageInitialize(&g_snapImg, &layout, g_snapMem, 0);
    return true;
}

int cap_snap(const Replay& r) {
    if (!r.pass || r.pass->colorCount == 0) return -1;
    const View* v = r.pass->colors[0].view;
    if (!v || !v->tex || !v->tex->valid || v->tex->width < 640 || v->tex->depth) return -1;
    const uint32_t seen = g_snapSeen++;
    if (seen >= kSnapFirst && (seen - kSnapFirst) % g_snapStride != g_snapStride - 1) return -1;
    if (g_snapUsed >= kSnapMax || !snap_init()) return -1;
    const uint32_t k = g_snapUsed++;
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    const DkImageView sv = level_view(v->tex, v->baseMip);
    DkImageView dv;
    dkImageViewDefaults(&dv, &g_snapImg);
    const DkImageRect srect = {0, 0, 0, v->tex->width, v->tex->height, 1};
    const DkImageRect drect = {(k % kSnapCols) * kSnapW, (k / kSnapCols) * kSnapH, 0, kSnapW, kSnapH, 1};
    dkCmdBufBlitImage(R.cmd, &sv, &srect, &dv, &drect, DkBlitFlag_FilterLinear, 0);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    return int(k);
}

void snap_write(const std::string& dir) {
    if (!g_snapUsed || !g_snapMem) return;
    const uint32_t rows = (g_snapUsed + kSnapCols - 1) / kSnapCols;
    const uint32_t w = kSnapW * kSnapCols, bandBytes = w * 4 * kSnapH;
    static swdk::Block band;
    if (!band) band = swdk::block_create_cached(bandBytes, "frame capture snapshots");
    if (!band) return;
    const std::string path = dir + "/draws_atlas.pam";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, rows * kSnapH);
    DkImageView v;
    dkImageViewDefaults(&v, &g_snapImg);
    for (uint32_t b = 0; b < rows; b++) {
        swdk::frame_open();
        dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
        const DkImageRect rect = {0, b * kSnapH, 0, w, kSnapH, 1};
        const DkCopyBuf buf = {band.gpu, w * 4, bandBytes};
        dkCmdBufCopyImageToBuffer(R.cmd, &v, &rect, &buf, 0);
        dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
        swdk::submit("frame capture snapshots");
        dkQueueWaitIdle(R.queue);
        armDCacheFlush(band.cpu, bandBytes);
        fwrite(band.cpu, 1, bandBytes, f);
    }
    fclose(f);
    log("frame capture: %u draw snapshots in %s", g_snapUsed, path.c_str());
    g_snapUsed = 0;
}

int cap_dump(const View* v) {
    const Tex* t = v->tex;
    for (const Dump& d : g_dumps)
        if (d.tex == t) return int(d.id);
    if (!t->valid || t->depth || t->external || t->samples > 1 || t->is3d || g_dumps.size() >= 400) return -1;
    if (!g_dumpBuf) g_dumpBuf = swdk::block_create_cached(kDumpBytes, "frame capture texture dumps");
    if (!g_dumpBuf) return -1;
    const uint32_t mip = v->baseMip;
    const uint32_t w = std::max(1u, t->width >> mip), h = std::max(1u, t->height >> mip);
    const uint32_t rowBytes = (w + t->blockW - 1) / t->blockW * t->blockBytes, rows = (h + t->blockH - 1) / t->blockH;
    const uint32_t size = (rowBytes * rows + 255) & ~255u;
    if (g_dumpUsed + size > kDumpBytes) return -1;
    Dump d{t, uint32_t(g_dumps.size()), w, h, rowBytes, rows, g_dumpUsed, unsigned(t->wformat),
           mip == 0 ? t->uploadHash : 0, t->label, v->slot};
    g_dumpUsed += size;
    const DkImageView iv = level_view(const_cast<Tex*>(t), mip);
    const DkImageRect rect = {0, 0, 0, w, h, 1};
    const DkCopyBuf buf = {g_dumpBuf.gpu + d.offset, rowBytes, rowBytes * rows};
    dkCmdBufCopyImageToBuffer(R.cmd, &iv, &rect, &buf, 0);
    g_dumps.push_back(d);
    return int(d.id);
}

void dumps_write(const std::string& dir) {
    if (g_dumps.empty() || !g_dumpBuf) return;
    dkQueueWaitIdle(R.queue);
    armDCacheFlush(g_dumpBuf.cpu, g_dumpUsed);
    std::string list = "textures sampled in the capture: id, label, size, WGPU format, slot, then whether level 0 as the GPU\n"
                       "holds it matches the bytes its upload copied from (match / DIFFERENT / unknown: not uploaded whole)\n\n";
    uint32_t different = 0;
    for (const Dump& d : g_dumps) {
        const char* verdict = "unknown";
        if (d.expect) {
            const uint64_t got = level_hash(g_dumpBuf.cpu + d.offset, d.rowBytes, d.tex, d.w, d.h);
            verdict = got == d.expect ? "match" : "DIFFERENT";
            different += got != d.expect;
        }
        char line[256];
        snprintf(line, sizeof line, "#%03u '%s' %ux%u fmt 0x%X slot %u %s\n", d.id, d.label.c_str(), d.w, d.h, d.fmt,
                 d.slot, verdict);
        list += line;
        char path[256];
        snprintf(path, sizeof path, "%s/tex%03u_%ux%u_fmt%02X.raw", dir.c_str(), d.id, d.w, d.h, d.fmt);
        if (FILE* f = fopen(path, "wb")) {
            fwrite(g_dumpBuf.cpu + d.offset, 1, size_t(d.rowBytes) * d.rows, f);
            fclose(f);
        }
    }
    if (FILE* f = fopen((dir + "/textures.txt").c_str(), "wb")) {
        fwrite(list.data(), 1, list.size(), f);
        fclose(f);
    }
    log("frame capture: %zu textures dumped (%u KiB), %u differ from their uploads", g_dumps.size(), g_dumpUsed >> 10,
        different);
    g_dumps.clear();
    g_dumpUsed = 0;
}

void cap_draw(const Replay& r, bool drawn, uint32_t count, uint32_t instances, bool indexed) {
    if (!cap_dir()) return;
    const RPipe* p = r.pipe;
    char line[1024];
    const int snap = drawn ? cap_snap(r) : -1;
    int n = snprintf(line, sizeof line, "draw %u%s %s %u x%u", g_capDraw++, drawn ? "" : " (NOT DRAWN)",
                     indexed ? "indexed" : "verts", count, instances);
    if (snap >= 0) n += snprintf(line + n, sizeof line - n, " | snap %d", snap);
    if (p) {
        const DkBlendState& b = p->blend[0];
        n += snprintf(line + n, sizeof line - n,
                      " | pipe %u '%s' | blend %s %d %d %d, %d %d %d | write 0x%X | depth %d %d %d | cull %d",
                      p->id, p->label.c_str(), (p->color.blendEnableMask & 1) ? "on" : "off",
                      int(b.srcColorBlendFactor), int(b.dstColorBlendFactor), int(b.colorBlendOp),
                      int(b.srcAlphaBlendFactor), int(b.dstAlphaBlendFactor), int(b.alphaBlendOp),
                      unsigned(p->colorWrite.masks & 0xF), int(p->depthStencil.depthTestEnable),
                      int(p->depthStencil.depthWriteEnable), int(p->depthStencil.depthCompareOp),
                      int(p->rasterizer.cullMode));
        n += snprintf(line + n, sizeof line - n, " | vp %.0f,%.0f %.0fx%.0f sc %.0f,%.0f %.0fx%.0f", g_capViewport[0],
                      g_capViewport[1], g_capViewport[2], g_capViewport[3], g_capScissor[0], g_capScissor[1],
                      g_capScissor[2], g_capScissor[3]);
        samples_any(r, [&](const View* v) {
            const int id = cap_dump(v);
            if (n < int(sizeof line) - 100)
                n += snprintf(line + n, sizeof line - n, " | tex '%s' %ux%u fmt 0x%X mips %u slot %u #%d",
                              v->tex->label.c_str(), v->tex->width, v->tex->height, unsigned(v->tex->wformat),
                              v->mipCount, v->slot, id);
            return false;
        });
    }
    g_capText += line;
    g_capText += "\n";
}

// at each present: a capture that has ended is written out
void cap_flush() {
    if (g_capDir.empty() || aurora_frame_capture_dir()) return;
    const std::string path = g_capDir + "/draws.txt";
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(g_capText.data(), 1, g_capText.size(), f);
        fclose(f);
        log("frame capture: %s", path.c_str());
    }
    snap_write(g_capDir);
    dumps_write(g_capDir);
    g_capText.clear();
    g_capText.shrink_to_fit();
    g_capDir.clear();
}

void replay(Replay& r, Enc* e) {
    for (const Op& op : e->ops) {
        note(kOpNotes[int(op.type)]);
        switch (op.type) {
        case OpType::BeginPass:
            cap_pass(e->passes[op.u[0]]);
            begin_pass(r, e->passes[op.u[0]]);
            break;
        case OpType::EndPass: end_pass(r); break;
        case OpType::SetPipeline:
            r.pipe = static_cast<RPipe*>(op.p);
            break;
        case OpType::SetBindGroup: {
            const uint32_t index = op.u[0];
            if (index >= 4) break;
            BoundGroup& bg = r.groups[index];
            bg.g = static_cast<BindGroup*>(op.p);
            bg.dynCount = std::min<uint32_t>(op.u[2], 8);
            for (uint32_t k = 0; k < bg.dynCount; k++) bg.dyn[k] = e->words[op.u[1] + k];
            r.bindingsDirty = true;
            break;
        }
        case OpType::SetVertexBuffer:
            if (op.u[0] < 8) {
                r.vb[op.u[0]].b = static_cast<Buf*>(op.p);
                r.vb[op.u[0]].offset = op.q[0];
                r.vb[op.u[0]].size = op.q[1];
                r.vbDirty = true;
            }
            break;
        case OpType::SetIndexBuffer: {
            Buf* b = static_cast<Buf*>(op.p);
            r.ibFormat = WGPUIndexFormat(op.u[0]);
            if (b && b->mem) {
                dkCmdBufBindIdxBuffer(R.cmd, r.ibFormat == WGPUIndexFormat_Uint32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16,
                                      b->mem.gpu + op.q[0]);
                mark(b, r);
            }
            break;
        }
        case OpType::Draw: {
            const bool ok = prepare_draw(r);
            if (ok && r.pass && r.pass->colorCount && r.pass->colors[0].view && r.pass->colors[0].view->tex &&
                r.pass->colors[0].view->tex->width >= 640)
                g_fullDraws++;
            if (ok) {
                dkCmdBufDraw(R.cmd, r.pipe->prim, op.u[0], op.u[1], op.u[2], op.u[3]);
                r.draws++;
            }
            if (!g_capDir.empty() || aurora_frame_capture_dir()) cap_draw(r, ok, op.u[0], op.u[1], false);
            break;
        }
        case OpType::DrawIndexed: {
            const bool ok = prepare_draw(r);
            if (ok && r.pass && r.pass->colorCount && r.pass->colors[0].view && r.pass->colors[0].view->tex &&
                r.pass->colors[0].view->tex->width >= 640)
                g_fullDraws++;
            if (ok) {
                dkCmdBufDrawIndexed(R.cmd, r.pipe->prim, op.u[0], op.u[1], op.u[2], op.i[3], op.u[4]);
                r.draws++;
            }
            if (!g_capDir.empty() || aurora_frame_capture_dir()) cap_draw(r, ok, op.u[0], op.u[1], true);
            break;
        }
        case OpType::Viewport: {
            if (!(op.f[2] > 0.f) || !(op.f[3] > 0.f)) break;
            const DkViewport v = {op.f[0], op.f[1], op.f[2], op.f[3], op.f[4], op.f[5]};
            for (int k = 0; k < 4; k++) g_capViewport[k] = op.f[k];
            dkCmdBufSetViewports(R.cmd, 0, &v, 1);
            break;
        }
        case OpType::Scissor: {
            const uint32_t x = std::min(op.u[0], r.width), y = std::min(op.u[1], r.height);
            const DkScissor s = {x, y, std::min(op.u[2], r.width - x), std::min(op.u[3], r.height - y)};
            dkCmdBufSetScissors(R.cmd, 0, &s, 1);
            g_capScissor[0] = float(s.x), g_capScissor[1] = float(s.y), g_capScissor[2] = float(s.width),
            g_capScissor[3] = float(s.height);
            break;
        }
        case OpType::BlendConstant: dkCmdBufSetBlendConst(R.cmd, op.f[0], op.f[1], op.f[2], op.f[3]); break;
        case OpType::Immediates:
            if (r.immediates && op.u[0] + op.u[1] <= 256)
                dkCmdBufPushConstants(R.cmd, r.immediates, 256, op.u[0], op.u[1], &e->words[op.u[2]]);
            break;
        case OpType::CopyB2B: {
            const CopyDesc& c = e->copies[op.u[0]];
            if (!c.buf || !c.buf2 || !c.buf->mem || !c.buf2->mem || c.size == 0) break;
            if (c.offset + c.size > c.buf->size || c.offset2 + c.size > c.buf2->size) {
                bad_copy("buffer to buffer", c.offset2 + c.size, c.buf2->size);
                break;
            }
            transfer_begin(r);
            dkCmdBufCopyBuffer(R.cmd, c.buf->mem.gpu + c.offset, c.buf2->mem.gpu + c.offset2, uint32_t(c.size));
            mark(c.buf, r);
            mark(c.buf2, r);
            g_totals.copies++;
            break;
        }
        case OpType::CopyB2T: {
            const CopyDesc& c = e->copies[op.u[0]];
            if (!c.buf || !c.buf->mem) break;
            buffer_to_image(r, c.buf->mem.gpu + c.offset, c.bytesPerRow, c.rowsPerImage ? c.rowsPerImage : c.extent.height,
                            c.tex, c.mip, c.origin, c.extent, c.buf->mem.cpu ? c.buf->mem.cpu + c.offset : nullptr);
            mark(c.buf, r);
            break;
        }
        case OpType::CopyT2B: {
            const CopyDesc& c = e->copies[op.u[0]];
            Tex* t = c.tex;
            if (!c.buf || !c.buf->mem || !t || !t->valid || c.mip >= t->mips) break;
            const uint32_t lw = level_size(t->width, c.mip), lh = level_size(t->height, c.mip);
            if (c.origin.x >= lw || c.origin.y >= lh) break;
            const uint32_t w = std::min(c.extent.width, lw - c.origin.x), h = std::min(c.extent.height, lh - c.origin.y);
            transfer_begin(r);
            const DkImageView v = level_view(t, c.mip);
            const DkImageRect rect = {c.origin.x, c.origin.y, c.origin.z, w, h,
                                      std::max<uint32_t>(c.extent.depthOrArrayLayers, 1)};
            const uint32_t rows = c.rowsPerImage ? c.rowsPerImage : h;
            const uint64_t need = c.offset + uint64_t(c.bytesPerRow) * (rows * (rect.depth - 1) + h - 1) +
                                  uint64_t(w) * t->blockBytes;
            if (need > c.buf->size) {
                bad_copy("texture to buffer", need, c.buf->size);
                break;
            }
            const DkCopyBuf out = {c.buf->mem.gpu + c.offset, c.bytesPerRow, c.bytesPerRow * rows};
            dkCmdBufCopyImageToBuffer(R.cmd, &v, &rect, &out, 0);
            mark(c.buf, r);
            g_totals.copies++;
            break;
        }
        case OpType::CopyT2T: {
            const CopyDesc& c = e->copies[op.u[0]];
            copy_image(r, c.tex, c.mip, c.origin, c.tex2, c.mip2, c.origin2, c.extent);
            break;
        }
        case OpType::Compute:
            g_totals.computes++;
            if (g_totals.computes <= 4) log("encoder: a compute dispatch, skipped (compute is not drawn yet)");
            break;
        }
    }
    end_pass(r);  // (a pass Aurora never ended)
}

// mp2-nx (deko-11): every level and layer of a new texture copied from zeros, a band of rows at a time
// (Dawn's textures start as zeros; without this a texture read before it is fully written, such as an
// empty lightmap or a target first loaded, showed whatever an earlier texture left in that memory).
// Depth textures are cleared to 0 the same way (deko-14b, queued since deko-15).
void zero_texture(Replay& r, Tex* t) {
    static const bool kNoZero = swdk::env_flag("MP2DK_NO_ZERO", false);  // deko3d_env.txt A/B
    if (kNoZero) return;
    if (!g_zero || !t || !t->valid || t->external) return;
    if (t->depth) {
        // mp2-nx (deko-14b): depth textures too (Dawn's start at 0; reused memory held another target's
        // depth): each level and layer is bound alone and cleared to depth 0, stencil 0
        static const bool kNoDepthZero = swdk::env_flag("MP2DK_NO_DEPTH_ZERO", false);
        if (kNoDepthZero) return;
        work_begin(r);
        DkDepthStencilState ds;
        dkDepthStencilStateDefaults(&ds);
        ds.depthTestEnable = true;
        ds.depthWriteEnable = true;
        ds.depthCompareOp = DkCompareOp_Always;
        dkCmdBufBindDepthStencilState(R.cmd, &ds);
        for (uint32_t m = 0; m < t->mips; m++) {
            const uint32_t lw = level_size(t->width, m), lh = level_size(t->height, m);
            for (uint32_t z = 0; z < t->layers; z++) {
                DkImageView v;
                dkImageViewDefaults(&v, &t->image);
                v.mipLevelOffset = m;
                v.mipLevelCount = 1;
                v.layerOffset = uint16_t(z);
                v.layerCount = 1;
                dkCmdBufBindRenderTargets(R.cmd, nullptr, 0, &v);
                const DkViewport vp = {0.f, 0.f, float(lw), float(lh), 0.f, 1.f};
                const DkScissor sc = {0, 0, lw, lh};
                dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
                dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
                dkCmdBufClearDepthStencil(R.cmd, true, 0.0f, 0xFF, 0);
            }
        }
        g_totals.depthZeroed++;
        return;
    }
    for (uint32_t m = 0; m < t->mips; m++) {
        const uint32_t lw = level_size(t->width, m), lh = level_size(t->height, m);
        const uint32_t blocksW = (lw + t->blockW - 1) / t->blockW, blocksH = (lh + t->blockH - 1) / t->blockH;
        const uint32_t pitch = (blocksW * t->blockBytes + 255u) & ~255u;
        if (pitch == 0 || pitch > kZeroSize) continue;
        const uint32_t band = std::max<uint32_t>(kZeroSize / pitch, 1);
        const uint32_t layers = t->is3d ? level_size(t->layers, m) : t->layers;
        for (uint32_t z = 0; z < layers; z++)
            for (uint32_t y = 0; y < blocksH; y += band) {
                const uint32_t rows = std::min(band, blocksH - y);
                const WGPUOrigin3D o{0, y * t->blockH, z};
                const WGPUExtent3D e{lw, std::min(rows * t->blockH, lh - y * t->blockH), 1};
                buffer_to_image(r, g_zero.gpu, pitch, rows, t, m, o, e);
            }
    }
}

void replay_queue_ops(Replay& r) {
    if (g_queueOps.empty()) return;
    note("replay: queue writes");
    for (QueueOp& q : g_queueOps) {
        if (q.zero) {
            zero_texture(r, q.tex);
            tex_release(q.tex);
        } else if (q.texture) {
            const uint8_t* cpu = q.own ? q.own.cpu
                                 : (q.src >= g_ring.gpu && q.src < g_ring.gpu + kRingSize) ? g_ring.cpu + (q.src - g_ring.gpu)
                                                                                         : nullptr;
            buffer_to_image(r, q.src, q.rowBytes, q.rows, q.tex, q.mip, q.origin, q.extent, cpu);
            tex_release(q.tex);
        } else {
            if (q.buf && q.buf->mem && q.dstOffset + q.size <= q.buf->size) {
                transfer_begin(r);
                dkCmdBufCopyBuffer(R.cmd, q.src, q.buf->mem.gpu + q.dstOffset, q.size);
                mark(q.buf, r);
            }
            if (q.buf) buffer_release(q.buf);
        }
        if (q.own) swdk::block_free_deferred(q.own.block);
    }
    g_queueOps.clear();
}

DkGpuAddr g_immediatesFrame = 0;
uint64_t g_immediatesFrameId = ~0ull;

// with g_submitMutex held: the pending queue ops and these command buffers to the GPU
void submit_cmds_locked(Enc* const* encs, size_t count, const char* why) {
    swdk::frame_open();
    Replay r;
    r.serial = g_nextSerial;
    if (g_immediatesFrameId != R.frame) {
        // the immediates block of this frame (256 bytes, updated in order by dkCmdBufPushConstants)
        const swdk::StreamAlloc s = swdk::stream_alloc(256, DK_UNIFORM_BUF_ALIGNMENT);
        g_immediatesFrame = s ? s.gpu : 0;
        if (s) memset(s.cpu, 0, 256);
        g_immediatesFrameId = R.frame;
    }
    r.immediates = g_immediatesFrame;
    replay_queue_ops(r);
    for (size_t i = 0; i < count; i++)
        if (encs[i]) replay(r, encs[i]);
    note("submit: fence and send");
    if (r.transfer) dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    // the submit's fence: its slot must be free (the submit kFenceRing earlier finished)
    const uint64_t serial = g_nextSerial;
    if (serial > kFenceRing) {
        const uint64_t old = serial - kFenceRing;
        if (!fence_done(old)) dkFenceWait(&g_fences[serial % kFenceRing], -1);
    }
    dkCmdBufSignalFence(R.cmd, &g_fences[serial % kFenceRing], true);
    swdk::submit(why);
    g_nextSerial = serial + 1;
    g_totals.submits++;
    g_totals.draws += r.draws;
    g_totals.skipped += r.skippedDraws;
    note("idle (between submits)");
}

void submit_locked(const char* why) { submit_cmds_locked(nullptr, 0, why); }

template <class T> uint32_t push_words(Enc* e, const T* data, size_t bytes) {
    const uint32_t at = uint32_t(e->words.size());
    e->words.resize(at + (bytes + 3) / 4);
    memcpy(&e->words[at], data, bytes);
    return at;
}

Op& add(Enc* e, OpType t) {
    e->ops.emplace_back();
    Op& op = e->ops.back();
    op.type = t;
    memset(op.u, 0, sizeof op.u);
    return op;
}

View* hold(Enc* e, WGPUTextureView v) {
    View* view = dko::find_view(v);
    if (view && !dko::alive(view, "recorded in a pass")) view = nullptr;
    if (view) {
        dko::retain(view);
        e->views.push_back(view);
    }
    return view;
}
Buf* hold(Enc* e, WGPUBuffer b) {
    Buf* buf = find_buffer(b);
    if (buf) {
        buffer_retain(buf);
        e->bufs.push_back(buf);
    }
    return buf;
}
Tex* hold(Enc* e, WGPUTexture t) {
    Tex* tex = dko::find_texture(t);
    if (tex && !dko::alive(tex, "recorded in a copy")) tex = nullptr;
    if (tex) {
        tex_retain(tex);
        e->texs.push_back(tex);
    }
    return tex;
}

}  // namespace

// ---- dk_bridge.hpp
uint64_t next_serial() {
    std::lock_guard<std::mutex> lock(g_submitMutex);
    return g_nextSerial;
}

bool serial_done(uint64_t serial) {
    if (serial == 0 || serial <= g_completed.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(g_submitMutex);
    return fence_done(serial);
}

void serial_wait(uint64_t serial) {
    if (serial_done(serial)) return;
    {
        std::lock_guard<std::mutex> lock(g_submitMutex);
        if (serial >= g_nextSerial) submit_locked("a wait for work not yet submitted");
        if (fence_done(serial)) return;
    }
    dkFenceWait(&g_fences[serial % kFenceRing], -1);
    std::lock_guard<std::mutex> lock(g_submitMutex);
    fence_done(serial);
}

void encoder_init() {
    g_ring = swdk::block_create(kRingSize, "upload ring (queue writes)");
    g_zero = swdk::block_create(kZeroSize, "zeros (new textures)");
    if (g_zero) memset(g_zero.cpu, 0, kZeroSize);
    log("encoder: WebGPU encoders recorded here and replayed into deko3d at each submit; upload ring %u MiB, %u "
        "submit fences",
        kRingSize >> 20, kFenceRing);
}

// dk_surface.cpp: the present's commands go after everything submitted (g_submitMutex held by the caller)
std::mutex& submit_mutex() { return g_submitMutex; }
void texture_zero_later(Tex* t) {
    // mp2-nx (deko-15): depth textures too (deko-14b added zero_texture's depth branch, but this filter
    // still left them out, so none were cleared: "0 depth textures cleared")
    if (!t || !t->valid || t->external || !g_zero) return;
    QueueOp q;
    q.zero = true;
    q.tex = t;
    tex_retain(t);
    std::lock_guard<std::mutex> lock(g_submitMutex);
    queue_push(q);
}

void flush_locked(const char* why) {
    if (!g_queueOps.empty()) submit_locked(why);
}

// mp2-nx (deko-14): the test cycle, at every present
void cycle_tick() {
    static bool decided = false;
    if (!decided) {
        decided = true;
        const long fixed = swdk::env_long("MP2DK_MODE", 0);
        g_cycleSeconds = uint64_t(std::max(0L, swdk::env_long("MP2DK_CYCLE", 0)));
        if (fixed >= 1 && fixed <= kModeCount) {
            g_cycleOn = true;
            g_cycleSeconds = 0;
            cycle_set(int(fixed) - 1);
            log("test cycle: holding mode %ld (MP2DK_MODE)", fixed);
        } else if (g_cycleSeconds) {
            g_cycleOn = true;
            g_cycleStart = armGetSystemTick();
            log("test cycle: %d modes, %llu s each (MP2DK_CYCLE)", int(kModeCount), (unsigned long long)g_cycleSeconds);
        }
    }
    if (!g_cycleOn) return;
    if (g_cycleSeconds) {
        // deko-16b: the cycle starts kCycleDelay seconds after the first present (time to get past the menus)
        constexpr uint64_t kCycleDelay = 30;
        const uint64_t since = armTicksToNs(armGetSystemTick() - g_cycleStart) / 1'000'000'000ull;
        if (since < kCycleDelay) {
            snprintf(g_cycleLabel, sizeof g_cycleLabel, "Test cycle starts in %d s", int(kCycleDelay - since));
            return;
        }
        const uint64_t s = since - kCycleDelay;
        cycle_set(int((s / g_cycleSeconds) % kModeCount));
        g_cycleLeft = int(g_cycleSeconds - s % g_cycleSeconds);
        // mp2-nx (deko-16): one screenshot halfway through each mode of the first loops
        static const bool kShots = swdk::env_flag("MP2DK_SHOTS", true);
        static uint64_t shotSlot = ~0ull;
        const uint64_t slot = s / g_cycleSeconds, loop = slot / kModeCount + 1;
        if (kShots && loop <= kShotLoops && slot != shotSlot && s % g_cycleSeconds >= g_cycleSeconds / 2) {
            shotSlot = slot;
            g_shotPending = true;
            snprintf(g_shotName, sizeof g_shotName, "loop%llu_test%d", (unsigned long long)loop,
                     int(slot % kModeCount) + 1);
        }
    }
    const int m = g_mode.load(std::memory_order_relaxed);
    if (g_cycleSeconds)
        snprintf(g_cycleLabel, sizeof g_cycleLabel, "Test %d of %d: %s (%d s)", m + 1, int(kModeCount), kModeNames[m],
                 g_cycleLeft);
    else
        snprintf(g_cycleLabel, sizeof g_cycleLabel, "Test %d of %d: %s", m + 1, int(kModeCount), kModeNames[m]);
}

const char* cycle_label() { return g_cycleOn ? g_cycleLabel : nullptr; }

// ---- mp2-nx (deko-16): the test cycle's screenshots
// Halfway through each mode of the first kShotLoops loops, the next present's picture is copied to memory and
// written to <app folder>/shots/loopL_testT.bmp (24-bit, about 2.7 MB), test label and all.
// deko3d_env.txt MP2DK_SHOTS=0: none.

void shot_take_locked(const DkImage& image, uint32_t w, uint32_t h) {
    if (!g_shotPending) return;
    g_shotPending = false;
    if (!w || !h) return;
    const uint32_t bytes = w * h * 4;
    // deko-16b: CPU-cached memory (deko-16's uncached reads and row-by-row SD writes froze the game)
    if (!g_shotBuf) g_shotBuf = swdk::block_create_cached(bytes, "screenshots");
    if (!g_shotBuf || g_shotBuf.size < bytes) return;
    swdk::frame_open();
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    DkImageView v;
    dkImageViewDefaults(&v, &image);
    const DkImageRect rect = {0, 0, 0, w, h, 1};
    const DkCopyBuf buf = {g_shotBuf.gpu, w * 4, w * 4 * h};
    dkCmdBufCopyImageToBuffer(R.cmd, &v, &rect, &buf, 0);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    // mp2-nx (deko-17): the small EFB copy too (the white floor's suspect)
    static swdk::Block resolvedBuf;
    Tex* rt = g_resolvedTex && g_resolvedTex->magic == dko::kAlive && g_resolvedTex->valid ? g_resolvedTex : nullptr;
    const bool rgba = rt && (rt->wformat == WGPUTextureFormat_RGBA8Unorm || rt->wformat == WGPUTextureFormat_BGRA8Unorm);
    if (rt && !rgba) log("screenshot: the small EFB copy is format 0x%X, not saved", unsigned(rt->wformat));
    const uint32_t rw = rgba ? rt->width : 0, rh = rgba ? rt->height : 0;
    if (rgba && !resolvedBuf) resolvedBuf = swdk::block_create_cached(128 * 128 * 4, "screenshots (EFB copy)");
    const bool dumpResolved = rgba && resolvedBuf && rw * rh * 4 <= resolvedBuf.size;
    if (dumpResolved) {
        const DkImageView rv = level_view(rt, 0);
        const DkImageRect rr = {0, 0, 0, rw, rh, 1};
        const DkCopyBuf rb = {resolvedBuf.gpu, rw * 4, rw * 4 * rh};
        dkCmdBufCopyImageToBuffer(R.cmd, &rv, &rr, &rb, 0);
        dkCmdBufBarrier(R.cmd, DkBarrier_Full, kInvalidateAll);
    }
    swdk::submit("a screenshot");
    dkQueueWaitIdle(R.queue);
    if (dumpResolved) {
        // colour on the left, alpha as grey on the right, 4x size; bottom row first
        armDCacheFlush(resolvedBuf.cpu, rw * rh * 4);
        const bool bgra = rt->wformat == WGPUTextureFormat_BGRA8Unorm;
        const uint32_t sc = 4, fw = rw * 2 * sc, fh = rh * sc, frow = (fw * 3 + 3) & ~3u, fdata = frow * fh;
        auto f2 = std::make_shared<std::vector<uint8_t>>(54 + size_t(fdata), 0);
        uint8_t* h2 = f2->data();
        auto p32 = [&](int at, uint32_t x) { memcpy(h2 + at, &x, 4); };
        h2[0] = 'B';
        h2[1] = 'M';
        p32(2, 54 + fdata);
        p32(10, 54);
        p32(14, 40);
        p32(18, fw);
        p32(22, fh);
        h2[26] = 1;
        h2[28] = 24;
        p32(34, fdata);
        for (uint32_t oy = 0; oy < fh; oy++) {
            const uint8_t* srow = resolvedBuf.cpu + size_t(rh - 1 - oy / sc) * rw * 4;
            uint8_t* d = h2 + 54 + size_t(oy) * frow;
            for (uint32_t x = 0; x < rw * 2 * sc; x++, d += 3) {
                const uint32_t sx = (x / sc) % rw;
                const uint8_t* px = srow + sx * 4;
                if (x / sc < rw) {
                    d[0] = bgra ? px[0] : px[2];
                    d[1] = px[1];
                    d[2] = bgra ? px[2] : px[0];
                } else {
                    d[0] = d[1] = d[2] = px[3];
                }
            }
        }
        std::string p2 = std::string(MP2NX_APP_DIR "/shots/") + g_shotName + "_efbcopy_" + std::to_string(rw) + "x" +
                         std::to_string(rh) + ".bmp";
        std::thread([f2, p2]() {
            mkdir(MP2NX_APP_DIR "/shots", 0777);
            if (FILE* f = fopen(p2.c_str(), "wb")) {
                fwrite(f2->data(), 1, f2->size(), f);
                fclose(f);
            }
        }).detach();
    }
    armDCacheFlush(g_shotBuf.cpu, bytes);  // (drops stale cache lines before the CPU reads the GPU's copy)
    // half size (every other pixel), a 24-bit BMP, bottom row first
    const uint32_t ow = w / 2, oh = h / 2, row = (ow * 3 + 3) & ~3u, data = row * oh;
    auto file = std::make_shared<std::vector<uint8_t>>(54 + size_t(data), 0);
    uint8_t* hdr = file->data();
    auto put32 = [&](int at, uint32_t x) { memcpy(hdr + at, &x, 4); };
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(2, 54 + data);
    put32(10, 54);
    put32(14, 40);
    put32(18, ow);
    put32(22, oh);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, data);
    const uint8_t* src = g_shotBuf.cpu;
    for (uint32_t oy = 0; oy < oh; oy++) {
        const uint8_t* p = src + size_t(oh - 1 - oy) * 2 * w * 4;
        uint8_t* d = hdr + 54 + size_t(oy) * row;
        for (uint32_t x = 0; x < ow; x++, p += 8, d += 3) {
            d[0] = p[2];
            d[1] = p[1];
            d[2] = p[0];
        }
    }
    // the SD card write on its own thread, in one piece
    std::string path = std::string(MP2NX_APP_DIR "/shots/") + g_shotName + ".bmp";
    std::thread([file, path]() {
        mkdir(MP2NX_APP_DIR "/shots", 0777);
        if (FILE* f = fopen(path.c_str(), "wb")) {
            fwrite(file->data(), 1, file->size(), f);
            fclose(f);
            log("screenshot: %s", path.c_str());
        } else {
            log("screenshot: could not write %s", path.c_str());
        }
    }).detach();
}
bool cycle_blank_resolved() { return g_cycleOn && g_mode.load(std::memory_order_relaxed) == kModeBlankResolved; }
bool cycle_discard_rules() { return g_cycleOn && g_mode.load(std::memory_order_relaxed) == kModeDiscardRules; }
void count_discarded_sample() { g_discardSamples.fetch_add(1, std::memory_order_relaxed); }
void capture_flush() {  // (dk_surface.cpp, at every present)
    g_fullDrawsLast = g_fullDraws;
    g_fullDraws = 0;
    cap_flush();
}

void encoder_report() {
    log("encoder: %llu submits, %llu draws (%llu skipped: shaders not ready or no pipeline), %llu passes, %llu copies, "
        "%llu uploads, %llu compute dispatches skipped",
        (unsigned long long)g_totals.submits, (unsigned long long)g_totals.draws, (unsigned long long)g_totals.skipped,
        (unsigned long long)g_totals.passes, (unsigned long long)g_totals.copies, (unsigned long long)g_totals.uploads,
        (unsigned long long)g_totals.computes);
    log("encoder: %llu depth textures cleared to 0 at creation", (unsigned long long)g_totals.depthZeroed);
    log("encoder: %llu passes loaded a target the pass before threw away (%llu of them depth), %llu draws sampled "
        "a thrown-away texture",
        (unsigned long long)g_discardLoads, (unsigned long long)g_discardDepthLoads,
        (unsigned long long)g_discardSamples.load());
}

}  // namespace mp2dk

extern "C" const char* mp2nx_dk_cycle_label(void) { return mp2dk::on() ? mp2dk::cycle_label() : nullptr; }

using namespace mp2dk;

extern "C" {

// ---- command encoders
WGPUCommandEncoder __wrap_wgpuDeviceCreateCommandEncoder(WGPUDevice device, WGPUCommandEncoderDescriptor const* d) {
    if (!on()) return __real_wgpuDeviceCreateCommandEncoder(device, d);
    return reinterpret_cast<WGPUCommandEncoder>(new Enc);
}

void __wrap_wgpuCommandEncoderRelease(WGPUCommandEncoder e) {
    if (!on()) return __real_wgpuCommandEncoderRelease(e);
    enc_release(enc_of(e));
}

WGPUCommandBuffer __wrap_wgpuCommandEncoderFinish(WGPUCommandEncoder e, WGPUCommandBufferDescriptor const* d) {
    if (!on()) return __real_wgpuCommandEncoderFinish(e, d);
    Enc* enc = enc_of(e);
    enc->finished = true;
    enc->refs.fetch_add(1, std::memory_order_relaxed);  // the command buffer's reference
    return reinterpret_cast<WGPUCommandBuffer>(enc);
}

void __wrap_wgpuCommandBufferRelease(WGPUCommandBuffer b) {
    if (!on()) return __real_wgpuCommandBufferRelease(b);
    enc_release(reinterpret_cast<Enc*>(b));
}

WGPURenderPassEncoder __wrap_wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e, WGPURenderPassDescriptor const* d) {
    if (!on()) return __real_wgpuCommandEncoderBeginRenderPass(e, d);
    Enc* enc = enc_of(e);
    PassDesc p;
    if (d->label.data) {
        const size_t n = d->label.length == WGPU_STRLEN ? strlen(d->label.data) : d->label.length;
        snprintf(p.label, sizeof p.label, "%.*s", int(std::min<size_t>(n, sizeof p.label - 1)), d->label.data);
    }
    p.colorCount = uint32_t(std::min<size_t>(d->colorAttachmentCount, 8));
    for (uint32_t i = 0; i < p.colorCount; i++) {
        const WGPURenderPassColorAttachment& a = d->colorAttachments[i];
        p.colors[i].view = hold(enc, a.view);
        p.colors[i].resolve = a.resolveTarget ? hold(enc, a.resolveTarget) : nullptr;
        p.colors[i].clear = a.loadOp == WGPULoadOp_Clear;
        p.colors[i].discard = a.storeOp == WGPUStoreOp_Discard;
        p.colors[i].clearValue = a.clearValue;
    }
    if (d->depthStencilAttachment) {
        const WGPURenderPassDepthStencilAttachment& z = *d->depthStencilAttachment;
        p.depth = hold(enc, z.view);
        p.clearDepth = z.depthLoadOp == WGPULoadOp_Clear;
        p.clearStencil = z.stencilLoadOp == WGPULoadOp_Clear;
        p.discardDepth = z.depthStoreOp == WGPUStoreOp_Discard;
        p.depthValue = z.depthClearValue;
        p.stencilValue = uint8_t(z.stencilClearValue);
    }
    Op& op = add(enc, OpType::BeginPass);
    op.u[0] = uint32_t(enc->passes.size());
    enc->passes.push_back(p);
    auto* pass = new PassEnc;
    pass->enc = enc;
    enc->refs.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<WGPURenderPassEncoder>(pass);
}

void __wrap_wgpuRenderPassEncoderEnd(WGPURenderPassEncoder p) {
    if (!on()) return __real_wgpuRenderPassEncoderEnd(p);
    add(pass_of(p)->enc, OpType::EndPass);
}

void __wrap_wgpuRenderPassEncoderRelease(WGPURenderPassEncoder p) {
    if (!on()) return __real_wgpuRenderPassEncoderRelease(p);
    PassEnc* pass = pass_of(p);
    if (pass->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        enc_release(pass->enc);
        delete pass;
    }
}

void __wrap_wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline pipeline) {
    if (!on()) return __real_wgpuRenderPassEncoderSetPipeline(p, pipeline);
    Enc* enc = pass_of(p)->enc;
    RPipe* pipe = find_render_pipeline(pipeline);
    if (pipe) {
        pipeline_retain(pipe);
        enc->pipes.push_back(pipe);
    }
    add(enc, OpType::SetPipeline).p = pipe;
}

void __wrap_wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder p, uint32_t index, WGPUBindGroup group,
                                              size_t dynCount, uint32_t const* dyn) {
    if (!on()) return __real_wgpuRenderPassEncoderSetBindGroup(p, index, group, dynCount, dyn);
    Enc* enc = pass_of(p)->enc;
    BindGroup* g = dko::find_bind_group(group);
    if (g && !dko::alive(g, "set on a pass")) g = nullptr;
    if (g) {
        dko::retain(g);
        enc->groups.push_back(g);
    }
    const uint32_t at = dynCount ? push_words(enc, dyn, dynCount * 4) : 0;
    Op& op = add(enc, OpType::SetBindGroup);
    op.p = g;
    op.u[0] = index;
    op.u[1] = at;
    op.u[2] = uint32_t(dynCount);
}

void __wrap_wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder p, uint32_t slot, WGPUBuffer buffer,
                                                 uint64_t offset, uint64_t size) {
    if (!on()) return __real_wgpuRenderPassEncoderSetVertexBuffer(p, slot, buffer, offset, size);
    Enc* enc = pass_of(p)->enc;
    Op& op = add(enc, OpType::SetVertexBuffer);
    op.p = buffer ? hold(enc, buffer) : nullptr;
    op.u[0] = slot;
    op.q[0] = offset;
    op.q[1] = size;
}

void __wrap_wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder p, WGPUBuffer buffer, WGPUIndexFormat format,
                                                uint64_t offset, uint64_t size) {
    if (!on()) return __real_wgpuRenderPassEncoderSetIndexBuffer(p, buffer, format, offset, size);
    Enc* enc = pass_of(p)->enc;
    Op& op = add(enc, OpType::SetIndexBuffer);
    op.p = hold(enc, buffer);
    op.u[0] = uint32_t(format);
    op.q[0] = offset;
    op.q[1] = size;
}

void __wrap_wgpuRenderPassEncoderDraw(WGPURenderPassEncoder p, uint32_t vertexCount, uint32_t instanceCount,
                                      uint32_t firstVertex, uint32_t firstInstance) {
    if (!on()) return __real_wgpuRenderPassEncoderDraw(p, vertexCount, instanceCount, firstVertex, firstInstance);
    Op& op = add(pass_of(p)->enc, OpType::Draw);
    op.u[0] = vertexCount;
    op.u[1] = instanceCount;
    op.u[2] = firstVertex;
    op.u[3] = firstInstance;
}

void __wrap_wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder p, uint32_t indexCount, uint32_t instanceCount,
                                             uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) {
    if (!on())
        return __real_wgpuRenderPassEncoderDrawIndexed(p, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
    Op& op = add(pass_of(p)->enc, OpType::DrawIndexed);
    op.u[0] = indexCount;
    op.u[1] = instanceCount;
    op.u[2] = firstIndex;
    op.i[3] = baseVertex;
    op.u[4] = firstInstance;
}

void __wrap_wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder p, float x, float y, float w, float h, float minDepth,
                                             float maxDepth) {
    if (!on()) return __real_wgpuRenderPassEncoderSetViewport(p, x, y, w, h, minDepth, maxDepth);
    Op& op = add(pass_of(p)->enc, OpType::Viewport);
    op.f[0] = x;
    op.f[1] = y;
    op.f[2] = w;
    op.f[3] = h;
    op.f[4] = minDepth;
    op.f[5] = maxDepth;
}

void __wrap_wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!on()) return __real_wgpuRenderPassEncoderSetScissorRect(p, x, y, w, h);
    Op& op = add(pass_of(p)->enc, OpType::Scissor);
    op.u[0] = x;
    op.u[1] = y;
    op.u[2] = w;
    op.u[3] = h;
}

void __wrap_wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder p, WGPUColor const* c) {
    if (!on()) return __real_wgpuRenderPassEncoderSetBlendConstant(p, c);
    Op& op = add(pass_of(p)->enc, OpType::BlendConstant);
    op.f[0] = float(c->r);
    op.f[1] = float(c->g);
    op.f[2] = float(c->b);
    op.f[3] = float(c->a);
}

void __wrap_wgpuRenderPassEncoderSetImmediates(WGPURenderPassEncoder p, uint32_t offset, void const* data, size_t size) {
    if (!on()) return __real_wgpuRenderPassEncoderSetImmediates(p, offset, data, size);
    Enc* enc = pass_of(p)->enc;
    const uint32_t at = push_words(enc, static_cast<const uint8_t*>(data), size);
    Op& op = add(enc, OpType::Immediates);
    op.u[0] = offset;
    op.u[1] = uint32_t(size);
    op.u[2] = at;
}

void __wrap_wgpuRenderPassEncoderPushDebugGroup(WGPURenderPassEncoder p, WGPUStringView label) {
    if (!on()) return __real_wgpuRenderPassEncoderPushDebugGroup(p, label);
}

void __wrap_wgpuRenderPassEncoderPopDebugGroup(WGPURenderPassEncoder p) {
    if (!on()) return __real_wgpuRenderPassEncoderPopDebugGroup(p);
}

// ---- compute: recorded as one op per dispatch, skipped at the replay
WGPUComputePassEncoder __wrap_wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder e, WGPUComputePassDescriptor const* d) {
    if (!on()) return __real_wgpuCommandEncoderBeginComputePass(e, d);
    auto* pass = new PassEnc;
    pass->enc = enc_of(e);
    pass->compute = true;
    pass->enc->refs.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<WGPUComputePassEncoder>(pass);
}

void __wrap_wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder p, uint32_t x, uint32_t y, uint32_t z) {
    if (!on()) return __real_wgpuComputePassEncoderDispatchWorkgroups(p, x, y, z);
    add(cpass_of(p)->enc, OpType::Compute);
}

void __wrap_wgpuComputePassEncoderEnd(WGPUComputePassEncoder p) {
    if (!on()) return __real_wgpuComputePassEncoderEnd(p);
}

void __wrap_wgpuComputePassEncoderRelease(WGPUComputePassEncoder p) {
    if (!on()) return __real_wgpuComputePassEncoderRelease(p);
    PassEnc* pass = cpass_of(p);
    if (pass->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        enc_release(pass->enc);
        delete pass;
    }
}

void __wrap_wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder p, uint32_t index, WGPUBindGroup group,
                                               size_t dynCount, uint32_t const* dyn) {
    if (!on()) return __real_wgpuComputePassEncoderSetBindGroup(p, index, group, dynCount, dyn);
}

void __wrap_wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder p, WGPUComputePipeline pipeline) {
    if (!on()) return __real_wgpuComputePassEncoderSetPipeline(p, pipeline);
}

// ---- copies
void __wrap_wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer src, uint64_t srcOffset, WGPUBuffer dst,
                                                 uint64_t dstOffset, uint64_t size) {
    if (!on()) return __real_wgpuCommandEncoderCopyBufferToBuffer(e, src, srcOffset, dst, dstOffset, size);
    Enc* enc = enc_of(e);
    CopyDesc c;
    c.buf = hold(enc, src);
    c.buf2 = hold(enc, dst);
    c.offset = srcOffset;
    c.offset2 = dstOffset;
    c.size = size == WGPU_WHOLE_SIZE && c.buf ? c.buf->size - srcOffset : size;
    add(enc, OpType::CopyB2B).u[0] = uint32_t(enc->copies.size());
    enc->copies.push_back(c);
}

void __wrap_wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder e, WGPUTexelCopyBufferInfo const* src,
                                                  WGPUTexelCopyTextureInfo const* dst, WGPUExtent3D const* size) {
    if (!on()) return __real_wgpuCommandEncoderCopyBufferToTexture(e, src, dst, size);
    Enc* enc = enc_of(e);
    CopyDesc c;
    c.buf = hold(enc, src->buffer);
    c.offset = src->layout.offset;
    c.bytesPerRow = src->layout.bytesPerRow;
    c.rowsPerImage = src->layout.rowsPerImage;
    c.tex = hold(enc, dst->texture);
    c.mip = dst->mipLevel;
    c.origin = dst->origin;
    c.extent = *size;
    add(enc, OpType::CopyB2T).u[0] = uint32_t(enc->copies.size());
    enc->copies.push_back(c);
}

void __wrap_wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder e, WGPUTexelCopyTextureInfo const* src,
                                                  WGPUTexelCopyBufferInfo const* dst, WGPUExtent3D const* size) {
    if (!on()) return __real_wgpuCommandEncoderCopyTextureToBuffer(e, src, dst, size);
    Enc* enc = enc_of(e);
    CopyDesc c;
    c.tex = hold(enc, src->texture);
    c.mip = src->mipLevel;
    c.origin = src->origin;
    c.buf = hold(enc, dst->buffer);
    c.offset = dst->layout.offset;
    c.bytesPerRow = dst->layout.bytesPerRow;
    c.rowsPerImage = dst->layout.rowsPerImage;
    c.extent = *size;
    add(enc, OpType::CopyT2B).u[0] = uint32_t(enc->copies.size());
    enc->copies.push_back(c);
}

void __wrap_wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder e, WGPUTexelCopyTextureInfo const* src,
                                                   WGPUTexelCopyTextureInfo const* dst, WGPUExtent3D const* size) {
    if (!on()) return __real_wgpuCommandEncoderCopyTextureToTexture(e, src, dst, size);
    Enc* enc = enc_of(e);
    CopyDesc c;
    c.tex = hold(enc, src->texture);
    c.mip = src->mipLevel;
    c.origin = src->origin;
    c.tex2 = hold(enc, dst->texture);
    c.mip2 = dst->mipLevel;
    c.origin2 = dst->origin;
    c.extent = *size;
    add(enc, OpType::CopyT2T).u[0] = uint32_t(enc->copies.size());
    enc->copies.push_back(c);
}

// GPU timers: none yet (the query buffers keep zeros)
void __wrap_wgpuCommandEncoderWriteTimestamp(WGPUCommandEncoder e, WGPUQuerySet q, uint32_t index) {
    if (!on()) return __real_wgpuCommandEncoderWriteTimestamp(e, q, index);
}

void __wrap_wgpuCommandEncoderResolveQuerySet(WGPUCommandEncoder e, WGPUQuerySet q, uint32_t first, uint32_t count,
                                              WGPUBuffer dst, uint64_t offset) {
    if (!on()) return __real_wgpuCommandEncoderResolveQuerySet(e, q, first, count, dst, offset);
}

// ---- the queue
void __wrap_wgpuQueueSubmit(WGPUQueue queue, size_t count, WGPUCommandBuffer const* commands) {
    if (!on()) return __real_wgpuQueueSubmit(queue, count, commands);
    std::lock_guard<std::mutex> lock(g_submitMutex);
    submit_cmds_locked(reinterpret_cast<Enc* const*>(commands), count, "a queue submit");
}

void __wrap_wgpuQueueWriteBuffer(WGPUQueue queue, WGPUBuffer buffer, uint64_t offset, void const* data, size_t size) {
    if (!on()) return __real_wgpuQueueWriteBuffer(queue, buffer, offset, data, size);
    Buf* b = find_buffer(buffer);
    if (!b || size == 0) return;
    QueueOp q;
    q.buf = b;
    q.dstOffset = offset;
    q.size = uint32_t(size);
    if (size > kRingSize / 4) {
        q.own = swdk::block_create(uint32_t(size), "a large buffer write");
        if (!q.own) return;
        memcpy(q.own.cpu, data, size);
        q.src = q.own.gpu;
    } else {
        uint8_t* cpu = nullptr;
        {
            std::unique_lock<std::mutex> lock(g_ringMutex);
            q.src = ring_alloc(lock, uint32_t(size), &cpu, &q.ringId);
        }
        if (!cpu) return;
        memcpy(cpu, data, size);
    }
    buffer_retain(b);
    std::lock_guard<std::mutex> lock(g_submitMutex);
    queue_push(q);
}

void __wrap_wgpuQueueWriteTexture(WGPUQueue queue, WGPUTexelCopyTextureInfo const* dst, void const* data, size_t dataSize,
                                  WGPUTexelCopyBufferLayout const* layout, WGPUExtent3D const* size) {
    if (!on()) return __real_wgpuQueueWriteTexture(queue, dst, data, dataSize, layout, size);
    Tex* t = dko::find_texture(dst->texture);
    if (!t || !t->valid || size->width == 0 || size->height == 0) return;
    note("queue write texture");
    // rows of blocks, repacked to 256-byte rows
    const uint32_t bw = t->blockW, bh = t->blockH;
    const uint32_t blocksW = (size->width + bw - 1) / bw, blocksH = (size->height + bh - 1) / bh;
    const uint32_t tight = blocksW * t->blockBytes;
    const uint32_t rowBytes = (tight + 255u) & ~255u;
    const uint32_t layers = std::max<uint32_t>(size->depthOrArrayLayers, 1);
    const uint32_t srcRow = layout->bytesPerRow ? layout->bytesPerRow : tight;
    const uint32_t srcRows = layout->rowsPerImage ? layout->rowsPerImage : blocksH;
    const uint64_t total = uint64_t(rowBytes) * blocksH * layers;
    QueueOp q;
    q.texture = true;
    q.tex = t;
    q.mip = dst->mipLevel;
    q.origin = dst->origin;
    q.extent = *size;
    q.rowBytes = rowBytes;
    q.rows = blocksH;
    uint8_t* cpu = nullptr;
    if (total > kRingSize / 4) {
        q.own = swdk::block_create(uint32_t(total), "a large texture write");
        if (!q.own) return;
        cpu = q.own.cpu;
        q.src = q.own.gpu;
    } else {
        std::unique_lock<std::mutex> lock(g_ringMutex);
        q.src = ring_alloc(lock, uint32_t(total), &cpu, &q.ringId);
    }
    if (!cpu) return;
    const uint8_t* src = static_cast<const uint8_t*>(data) + layout->offset;
    for (uint32_t z = 0; z < layers; z++)
        for (uint32_t y = 0; y < blocksH; y++) {
            const uint64_t from = (uint64_t(z) * srcRows + y) * srcRow;
            if (from + tight > dataSize - layout->offset) break;
            memcpy(cpu + (uint64_t(z) * blocksH + y) * rowBytes, src + from, tight);
        }
    tex_retain(t);
    std::lock_guard<std::mutex> lock(g_submitMutex);
    queue_push(q);
}

}  // extern "C"

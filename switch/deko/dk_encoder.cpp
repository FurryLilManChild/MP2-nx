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

#include <switch.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>

extern "C" {
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
    WGPUColor clearValue{};
};
struct PassDesc {
    Attachment colors[8];
    uint32_t colorCount = 0;
    View* depth = nullptr;
    bool clearDepth = false, clearStencil = false;
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
    if (!src || !dst || !src->valid || !dst->valid) return;
    uint32_t w = e.width, h = e.height;
    const uint32_t sw = level_size(src->width, srcMip), shh = level_size(src->height, srcMip);
    const uint32_t dw = level_size(dst->width, dstMip), dh = level_size(dst->height, dstMip);
    uint32_t sx = so.x, sy = so.y, dx = dO.x, dy = dO.y;
    if (sx >= sw || sy >= shh || dx >= dw || dy >= dh) return;
    w = std::min({w, sw - sx, dw - dx});
    h = std::min({h, shh - sy, dh - dy});
    if (w == 0 || h == 0) return;
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

void buffer_to_image(Replay& r, DkGpuAddr src, uint32_t rowBytes, uint32_t rows, Tex* t, uint32_t mip, WGPUOrigin3D o,
                     WGPUExtent3D e) {
    if (!t || !t->valid || mip >= t->mips) return;
    const uint32_t lw = level_size(t->width, mip), lh = level_size(t->height, mip);
    if (o.x >= lw || o.y >= lh) return;
    const uint32_t w = std::min(e.width, lw - o.x), h = std::min(e.height, lh - o.y);
    if (w == 0 || h == 0) return;
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

void begin_pass(Replay& r, const PassDesc& p) {
    work_begin(r);
    r.inPass = true;
    r.pass = &p;
    r.width = r.height = 0;
    DkImageView colors[8];
    const DkImageView* colorPtrs[8] = {};
    for (uint32_t i = 0; i < p.colorCount; i++) {
        View* v = p.colors[i].view;
        if (v && v->tex && v->tex->valid) {
            colors[i] = v->view;
            colors[i].pImage = &v->tex->image;
            colors[i].mipLevelCount = 1;
            colorPtrs[i] = &colors[i];
            if (!r.width) {
                r.width = level_size(v->tex->width, v->baseMip);
                r.height = level_size(v->tex->height, v->baseMip);
            }
        }
    }
    DkImageView depth;
    const DkImageView* depthPtr = nullptr;
    if (p.depth && p.depth->tex && p.depth->tex->valid) {
        depth = p.depth->view;
        depth.pImage = &p.depth->tex->image;
        depth.mipLevelCount = 1;
        depthPtr = &depth;
        if (!r.width) {
            r.width = level_size(p.depth->tex->width, p.depth->baseMip);
            r.height = level_size(p.depth->tex->height, p.depth->baseMip);
        }
    }
    dkCmdBufBindRenderTargets(R.cmd, colorPtrs, p.colorCount, depthPtr);
    static const DkViewportSwizzle kSwizzle = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                               DkSwizzle_PositiveW};
    dkCmdBufSetViewportSwizzles(R.cmd, 0, &kSwizzle, 1);
    const DkViewport vp = {0.f, 0.f, float(r.width), float(r.height), 0.f, 1.f};
    const DkScissor sc = {0, 0, r.width, r.height};
    dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
    dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
    for (uint32_t i = 0; i < p.colorCount; i++) {
        const Attachment& a = p.colors[i];
        if (!a.clear || !colorPtrs[i]) continue;
        const WGPUColor& c = a.clearValue;
        if (is_int_format(a.view->tex->wformat)) {
            dkCmdBufClearColorUint(R.cmd, i, DkColorMask_RGBA, uint32_t(c.r), uint32_t(c.g), uint32_t(c.b), uint32_t(c.a));
        } else {
            dkCmdBufClearColorFloat(R.cmd, i, DkColorMask_RGBA, float(c.r), float(c.g), float(c.b), float(c.a));
        }
    }
    if (depthPtr && (p.clearDepth || p.clearStencil))
        dkCmdBufClearDepthStencil(R.cmd, p.clearDepth, p.depthValue, p.clearStencil ? 0xFF : 0, p.stencilValue);
    // everything is bound again in a pass
    r.boundPipe = nullptr;
    r.vs = r.fs = nullptr;
    r.bindingsDirty = r.vbDirty = r.ibDirty = true;
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
        const BoundGroup& bg = r.groups[b.group];
        const BindGroup::Entry* e = dko::bind_group_entry(bg.g, b.binding);
        if (b.kind == swdk::SlotBinding::Kind::Sampler) {
            View* view = e ? e->view : nullptr;
            Samp* samp = nullptr;
            if (b.samplerGroup < 4 && r.groups[b.samplerGroup].g) {
                const BindGroup::Entry* se = dko::bind_group_entry(r.groups[b.samplerGroup].g, b.samplerBinding);
                if (se) samp = se->sampler;
            }
            dkCmdBufBindTexture(R.cmd, stage, b.slot, dko::texture_handle(view, samp));
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

bool prepare_draw(Replay& r) {
    if (!r.inPass || !r.pipe) return false;
    if (!pipeline_ready(r.pipe)) {
        r.skippedDraws++;
        return false;
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

void replay(Replay& r, Enc* e) {
    for (const Op& op : e->ops) {
        note(kOpNotes[int(op.type)]);
        switch (op.type) {
        case OpType::BeginPass: begin_pass(r, e->passes[op.u[0]]); break;
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
        case OpType::Draw:
            if (prepare_draw(r)) {
                dkCmdBufDraw(R.cmd, r.pipe->prim, op.u[0], op.u[1], op.u[2], op.u[3]);
                r.draws++;
            }
            break;
        case OpType::DrawIndexed:
            if (prepare_draw(r)) {
                dkCmdBufDrawIndexed(R.cmd, r.pipe->prim, op.u[0], op.u[1], op.u[2], op.i[3], op.u[4]);
                r.draws++;
            }
            break;
        case OpType::Viewport: {
            if (!(op.f[2] > 0.f) || !(op.f[3] > 0.f)) break;
            const DkViewport v = {op.f[0], op.f[1], op.f[2], op.f[3], op.f[4], op.f[5]};
            dkCmdBufSetViewports(R.cmd, 0, &v, 1);
            break;
        }
        case OpType::Scissor: {
            const uint32_t x = std::min(op.u[0], r.width), y = std::min(op.u[1], r.height);
            const DkScissor s = {x, y, std::min(op.u[2], r.width - x), std::min(op.u[3], r.height - y)};
            dkCmdBufSetScissors(R.cmd, 0, &s, 1);
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
                            c.tex, c.mip, c.origin, c.extent);
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

void replay_queue_ops(Replay& r) {
    if (g_queueOps.empty()) return;
    note("replay: queue writes");
    for (QueueOp& q : g_queueOps) {
        if (q.texture) {
            buffer_to_image(r, q.src, q.rowBytes, q.rows, q.tex, q.mip, q.origin, q.extent);
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
    log("encoder: WebGPU encoders recorded here and replayed into deko3d at each submit; upload ring %u MiB, %u "
        "submit fences",
        kRingSize >> 20, kFenceRing);
}

// dk_surface.cpp: the present's commands go after everything submitted (g_submitMutex held by the caller)
std::mutex& submit_mutex() { return g_submitMutex; }
void flush_locked(const char* why) {
    if (!g_queueOps.empty()) submit_locked(why);
}

void encoder_report() {
    log("encoder: %llu submits, %llu draws (%llu skipped: shaders not ready or no pipeline), %llu passes, %llu copies, "
        "%llu uploads, %llu compute dispatches skipped",
        (unsigned long long)g_totals.submits, (unsigned long long)g_totals.draws, (unsigned long long)g_totals.skipped,
        (unsigned long long)g_totals.passes, (unsigned long long)g_totals.copies, (unsigned long long)g_totals.uploads,
        (unsigned long long)g_totals.computes);
}

}  // namespace mp2dk

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
    p.colorCount = uint32_t(std::min<size_t>(d->colorAttachmentCount, 8));
    for (uint32_t i = 0; i < p.colorCount; i++) {
        const WGPURenderPassColorAttachment& a = d->colorAttachments[i];
        p.colors[i].view = hold(enc, a.view);
        p.colors[i].resolve = a.resolveTarget ? hold(enc, a.resolveTarget) : nullptr;
        p.colors[i].clear = a.loadOp == WGPULoadOp_Clear;
        p.colors[i].clearValue = a.clearValue;
    }
    if (d->depthStencilAttachment) {
        const WGPURenderPassDepthStencilAttachment& z = *d->depthStencilAttachment;
        p.depth = hold(enc, z.view);
        p.clearDepth = z.depthLoadOp == WGPULoadOp_Clear;
        p.clearStencil = z.stencilLoadOp == WGPULoadOp_Clear;
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

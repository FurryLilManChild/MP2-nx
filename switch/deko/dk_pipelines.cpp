// mp2-nx deko3d renderer: shader modules and render pipelines (dk_bridge.hpp). MIT.
//
// A shader module keeps its WGSL (and an XXH3 of it). A render pipeline is built when Aurora creates it
// (its compile thread for the GX pipelines, the warm-up included), from the descriptor alone:
//   - each stage: the cache's named record "W<module hash>.<entry point>" (initial_dksh_cache.bin, then
//     dksh_local.bin) gives the DKSH and the binding table; on a miss the stage is translated (Tint GLSL,
//     the Sequential scheme: uniform buffers, storage buffers and texture/sampler pairs numbered in
//     (group, binding) order), compiled by the uam worker (this thread waits, so a pipeline Aurora gets
//     back is drawable), and recorded in dksh_local.bin with its table
//   - the fixed state: primitive, depth and stencil, blend and write masks per target, vertex buffers,
//     converted as Alek's dk_pipeline.cpp does (from SwitchWaker, MIT)
// A pipeline that cannot be made (a stage Tint or uam refuses) stays invalid: its draws are skipped and
// logged, as Aurora skips a missing pipeline.
#include "dk_bridge.hpp"

#include "dksh_file.h"

#include <xxhash.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

extern "C" {
WGPUShaderModule __real_wgpuDeviceCreateShaderModule(WGPUDevice device, WGPUShaderModuleDescriptor const* d);
void __real_wgpuShaderModuleAddRef(WGPUShaderModule module);
void __real_wgpuShaderModuleRelease(WGPUShaderModule module);
WGPURenderPipeline __real_wgpuDeviceCreateRenderPipeline(WGPUDevice device, WGPURenderPipelineDescriptor const* d);
void __real_wgpuRenderPipelineAddRef(WGPURenderPipeline pipeline);
void __real_wgpuRenderPipelineRelease(WGPURenderPipeline pipeline);
}

namespace swdk {
// shader_cache.cpp (mp2-nx): the named records of both cache files, and new ones
struct NamedStage {
    ShaderEntry* entry = nullptr;
    std::vector<SlotBinding> bindings;
};
bool named_find(const std::string& name, NamedStage* out);
void named_add(const std::string& name, ShaderStage stage, ShaderEntry* entry, const std::vector<SlotBinding>& bindings);
}  // namespace swdk

namespace mp2dk {
namespace {

struct Mod {
    std::atomic<int32_t> refs{1};
    const void* handle = nullptr;
    std::string wgsl;
    uint64_t hash = 0;
};

std::shared_mutex g_mutex;
std::unordered_map<const void*, Mod*> g_modules;
std::unordered_map<const void*, RPipe*> g_pipelines;
std::mutex g_translateMutex;  // Tint: one translation at a time
struct Stats {
    uint64_t pipelines = 0, invalid = 0, stageHits = 0, stageMisses = 0, translateNs = 0, waitNs = 0;
} g_stats;

template <class T> T* find(std::unordered_map<const void*, T*>& map, const void* h) {
    if (!h) return nullptr;
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    auto it = map.find(h);
    return it == map.end() ? nullptr : it->second;
}

void release_module(Mod* m, const void* handle) {
    if (m->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    {
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_modules.find(handle);
        if (it != g_modules.end() && it->second == m) g_modules.erase(it);
    }
    delete m;
}

std::string str(WGPUStringView s) {
    if (!s.data) return {};
    return s.length == WGPU_STRLEN ? std::string(s.data) : std::string(s.data, s.length);
}

// the entry point of a stage: the descriptor's, or the module's only one of that stage
std::string entry_point(const Mod* m, WGPUStringView e, const char* attr) {
    std::string name = str(e);
    if (!name.empty()) return name;
    const size_t at = m->wgsl.find(attr);
    if (at == std::string::npos) return {};
    const size_t fn = m->wgsl.find("fn ", at);
    if (fn == std::string::npos) return {};
    size_t b = fn + 3, end = b;
    while (end < m->wgsl.size() && (isalnum(uint8_t(m->wgsl[end])) || m->wgsl[end] == '_')) end++;
    return m->wgsl.substr(b, end - b);
}

const char* stage_tag(swdk::ShaderStage s) { return s == swdk::ShaderStage::Vertex ? "vs" : "fs"; }

bool make_stage(const Mod* m, const std::string& entry, swdk::ShaderStage stage, const std::string& label, Stage* out) {
    char key[96];
    snprintf(key, sizeof key, "W%016llx.%s", (unsigned long long)m->hash, entry.c_str());
    swdk::NamedStage named;
    if (swdk::named_find(key, &named) && named.entry &&
        named.entry->state.load(std::memory_order_acquire) != swdk::ShaderState::Missing &&
        named.entry->state.load(std::memory_order_acquire) != swdk::ShaderState::Failed) {
        out->entry = named.entry;
        out->bindings = std::move(named.bindings);
        std::lock_guard<std::mutex> lock(g_translateMutex);
        g_stats.stageHits++;
        return true;
    }
    const uint64_t t0 = swdk::now_ns();
    swdk::TranslateResult tr;
    {
        std::lock_guard<std::mutex> lock(g_translateMutex);
        tr = swdk::wgsl_to_glsl(m->wgsl, entry, stage, swdk::BindingScheme::Sequential);
    }
    const uint64_t t1 = swdk::now_ns();
    {
        // mp2-nx (deko-5): the WGSL of every stage translated here, kept with the GLSL (shaders.cpp) to run
        // Tint and uam on a PC with a memory checker
        char path[256];
        snprintf(path, sizeof path, "sdmc:/switch/" MP2NX_APP_NAME "/cache/glsl/W%016llx.wgsl", (unsigned long long)m->hash);
        if (FILE* f = fopen(path, "rb")) {
            fclose(f);
        } else if (FILE* w = fopen(path, "wb")) {
            fwrite(m->wgsl.data(), 1, m->wgsl.size(), w);
            fclose(w);
        }
    }
    if (!tr.ok) {
        log("pipelines: %s (%s %s) does not translate: %.300s", label.c_str(), stage_tag(stage), entry.c_str(),
            tr.error.c_str());
        return false;
    }
    swdk::ShaderEntry* e = swdk::shader_entry(stage, swdk::glsl_hash(tr.glsl));
    swdk::shader_compile(e, tr.glsl, true);
    const bool ok = swdk::shader_wait(e);
    const uint64_t t2 = swdk::now_ns();
    {
        std::lock_guard<std::mutex> lock(g_translateMutex);
        g_stats.stageMisses++;
        g_stats.translateNs += t1 - t0;
        g_stats.waitNs += t2 - t1;
        if (g_stats.stageMisses <= 48 || !ok)
            log("pipelines: %s %s %s compiled here: Tint %.0f ms, uam %.0f ms%s", label.c_str(), stage_tag(stage),
                entry.c_str(), double(t1 - t0) / 1e6, double(t2 - t1) / 1e6, ok ? "" : " (uam FAILED)");
    }
    if (!ok) return false;
    swdk::named_add(key, stage, e, tr.bindings);
    out->entry = e;
    out->bindings = std::move(tr.bindings);
    return true;
}

DkCompareOp compare_op(WGPUCompareFunction f) {
    switch (f) {
    case WGPUCompareFunction_Never: return DkCompareOp_Never;
    case WGPUCompareFunction_Less: return DkCompareOp_Less;
    case WGPUCompareFunction_Equal: return DkCompareOp_Equal;
    case WGPUCompareFunction_LessEqual: return DkCompareOp_Lequal;
    case WGPUCompareFunction_Greater: return DkCompareOp_Greater;
    case WGPUCompareFunction_NotEqual: return DkCompareOp_NotEqual;
    case WGPUCompareFunction_GreaterEqual: return DkCompareOp_Gequal;
    default: return DkCompareOp_Always;
    }
}

// mp2-nx (deko-14): pipelines built, blended, reading the destination, dual-source, constant
std::atomic<uint32_t> g_blendCensus[5];

DkBlendFactor blend_factor(WGPUBlendFactor f, bool alpha) {
    switch (f) {
    case WGPUBlendFactor_Zero: return DkBlendFactor_Zero;
    case WGPUBlendFactor_One: return DkBlendFactor_One;
    case WGPUBlendFactor_Src: return DkBlendFactor_SrcColor;
    case WGPUBlendFactor_OneMinusSrc: return DkBlendFactor_InvSrcColor;
    case WGPUBlendFactor_SrcAlpha: return DkBlendFactor_SrcAlpha;
    case WGPUBlendFactor_OneMinusSrcAlpha: return DkBlendFactor_InvSrcAlpha;
    case WGPUBlendFactor_Dst: return DkBlendFactor_DstColor;
    case WGPUBlendFactor_OneMinusDst: return DkBlendFactor_InvDstColor;
    case WGPUBlendFactor_DstAlpha: return DkBlendFactor_DstAlpha;
    case WGPUBlendFactor_OneMinusDstAlpha: return DkBlendFactor_InvDstAlpha;
    case WGPUBlendFactor_SrcAlphaSaturated: return DkBlendFactor_SrcAlphaSaturate;
    case WGPUBlendFactor_Constant: return alpha ? DkBlendFactor_ConstAlpha : DkBlendFactor_ConstColor;
    case WGPUBlendFactor_OneMinusConstant: return alpha ? DkBlendFactor_InvConstAlpha : DkBlendFactor_InvConstColor;
    case WGPUBlendFactor_Src1: return DkBlendFactor_Src1Color;
    case WGPUBlendFactor_OneMinusSrc1: return DkBlendFactor_InvSrc1Color;
    case WGPUBlendFactor_Src1Alpha: return DkBlendFactor_Src1Alpha;
    case WGPUBlendFactor_OneMinusSrc1Alpha: return DkBlendFactor_InvSrc1Alpha;
    default: return DkBlendFactor_One;
    }
}

DkBlendOp blend_op(WGPUBlendOperation o) {
    switch (o) {
    case WGPUBlendOperation_Subtract: return DkBlendOp_Sub;
    case WGPUBlendOperation_ReverseSubtract: return DkBlendOp_RevSub;
    case WGPUBlendOperation_Min: return DkBlendOp_Min;
    case WGPUBlendOperation_Max: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
    }
}

bool vertex_format(WGPUVertexFormat f, DkVtxAttribSize* size, DkVtxAttribType* type) {
    using S = DkVtxAttribSize;
    auto set = [&](S s, DkVtxAttribType t) {
        *size = s;
        *type = t;
        return true;
    };
    switch (f) {
    case WGPUVertexFormat_Uint8x2: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Uint8x4: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Sint8x2: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Sint8x4: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Unorm8x2: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Unorm);
    case WGPUVertexFormat_Unorm8x4: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
    case WGPUVertexFormat_Snorm8x2: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Snorm);
    case WGPUVertexFormat_Snorm8x4: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Snorm);
    case WGPUVertexFormat_Uint16x2: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Uint16x4: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Sint16x2: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Sint16x4: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Unorm16x2: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Unorm);
    case WGPUVertexFormat_Unorm16x4: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Unorm);
    case WGPUVertexFormat_Snorm16x2: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Snorm);
    case WGPUVertexFormat_Snorm16x4: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Snorm);
    case WGPUVertexFormat_Float16x2: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Float);
    case WGPUVertexFormat_Float16x4: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Float);
    case WGPUVertexFormat_Float32: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Float);
    case WGPUVertexFormat_Float32x2: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Float);
    case WGPUVertexFormat_Float32x3: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Float);
    case WGPUVertexFormat_Float32x4: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Float);
    case WGPUVertexFormat_Uint32: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Uint32x2: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Uint32x3: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Uint32x4: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Uint);
    case WGPUVertexFormat_Sint32: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Sint32x2: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Sint32x3: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Sint32x4: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Sint);
    case WGPUVertexFormat_Unorm10_10_10_2: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Unorm);
    default: return false;
    }
}

DkPrimitive primitive(WGPUPrimitiveTopology t) {
    switch (t) {
    case WGPUPrimitiveTopology_PointList: return DkPrimitive_Points;
    case WGPUPrimitiveTopology_LineList: return DkPrimitive_Lines;
    case WGPUPrimitiveTopology_LineStrip: return DkPrimitive_LineStrip;
    case WGPUPrimitiveTopology_TriangleStrip: return DkPrimitive_TriangleStrip;
    default: return DkPrimitive_Triangles;
    }
}

void build(RPipe* p, const WGPURenderPipelineDescriptor& d) {
    p->label = str(d.label);
    if (p->label.empty()) p->label = "(unnamed pipeline)";
    Mod* vm = find(g_modules, d.vertex.module);
    if (!vm) {
        log("pipelines: %s: its vertex module has no shadow", p->label.c_str());
        return;
    }
    if (!make_stage(vm, entry_point(vm, d.vertex.entryPoint, "@vertex"), swdk::ShaderStage::Vertex, p->label, &p->vs))
        return;
    if (d.fragment) {
        Mod* fm = find(g_modules, d.fragment->module);
        if (!fm) {
            log("pipelines: %s: its fragment module has no shadow", p->label.c_str());
            return;
        }
        if (!make_stage(fm, entry_point(fm, d.fragment->entryPoint, "@fragment"), swdk::ShaderStage::Fragment,
                        p->label, &p->fs))
            return;
        p->hasFs = true;
    }
    // zeroed first: the encoder compares states byte for byte
    memset(&p->rasterizer, 0, sizeof p->rasterizer);
    memset(&p->color, 0, sizeof p->color);
    memset(&p->colorWrite, 0, sizeof p->colorWrite);
    memset(p->blend, 0, sizeof p->blend);
    memset(&p->depthStencil, 0, sizeof p->depthStencil);
    dkRasterizerStateDefaults(&p->rasterizer);
    dkColorStateDefaults(&p->color);
    dkColorWriteStateDefaults(&p->colorWrite);
    for (auto& b : p->blend) dkBlendStateDefaults(&b);
    dkDepthStencilStateDefaults(&p->depthStencil);

    const WGPUPrimitiveState& pr = d.primitive;
    p->prim = primitive(pr.topology);
    p->restart = pr.stripIndexFormat != WGPUIndexFormat_Undefined;
    DkRasterizerState& rs = p->rasterizer;
    rs.cullMode = pr.cullMode == WGPUCullMode_Front ? DkFace_Front
                  : pr.cullMode == WGPUCullMode_Back ? DkFace_Back
                                                     : DkFace_None;
    const bool ccw = pr.frontFace != WGPUFrontFace_CW;  // WebGPU's default is CCW
    rs.frontFace = ccw != swdk::C.flipFront ? DkFrontFace_CCW : DkFrontFace_CW;
    rs.depthClampEnable = pr.unclippedDepth;

    DkDepthStencilState& ds = p->depthStencil;
    if (d.depthStencil) {
        const WGPUDepthStencilState& z = *d.depthStencil;
        const bool test = z.depthCompare != WGPUCompareFunction_Always && z.depthCompare != WGPUCompareFunction_Undefined;
        const bool write = z.depthWriteEnabled == WGPUOptionalBool_True;
        ds.depthTestEnable = test || write;  // (deko3d writes depth only with the test on)
        ds.depthWriteEnable = write;
        ds.depthCompareOp = test ? compare_op(z.depthCompare) : DkCompareOp_Always;
        if (z.depthBias != 0 || z.depthBiasSlopeScale != 0.0f) {
            rs.depthBiasEnableMask = DkPolygonFlag_All;
            p->depthBias = true;
            p->bias[0] = float(z.depthBias);
            p->bias[1] = z.depthBiasClamp;
            p->bias[2] = z.depthBiasSlopeScale;
        }
        if (z.stencilFront.compare != WGPUCompareFunction_Always && z.stencilFront.compare != WGPUCompareFunction_Undefined) {
            static int logged = 0;
            if (logged++ < 4) log("pipelines: %s uses a stencil test, which is not drawn yet", p->label.c_str());
        }
    } else {
        ds.depthTestEnable = false;
        ds.depthWriteEnable = false;
    }

    if (d.fragment) {
        p->colorCount = uint32_t(std::min<size_t>(d.fragment->targetCount, 8));
        for (uint32_t i = 0; i < p->colorCount; i++) {
            const WGPUColorTargetState& t = d.fragment->targets[i];
            dkColorWriteStateSetMask(&p->colorWrite, i, uint32_t(t.writeMask) & 0xF);
            if (t.blend) {
                dkColorStateSetBlendEnable(&p->color, i, true);
                dkBlendStateSetOps(&p->blend[i], blend_op(t.blend->color.operation), blend_op(t.blend->alpha.operation));
                dkBlendStateSetFactors(&p->blend[i], blend_factor(t.blend->color.srcFactor, false),
                                       blend_factor(t.blend->color.dstFactor, false),
                                       blend_factor(t.blend->alpha.srcFactor, true),
                                       blend_factor(t.blend->alpha.dstFactor, true));
                // mp2-nx (deko-14 test cycle): what this blending reads
                const WGPUBlendFactor fs[4] = {t.blend->color.srcFactor, t.blend->color.dstFactor,
                                               t.blend->alpha.srcFactor, t.blend->alpha.dstFactor};
                const bool trivial = t.blend->color.srcFactor == WGPUBlendFactor_One &&
                                     t.blend->color.dstFactor == WGPUBlendFactor_Zero &&
                                     t.blend->alpha.srcFactor == WGPUBlendFactor_One &&
                                     t.blend->alpha.dstFactor == WGPUBlendFactor_Zero &&
                                     t.blend->color.operation == WGPUBlendOperation_Add &&
                                     t.blend->alpha.operation == WGPUBlendOperation_Add;
                if (!trivial) p->blendKinds |= kBlendAny;
                if (!trivial && t.blend->color.dstFactor == WGPUBlendFactor_One) p->blendKinds |= kBlendAdd;
                if (t.blend->color.srcFactor == WGPUBlendFactor_SrcAlpha &&
                    t.blend->color.dstFactor == WGPUBlendFactor_OneMinusSrcAlpha)
                    p->blendKinds |= kBlendAlpha;
                for (WGPUBlendFactor f : fs) {
                    switch (f) {
                    case WGPUBlendFactor_Dst: case WGPUBlendFactor_OneMinusDst: case WGPUBlendFactor_DstAlpha:
                    case WGPUBlendFactor_OneMinusDstAlpha: case WGPUBlendFactor_SrcAlphaSaturated:
                        p->blendKinds |= kBlendDst; break;
                    case WGPUBlendFactor_Src1: case WGPUBlendFactor_OneMinusSrc1: case WGPUBlendFactor_Src1Alpha:
                    case WGPUBlendFactor_OneMinusSrc1Alpha:
                        p->blendKinds |= kBlendSrc1; break;
                    case WGPUBlendFactor_Constant: case WGPUBlendFactor_OneMinusConstant:
                        p->blendKinds |= kBlendConst; break;
                    default: break;
                    }
                }
            }
        }
    }
    for (uint32_t i = p->colorCount; i < 8; i++) dkColorWriteStateSetMask(&p->colorWrite, i, 0);
    // mp2-nx (deko-14): the blending census (pipelines_report)
    g_blendCensus[0].fetch_add(1, std::memory_order_relaxed);
    {
        static std::atomic<uint32_t> nextId{0};
        p->id = nextId.fetch_add(1, std::memory_order_relaxed);
    }
    if (p->blendKinds & kBlendAny) g_blendCensus[1].fetch_add(1, std::memory_order_relaxed);
    if (p->blendKinds & kBlendDst) g_blendCensus[2].fetch_add(1, std::memory_order_relaxed);
    if (p->blendKinds & kBlendSrc1) g_blendCensus[3].fetch_add(1, std::memory_order_relaxed);
    if (p->blendKinds & kBlendConst) g_blendCensus[4].fetch_add(1, std::memory_order_relaxed);

    // vertex buffers: attribute i is shader location i; unused locations read a fixed zero
    p->vbufCount = uint32_t(std::min<size_t>(d.vertex.bufferCount, 8));
    uint32_t maxLocation = 0;
    for (uint32_t b = 0; b < p->vbufCount; b++) {
        const WGPUVertexBufferLayout& l = d.vertex.buffers[b];
        p->vbufs[b].stride = uint32_t(l.arrayStride);
        p->vbufs[b].divisor = l.stepMode == WGPUVertexStepMode_Instance ? 1 : 0;
        for (size_t a = 0; a < l.attributeCount; a++) {
            const WGPUVertexAttribute& at = l.attributes[a];
            if (at.shaderLocation >= 16) continue;
            DkVtxAttribSize size;
            DkVtxAttribType type;
            if (!vertex_format(at.format, &size, &type)) {
                log("pipelines: %s: vertex format 0x%X is not supported", p->label.c_str(), unsigned(at.format));
                return;
            }
            DkVtxAttribState& s = p->attribs[at.shaderLocation];
            s.bufferId = b;
            s.isFixed = 0;
            s.offset = uint32_t(at.offset);
            s.size = size;
            s.type = type;
            maxLocation = std::max(maxLocation, at.shaderLocation + 1);
        }
    }
    for (uint32_t i = 0; i < maxLocation; i++) {
        bool used = false;
        for (uint32_t b = 0; b < p->vbufCount && !used; b++)
            for (size_t a = 0; a < d.vertex.buffers[b].attributeCount; a++)
                if (d.vertex.buffers[b].attributes[a].shaderLocation == i) used = true;
        if (!used) {
            p->attribs[i] = DkVtxAttribState{};
            p->attribs[i].isFixed = 1;
            p->attribs[i].size = DkVtxAttribSize_1x32;
            p->attribs[i].type = DkVtxAttribType_Float;
        }
    }
    p->attribCount = maxLocation;
    if (d.multisample.count > 1) {
        static int logged = 0;
        if (logged++ < 2) log("pipelines: %s asks for %u samples: drawn single-sampled", p->label.c_str(), d.multisample.count);
    }
    p->valid = true;
}

}  // namespace

RPipe* find_render_pipeline(WGPURenderPipeline p) { return find(g_pipelines, p); }
void pipeline_retain(RPipe* p) { p->refs.fetch_add(1, std::memory_order_relaxed); }
void pipeline_release(RPipe* p) {
    if (p->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    {
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_pipelines.find(p->handle);
        if (it != g_pipelines.end() && it->second == p) g_pipelines.erase(it);
    }
    delete p;  // (its shaders stay loaded: the registry keeps every stage of the session)
}

bool pipeline_ready(RPipe* p) {
    if (!p || !p->valid) return false;
    const bool vs = swdk::shader_usable(p->vs.entry);
    const bool fs = !p->hasFs || swdk::shader_usable(p->fs.entry);
    return vs && fs;
}

void pipelines_report() {
    std::lock_guard<std::mutex> lock(g_translateMutex);
    log("pipelines: %llu built (%llu could not be), stages from the caches %llu, compiled here %llu (Tint %.1f s, "
        "uam wait %.1f s)",
        (unsigned long long)g_stats.pipelines, (unsigned long long)g_stats.invalid, (unsigned long long)g_stats.stageHits,
        (unsigned long long)g_stats.stageMisses, double(g_stats.translateNs) / 1e9, double(g_stats.waitNs) / 1e9);
    log("pipelines: blending census: %u built, %u blended, %u read the destination, %u dual-source, %u constant",
        g_blendCensus[0].load(), g_blendCensus[1].load(), g_blendCensus[2].load(), g_blendCensus[3].load(),
        g_blendCensus[4].load());
}

// mp2-nx (the white-texture fix): a pipeline descriptor's contents (shader sources by hash, not by handle),
// to tell Dawn's cache hit from a new pipeline at a freed one's address (see dk_objects.cpp CreateView)
uint64_t pipeline_fingerprint(const WGPURenderPipelineDescriptor& d) {
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    auto put = [&](const void* p, size_t n) { XXH3_64bits_update(st, p, n); };
    auto put_u = [&](uint64_t v) { put(&v, sizeof v); };
    auto put_s = [&](WGPUStringView s) {
        const std::string t = str(s);
        put_u(t.size());
        put(t.data(), t.size());
    };
    auto put_mod = [&](WGPUShaderModule m) {
        const Mod* mod = find(g_modules, m);
        put_u(mod ? mod->hash : uint64_t(reinterpret_cast<uintptr_t>(m)));
    };
    put_u(uint64_t(reinterpret_cast<uintptr_t>(d.layout)));
    put_mod(d.vertex.module);
    put_s(d.vertex.entryPoint);
    put_u(d.vertex.bufferCount);
    for (size_t i = 0; i < d.vertex.bufferCount; i++) {
        const WGPUVertexBufferLayout& b = d.vertex.buffers[i];
        put_u(b.stepMode);
        put_u(b.arrayStride);
        put_u(b.attributeCount);
        for (size_t a = 0; a < b.attributeCount; a++) {
            put_u(b.attributes[a].format);
            put_u(b.attributes[a].offset);
            put_u(b.attributes[a].shaderLocation);
        }
    }
    put_u(d.primitive.topology);
    put_u(d.primitive.stripIndexFormat);
    put_u(d.primitive.frontFace);
    put_u(d.primitive.cullMode);
    put_u(d.primitive.unclippedDepth);
    if (const WGPUDepthStencilState* z = d.depthStencil) {
        put_u(1);
        put_u(z->format);
        put_u(z->depthWriteEnabled);
        put_u(z->depthCompare);
        for (const WGPUStencilFaceState* f : {&z->stencilFront, &z->stencilBack}) {
            put_u(f->compare);
            put_u(f->failOp);
            put_u(f->depthFailOp);
            put_u(f->passOp);
        }
        put_u(z->stencilReadMask);
        put_u(z->stencilWriteMask);
        put_u(uint64_t(uint32_t(z->depthBias)));
        put(&z->depthBiasSlopeScale, sizeof z->depthBiasSlopeScale);
        put(&z->depthBiasClamp, sizeof z->depthBiasClamp);
    } else {
        put_u(0);
    }
    put_u(d.multisample.count);
    put_u(d.multisample.mask);
    put_u(d.multisample.alphaToCoverageEnabled);
    if (const WGPUFragmentState* f = d.fragment) {
        put_mod(f->module);
        put_s(f->entryPoint);
        put_u(f->targetCount);
        for (size_t i = 0; i < f->targetCount; i++) {
            const WGPUColorTargetState& t = f->targets[i];
            put_u(t.format);
            put_u(t.writeMask);
            if (t.blend) {
                const WGPUBlendComponent* cs[2] = {&t.blend->color, &t.blend->alpha};
                for (const WGPUBlendComponent* c : cs) {
                    put_u(c->operation);
                    put_u(c->srcFactor);
                    put_u(c->dstFactor);
                }
            } else {
                put_u(0xFFFF);
            }
        }
    } else {
        put_u(0);
    }
    const uint64_t h = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    return h;
}

}  // namespace mp2dk

using namespace mp2dk;

extern "C" {

WGPUShaderModule __wrap_wgpuDeviceCreateShaderModule(WGPUDevice device, WGPUShaderModuleDescriptor const* d) {
    WGPUShaderModule m = __real_wgpuDeviceCreateShaderModule(device, d);
    if (!m || !d || !on() || !swdk::R.device) return m;
    // (Dawn returns a cached module for the same source with one more reference: its shadow takes it)
    // mp2-nx (the white-texture fix): ...but only when the source is the same; a new module can come back at
    // a freed one's address while its shadow still lives (see dk_objects.cpp CreateView)
    Mod* existing = find(g_modules, m);
    for (const WGPUChainedStruct* c = d->nextInChain; c; c = c->next) {
        if (c->sType != WGPUSType_ShaderSourceWGSL) continue;
        auto* mod = new Mod;
        mod->wgsl = str(reinterpret_cast<const WGPUShaderSourceWGSL*>(c)->code);
        mod->hash = XXH3_64bits(mod->wgsl.data(), mod->wgsl.size());
        if (existing && existing->hash == mod->hash) {
            existing->refs.fetch_add(1, std::memory_order_relaxed);
            delete mod;
            return m;
        }
        if (existing) log("pipelines: a new shader module came back at the address of a live one: it replaces it");
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        g_modules[m] = mod;
        return m;
    }
    if (existing) existing->refs.fetch_add(1, std::memory_order_relaxed);  // (no WGSL: as before)
    return m;
}

void __wrap_wgpuShaderModuleAddRef(WGPUShaderModule module) {
    if (on())
        if (Mod* m = find(g_modules, module)) m->refs.fetch_add(1, std::memory_order_relaxed);
    __real_wgpuShaderModuleAddRef(module);
}

void __wrap_wgpuShaderModuleRelease(WGPUShaderModule module) {
    if (on())
        if (Mod* m = find(g_modules, module)) release_module(m, module);
    __real_wgpuShaderModuleRelease(module);
}

WGPURenderPipeline __wrap_wgpuDeviceCreateRenderPipeline(WGPUDevice device, WGPURenderPipelineDescriptor const* d) {
    WGPURenderPipeline p = __real_wgpuDeviceCreateRenderPipeline(device, d);
    if (!p || !d || !on() || !swdk::R.device) return p;
    // (Dawn returns a cached pipeline for an equal descriptor with one more reference: its shadow takes it)
    // mp2-nx (the white-texture fix): ...but only when it was built from the same descriptor
    const uint64_t fingerprint = pipeline_fingerprint(*d);
    if (RPipe* existing = find(g_pipelines, p)) {
        if (existing->fingerprint == fingerprint) {
            pipeline_retain(existing);
            return p;
        }
        log("pipelines: a new pipeline came back at the address of a live one built from another descriptor: it "
            "replaces it");
    }
    auto* pipe = new RPipe;
    pipe->handle = p;
    pipe->fingerprint = fingerprint;
    note("pipeline build (compile thread)");
    build(pipe, *d);
    {
        std::lock_guard<std::mutex> lock(g_translateMutex);
        g_stats.pipelines++;
        if (!pipe->valid) g_stats.invalid++;
        if (g_stats.pipelines % 250 == 0) {
            // (a running total in the log during the warm-up)
            log("pipelines: %llu built, stages from the caches %llu, compiled here %llu",
                (unsigned long long)g_stats.pipelines, (unsigned long long)g_stats.stageHits,
                (unsigned long long)g_stats.stageMisses);
        }
    }
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_pipelines[p] = pipe;
    return p;
}

void __wrap_wgpuRenderPipelineAddRef(WGPURenderPipeline pipeline) {
    if (on())
        if (RPipe* p = find(g_pipelines, pipeline)) pipeline_retain(p);
    __real_wgpuRenderPipelineAddRef(pipeline);
}

void __wrap_wgpuRenderPipelineRelease(WGPURenderPipeline pipeline) {
    if (on())
        if (RPipe* p = find(g_pipelines, pipeline)) pipeline_release(p);
    __real_wgpuRenderPipelineRelease(pipeline);
}

}  // extern "C"

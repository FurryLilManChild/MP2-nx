// Adapted from SwitchWaker (MIT), 084291f: switch/deko/aurora/dk_objects.cpp. Changes for Dusklight
// (aleks2 deko3d P1): with the Dawn backend (no deko3d device) every wrapper only forwards, without a
// lookup; bind groups also keep their buffer entries (RmlUi's uniforms, dk_rmlui.cpp).
//
// The shadows of Aurora's WebGPU textures, views, samplers and bind groups (dk_objects.hpp;
// docs/DEKO3D_MIGRATION_PLAN.md phase 3). The __wrap_ functions below take the place of Dawn's for
// every caller in the NRO (switch/native/CMakeLists.txt: -Wl,--wrap=wgpuDeviceCreateTexture ...):
// each calls Dawn's (__real_) and keeps the shadow in step.
#include "dk_objects.hpp"

#include "dk_bridge.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

extern "C" {
WGPUTexture __real_wgpuDeviceCreateTexture(WGPUDevice device, WGPUTextureDescriptor const* descriptor);
void __real_wgpuTextureAddRef(WGPUTexture texture);
void __real_wgpuTextureRelease(WGPUTexture texture);
WGPUTextureView __real_wgpuTextureCreateView(WGPUTexture texture, WGPUTextureViewDescriptor const* descriptor);
void __real_wgpuTextureViewAddRef(WGPUTextureView view);
void __real_wgpuTextureViewRelease(WGPUTextureView view);
WGPUSampler __real_wgpuDeviceCreateSampler(WGPUDevice device, WGPUSamplerDescriptor const* descriptor);
void __real_wgpuSamplerAddRef(WGPUSampler sampler);
void __real_wgpuSamplerRelease(WGPUSampler sampler);
WGPUBindGroup __real_wgpuDeviceCreateBindGroup(WGPUDevice device, WGPUBindGroupDescriptor const* descriptor);
void __real_wgpuBindGroupAddRef(WGPUBindGroup group);
void __real_wgpuBindGroupRelease(WGPUBindGroup group);
WGPUBindGroupLayout __real_wgpuDeviceCreateBindGroupLayout(WGPUDevice device, WGPUBindGroupLayoutDescriptor const* d);
void __real_wgpuBindGroupLayoutAddRef(WGPUBindGroupLayout layout);
void __real_wgpuBindGroupLayoutRelease(WGPUBindGroupLayout layout);
void __real_wgpuTextureDestroy(WGPUTexture texture);
}

namespace aurora::gfx::dk {
using swdk::R;
using swdk::dklog;

namespace {

std::shared_mutex g_mutex;
std::unordered_map<const void*, Tex*> g_textures;
std::unordered_map<const void*, View*> g_views;
// mp2-nx: new objects that came back at a live shadow's address (logged with the object counts)
std::atomic<uint64_t> g_reusedViews{0}, g_reusedSamplers{0};
std::unordered_map<const void*, Samp*> g_samplers;
std::unordered_map<const void*, BindGroup*> g_bindGroups;
// mp2-nx: bind group layouts' dynamic-offset bindings, in binding order (the order of the offsets
// SetBindGroup gets), with the layout's reference count
struct Layout {
    std::atomic<int32_t> refs{1};
    std::vector<uint32_t> dynamic;
};
std::unordered_map<const void*, Layout*> g_layouts;
ObjectStats g_stats;  // under g_mutex

// descriptor slots of released views, retired by the render worker
std::mutex g_slotMutex;
std::vector<uint32_t> g_freedSlots;

bool g_compression = false;  // COS_DK_COMPRESSION=1: hardware compression on colour render targets

template <class T> T* find(std::unordered_map<const void*, T*>& map, const void* h) {
    if (!h) return nullptr;
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    auto it = map.find(h);
    return it == map.end() ? nullptr : it->second;
}

const char* label_of(WGPUStringView s) {
    return s.data ? s.data : "";
}

DkImageSwizzle swizzle_of(WGPUComponentSwizzle s, DkImageSwizzle identity) {
    switch (s) {
    case WGPUComponentSwizzle_Zero: return DkImageSwizzle_Zero;
    case WGPUComponentSwizzle_One: return DkImageSwizzle_One;
    case WGPUComponentSwizzle_R: return DkImageSwizzle_Red;
    case WGPUComponentSwizzle_G: return DkImageSwizzle_Green;
    case WGPUComponentSwizzle_B: return DkImageSwizzle_Blue;
    case WGPUComponentSwizzle_A: return DkImageSwizzle_Alpha;
    default: return identity;
    }
}

DkWrapMode wrap_of(WGPUAddressMode m) {
    switch (m) {
    case WGPUAddressMode_Repeat: return DkWrapMode_Repeat;
    case WGPUAddressMode_MirrorRepeat: return DkWrapMode_MirroredRepeat;
    default: return DkWrapMode_ClampToEdge;  // ClampToEdge, Undefined (WebGPU's default)
    }
}

uint64_t fnv_bytes(const void* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

// ---- destruction (the last reference): under no lock; the maps were updated by the caller. The object
// is marked dead and kept kGraveFrames frames before it is freed (objects_frame_start).
std::mutex g_graveMutex;
struct Grave {
    uint64_t frame;
    int kind;  // 0 Tex, 1 View, 2 Samp, 3 BindGroup
    void* p;
};
std::vector<Grave> g_graves;
void bury(int kind, void* p) {
    std::lock_guard<std::mutex> lock(g_graveMutex);
    g_graves.push_back({R.frame, kind, p});
}

void destroy(Tex* t) {
    if (t->valid && !t->external) swdk::image_free_deferred(t->mem);
    t->magic = kDead;
    bury(0, t);
}
void release_tex(Tex* t);

// ---- mp2-nx (the scan-visor build): the history of each view handle (the last 6 events at its address)
struct HandleEvent {
    uint64_t frame = 0;
    char what[120] = {};
};
struct HandleHistory {
    HandleEvent ev[6];
    uint32_t next = 0;
};
std::mutex g_histMutex;
std::unordered_map<const void*, HandleHistory> g_viewHistory;
std::atomic<uint32_t> g_nextViewId{1};
std::atomic<uint64_t> g_unknownReleases{0}, g_apiNegative{0}, g_heldMismatch{0}, g_missingViews{0};

void view_event(const void* handle, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void view_event(const void* handle, const char* fmt, ...) {
    HandleEvent e;
    e.frame = R.frame;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.what, sizeof e.what, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_histMutex);
    HandleHistory& h = g_viewHistory[handle];
    h.ev[h.next % 6] = e;
    h.next++;
}

void log_view_history(const void* handle) {
    std::lock_guard<std::mutex> lock(g_histMutex);
    auto it = g_viewHistory.find(handle);
    if (it == g_viewHistory.end()) {
        mp2dk::log("objects:   handle %p: no history (never created through the bridge)", handle);
        return;
    }
    const HandleHistory& h = it->second;
    const uint32_t count = std::min<uint32_t>(h.next, 6);
    for (uint32_t k = 0; k < count; k++) {
        const HandleEvent& e = h.ev[(h.next - count + k) % 6];
        mp2dk::log("objects:   handle %p, frame %llu (now %llu): %s", handle, (unsigned long long)e.frame,
                   (unsigned long long)R.frame, e.what);
    }
}

const char* view_label(const View* v) { return v && v->tex ? v->tex->label.c_str() : "?"; }

void destroy(View* v) {
    v->diedFrame = R.frame;
    view_event(v->handle, "view #%u of '%s' destroyed (its last release: %s)", v->id, view_label(v), v->lastRelease);
    if (v->slot) {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        g_freedSlots.push_back(v->slot);
    }
    Tex* t = v->tex;
    v->magic = kDead;
    bury(1, v);
    if (t) release_tex(t);
}
void release_view(View* v, const char* who);
void release_sampler(Samp* s);
void destroy(Samp* s) {
    s->magic = kDead;
    bury(2, s);
}
void destroy(BindGroup* g) {
    for (auto& e : g->entries) {
        if (e.view) release_view(e.view, "its bind group's end");
        if (e.sampler) release_sampler(e.sampler);
        if (e.buf) mp2dk::buffer_release(e.buf);
    }
    g->magic = kDead;
    bury(3, g);
}

template <class T> bool drop(std::unordered_map<const void*, T*>& map, T* obj, uint64_t ObjectStats::*alive) {
    if (obj->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return false;
    {
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        auto it = map.find(obj->handle);
        if (it != map.end() && it->second == obj) map.erase(it);
        g_stats.*alive -= 1;
    }
    return true;
}

void release_tex(Tex* t) {
    if (drop(g_textures, t, &ObjectStats::textures)) {
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            if (t->valid && !t->external) g_stats.imageBytes -= t->mem.size;
        }
        destroy(t);
    }
}
void release_view(View* v, const char* who) {
    v->lastRelease = who;
    if (drop(g_views, v, &ObjectStats::views)) destroy(v);
}
void release_sampler(Samp* s) {
    if (drop(g_samplers, s, &ObjectStats::samplers)) destroy(s);
}
void release_bind_group(BindGroup* g) {
    if (drop(g_bindGroups, g, &ObjectStats::bindGroups)) destroy(g);
}

// ---- creation
Tex* make_texture(WGPUTexture handle, const WGPUTextureDescriptor& d) {
    static bool once = [] {
        g_compression = swdk::env_flag("COS_DK_COMPRESSION", false);
        dklog("textures: images in the deko3d image heap; colour render targets %s (COS_DK_COMPRESSION=1)",
              g_compression ? "with hardware compression" : "without hardware compression");
        return true;
    }();
    (void)once;
    auto* t = new Tex;
    t->handle = handle;
    t->wformat = d.format;
    t->width = std::max<uint32_t>(d.size.width, 1);
    t->height = std::max<uint32_t>(d.size.height, 1);
    t->layers = std::max<uint32_t>(d.size.depthOrArrayLayers, 1);
    t->mips = std::max<uint32_t>(d.mipLevelCount, 1);
    t->samples = std::max<uint32_t>(d.sampleCount, 1);
    t->usage = d.usage;
    t->is3d = d.dimension == WGPUTextureDimension_3D;
    t->label = label_of(d.label);
    const FormatInfo f = format_info(d.format);
    t->format = f.format;
    t->depth = f.depth;
    t->compressed = f.compressed;
    t->blockW = f.blockW;
    t->blockH = f.blockH;
    t->blockBytes = f.blockBytes;
    if (f.format == DkImageFormat_None) {
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        if (g_stats.unknownFormats++ < 16)
            dklog("textures: WebGPU format 0x%X (%s, %ux%u) has no deko3d format: not drawn", unsigned(d.format),
                  t->label.c_str(), t->width, t->height);
        return t;
    }
    if (t->samples > 1) {
        static int logged = 0;
        if (logged++ < 4)
            dklog("textures: %s asks for %u samples; drawn single-sampled (multisampling is not supported here)",
                  t->label.c_str(), t->samples);
    }
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.type = d.dimension == WGPUTextureDimension_3D ? DkImageType_3D
             : t->layers > 1                         ? DkImageType_2DArray
                                                     : DkImageType_2D;
    m.format = f.format;
    m.dimensions[0] = t->width;
    m.dimensions[1] = t->height;
    m.dimensions[2] = m.type == DkImageType_2D ? 0 : t->layers;
    m.mipLevels = t->mips;
    const bool render = (d.usage & WGPUTextureUsage_RenderAttachment) != 0;
    if (render && f.canRender) m.flags |= DkImageFlags_UsageRender;
    if (f.can2d) m.flags |= DkImageFlags_Usage2DEngine;
    if (render && g_compression && !f.depth) m.flags |= DkImageFlags_HwCompression;
    swdk::image_tile_size_fix(m, f.compressed ? (t->height + f.blockH - 1) / f.blockH : t->height);
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    t->mem = swdk::image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&t->image, &layout, t->mem.block, t->mem.offset);
    t->valid = true;
    return t;
}

}  // namespace

bool dead_use(const char* kind, const char* where, uint32_t magic) {
    static int logged = 0;
    if (logged++ < 24)
        mp2dk::log("objects: a %s used after its last release (%s, magic 0x%08X): skipped", kind, where, magic);
    return false;
}
bool alive(const Tex* t, const char* where) { return !t || t->magic == kAlive || dead_use("texture", where, t->magic); }
bool alive(const View* v, const char* where) {
    if (!v || v->magic == kAlive) return true;
    static std::atomic<uint32_t> detailed{0};
    if (detailed.fetch_add(1, std::memory_order_relaxed) < 24) {
        mp2dk::log("objects: %s: view #%u of '%s' %ux%u, born frame %llu, destroyed frame %llu (now %llu) by its last "
                   "release (%s), game refs %d, refs %d; the history of its handle:",
                   where, v->id, view_label(v), v->tex ? v->tex->width : 0, v->tex ? v->tex->height : 0,
                   (unsigned long long)v->bornFrame, (unsigned long long)v->diedFrame, (unsigned long long)R.frame,
                   v->lastRelease, v->apiRefs.load(), v->refs.load());
        log_view_history(v->handle);
    }
    return dead_use("texture view", where, v->magic);
}
bool alive(const Samp* s, const char* where) { return !s || s->magic == kAlive || dead_use("sampler", where, s->magic); }
bool alive(const BindGroup* g, const char* where) {
    return !g || g->magic == kAlive || dead_use("bind group", where, g->magic);
}

void retain(Tex* t) { t->refs.fetch_add(1, std::memory_order_relaxed); }
void release(Tex* t) { release_tex(t); }
void retain(View* v) { v->refs.fetch_add(1, std::memory_order_relaxed); }
void retain(Samp* s) { s->refs.fetch_add(1, std::memory_order_relaxed); }
void retain(BindGroup* g) { g->refs.fetch_add(1, std::memory_order_relaxed); }
void release(View* v) { release_view(v, "an encoder after its replay"); }

void explain_missing_view(WGPUTextureView v, const char* where) {
    const uint64_t n = g_missingViews.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 24 && (n & (n - 1)) != 0) return;
    mp2dk::log("objects: %s names view handle %p, which has no live shadow here (%llu so far); its history:", where,
               (void*)v, (unsigned long long)n);
    log_view_history(v);
}
void release(Samp* s) { release_sampler(s); }
void release(BindGroup* g) { release_bind_group(g); }

Tex* make_external_texture(WGPUTexture handle, const WGPUTextureDescriptor& d, const DkImage& image) {
    auto* t = new Tex;
    t->handle = handle;
    t->wformat = d.format;
    t->width = d.size.width;
    t->height = d.size.height;
    t->usage = d.usage;
    t->label = label_of(d.label);
    const FormatInfo f = format_info(d.format);
    t->format = f.format;
    t->image = image;
    t->valid = true;
    t->external = true;
    t->written = true;
    t->mipsWritten = ~0u;
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_textures[handle] = t;
    g_stats.textures++;
    return t;
}

const BindGroup::Entry* bind_group_entry(const BindGroup* g, uint32_t binding) {
    for (const auto& e : g->entries)
        if (e.binding == binding) return &e;
    return nullptr;
}

FormatInfo format_info(WGPUTextureFormat f) {
    FormatInfo i;
    auto plain = [&](DkImageFormat fmt, uint8_t bytes, bool canRender = true, bool can2d = true) {
        i.format = fmt;
        i.blockBytes = bytes;
        i.canRender = canRender;
        i.can2d = can2d;
    };
    auto depth = [&](DkImageFormat fmt, uint8_t bytes) {
        plain(fmt, bytes, true, false);
        i.depth = true;
    };
    auto block = [&](DkImageFormat fmt, uint8_t w, uint8_t h, uint8_t bytes) {
        i.format = fmt;
        i.blockW = w;
        i.blockH = h;
        i.blockBytes = bytes;
        i.compressed = true;
    };
    switch (f) {
    case WGPUTextureFormat_R8Unorm: plain(DkImageFormat_R8_Unorm, 1); break;
    case WGPUTextureFormat_R8Snorm: plain(DkImageFormat_R8_Snorm, 1); break;
    case WGPUTextureFormat_R8Uint: plain(DkImageFormat_R8_Uint, 1, true, false); break;
    case WGPUTextureFormat_R8Sint: plain(DkImageFormat_R8_Sint, 1, true, false); break;
    case WGPUTextureFormat_R16Unorm: plain(DkImageFormat_R16_Unorm, 2); break;
    case WGPUTextureFormat_R16Snorm: plain(DkImageFormat_R16_Snorm, 2); break;
    case WGPUTextureFormat_R16Uint: plain(DkImageFormat_R16_Uint, 2, true, false); break;
    case WGPUTextureFormat_R16Sint: plain(DkImageFormat_R16_Sint, 2, true, false); break;
    case WGPUTextureFormat_R16Float: plain(DkImageFormat_R16_Float, 2); break;
    case WGPUTextureFormat_RG8Unorm: plain(DkImageFormat_RG8_Unorm, 2); break;
    case WGPUTextureFormat_RG8Snorm: plain(DkImageFormat_RG8_Snorm, 2); break;
    case WGPUTextureFormat_RG8Uint: plain(DkImageFormat_RG8_Uint, 2, true, false); break;
    case WGPUTextureFormat_RG8Sint: plain(DkImageFormat_RG8_Sint, 2, true, false); break;
    case WGPUTextureFormat_R32Float: plain(DkImageFormat_R32_Float, 4); break;
    case WGPUTextureFormat_R32Uint: plain(DkImageFormat_R32_Uint, 4, true, false); break;
    case WGPUTextureFormat_R32Sint: plain(DkImageFormat_R32_Sint, 4, true, false); break;
    case WGPUTextureFormat_RG16Unorm: plain(DkImageFormat_RG16_Unorm, 4); break;
    case WGPUTextureFormat_RG16Snorm: plain(DkImageFormat_RG16_Snorm, 4); break;
    case WGPUTextureFormat_RG16Uint: plain(DkImageFormat_RG16_Uint, 4, true, false); break;
    case WGPUTextureFormat_RG16Sint: plain(DkImageFormat_RG16_Sint, 4, true, false); break;
    case WGPUTextureFormat_RG16Float: plain(DkImageFormat_RG16_Float, 4); break;
    case WGPUTextureFormat_RGBA8Unorm: plain(DkImageFormat_RGBA8_Unorm, 4); break;
    case WGPUTextureFormat_RGBA8UnormSrgb: plain(DkImageFormat_RGBA8_Unorm_sRGB, 4); break;
    case WGPUTextureFormat_RGBA8Snorm: plain(DkImageFormat_RGBA8_Snorm, 4); break;
    case WGPUTextureFormat_RGBA8Uint: plain(DkImageFormat_RGBA8_Uint, 4, true, false); break;
    case WGPUTextureFormat_RGBA8Sint: plain(DkImageFormat_RGBA8_Sint, 4, true, false); break;
    case WGPUTextureFormat_BGRA8Unorm: plain(DkImageFormat_BGRA8_Unorm, 4); break;
    case WGPUTextureFormat_BGRA8UnormSrgb: plain(DkImageFormat_BGRA8_Unorm_sRGB, 4); break;
    case WGPUTextureFormat_RGB10A2Uint: plain(DkImageFormat_RGB10A2_Uint, 4, true, false); break;
    case WGPUTextureFormat_RGB10A2Unorm: plain(DkImageFormat_RGB10A2_Unorm, 4); break;
    case WGPUTextureFormat_RG11B10Ufloat: plain(DkImageFormat_RG11B10_Float, 4); break;
    case WGPUTextureFormat_RGB9E5Ufloat: plain(DkImageFormat_E5BGR9_Float, 4, false, true); break;
    case WGPUTextureFormat_RG32Float: plain(DkImageFormat_RG32_Float, 8); break;
    case WGPUTextureFormat_RG32Uint: plain(DkImageFormat_RG32_Uint, 8, true, false); break;
    case WGPUTextureFormat_RG32Sint: plain(DkImageFormat_RG32_Sint, 8, true, false); break;
    case WGPUTextureFormat_RGBA16Unorm: plain(DkImageFormat_RGBA16_Unorm, 8); break;
    case WGPUTextureFormat_RGBA16Snorm: plain(DkImageFormat_RGBA16_Snorm, 8); break;
    case WGPUTextureFormat_RGBA16Uint: plain(DkImageFormat_RGBA16_Uint, 8, true, false); break;
    case WGPUTextureFormat_RGBA16Sint: plain(DkImageFormat_RGBA16_Sint, 8, true, false); break;
    case WGPUTextureFormat_RGBA16Float: plain(DkImageFormat_RGBA16_Float, 8); break;
    case WGPUTextureFormat_RGBA32Float: plain(DkImageFormat_RGBA32_Float, 16); break;
    case WGPUTextureFormat_RGBA32Uint: plain(DkImageFormat_RGBA32_Uint, 16, true, false); break;
    case WGPUTextureFormat_RGBA32Sint: plain(DkImageFormat_RGBA32_Sint, 16, true, false); break;
    case WGPUTextureFormat_Stencil8: depth(DkImageFormat_S8, 1); break;
    case WGPUTextureFormat_Depth16Unorm: depth(DkImageFormat_Z16, 2); break;
    case WGPUTextureFormat_Depth24Plus: depth(DkImageFormat_Z24X8, 4); break;
    case WGPUTextureFormat_Depth24PlusStencil8: depth(DkImageFormat_Z24S8, 4); break;
    case WGPUTextureFormat_Depth32Float: depth(DkImageFormat_ZF32, 4); break;
    case WGPUTextureFormat_Depth32FloatStencil8: depth(DkImageFormat_ZF32_X24S8, 8); break;
    case WGPUTextureFormat_BC1RGBAUnorm: block(DkImageFormat_RGBA_BC1, 4, 4, 8); break;
    case WGPUTextureFormat_BC1RGBAUnormSrgb: block(DkImageFormat_RGBA_BC1_sRGB, 4, 4, 8); break;
    case WGPUTextureFormat_BC2RGBAUnorm: block(DkImageFormat_RGBA_BC2, 4, 4, 16); break;
    case WGPUTextureFormat_BC2RGBAUnormSrgb: block(DkImageFormat_RGBA_BC2_sRGB, 4, 4, 16); break;
    case WGPUTextureFormat_BC3RGBAUnorm: block(DkImageFormat_RGBA_BC3, 4, 4, 16); break;
    case WGPUTextureFormat_BC3RGBAUnormSrgb: block(DkImageFormat_RGBA_BC3_sRGB, 4, 4, 16); break;
    case WGPUTextureFormat_BC4RUnorm: block(DkImageFormat_R_BC4_Unorm, 4, 4, 8); break;
    case WGPUTextureFormat_BC4RSnorm: block(DkImageFormat_R_BC4_Snorm, 4, 4, 8); break;
    case WGPUTextureFormat_BC5RGUnorm: block(DkImageFormat_RG_BC5_Unorm, 4, 4, 16); break;
    case WGPUTextureFormat_BC5RGSnorm: block(DkImageFormat_RG_BC5_Snorm, 4, 4, 16); break;
    case WGPUTextureFormat_BC6HRGBUfloat: block(DkImageFormat_RGBA_BC6H_UF16_Float, 4, 4, 16); break;
    case WGPUTextureFormat_BC6HRGBFloat: block(DkImageFormat_RGBA_BC6H_SF16_Float, 4, 4, 16); break;
    case WGPUTextureFormat_BC7RGBAUnorm: block(DkImageFormat_RGBA_BC7_Unorm, 4, 4, 16); break;
    case WGPUTextureFormat_BC7RGBAUnormSrgb: block(DkImageFormat_RGBA_BC7_Unorm_sRGB, 4, 4, 16); break;
    case WGPUTextureFormat_ASTC4x4Unorm: block(DkImageFormat_RGBA_ASTC_4x4, 4, 4, 16); break;
    case WGPUTextureFormat_ASTC4x4UnormSrgb: block(DkImageFormat_RGBA_ASTC_4x4_sRGB, 4, 4, 16); break;
    case WGPUTextureFormat_ASTC5x4Unorm: block(DkImageFormat_RGBA_ASTC_5x4, 5, 4, 16); break;
    case WGPUTextureFormat_ASTC5x4UnormSrgb: block(DkImageFormat_RGBA_ASTC_5x4_sRGB, 5, 4, 16); break;
    case WGPUTextureFormat_ASTC5x5Unorm: block(DkImageFormat_RGBA_ASTC_5x5, 5, 5, 16); break;
    case WGPUTextureFormat_ASTC5x5UnormSrgb: block(DkImageFormat_RGBA_ASTC_5x5_sRGB, 5, 5, 16); break;
    case WGPUTextureFormat_ASTC6x5Unorm: block(DkImageFormat_RGBA_ASTC_6x5, 6, 5, 16); break;
    case WGPUTextureFormat_ASTC6x5UnormSrgb: block(DkImageFormat_RGBA_ASTC_6x5_sRGB, 6, 5, 16); break;
    case WGPUTextureFormat_ASTC6x6Unorm: block(DkImageFormat_RGBA_ASTC_6x6, 6, 6, 16); break;
    case WGPUTextureFormat_ASTC6x6UnormSrgb: block(DkImageFormat_RGBA_ASTC_6x6_sRGB, 6, 6, 16); break;
    case WGPUTextureFormat_ASTC8x5Unorm: block(DkImageFormat_RGBA_ASTC_8x5, 8, 5, 16); break;
    case WGPUTextureFormat_ASTC8x5UnormSrgb: block(DkImageFormat_RGBA_ASTC_8x5_sRGB, 8, 5, 16); break;
    case WGPUTextureFormat_ASTC8x6Unorm: block(DkImageFormat_RGBA_ASTC_8x6, 8, 6, 16); break;
    case WGPUTextureFormat_ASTC8x6UnormSrgb: block(DkImageFormat_RGBA_ASTC_8x6_sRGB, 8, 6, 16); break;
    case WGPUTextureFormat_ASTC8x8Unorm: block(DkImageFormat_RGBA_ASTC_8x8, 8, 8, 16); break;
    case WGPUTextureFormat_ASTC8x8UnormSrgb: block(DkImageFormat_RGBA_ASTC_8x8_sRGB, 8, 8, 16); break;
    case WGPUTextureFormat_ASTC10x5Unorm: block(DkImageFormat_RGBA_ASTC_10x5, 10, 5, 16); break;
    case WGPUTextureFormat_ASTC10x5UnormSrgb: block(DkImageFormat_RGBA_ASTC_10x5_sRGB, 10, 5, 16); break;
    case WGPUTextureFormat_ASTC10x6Unorm: block(DkImageFormat_RGBA_ASTC_10x6, 10, 6, 16); break;
    case WGPUTextureFormat_ASTC10x6UnormSrgb: block(DkImageFormat_RGBA_ASTC_10x6_sRGB, 10, 6, 16); break;
    case WGPUTextureFormat_ASTC10x8Unorm: block(DkImageFormat_RGBA_ASTC_10x8, 10, 8, 16); break;
    case WGPUTextureFormat_ASTC10x8UnormSrgb: block(DkImageFormat_RGBA_ASTC_10x8_sRGB, 10, 8, 16); break;
    case WGPUTextureFormat_ASTC10x10Unorm: block(DkImageFormat_RGBA_ASTC_10x10, 10, 10, 16); break;
    case WGPUTextureFormat_ASTC10x10UnormSrgb: block(DkImageFormat_RGBA_ASTC_10x10_sRGB, 10, 10, 16); break;
    case WGPUTextureFormat_ASTC12x10Unorm: block(DkImageFormat_RGBA_ASTC_12x10, 12, 10, 16); break;
    case WGPUTextureFormat_ASTC12x10UnormSrgb: block(DkImageFormat_RGBA_ASTC_12x10_sRGB, 12, 10, 16); break;
    case WGPUTextureFormat_ASTC12x12Unorm: block(DkImageFormat_RGBA_ASTC_12x12, 12, 12, 16); break;
    case WGPUTextureFormat_ASTC12x12UnormSrgb: block(DkImageFormat_RGBA_ASTC_12x12_sRGB, 12, 12, 16); break;
    default: break;
    }
    return i;
}

Tex* find_texture(WGPUTexture t) { return find(g_textures, t); }
View* find_view(WGPUTextureView v) { return find(g_views, v); }
Samp* find_sampler(WGPUSampler s) { return find(g_samplers, s); }
BindGroup* find_bind_group(WGPUBindGroup g) { return find(g_bindGroups, g); }

uint32_t view_slot(View* v) {
    if (!v || !alive(v, "sampled") || !v->tex || !alive(v->tex, "a sampled view's texture") || !v->tex->valid)
        return swdk::kEmptyImage;
    if (v->slot == 0) {
        const uint32_t slot = swdk::image_slot_alloc();
        if (slot == 0) return swdk::kEmptyImage;
        v->view.pImage = &v->tex->image;
        swdk::write_image_view_descriptor(slot, v->view);
        v->slot = slot;
    }
    return v->slot;
}

DkResHandle texture_handle(View* v, Samp* s) {
    const uint32_t image = view_slot(v);
    // mp2-nx (deko-12 diagnostics): a texture sampled before anything wrote it
    if (image != swdk::kEmptyImage && v && v->tex && !v->tex->external &&
        !v->tex->written.load(std::memory_order_relaxed) && !v->tex->warnedUnwritten.exchange(true)) {
        static int logged = 0;
        if (logged++ < 60)
            mp2dk::log("diag: '%s' (%ux%u, %u layers, %u mips, format 0x%X) is sampled before anything wrote it",
                       v->tex->label.c_str(), v->tex->width, v->tex->height, v->tex->layers, v->tex->mips,
                       unsigned(v->tex->wformat));
    }
    // mp2-nx (deko-15 diagnostics): a texture sampled with mip levels nothing has written yet (a smaller copy
    // left as zeros, or never uploaded)
    if (image != swdk::kEmptyImage && v && v->tex && !v->tex->external && v->mipCount > 1 &&
        v->mipCount < 32 && !v->tex->warnedMips.load(std::memory_order_relaxed)) {
        const uint32_t want = ((1u << v->mipCount) - 1u) << v->baseMip;
        const uint32_t have = v->tex->mipsWritten.load(std::memory_order_relaxed);
        if ((have & want) != want && !v->tex->warnedMips.exchange(true)) {
            static int logged = 0;
            if (logged++ < 60)
                mp2dk::log("diag: '%s' (%ux%u, %u mips, format 0x%X) is sampled with levels never written "
                           "(written 0x%X of 0x%X)",
                           v->tex->label.c_str(), v->tex->width, v->tex->height, v->tex->mips,
                           unsigned(v->tex->wformat), have & want, want);
        }
    }
    const uint32_t sampler = s ? swdk::sampler_slot(s->sampler, s->key) : uint32_t(swdk::kSamplerNearestClamp);
    // mp2-nx (deko-17 test cycle)
    if (image != swdk::kEmptyImage && v && v->tex) {
        Tex* t = v->tex;
        if (t->smallResolved < 0)
            t->smallResolved = t->label == "Resolved Texture" && t->width <= 128 && t->height <= 128 ? 1 : 0;
        if (t->smallResolved && mp2dk::cycle_blank_resolved()) return dkMakeTextureHandle(swdk::kEmptyImage, sampler);
        if (t->discarded.load(std::memory_order_relaxed)) {
            mp2dk::count_discarded_sample();
            if (mp2dk::cycle_discard_rules()) return dkMakeTextureHandle(swdk::kEmptyImage, sampler);
        }
    }
    return dkMakeTextureHandle(image, sampler);
}

const DkResHandle* gx_handles(BindGroup* g) {
    const uint64_t frame = R.frame + 1;
    if (g->handlesFrame == frame) return g->handles;
    View* views[8] = {};
    Samp* samplers[8] = {};
    for (const auto& e : g->entries) {
        const uint32_t unit = e.binding / 2;
        if (unit >= 8) continue;
        if (e.binding & 1) {
            samplers[unit] = e.sampler;
        } else {
            views[unit] = e.view;
        }
    }
    for (uint32_t i = 0; i < 8; i++) g->handles[i] = texture_handle(views[i], samplers[i]);
    g->handlesFrame = frame;
    return g->handles;
}

void objects_frame_start() {
    {
        std::vector<Grave> free;
        {
            std::lock_guard<std::mutex> lock(g_graveMutex);
            auto it = std::partition(g_graves.begin(), g_graves.end(),
                                     [](const Grave& g) { return g.frame + kGraveFrames > R.frame; });
            free.assign(it, g_graves.end());
            g_graves.erase(it, g_graves.end());
        }
        for (const Grave& g : free) {
            switch (g.kind) {
            case 0: delete static_cast<Tex*>(g.p); break;
            case 1: delete static_cast<View*>(g.p); break;
            case 2: delete static_cast<Samp*>(g.p); break;
            default: delete static_cast<BindGroup*>(g.p); break;
            }
        }
    }
    std::vector<uint32_t> slots;
    {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        slots.swap(g_freedSlots);
    }
    for (uint32_t s : slots) swdk::image_slot_free(s);
}

DkImageView level_view(Tex* t, uint32_t level) {
    DkImageView v;
    dkImageViewDefaults(&v, &t->image);
    v.mipLevelOffset = uint8_t(level);
    return v;
}

ObjectStats object_stats() {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    return g_stats;
}

}  // namespace aurora::gfx::dk

using namespace aurora::gfx::dk;

extern "C" {

WGPUTexture __wrap_wgpuDeviceCreateTexture(WGPUDevice device, WGPUTextureDescriptor const* descriptor) {
    WGPUTexture t = __real_wgpuDeviceCreateTexture(device, descriptor);
    if (t && descriptor && R.device) {
        mp2dk::note("texture create");
        Tex* shadow = make_texture(t, *descriptor);
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        auto [it, inserted] = g_textures.emplace(t, shadow);
        if (!inserted) it->second = shadow;  // (a stale entry: Dawn reused the address)
        g_stats.textures++;
        g_stats.texturesCreated++;
        if (shadow->valid) g_stats.imageBytes += shadow->mem.size;
        lock.unlock();
        if (shadow->valid) mp2dk::texture_zero_later(shadow);
    }
    return t;
}

void __wrap_wgpuTextureAddRef(WGPUTexture texture) {
    if (R.device)
        if (Tex* t = find(g_textures, texture)) t->refs.fetch_add(1, std::memory_order_relaxed);
    __real_wgpuTextureAddRef(texture);
}

void __wrap_wgpuTextureRelease(WGPUTexture texture) {
    if (R.device)
        if (Tex* t = find(g_textures, texture)) release_tex(t);
    __real_wgpuTextureRelease(texture);
}

WGPUTextureView __wrap_wgpuTextureCreateView(WGPUTexture texture, WGPUTextureViewDescriptor const* d) {
    WGPUTextureView v = __real_wgpuTextureCreateView(texture, d);
    if (!R.device) return v;
    // mp2-nx (deko-9): Dawn hands back a cached object for an equal descriptor (views, samplers, layouts,
    // shader modules, pipelines) with one more reference; its shadow takes that reference instead of being
    // replaced by a new one (a replaced shadow died at the first release while Aurora still used it)
    // mp2-nx (the white-texture fix): ...but the same address also comes back for a NEW view once Dawn has freed
    // the old one, while this shadow still lives (an encoder or a bind group here holds it longer than Dawn
    // does). Taking that as a cache hit gave the new view the old one's texture: a wall sampled an EFB copy
    // (frame capture cap008: draw 6, a rock wall, reading 'Resolved Texture' 51x51), different textures on
    // every boot. So the existing shadow is kept only when it is the same view of the same texture; otherwise
    // the new one replaces it in the map (the old one lives on for whoever still holds it).
    View* existing = find(g_views, v);
    if (existing && existing->magic != kAlive) existing = nullptr;
    Tex* t = find(g_textures, texture);
    if (!v || !t) {
        if (existing) {
            existing->refs.fetch_add(1, std::memory_order_relaxed);
            existing->apiRefs.fetch_add(1, std::memory_order_relaxed);
            view_event(v, "CreateView of a texture with no shadow came back as view #%u of '%s': taken as the same",
                       existing->id, view_label(existing));
        }
        return v;
    }
    auto* view = new View;
    view->handle = v;
    view->tex = t;
    t->refs.fetch_add(1, std::memory_order_relaxed);
    dkImageViewDefaults(&view->view, &t->image);
    const uint32_t base = d ? d->baseMipLevel : 0;
    uint32_t count = d && d->mipLevelCount != WGPU_MIP_LEVEL_COUNT_UNDEFINED ? d->mipLevelCount : t->mips - base;
    count = std::min(count, t->mips > base ? t->mips - base : 1u);
    // mp2-nx (deko-12 A/B, deko3d_env.txt MP2DK_MIP0=1): sampled views see only their first level
    static const bool kMip0 = swdk::env_flag("MP2DK_MIP0", false);
    if (kMip0 && count > 1) count = 1;
    view->baseMip = base;
    view->mipCount = count;
    view->view.mipLevelOffset = uint8_t(base);
    view->view.mipLevelCount = uint8_t(count);
    if (d && d->format != WGPUTextureFormat_Undefined && d->format != t->wformat) {
        const FormatInfo f = format_info(d->format);
        if (f.format != DkImageFormat_None) view->view.format = f.format;
    }
    // mp2-nx (deko-11): the view's type is the view dimension, not the image's. The shader's sampler type
    // follows the view (a cube probe is a 6-layer 2D texture viewed as a cube; the VFX path views a
    // one-layer 2D texture as a 2D array), and a descriptor of another type samples nothing sensible.
    // (MP2DK_OLD_VIEWS=1 in deko3d_env.txt: deko-10's handling, for an A/B test)
    static const bool kOldViews = swdk::env_flag("MP2DK_OLD_VIEWS", false);
    if (kOldViews) {
        if (d && d->arrayLayerCount != WGPU_ARRAY_LAYER_COUNT_UNDEFINED && t->layers > 1) {
            view->view.layerOffset = uint16_t(d->baseArrayLayer);
            view->view.layerCount = uint16_t(d->arrayLayerCount);
            if (d->dimension == WGPUTextureViewDimension_2D) view->view.type = DkImageType_2D;
        }
    } else if (d && t->layers > 1 && !t->is3d && d->baseArrayLayer < t->layers) {
        view->view.layerOffset = uint16_t(d->baseArrayLayer);
        const uint32_t left = t->layers - d->baseArrayLayer;
        view->view.layerCount = uint16_t(d->arrayLayerCount != WGPU_ARRAY_LAYER_COUNT_UNDEFINED
                                             ? std::min(d->arrayLayerCount, left)
                                             : left);
    }
    if (d && t->valid && !kOldViews) {
        switch (d->dimension) {
        case WGPUTextureViewDimension_2D: view->view.type = DkImageType_2D; break;
        case WGPUTextureViewDimension_2DArray: view->view.type = DkImageType_2DArray; break;
        case WGPUTextureViewDimension_Cube:
            if (t->layers >= 6) view->view.type = DkImageType_Cubemap;
            break;
        case WGPUTextureViewDimension_CubeArray:
            if (t->layers >= 6) view->view.type = DkImageType_CubemapArray;
            break;
        default: break;  // 3D, 1D, undefined: the image's own type
        }
    }
    if (d && d->aspect == WGPUTextureAspect_StencilOnly) view->view.dsSource = DkDsSource_Stencil;
    for (const WGPUChainedStruct* c = d ? d->nextInChain : nullptr; c; c = c->next) {
        if (c->sType == WGPUSType_TextureComponentSwizzleDescriptor) {
            const auto* s = reinterpret_cast<const WGPUTextureComponentSwizzleDescriptor*>(c);
            view->view.swizzle[0] = swizzle_of(s->swizzle.r, DkImageSwizzle_Red);
            view->view.swizzle[1] = swizzle_of(s->swizzle.g, DkImageSwizzle_Green);
            view->view.swizzle[2] = swizzle_of(s->swizzle.b, DkImageSwizzle_Blue);
            view->view.swizzle[3] = swizzle_of(s->swizzle.a, DkImageSwizzle_Alpha);
        }
    }
    const bool matches =
        existing && existing->tex == t && existing->baseMip == view->baseMip && existing->mipCount == view->mipCount &&
        existing->view.format == view->view.format && existing->view.type == view->view.type &&
        existing->view.layerOffset == view->view.layerOffset && existing->view.layerCount == view->view.layerCount &&
        existing->view.dsSource == view->view.dsSource &&
        memcmp(existing->view.swizzle, view->view.swizzle, sizeof view->view.swizzle) == 0;
    // mp2-nx (the scan-visor build): while the game still holds the existing view (apiRefs), Dawn's object is
    // alive, so this is Dawn's cache handing it back: the same view, whatever the comparison says. The
    // white-texture fix replaced it whenever the comparison failed, and the game's next release then killed the
    // replacement while the game still held the view (the scan visor's passes with no colour target, its bind
    // groups' dead views: log of 2026-11-07). Logged, with what differed, to see why the comparison failed.
    const int32_t held = existing ? existing->apiRefs.load(std::memory_order_relaxed) : 0;
    if (existing && (matches || held > 0)) {
        if (!matches) {
            const uint64_t n = g_heldMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 24 || (n & (n - 1)) == 0)
                mp2dk::log("objects: Dawn handed back view #%u of '%s' %ux%u, which the game still holds (game refs %d), for "
                           "a CreateView that differs (%llu so far): kept as the same view (the old rule replaced it). "
                           "Old: texture shadow %p, mips %u+%u, format %d, type %d, layers %u+%u; new: texture shadow %p "
                           "'%s', mips %u+%u, format %d, type %d, layers %u+%u",
                           existing->id, view_label(existing), existing->tex ? existing->tex->width : 0,
                           existing->tex ? existing->tex->height : 0, held, (unsigned long long)n, (void*)existing->tex,
                           existing->baseMip, existing->mipCount, int(existing->view.format), int(existing->view.type),
                           existing->view.layerOffset, existing->view.layerCount, (void*)t, t->label.c_str(),
                           view->baseMip, view->mipCount, int(view->view.format), int(view->view.type),
                           view->view.layerOffset, view->view.layerCount);
        }
        view_event(v, "CreateView came back as view #%u of '%s' (%s; game refs %d)", existing->id, view_label(existing),
                   matches ? "a cache hit" : "held by the game, though it differs", held + 1);
        existing->refs.fetch_add(1, std::memory_order_relaxed);
        existing->apiRefs.fetch_add(1, std::memory_order_relaxed);
        release_tex(t);
        delete view;
        return v;
    }
    view->id = g_nextViewId.fetch_add(1, std::memory_order_relaxed);
    view->bornFrame = R.frame;
    if (existing)
        view_event(v, "new view #%u of '%s' %ux%u replaced view #%u of '%s' (no game refs left; refs %d)", view->id,
                   t->label.c_str(), t->width, t->height, existing->id, view_label(existing), existing->refs.load());
    else
        view_event(v, "new view #%u of '%s' %ux%u", view->id, t->label.c_str(), t->width, t->height);
    if (existing) {
        const uint64_t n = g_reusedViews.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 20 || (n & (n - 1)) == 0)
            mp2dk::log("objects: a new view of '%s' %ux%u came back at the address of a live view of '%s' %ux%u "
                       "(%llu so far): the new one replaces it",
                       t->label.c_str(), t->width, t->height, existing->tex ? existing->tex->label.c_str() : "?",
                       existing->tex ? existing->tex->width : 0, existing->tex ? existing->tex->height : 0,
                       (unsigned long long)n);
    }
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_views[v] = view;
    g_stats.views++;
    return v;
}

void __wrap_wgpuTextureViewAddRef(WGPUTextureView view) {
    if (R.device)
        if (View* v = find(g_views, view)) {
            v->refs.fetch_add(1, std::memory_order_relaxed);
            v->apiRefs.fetch_add(1, std::memory_order_relaxed);
        }
    __real_wgpuTextureViewAddRef(view);
}

void __wrap_wgpuTextureViewRelease(WGPUTextureView view) {
    if (R.device) {
        if (View* v = find(g_views, view)) {
            const int32_t api = v->apiRefs.fetch_sub(1, std::memory_order_relaxed) - 1;
            view_event(view, "the game released view #%u of '%s' (game refs now %d, refs before %d)", v->id,
                       view_label(v), api, v->refs.load());
            if (api < 0) {
                const uint64_t n = g_apiNegative.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 16)
                    mp2dk::log("objects: the game released view #%u of '%s' more often than it took it (game refs %d)",
                               v->id, view_label(v), api);
            }
            release_view(v, "the game's release");
        } else {
            const uint64_t n = g_unknownReleases.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 16 || (n & (n - 1)) == 0) {
                mp2dk::log("objects: the game released view handle %p, which has no live shadow here (%llu so far); "
                           "its history:",
                           (void*)view, (unsigned long long)n);
                log_view_history(view);
            }
        }
    }
    __real_wgpuTextureViewRelease(view);
}

WGPUSampler __wrap_wgpuDeviceCreateSampler(WGPUDevice device, WGPUSamplerDescriptor const* d) {
    WGPUSampler s = __real_wgpuDeviceCreateSampler(device, d);
    if (!s || !R.device) return s;
    // mp2-nx (deko-9): Dawn hands back a cached object for an equal descriptor (views, samplers, layouts,
    // shader modules, pipelines) with one more reference; its shadow takes that reference instead of being
    // replaced by a new one (a replaced shadow died at the first release while Aurora still used it)
    // mp2-nx (the white-texture fix): a cache hit only when the state is the same (see CreateView)
    Samp* existing = find(g_samplers, s);
    if (existing && existing->magic != kAlive) existing = nullptr;
    auto* samp = new Samp;
    samp->handle = s;
    DkSampler& k = samp->sampler;
    memset(&k, 0, sizeof k);  // (the key hashes its bytes, padding included)
    dkSamplerDefaults(&k);
    WGPUSamplerDescriptor def = WGPU_SAMPLER_DESCRIPTOR_INIT;
    const WGPUSamplerDescriptor& sd = d ? *d : def;
    k.wrapMode[0] = wrap_of(sd.addressModeU);
    k.wrapMode[1] = wrap_of(sd.addressModeV);
    k.wrapMode[2] = wrap_of(sd.addressModeW);
    k.magFilter = sd.magFilter == WGPUFilterMode_Linear ? DkFilter_Linear : DkFilter_Nearest;
    k.minFilter = sd.minFilter == WGPUFilterMode_Linear ? DkFilter_Linear : DkFilter_Nearest;
    k.mipFilter = sd.mipmapFilter == WGPUMipmapFilterMode_Linear ? DkMipFilter_Linear : DkMipFilter_Nearest;
    // deko3d raises the max to the min when min > max where GL's state tracker swaps them (SwitchWakerHD
    // descriptors.cpp): keep them ordered
    k.lodClampMin = std::min(sd.lodMinClamp, sd.lodMaxClamp);
    k.lodClampMax = std::max(sd.lodMinClamp, sd.lodMaxClamp);
    k.maxAnisotropy = float(std::max<uint16_t>(sd.maxAnisotropy, 1));
    if (sd.compare != WGPUCompareFunction_Undefined) {
        k.compareEnable = true;
        k.compareOp = DkCompareOp(DkCompareOp_Never + (int(sd.compare) - int(WGPUCompareFunction_Never)));
    }
    samp->key = fnv_bytes(&k, sizeof k);
    if (existing && existing->key == samp->key) {
        existing->refs.fetch_add(1, std::memory_order_relaxed);
        delete samp;
        return s;
    }
    if (existing) {
        const uint64_t n = g_reusedSamplers.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 20 || (n & (n - 1)) == 0)
            mp2dk::log("objects: a new sampler came back at the address of a live one with other state (%llu so far): "
                       "the new one replaces it",
                       (unsigned long long)n);
    }
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_samplers[s] = samp;
    g_stats.samplers++;
    return s;
}

void __wrap_wgpuSamplerAddRef(WGPUSampler sampler) {
    if (R.device)
        if (Samp* s = find(g_samplers, sampler)) s->refs.fetch_add(1, std::memory_order_relaxed);
    __real_wgpuSamplerAddRef(sampler);
}

void __wrap_wgpuSamplerRelease(WGPUSampler sampler) {
    if (R.device)
        if (Samp* s = find(g_samplers, sampler)) release_sampler(s);
    __real_wgpuSamplerRelease(sampler);
}

WGPUBindGroup __wrap_wgpuDeviceCreateBindGroup(WGPUDevice device, WGPUBindGroupDescriptor const* d) {
    WGPUBindGroup g = __real_wgpuDeviceCreateBindGroup(device, d);
    if (!g || !d || !R.device) return g;
    auto* group = new BindGroup;
    group->handle = g;
    std::vector<uint32_t> dynamic;
    {
        std::shared_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_layouts.find(d->layout);
        if (it != g_layouts.end()) dynamic = it->second->dynamic;
    }
    for (size_t i = 0; i < d->entryCount; i++) {
        const WGPUBindGroupEntry& e = d->entries[i];
        if (!e.textureView && !e.sampler && !e.buffer) continue;
        BindGroup::Entry entry;
        entry.binding = e.binding;
        entry.buffer = e.buffer;
        entry.offset = e.offset;
        entry.size = e.size;
        entry.view = find(g_views, e.textureView);
        if (!entry.view && e.textureView) explain_missing_view(e.textureView, "a new bind group");  // (scan-visor build)
        entry.sampler = find(g_samplers, e.sampler);
        if ((entry.buf = mp2dk::find_buffer(e.buffer))) mp2dk::buffer_retain(entry.buf);
        for (size_t k = 0; k < dynamic.size(); k++)
            if (dynamic[k] == e.binding) entry.dyn = int(k);
        // the bind group holds its views and samplers as Dawn's does
        if (entry.view) entry.view->refs.fetch_add(1, std::memory_order_relaxed);
        if (entry.sampler) entry.sampler->refs.fetch_add(1, std::memory_order_relaxed);
        group->entries.push_back(entry);
    }
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_bindGroups[g] = group;
    g_stats.bindGroups++;
    return g;
}

void __wrap_wgpuBindGroupAddRef(WGPUBindGroup group) {
    if (R.device)
        if (BindGroup* g = find(g_bindGroups, group)) g->refs.fetch_add(1, std::memory_order_relaxed);
    __real_wgpuBindGroupAddRef(group);
}

void __wrap_wgpuBindGroupRelease(WGPUBindGroup group) {
    if (R.device)
        if (BindGroup* g = find(g_bindGroups, group)) release_bind_group(g);
    __real_wgpuBindGroupRelease(group);
}

}  // extern "C"

// ---- mp2-nx: bind group layouts (their dynamic offsets) and texture destruction
extern "C" {

WGPUBindGroupLayout __wrap_wgpuDeviceCreateBindGroupLayout(WGPUDevice device, WGPUBindGroupLayoutDescriptor const* d) {
    WGPUBindGroupLayout l = __real_wgpuDeviceCreateBindGroupLayout(device, d);
    if (!l || !d || !R.device) return l;
    if (Layout* existing = find(g_layouts, l)) {  // (a cached layout, see the views above)
        existing->refs.fetch_add(1, std::memory_order_relaxed);
        return l;
    }
    auto* layout = new Layout;
    for (size_t i = 0; i < d->entryCount; i++)
        if (d->entries[i].buffer.hasDynamicOffset) layout->dynamic.push_back(d->entries[i].binding);
    std::sort(layout->dynamic.begin(), layout->dynamic.end());
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_layouts[l] = layout;
    return l;
}

void __wrap_wgpuBindGroupLayoutAddRef(WGPUBindGroupLayout layout) {
    if (R.device)
        if (Layout* l = find(g_layouts, layout)) l->refs.fetch_add(1, std::memory_order_relaxed);
    __real_wgpuBindGroupLayoutAddRef(layout);
}

void __wrap_wgpuBindGroupLayoutRelease(WGPUBindGroupLayout layout) {
    if (R.device)
        if (Layout* l = find(g_layouts, layout)) {
            if (l->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                {
                    std::unique_lock<std::shared_mutex> lock(g_mutex);
                    auto it = g_layouts.find(layout);
                    if (it != g_layouts.end() && it->second == l) g_layouts.erase(it);
                }
                delete l;
            }
        }
    __real_wgpuBindGroupLayoutRelease(layout);
}

void __wrap_wgpuTextureDestroy(WGPUTexture texture) {
    // Dawn frees a destroyed texture's memory at once while the object lives on; the image goes when the
    // last reference does (views may still name it until then)
    __real_wgpuTextureDestroy(texture);
}

}  // extern "C"

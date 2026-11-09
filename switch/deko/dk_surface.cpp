// mp2-nx deko3d renderer: the run-time choice, the set-up, the surface and the present (dk_bridge.hpp). MIT.
//
// The choice: the Switch menu's "Renderer" row writes <app folder>/renderer_deko3d (the deko3d renderer on
// the next start) or removes it. A boot marker (<app folder>/deko3d_boot, the number of starts that did not
// reach a first present) sends the game back to Vulkan after two such starts: the choice file is removed and
// the menu says why (mp2nx_dk_fell_back).
//
// The surface is this renderer's (Dawn's Null device has no window): GetCapabilities offers RGBA8Unorm and
// FIFO; Configure (re)creates the deko3d swapchain at Aurora's size and a Null texture per swapchain image,
// whose shadow is that image; GetCurrentTexture acquires this frame's image; Present submits what is left
// and presents it.
#include "dk_bridge.hpp"

#include "../include/mp2nx_app.h"

#include <switch.h>

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern "C" {
WGPUSurface __real_wgpuInstanceCreateSurface(WGPUInstance, WGPUSurfaceDescriptor const*);
void __real_wgpuSurfaceAddRef(WGPUSurface);
void __real_wgpuSurfaceRelease(WGPUSurface);
void __real_wgpuSurfaceConfigure(WGPUSurface, WGPUSurfaceConfiguration const*);
WGPUStatus __real_wgpuSurfaceGetCapabilities(WGPUSurface, WGPUAdapter, WGPUSurfaceCapabilities*);
void __real_wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities);
void __real_wgpuSurfaceGetCurrentTexture(WGPUSurface, WGPUSurfaceTexture*);
WGPUStatus __real_wgpuSurfacePresent(WGPUSurface);
WGPUTexture __real_wgpuDeviceCreateTexture(WGPUDevice, WGPUTextureDescriptor const*);
void __wrap_wgpuTextureAddRef(WGPUTexture);
void __wrap_wgpuTextureRelease(WGPUTexture);
}

namespace swdk {
void objects_frame_start_hook() { aurora::gfx::dk::objects_frame_start(); }
}

namespace mp2dk {
extern bool g_down;
void encoder_init();
void encoder_report();
void cycle_tick();
void capture_flush();
void perf_tick();  // dk_encoder.cpp (the map-speed build)
void shot_take_locked(const DkImage& image, uint32_t w, uint32_t h);
void pipelines_report();
std::mutex& submit_mutex();
void flush_locked(const char* why);

namespace {

// mp2-nx v0.5.0: deko3d is the default renderer. This file, written from the Switch menu, picks Vulkan instead.
// (The test builds' opposite marker, renderer_deko3d, is removed when seen.)
constexpr const char* kVulkanPath = MP2NX_APP_DIR "/renderer_vulkan";
constexpr const char* kOldChoicePath = MP2NX_APP_DIR "/renderer_deko3d";
constexpr const char* kBootPath = MP2NX_APP_DIR "/deko3d_boot";

bool g_on = false;
bool g_fellBack = false;
bool g_presented = false;

struct Surface {
    std::atomic<int32_t> refs{1};
    WGPUDevice device = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
    uint32_t width = 0, height = 0;
    WGPUTexture textures[4] = {};
    uint32_t count = 0;
};
Surface g_surface;  // the one surface (Aurora makes one per window)
const WGPUTextureFormat kFormats[] = {WGPUTextureFormat_RGBA8Unorm};
const WGPUPresentMode kPresentModes[] = {WGPUPresentMode_Fifo};
const WGPUCompositeAlphaMode kAlphaModes[] = {WGPUCompositeAlphaMode_Opaque};

bool is_ours(WGPUSurface s) { return reinterpret_cast<Surface*>(s) == &g_surface; }

int read_boot_count() {
    FILE* f = fopen(kBootPath, "r");
    if (!f) return 0;
    int n = 0;
    if (fscanf(f, "%d", &n) != 1) n = 0;
    fclose(f);
    return n;
}

void write_boot_count(int n) {
    if (FILE* f = fopen(kBootPath, "w")) {
        fprintf(f, "%d\n", n);
        fclose(f);
    }
}

uint64_t g_reportAt = 0;
int g_slot = -1;  // mp2-nx (deko-16): this frame's swapchain image (the test cycle's screenshots)

// ---- mp2-nx (the boot-time build): the boot screen
// From the swapchain's creation to Aurora's first GetCurrentTexture, a thread here shows what the start is
// doing instead of a black screen: the stage the game thread is in (load_boost.cpp's mp2nx_boot_mark) and the
// stored pipelines built so far (Aurora's aurora_pipeline_boot_progress). Drawn by the CPU into a buffer and
// copied into a swapchain image about 15 times a second, under the submit lock, each frame acquired and
// presented in one hold of it, so it never meets a frame of the game's. MP2DK_BOOT_SCREEN=0 turns it off.
std::atomic<bool> g_gameScreen{false};  // Aurora has acquired an image: the screen is the game's
bool g_bootScreenStarted = false;

}  // namespace
}  // namespace mp2dk

extern "C" const char* mp2nx_boot_stage(void);                            // load_boost.cpp
extern "C" uint64_t mp2nx_boot_ms(void);                                  // load_boost.cpp
extern "C" void mp2nx_boot_mark(const char* what, const char* stage);     // load_boost.cpp
extern "C" void aurora_pipeline_boot_progress(uint32_t* done, uint32_t* total);  // Aurora's pipeline_cache.cpp
extern "C" void mp2nx_boot_screen_draw(uint32_t* px, uint32_t w, uint32_t h);   // switch/source/platform/boot_screen.cpp

namespace mp2dk {
namespace {

void boot_screen_thread() {
    swdk::Block buf;
    uint32_t bw = 0, bh = 0;
    uint32_t frames = 0;
    int half = 0;
    const uint64_t startMs = mp2nx_boot_ms();
    for (;;) {
        const uint64_t ms = mp2nx_boot_ms();
        {
            std::lock_guard<std::mutex> lock(submit_mutex());
            if (g_gameScreen.load() || g_down || g_surface.count == 0) break;
            if (ms - startMs > 120000) {  // (a safety: two minutes)
                log("boot screen: still up after two minutes; it stops");
                break;
            }
            const uint32_t w = swdk::R.width, h = swdk::R.height;
            if (!buf || w != bw || h != bh) {
                if (buf) swdk::block_free_deferred(buf.block);
                buf = swdk::block_create(w * h * 4 * 2, "boot screen (two frames)");
                bw = w;
                bh = h;
                if (!buf) break;
            }
            // (two halves in turn: the CPU never draws into the one the last copy may still read)
            mp2nx_boot_screen_draw(reinterpret_cast<uint32_t*>(buf.cpu + size_t(half) * w * h * 4), w, h);
            flush_locked("the boot screen");
            const int slot = swdk::swapchain_acquire();
            DkImageView view;
            dkImageViewDefaults(&view, &swdk::swapchain_image(slot));
            const DkCopyBuf src = {buf.gpu + DkGpuAddr(half) * w * h * 4, 0, 0};
            const DkImageRect r = {0, 0, 0, w, h, 1};
            dkCmdBufCopyBufferToImage(swdk::R.cmd, &src, &view, &r, 0);
            dkCmdBufBarrier(swdk::R.cmd, DkBarrier_Full, 0);
            swdk::swapchain_present();
            half ^= 1;
            if (frames++ == 0) mp2nx_boot_mark("the boot screen's first frame", nullptr);
        }
        svcSleepThread(66'000'000ll);
    }
    log("boot screen: %u frames, ended at %llu ms", frames, (unsigned long long)mp2nx_boot_ms());
    if (buf) swdk::block_free_deferred(buf.block);
}

void start_boot_screen() {
    if (g_bootScreenStarted) return;
    g_bootScreenStarted = true;
    if (const char* e = getenv("MP2DK_BOOT_SCREEN"); e && *e == '0') {
        log("boot screen: off (MP2DK_BOOT_SCREEN=0)");
        return;
    }
    std::thread(boot_screen_thread).detach();
}

}  // namespace

bool on() noexcept { return g_on; }

void log(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    fprintf(stderr, "[deko3d] %s\n", line);
}

bool choose() {
    static bool decided = false;
    if (decided) return g_on;
    decided = true;
    remove(kOldChoicePath);
    if (FILE* f = fopen(kVulkanPath, "r")) {
        fclose(f);
        log("renderer: Vulkan (chosen in the menu)");
        return false;
    }
    const int failed = read_boot_count();
    if (failed >= 2) {
        log("the last %d starts with deko3d never showed a picture: Vulkan from now on (System > Renderer "
            "switches back)", failed);
        if (FILE* f = fopen(kVulkanPath, "w")) {
            fputs("vulkan\n", f);
            fclose(f);
        }
        remove(kBootPath);
        g_fellBack = true;
        return false;
    }
    write_boot_count(failed + 1);
    g_on = true;
    log("renderer: deko3d (the default); Aurora's device is Dawn's Null device");
    // (v0.5.0: deko3d_env.txt, the test builds' A/B settings file, is no longer read; every MP2DK_* setting is at
    // its default)
    return true;
}

void init(uint32_t gxConfigVersion) {
    mkdir(MP2NX_APP_DIR "/cache", 0777);
    mkdir(MP2NX_APP_DIR "/cache/glsl", 0777);
    swdk::device_init(gxConfigVersion);
    encoder_init();
}

void presented() {
    if (g_presented) return;
    g_presented = true;
    remove(kBootPath);
    log("first present: the boot marker is cleared");
}

int current_image() { return -1; }

}  // namespace mp2dk

using namespace mp2dk;

extern "C" {

// mp2-nx: Aurora's gpu.cpp
bool mp2nx_dk_choose(void) { return mp2dk::choose(); }
bool mp2nx_dk_active(void) { return mp2dk::on(); }
void mp2nx_dk_init(uint32_t gxConfigVersion) { mp2dk::init(gxConfigVersion); }
// the Switch menu: the renderer of this start, the choice for the next, and whether it fell back
bool mp2nx_dk_fell_back(void) { return g_fellBack; }
// Aurora's webgpu::shutdown, before it lets go of its objects: nothing is replayed or presented after this
// (the crash on quit: crash.txt "replay: begin render pass", a freed buffer in prepare_draw, after Aurora's
// "Device lost: Device was destroyed")
void mp2nx_dk_shutdown(void) {
    std::lock_guard<std::mutex> lock(mp2dk::submit_mutex());
    mp2dk::g_down = true;
    log("shutting down: no more submits or presents");
}
bool mp2nx_dk_chosen(void) {
    FILE* f = fopen(kVulkanPath, "r");
    if (!f) return true;
    fclose(f);
    return false;
}
void mp2nx_dk_set_chosen(bool on) {
    remove(kBootPath);
    if (on) {
        remove(kVulkanPath);
    } else if (FILE* f = fopen(kVulkanPath, "w")) {
        fputs("vulkan\n", f);
        fclose(f);
    }
}

// lib/deko's log lines
void aurora_dk_log(const char* line, size_t length) { fprintf(stderr, "[deko3d] %.*s\n", int(length), line); }

WGPUSurface __wrap_wgpuInstanceCreateSurface(WGPUInstance instance, WGPUSurfaceDescriptor const* d) {
    if (!on()) return __real_wgpuInstanceCreateSurface(instance, d);
    g_surface.refs.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<WGPUSurface>(&g_surface);
}

void __wrap_wgpuSurfaceAddRef(WGPUSurface s) {
    if (!is_ours(s)) return __real_wgpuSurfaceAddRef(s);
    g_surface.refs.fetch_add(1, std::memory_order_relaxed);
}

void __wrap_wgpuSurfaceRelease(WGPUSurface s) {
    if (!is_ours(s)) return __real_wgpuSurfaceRelease(s);
    g_surface.refs.fetch_sub(1, std::memory_order_relaxed);  // (static: lives as long as the process)
}

WGPUStatus __wrap_wgpuSurfaceGetCapabilities(WGPUSurface s, WGPUAdapter adapter, WGPUSurfaceCapabilities* caps) {
    if (!is_ours(s)) return __real_wgpuSurfaceGetCapabilities(s, adapter, caps);
    caps->usages = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst |
                   WGPUTextureUsage_TextureBinding;
    caps->formatCount = 1;
    caps->formats = kFormats;
    caps->presentModeCount = 1;
    caps->presentModes = kPresentModes;
    caps->alphaModeCount = 1;
    caps->alphaModes = kAlphaModes;
    return WGPUStatus_Success;
}

void __wrap_wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities caps) {
    if (on() && caps.formats == kFormats) return;  // (ours are static)
    __real_wgpuSurfaceCapabilitiesFreeMembers(caps);
}

void __wrap_wgpuSurfaceConfigure(WGPUSurface s, WGPUSurfaceConfiguration const* c) {
    if (!is_ours(s)) return __real_wgpuSurfaceConfigure(s, c);
    std::lock_guard<std::mutex> lock(submit_mutex());
    for (uint32_t i = 0; i < g_surface.count; i++) __wrap_wgpuTextureRelease(g_surface.textures[i]);
    g_surface.count = 0;
    g_surface.device = c->device;
    g_surface.width = c->width;
    g_surface.height = c->height;
    swdk::swapchain_configure(c->width, c->height);
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    static const char kLabel[] = "deko3d swapchain image";
    d.label = WGPUStringView{kLabel, sizeof kLabel - 1};
    d.usage = c->usage | WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst |
              WGPUTextureUsage_TextureBinding;
    d.size = {c->width, c->height, 1};
    d.format = WGPUTextureFormat_RGBA8Unorm;
    for (uint32_t i = 0; i < swdk::swapchain_images() && i < 4; i++) {
        WGPUTexture t = __real_wgpuDeviceCreateTexture(c->device, &d);
        aurora::gfx::dk::make_external_texture(t, d, swdk::swapchain_image(int(i)));
        g_surface.textures[g_surface.count++] = t;
    }
    log("surface: %ux%u (%u images), format RGBA8Unorm, FIFO", c->width, c->height, g_surface.count);
    if (!g_gameScreen.load()) start_boot_screen();
}

void __wrap_wgpuSurfaceGetCurrentTexture(WGPUSurface s, WGPUSurfaceTexture* out) {
    if (!is_ours(s)) return __real_wgpuSurfaceGetCurrentTexture(s, out);
    std::lock_guard<std::mutex> lock(submit_mutex());
    if (g_surface.count == 0) {
        out->texture = nullptr;
        out->status = WGPUSurfaceGetCurrentTextureStatus_Error;
        return;
    }
    if (!g_gameScreen.exchange(true)) mp2nx_boot_mark("the game's first frame (the boot screen ends)", nullptr);
    const int slot = swdk::swapchain_acquire();
    g_slot = slot;
    out->texture = g_surface.textures[slot % g_surface.count];
    __wrap_wgpuTextureAddRef(out->texture);  // (the caller's reference)
    out->status = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;
}

WGPUStatus __wrap_wgpuSurfacePresent(WGPUSurface s) {
    if (!is_ours(s)) return __real_wgpuSurfacePresent(s);
    if (mp2dk::g_down) return WGPUStatus_Success;
    {
        std::lock_guard<std::mutex> lock(submit_mutex());
        flush_locked("the present");
        if (g_slot >= 0) shot_take_locked(swdk::swapchain_image(g_slot), g_surface.width, g_surface.height);
        swdk::swapchain_present();
    }
    presented();
    cycle_tick();
    {
        std::lock_guard<std::mutex> lock(submit_mutex());
        capture_flush();
    }
    perf_tick();
    maps_poll();
    const uint64_t now = armGetSystemTick();
    if (g_reportAt == 0) g_reportAt = now;
    if (armTicksToNs(now - g_reportAt) > 60'000'000'000ull) {
        g_reportAt = now;
        encoder_report();
        pipelines_report();
    }
    return WGPUStatus_Success;
}

}  // extern "C"

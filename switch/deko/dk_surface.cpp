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

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

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
void encoder_init();
void encoder_report();
void cycle_tick();
void pipelines_report();
std::mutex& submit_mutex();
void flush_locked(const char* why);

namespace {

constexpr const char* kChoicePath = MP2NX_APP_DIR "/renderer_deko3d";
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
    FILE* f = fopen(kChoicePath, "r");
    if (!f) return false;
    fclose(f);
    const int failed = read_boot_count();
    if (failed >= 2) {
        log("the last %d starts with deko3d never showed a picture: back to Vulkan (the choice is reset)", failed);
        remove(kChoicePath);
        remove(kBootPath);
        g_fellBack = true;
        return false;
    }
    write_boot_count(failed + 1);
    g_on = true;
    log("renderer: deko3d (the test renderer, chosen in the menu); Aurora's device is Dawn's Null device");
    // mp2-nx (deko-11): <app folder>/deko3d_env.txt, NAME=VALUE lines set before Aurora reads them (for A/B
    // tests without a new build, e.g. AURORA_DUAL_SOURCE=0)
    if (FILE* env = fopen(MP2NX_APP_DIR "/deko3d_env.txt", "r")) {
        char line[256];
        while (fgets(line, sizeof line, env)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq || eq == line) continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            log("deko3d_env.txt: %s=%s", line, eq + 1);
        }
        fclose(env);
    }
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
bool mp2nx_dk_chosen(void) {
    FILE* f = fopen(kChoicePath, "r");
    if (!f) return false;
    fclose(f);
    return true;
}
void mp2nx_dk_set_chosen(bool on) {
    if (on) {
        if (FILE* f = fopen(kChoicePath, "w")) {
            fputs("deko3d\n", f);
            fclose(f);
        }
        remove(kBootPath);
    } else {
        remove(kChoicePath);
        remove(kBootPath);
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
}

void __wrap_wgpuSurfaceGetCurrentTexture(WGPUSurface s, WGPUSurfaceTexture* out) {
    if (!is_ours(s)) return __real_wgpuSurfaceGetCurrentTexture(s, out);
    std::lock_guard<std::mutex> lock(submit_mutex());
    if (g_surface.count == 0) {
        out->texture = nullptr;
        out->status = WGPUSurfaceGetCurrentTextureStatus_Error;
        return;
    }
    const int slot = swdk::swapchain_acquire();
    out->texture = g_surface.textures[slot % g_surface.count];
    __wrap_wgpuTextureAddRef(out->texture);  // (the caller's reference)
    out->status = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;
}

WGPUStatus __wrap_wgpuSurfacePresent(WGPUSurface s) {
    if (!is_ours(s)) return __real_wgpuSurfacePresent(s);
    {
        std::lock_guard<std::mutex> lock(submit_mutex());
        flush_locked("the present");
        swdk::swapchain_present();
    }
    presented();
    cycle_tick();
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

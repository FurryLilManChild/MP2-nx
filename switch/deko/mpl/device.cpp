// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWakerHD (https://github.com/centollOS/SwitchWakerHD) at df8fbde:
// runtime/src/gfx/deko/backend.cpp (device, queue, swapchain, present, frame fences, debug callback,
// queue checks, the set-up cost and the statistics).
// Changed for Dusklight Switch (aleks2 deko3d P1, from SwitchWaker 084291f switch/deko/device.cpp): the log
// goes to Aurora's (aurora_dk_log, dk_runtime.hpp); no host allocation scope; the device is created only
// when the deko3d renderer was chosen.
// Changed for mp2-nx (deko3d phase 1, docs/deko3d-phase1-design.md): Aurora draws its picture and ImGui
// through WebGPU like everything else, so this file keeps the device, the queue, the frame and the
// swapchain, and the present is three calls the surface wrappers make (dk_surface.cpp):
// swapchain_configure (the size Aurora asks for), swapchain_acquire (the image of this frame) and
// swapchain_present (the frame's fence, the submit and the present). The embedded ImGui and text
// shaders, the overlay, the test pattern and the window sizing of Dusklight's are not used.
#include <switch.h>
#include <unistd.h>

#include "dk.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" char* fake_heap_end;
extern "C" void aurora_dk_log(const char* line, size_t length);

namespace swdk {

Renderer R;
Conventions C;

namespace {

constexpr uint32_t kSwapImages = 3;
#ifdef COS_DK_DEBUG_LIB
constexpr bool kDebugLib = true;
#else
constexpr bool kDebugLib = false;
#endif

DkSwapchain g_swapchain = nullptr;
DkImage g_swapImages[kSwapImages];
ImageAlloc g_swapMem[kSwapImages];
int g_debugMessages = 0;

size_t heap_never_used_mib() {
    char* top = static_cast<char*>(sbrk(0));
    if (!fake_heap_end || !top || top == reinterpret_cast<char*>(-1)) return 0;
    return size_t(fake_heap_end - top) >> 20;
}

void debug_message(void*, const char* context, DkResult result, const char* message) {
    if (result != DkResult_Success) {
        fatal("deko3d error in %s: result %d: %s (frame %llu)", context ? context : "?", int(result),
              message ? message : "", (unsigned long long)R.frame + 1);
    }
    const int n = g_debugMessages++;
    if (n < 200) {
        dklog("deko3d %s: %s", context ? context : "?", message ? message : "");
    } else if (n == 200) {
        dklog("deko3d: more messages, not logged");
    }
}

void init_image(DkImage& image, ImageAlloc& mem, DkImageFormat format, uint32_t flags, const char* what) {
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.flags = flags;
    m.format = format;
    m.dimensions[0] = R.width;
    m.dimensions[1] = R.height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    mem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&image, &layout, mem.block, mem.offset);
    dklog("%s: %ux%u, %u KiB at image heap offset 0x%X", what, R.width, R.height, mem.size >> 10, mem.offset);
}

void create_swapchain() {
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) {
        char what[48];
        snprintf(what, sizeof what, "swapchain image %u (RGBA8)", i);
        init_image(g_swapImages[i], g_swapMem[i], DkImageFormat_RGBA8_Unorm,
                   DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression |
                       DkImageFlags_Usage2DEngine,
                   what);
        images[i] = &g_swapImages[i];
    }
    DkSwapchainMaker m;
    dkSwapchainMakerDefaults(&m, R.device, nwindowGetDefault(), images, kSwapImages);
    dklog("creating the swapchain on the default window");
    g_swapchain = dkSwapchainCreate(&m);
    dkSwapchainSetSwapInterval(g_swapchain, 1);
    dklog("swapchain: %u RGBA8 images of %ux%u, swap interval 1", kSwapImages, R.width, R.height);
}

// ---- statistics every 30 s
struct Times {
    uint64_t presents = 0, fenceNs = 0, acquireNs = 0, submitNs = 0, maxNs = 0;
    uint64_t start = 0;
} g_times;
SubmitStats g_submits;
uint64_t g_submitsAtStats = 0;
uint64_t g_frameOpenNs = 0;
int g_acquired = -1;

void frame_stats(uint64_t frameNs) {
    g_times.presents++;
    g_times.maxNs = frameNs > g_times.maxNs ? frameNs : g_times.maxNs;
    const uint64_t now = now_ns();
    if (g_times.start == 0) g_times.start = now;
    if (now - g_times.start < 30'000'000'000ull) return;
    const double n = double(g_times.presents);
    const MemoryStats m = memory_stats_take();
    const DescriptorStats d = descriptor_stats();
    dklog("frames %llu-%llu: %.1f presents/s; per present: frame fence %.2f ms, acquire %.2f ms, submit+present "
          "%.2f ms, longest frame %.2f ms, %.1f submits; command memory max %llu KiB/frame (%llu overflows), stream "
          "max %llu KiB/frame (%llu full); image heap %llu KiB in %llu chunks, blocks %llu KiB, shader code %llu KiB; "
          "descriptors: %u images, %u samplers in use; deko3d messages %d; heap never used %zu MiB",
          (unsigned long long)(R.frame - g_times.presents + 1), (unsigned long long)R.frame,
          n * 1e9 / double(now - g_times.start), double(g_times.fenceNs) / n / 1e6, double(g_times.acquireNs) / n / 1e6,
          double(g_times.submitNs) / n / 1e6, double(g_times.maxNs) / 1e6,
          double(g_submits.submits - g_submitsAtStats) / n, (unsigned long long)(m.cmdBytesMax >> 10),
          (unsigned long long)m.cmdOverflows, (unsigned long long)(m.streamBytesMax >> 10),
          (unsigned long long)m.streamFull, (unsigned long long)(m.imageBytes >> 10),
          (unsigned long long)m.imageChunks, (unsigned long long)(m.blockBytes >> 10),
          (unsigned long long)(m.codeBytes >> 10), d.imagesUsed, d.samplersUsed, g_debugMessages,
          heap_never_used_mib());
    g_submitsAtStats = g_submits.submits;
    g_times = Times{};
    g_times.start = now;
}

// a 1x1 black texture in kEmptyImage: what a draw samples where its view is missing
bool g_emptyReady = false;
DkImage g_empty;
ImageAlloc g_emptyMem;
void empty_image_init() {
    if (g_emptyReady) return;
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = 1;
    m.dimensions[1] = 1;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    StreamAlloc s = stream_alloc(4, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    if (!s) return;
    g_emptyMem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&g_empty, &layout, g_emptyMem.block, g_emptyMem.offset);
    memset(s.cpu, 0, 4);
    static_cast<uint8_t*>(s.cpu)[3] = 0xFF;
    DkImageView view;
    dkImageViewDefaults(&view, &g_empty);
    const DkCopyBuf src = {s.gpu, 0, 0};
    const DkImageRect r = {0, 0, 0, 1, 1, 1};
    dkCmdBufCopyBufferToImage(R.cmd, &src, &view, &r, 0);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    write_image_descriptor(kEmptyImage, g_empty);
    g_emptyReady = true;
}

void submit_list() {
    check_queue("a submit");
    const DkCmdList list = dkCmdBufFinishList(R.cmd);
    std::lock_guard<std::mutex> lock(fence_mutex());
    dkQueueSubmitCommands(R.queue, list);
    fence_submitted();
    g_submits.submits++;
}

}  // namespace

void objects_frame_start_hook();  // dk_objects.cpp's slot retirement (dk_surface.cpp)

void frame_open() {
    if (R.frameOpen) return;
    const uint64_t frame = R.frame + 1;
    const uint64_t t0 = now_ns();
    check_queue("the frame fence");
    frame_begin(frame);
    g_times.fenceNs += now_ns() - t0;
    g_frameOpenNs = now_ns();
    descriptors_frame_start();
    objects_frame_start_hook();
    empty_image_init();
    shaders_frame_start();
    R.frameOpen = true;
}

void submit(const char* why) {
    (void)why;
    if (!R.frameOpen) return;
    submit_list();
}

SubmitStats submit_stats() { return g_submits; }

// ---- the swapchain (dk_surface.cpp, under the encoder's submit lock)
void swapchain_configure(uint32_t w, uint32_t h) {
    if (g_swapchain && w == R.width && h == R.height) return;
    if (g_swapchain) {
        check_queue("recreating the swapchain");
        dkQueueWaitIdle(R.queue);
        dkSwapchainDestroy(g_swapchain);
        g_swapchain = nullptr;
        for (uint32_t i = 0; i < kSwapImages; i++) image_free_later(g_swapMem[i]);
        g_acquired = -1;
    }
    R.width = w;
    R.height = h;
    create_swapchain();
}

int swapchain_acquire() {
    if (g_acquired >= 0) return g_acquired;
    frame_open();
    const uint64_t t0 = now_ns();
    check_queue("acquiring a swapchain image");
    g_acquired = dkQueueAcquireImage(R.queue, g_swapchain);
    g_times.acquireNs += now_ns() - t0;
    return g_acquired;
}

const DkImage& swapchain_image(int slot) { return g_swapImages[slot]; }
uint32_t swapchain_images() { return kSwapImages; }

void swapchain_present() {
    if (g_acquired < 0) return;
    frame_open();
    const uint64_t frame = R.frame + 1;
    const uint64_t t0 = now_ns();
    frame_end();
    submit_list();
    check_queue("the present");
    dkQueuePresentImage(R.queue, g_swapchain, g_acquired);
    g_acquired = -1;
    R.frame = frame;
    R.frameOpen = false;
    g_submits.frames++;
    g_times.submitNs += now_ns() - t0;
    if (frame == 1) dklog("first present; heap never used %zu MiB", heap_never_used_mib());
    frame_stats(now_ns() - g_frameOpenNs);
}

bool env_flag(const char* name, bool def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}

long env_long(const char* name, long def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    char* end = nullptr;
    const long v = strtol(e, &end, 0);
    return end && end != e ? v : def;
}

void dklog(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    aurora_dk_log(line, size_t(n) < sizeof line ? size_t(n) : sizeof line - 1);
}

void log_flush() {}
void host_alloc_thread() {}

void fatal(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    aurora_dk_log(line, strlen(line));
    abort();
}

void check_queue(const char* before) {
    if (!dkQueueIsInErrorState(R.queue)) return;
    fatal("the deko3d queue is in an error state (a GPU fault) before %s of frame %llu; deko3d messages %d", before,
          (unsigned long long)R.frame + 1, g_debugMessages);
}

// The device, the queue, memory, the shader caches and the uam worker (dk_surface.cpp, from Aurora's
// webgpu::initialize once the Null device exists). The swapchain comes with the surface's configuration.
void device_init(uint32_t gxConfigVersion) {
    const uint64_t t0 = now_ns();
    const size_t heapBefore = heap_never_used_mib();
    dklog("deko3d renderer (mp2-nx, docs/deko3d-phase1-design.md): %s library; heap never used before the set-up "
          "%zu MiB",
          kDebugLib ? "debug" : "release", heapBefore);
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_message;
    dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
    dklog("creating the device");
    R.device = dkDeviceCreate(&dm);
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, R.device);
    R.zcull = env_flag("COS_DK_ZCULL", false);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio |
               (R.zcull ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull);
    qm.commandMemorySize = kQueueCommandMemory;
    qm.flushThreshold = qm.commandMemorySize / 8;
    dklog("creating the queue");
    R.queue = dkQueueCreate(&qm);
    memory_init();
    shader_cache_init(gxConfigVersion);
    shaders_init(gxConfigVersion);
    const size_t after = heap_never_used_mib();
    dklog("set-up cost: %zu MiB of heap (never used: %zu MiB before, %zu MiB after), %.0f ms",
          heapBefore > after ? heapBefore - after : 0, heapBefore, after, double(now_ns() - t0) / 1e6);
}

void device_shutdown() {
    if (R.queue == nullptr) return;
    dkQueueWaitIdle(R.queue);
    if (g_swapchain != nullptr) {
        dkSwapchainDestroy(g_swapchain);
        g_swapchain = nullptr;
    }
    dklog("shut down after %llu frames", (unsigned long long)R.frame);
}

}  // namespace swdk

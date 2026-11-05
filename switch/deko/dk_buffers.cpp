// mp2-nx deko3d renderer: buffers and their mapping (dk_bridge.hpp). MIT.
//
// Every WebGPU buffer Aurora creates on the Null device gets deko3d memory of its size: CPU-uncached
// and GPU-cached for most (the staging buffers Aurora writes through mappings, the device buffers the
// GPU reads), CPU-cached for map-read buffers (read-backs: the CPU reads them, after invalidating its
// cache). GetMappedRange hands out that memory itself, so a mapped write lands where the GPU reads and
// a read-back reads what the GPU wrote, with no copies.
//
// Mapping is this renderer's: Dawn's MapAsync never sees the buffer (its Null device would complete it
// at once, before the GPU is done). A map waits for the last submit that used the buffer, then calls
// the callback (from MapAsync itself when that submit is already finished, else from ProcessEvents or
// WaitAny once it is: Aurora asks for AllowSpontaneous callbacks).
#include "dk_bridge.hpp"

#include <switch.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>

extern "C" {
WGPUBuffer __real_wgpuDeviceCreateBuffer(WGPUDevice device, WGPUBufferDescriptor const* descriptor);
void __real_wgpuBufferAddRef(WGPUBuffer buffer);
void __real_wgpuBufferRelease(WGPUBuffer buffer);
void __real_wgpuBufferDestroy(WGPUBuffer buffer);
void* __real_wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t size);
void const* __real_wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size);
WGPUFuture __real_wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size,
                                     WGPUBufferMapCallbackInfo callbackInfo);
void __real_wgpuBufferUnmap(WGPUBuffer buffer);
void __real_wgpuInstanceProcessEvents(WGPUInstance instance);
WGPUWaitStatus __real_wgpuInstanceWaitAny(WGPUInstance instance, size_t futureCount, WGPUFutureWaitInfo* futures,
                                          uint64_t timeoutNS);
}

namespace mp2dk {
namespace {

std::shared_mutex g_mutex;
std::unordered_map<const void*, Buf*> g_buffers;
uint64_t g_bytes = 0, g_count = 0;

// futures of this renderer's maps carry this bit (Dawn's count up from 1)
constexpr uint64_t kFutureTag = 1ull << 62;

struct PendingMap {
    Buf* buf = nullptr;
    uint64_t serial = 0;
    uint64_t future = 0;
    WGPUMapMode mode = WGPUMapMode_None;
    size_t offset = 0, size = 0;
    WGPUBufferMapCallbackInfo cb{};
};
std::mutex g_mapMutex;
std::deque<PendingMap> g_maps;
uint64_t g_nextFuture = 1;
uint64_t g_lastCompleted = 0;  // futures below this have completed (they complete in order)

Buf* lookup(const void* h) {
    if (!h) return nullptr;
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    auto it = g_buffers.find(h);
    return it == g_buffers.end() ? nullptr : it->second;
}

void destroy(Buf* b) {
    if (b->mem) swdk::block_free_deferred(b->mem.block);
    {
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        g_bytes -= b->mem.size;
        g_count--;
    }
    delete b;
}

void complete(PendingMap& m) {
    if (m.buf->cached && (m.mode & WGPUMapMode_Read)) {
        // the GPU wrote it: drop what the CPU's cache holds of it
        armDCacheFlush(m.buf->mem.cpu + m.offset, std::max<size_t>(m.size, 1));
    }
    if (m.cb.callback) {
        static const char kOk[] = "";
        m.cb.callback(WGPUMapAsyncStatus_Success, WGPUStringView{kOk, 0}, m.cb.userdata1, m.cb.userdata2);
    }
    buffer_release(m.buf);
}

}  // namespace

Buf* find_buffer(WGPUBuffer b) { return lookup(b); }
void buffer_retain(Buf* b) { b->refs.fetch_add(1, std::memory_order_relaxed); }
void buffer_release(Buf* b) {
    if (b->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    {
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_buffers.find(b->handle);
        if (it != g_buffers.end() && it->second == b) g_buffers.erase(it);
    }
    destroy(b);
}

void maps_poll() {
    for (;;) {
        PendingMap m;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            if (g_maps.empty() || !serial_done(g_maps.front().serial)) return;
            m = g_maps.front();
            g_maps.pop_front();
            g_lastCompleted = m.future & ~kFutureTag;
        }
        complete(m);  // (outside the lock: the callback may map again)
    }
}

bool map_future(uint64_t id, bool* completed) {
    if (!(id & kFutureTag)) return false;
    std::lock_guard<std::mutex> lock(g_mapMutex);
    *completed = (id & ~kFutureTag) <= g_lastCompleted;
    return true;
}

}  // namespace mp2dk

using namespace mp2dk;

extern "C" {

WGPUBuffer __wrap_wgpuDeviceCreateBuffer(WGPUDevice device, WGPUBufferDescriptor const* d) {
    WGPUBuffer b = __real_wgpuDeviceCreateBuffer(device, d);
    if (!b || !d || !on() || !swdk::R.device) return b;
    auto* buf = new Buf;
    buf->handle = b;
    buf->size = d->size;
    buf->usage = d->usage;
    buf->cached = (d->usage & WGPUBufferUsage_MapRead) != 0;
    buf->unmapDawn = d->mappedAtCreation;
    buf->ui = d->label.data && std::string_view(d->label.data, d->label.length == WGPU_STRLEN ? strlen(d->label.data)
                                                                                                : d->label.length)
                                       .find("ImGui") != std::string_view::npos;
    char what[96];
    snprintf(what, sizeof what, "buffer %.*s (%llu bytes)", int(d->label.data ? std::min<size_t>(d->label.length, 48) : 0),
             d->label.data ? d->label.data : "", (unsigned long long)d->size);
    buf->mem = buf->cached ? swdk::block_create_cached(uint32_t(std::max<uint64_t>(d->size, 4)), what)
                           : swdk::block_create(uint32_t(std::max<uint64_t>(d->size, 4)), what);
    if (!buf->mem) log("buffers: no deko3d memory for %s", what);
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_buffers[b] = buf;
    g_bytes += buf->mem.size;
    g_count++;
    return b;
}

void __wrap_wgpuBufferAddRef(WGPUBuffer buffer) {
    if (on())
        if (Buf* b = lookup(buffer)) buffer_retain(b);
    __real_wgpuBufferAddRef(buffer);
}

void __wrap_wgpuBufferRelease(WGPUBuffer buffer) {
    if (on())
        if (Buf* b = lookup(buffer)) buffer_release(b);
    __real_wgpuBufferRelease(buffer);
}

void __wrap_wgpuBufferDestroy(WGPUBuffer buffer) {
    // the memory goes with the last reference (a pending submit may still use it)
    __real_wgpuBufferDestroy(buffer);
}

void* __wrap_wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
    if (!on()) return __real_wgpuBufferGetMappedRange(buffer, offset, size);
    Buf* b = lookup(buffer);
    if (!b || !b->mem || offset > b->size) return nullptr;
    return b->mem.cpu + offset;
}

void const* __wrap_wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
    if (!on()) return __real_wgpuBufferGetConstMappedRange(buffer, offset, size);
    Buf* b = lookup(buffer);
    if (!b || !b->mem || offset > b->size) return nullptr;
    return b->mem.cpu + offset;
}

WGPUFuture __wrap_wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size,
                                     WGPUBufferMapCallbackInfo cb) {
    if (!on()) return __real_wgpuBufferMapAsync(buffer, mode, offset, size, cb);
    Buf* b = lookup(buffer);
    PendingMap m;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        m.future = kFutureTag | g_nextFuture++;
    }
    if (!b || !b->mem) {
        if (cb.callback) {
            static const char kWhy[] = "no deko3d shadow";
            cb.callback(WGPUMapAsyncStatus_Error, WGPUStringView{kWhy, sizeof kWhy - 1}, cb.userdata1, cb.userdata2);
        }
        std::lock_guard<std::mutex> lock(g_mapMutex);
        g_lastCompleted = std::max(g_lastCompleted, m.future & ~kFutureTag);
        return WGPUFuture{m.future};
    }
    buffer_retain(b);
    m.buf = b;
    m.serial = b->lastUse.load(std::memory_order_acquire);
    m.mode = mode;
    m.offset = offset;
    m.size = size == WGPU_WHOLE_MAP_SIZE ? size_t(b->size - std::min<uint64_t>(offset, b->size)) : size;
    m.cb = cb;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        g_maps.push_back(m);
    }
    maps_poll();  // already done: the callback now
    return WGPUFuture{m.future};
}

void __wrap_wgpuBufferUnmap(WGPUBuffer buffer) {
    if (!on()) return __real_wgpuBufferUnmap(buffer);
    Buf* b = lookup(buffer);
    if (b && b->unmapDawn) {
        b->unmapDawn = false;
        __real_wgpuBufferUnmap(buffer);  // Dawn's mapping at creation
    }
}

void __wrap_wgpuInstanceProcessEvents(WGPUInstance instance) {
    __real_wgpuInstanceProcessEvents(instance);
    if (on()) maps_poll();
}

WGPUWaitStatus __wrap_wgpuInstanceWaitAny(WGPUInstance instance, size_t count, WGPUFutureWaitInfo* futures,
                                          uint64_t timeoutNS) {
    if (!on() || count == 0 || !futures) return __real_wgpuInstanceWaitAny(instance, count, futures, timeoutNS);
    bool ours = false;
    for (size_t i = 0; i < count; i++) {
        bool done = false;
        if (map_future(futures[i].future.id, &done)) ours = true;
    }
    if (!ours) return __real_wgpuInstanceWaitAny(instance, count, futures, timeoutNS);
    // this renderer's maps: wait for their submits (up to the timeout), then report
    const uint64_t start = armGetSystemTick();
    for (;;) {
        maps_poll();
        bool any = false;
        for (size_t i = 0; i < count; i++) {
            bool done = false;
            if (map_future(futures[i].future.id, &done) && done) {
                futures[i].completed = true;
                any = true;
            }
        }
        if (any) return WGPUWaitStatus_Success;
        const uint64_t ns = armTicksToNs(armGetSystemTick() - start);
        if (timeoutNS == 0 || ns >= timeoutNS) return WGPUWaitStatus_TimedOut;
        // the oldest pending map's submit, waited for (a bounded wait, then poll again)
        uint64_t serial = 0;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            if (!g_maps.empty()) serial = g_maps.front().serial;
        }
        if (serial) serial_wait(serial);
        else svcSleepThread(100'000);
    }
}

}  // extern "C"

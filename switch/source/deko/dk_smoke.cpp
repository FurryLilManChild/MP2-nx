// mp2-nx deko3d phase 0 (docs/deko3d-plan.md): a smoke test of the deko3d toolchain on the console,
// while the game keeps drawing with Vulkan. Chosen once in the Switch menu (System > deko3d test),
// it runs on the next start, on its own thread:
//   1. creates a deko3d device and queue next to Dawn/NVK (two GPU clients in one process),
//   2. translates a fixed test shader WGSL -> GLSL (Tint, SwitchWaker's shader_translate) -> DKSH
//      (uam, built for the console) and loads it into deko3d code memory,
//   3. does the same for the first GX shaders Aurora generates for this game (handed over by the
//      tap in gx/shader.cpp), timing each step, so the log answers how long uam takes per real
//      Echoes shader against NVK's ~500 ms,
//   4. destroys everything and logs a summary ("[deko3d test]" lines; the menu shows it too).
// A marker file is written first and removed at the end: a start that finds it (the test crashed
// or hung the last time) skips the test and says so.
#include "shader_translate.h"
#include "uam_api.h"

#include <deko3d.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aurora::gx {
extern void (*g_mp2nxShaderTap)(const std::string& wgsl);  // gx/shader.cpp (mp2-nx)
}

namespace {

constexpr const char* kMarker = "sdmc:/switch/mp2-nx/deko3d_test_running";
constexpr int kMaxGxShaders = 40;
constexpr uint32_t kCodeSize = 8u << 20;

std::mutex sQueueMutex;
std::condition_variable sQueueCv;
std::deque<std::string> sQueue;
std::atomic<int> sOffered{0};
std::atomic<bool> sRunning{false};
std::mutex sSummaryMutex;
std::string sSummary;
Thread sThread;

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[deko3d test] %s\n", line);
}

void set_summary(const std::string& text) {
  std::lock_guard lock{sSummaryMutex};
  sSummary = text;
}

void tap(const std::string& wgsl) {
  if (!sRunning.load(std::memory_order_relaxed) || sOffered.fetch_add(1) >= kMaxGxShaders) {
    return;
  }
  {
    std::lock_guard lock{sQueueMutex};
    sQueue.push_back(wgsl);
  }
  sQueueCv.notify_one();
}

// DKSH (uam's output): a header and control section of control_sz bytes, then code_sz bytes of code.
struct DkshHeader {
  uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
};

struct Loader {
  DkMemBlock code = nullptr;
  uint32_t used = 0;
  std::vector<std::vector<uint8_t>> controls;  // kept alive while the shaders are
  // Loads a whole DKSH into code memory; false when it does not fit or is malformed.
  bool load(const std::vector<uint8_t>& dksh, DkShader& out) {
    if (dksh.size() < sizeof(DkshHeader)) return false;
    DkshHeader h;
    std::memcpy(&h, dksh.data(), sizeof(h));
    if (h.control_sz > dksh.size() || dksh.size() - h.control_sz < h.code_sz) return false;
    const uint32_t offset = (used + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    if (offset + h.code_sz + DK_SHADER_CODE_UNUSABLE_SIZE > kCodeSize) return false;
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(code)) + offset, dksh.data() + h.control_sz, h.code_sz);
    controls.emplace_back(dksh.begin(), dksh.begin() + h.control_sz);
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, code, offset);
    maker.control = controls.back().data();
    dkShaderInitialize(&out, &maker);
    used = offset + h.code_sz;
    return dkShaderIsValid(&out);
  }
};

struct Stats {
  int ok = 0, failed = 0;
  double translateMs = 0, uamMs = 0, worstMs = 0;
};

// One entry point: WGSL -> GLSL -> DKSH -> loaded. Logs failures with their reason.
bool compile_stage(Loader& loader, Stats& stats, const std::string& wgsl, const char* entry, swdk::ShaderStage stage,
                   swdk::BindingScheme scheme, const char* what) {
  const auto t0 = Clock::now();
  const swdk::TranslateResult tr = swdk::wgsl_to_glsl(wgsl, entry, stage, scheme);
  const double translateMs = ms_since(t0);
  if (!tr.ok) {
    ++stats.failed;
    log("%s %s: Tint GLSL failed: %.300s", what, entry, tr.error.c_str());
    return false;
  }
  const auto t1 = Clock::now();
  const uam::Result ur =
      uam::compile(stage == swdk::ShaderStage::Vertex ? uam::Stage::Vertex : uam::Stage::Fragment, tr.glsl.c_str());
  const double uamMs = ms_since(t1);
  if (!ur.ok) {
    ++stats.failed;
    log("%s %s: uam failed: %.300s", what, entry, ur.log.c_str());
    return false;
  }
  DkShader shader;
  if (!loader.load(ur.dksh, shader)) {
    ++stats.failed;
    log("%s %s: DKSH (%zu bytes) did not load", what, entry, ur.dksh.size());
    return false;
  }
  ++stats.ok;
  stats.translateMs += translateMs;
  stats.uamMs += uamMs;
  stats.worstMs = std::max(stats.worstMs, translateMs + uamMs);
  log("%s %s: GLSL %.1f ms, uam %.1f ms, DKSH %zu bytes", what, entry, translateMs, uamMs, ur.dksh.size());
  return true;
}

constexpr const char* kTestWgsl = R"(
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs_main(@builtin(vertex_index) i: u32) -> VOut {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  return VOut(vec4f(p * 2.0 - 1.0, 0.0, 1.0), p);
}
@fragment fn fs_main(v: VOut) -> @location(0) vec4f { return vec4f(v.uv, 0.5, 1.0); }
)";

void run(void*) {
  const auto start = Clock::now();
  if (FILE* f = std::fopen(kMarker, "w")) std::fclose(f);
  log("start");

  DkDeviceMaker dm;
  dkDeviceMakerDefaults(&dm);
  dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
  const auto td = Clock::now();
  DkDevice device = dkDeviceCreate(&dm);
  if (device == nullptr) {
    log("dkDeviceCreate failed");
    set_summary("deko3d test: the device could not be created");
    std::remove(kMarker);
    sRunning = false;
    return;
  }
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, device, kCodeSize);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
  Loader loader;
  loader.code = dkMemBlockCreate(&mm);
  DkQueueMaker qm;
  dkQueueMakerDefaults(&qm, device);
  qm.flags = DkQueueFlags_Graphics;
  DkQueue queue = dkQueueCreate(&qm);
  dkQueueWaitIdle(queue);
  log("device, 8 MiB code block and queue in %.1f ms (Vulkan keeps drawing)", ms_since(td));

  swdk::translate_init();
  const auto ti = Clock::now();
  uam::init(true);
  log("uam ready in %.1f ms", ms_since(ti));

  Stats fixed;
  compile_stage(loader, fixed, kTestWgsl, "vs_main", swdk::ShaderStage::Vertex, swdk::BindingScheme::Sequential,
                "test shader");
  compile_stage(loader, fixed, kTestWgsl, "fs_main", swdk::ShaderStage::Fragment, swdk::BindingScheme::Sequential,
                "test shader");

  // The game's own GX shaders, as Aurora generates them (the first kMaxGxShaders after the start).
  Stats gx;
  int index = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(90);
  while (index < kMaxGxShaders && Clock::now() < deadline) {
    std::string wgsl;
    {
      std::unique_lock lock{sQueueMutex};
      if (!sQueueCv.wait_until(lock, deadline, [] { return !sQueue.empty(); })) break;
      wgsl = std::move(sQueue.front());
      sQueue.pop_front();
    }
    char what[32];
    std::snprintf(what, sizeof(what), "GX shader %d", ++index);
    compile_stage(loader, gx, wgsl, "vs_main", swdk::ShaderStage::Vertex, swdk::BindingScheme::GX, what);
    compile_stage(loader, gx, wgsl, "fs_main", swdk::ShaderStage::Fragment, swdk::BindingScheme::GX, what);
  }
  sRunning = false;

  dkQueueWaitIdle(queue);
  dkQueueDestroy(queue);
  dkMemBlockDestroy(loader.code);
  dkDeviceDestroy(device);
  uam::shutdown();
  std::remove(kMarker);

  const int stages = gx.ok + gx.failed;
  char summary[256];
  if (gx.ok > 0) {
    std::snprintf(summary, sizeof(summary),
                  "deko3d test passed: %d of %d GX shader stages compiled, average %.0f ms (Tint %.0f + uam %.0f), "
                  "worst %.0f ms",
                  gx.ok, stages, (gx.translateMs + gx.uamMs) / gx.ok, gx.translateMs / gx.ok, gx.uamMs / gx.ok,
                  gx.worstMs);
  } else {
    std::snprintf(summary, sizeof(summary), "deko3d test: test shader %s, %d GX shader stages compiled of %d",
                  fixed.ok == 2 ? "OK" : "FAILED", gx.ok, stages);
  }
  log("%s; whole test %.1f s", summary, ms_since(start) / 1000.0);
  set_summary(summary);
}

}  // namespace

// The menu's first frame (switch_menu.cpp). want: the setting asked for the test on this start.
// Returns a line for the menu to show when the last test did not finish.
extern "C" const char* mp2nx_dk_smoke_start(int want) {
  static bool started = false;
  if (started) return nullptr;
  started = true;
  if (FILE* f = std::fopen(kMarker, "r")) {
    std::fclose(f);
    std::remove(kMarker);
    log("the last deko3d test did not finish (crash or hang); skipped");
    return "The last deko3d test did not finish, so it was skipped";
  }
  if (!want) return nullptr;
  sRunning = true;
  aurora::gx::g_mp2nxShaderTap = tap;
  // Mesa's GLSL parser and nv50_ir recurse deeply: 8 MiB of stack (uam_api.h). Priority 0x2C (the
  // game's), core 2 (Aurora's compile and render threads), so it does not crowd the game thread.
  if (R_FAILED(threadCreate(&sThread, run, nullptr, nullptr, 8u << 20, 0x2C, 2)) || R_FAILED(threadStart(&sThread))) {
    log("could not start the test thread");
    sRunning = false;
    return "The deko3d test could not start";
  }
  return nullptr;
}

extern "C" bool mp2nx_dk_smoke_summary(char* out, size_t size) {
  std::lock_guard lock{sSummaryMutex};
  if (sSummary.empty() || size == 0) return false;
  std::snprintf(out, size, "%s", sSummary.c_str());
  sSummary.clear();
  return true;
}

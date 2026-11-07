// mp2-nx: more CPU for shader preparation while the game is loading, after Alek's Dusklight
// (aurora-aleks pipeline_cache.cpp:1099 and Dusklight shader_warmup_runtime.cpp, CC0/MIT).
// Reasons (bits), any of which turns the compile thread boost on (it may also use core 0, at the
// game's priority; aurora_shader_set_compile_boost):
//   0  boot: the title screen while the stored pipeline list is prepared again
//   1  CMFGameLoader: the black loading screen before the game starts
//   2  CWorldTransManager: an elevator or portal ride while the next world loads
//   3  startup: from the process start (startup.cpp) to the Switch menu's first frame (the black
//      screen and the boot screen: the pipeline warm-up and the disc). FastLoad only: the compile
//      thread stays off core 0, where the game thread opens the disc.
// Reason 1 also sets the CPU boost mode FastLoad (CPU up, GPU down: the screen is black there).
// Alek measured each pipeline taking about 5x longer without it. The Switch menu sets reason 0
// on its first frame; CMFGameLoader clears it.
#include <switch.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

extern "C" void aurora_shader_set_compile_boost(uint32_t on);

namespace {
std::atomic<uint32_t> sReasons{0};
constexpr uint32_t kFastLoadReasons = (1u << 1) | (1u << 3);
constexpr uint32_t kCompileBoostReasons = (1u << 0) | (1u << 1) | (1u << 2);
// mp2-nx (the boot-time build): the time zero of the "[boot]" lines, the first call (startup.cpp's
// userAppInit, which libnx runs before the static constructors and main)
uint64_t process_start() {
  static const uint64_t start = armGetSystemTick();
  return start;
}
std::atomic<const char*> g_bootStage{"Starting"};
}  // namespace

extern "C" uint64_t mp2nx_boot_ms(void) {
  const uint64_t start = process_start();
  return armTicksToNs(armGetSystemTick() - start) / 1000000ull;
}

// A boot milestone: logged with the time since the process start; `stage` (null: unchanged) is what the
// deko3d boot screen shows on its top line (dk_surface.cpp).
extern "C" void mp2nx_boot_mark(const char* what, const char* stage) {
  if (stage != nullptr) {
    g_bootStage.store(stage, std::memory_order_release);
  }
  std::fprintf(stderr, "[boot] %s at %llu ms\n", what, static_cast<unsigned long long>(mp2nx_boot_ms()));
}

extern "C" const char* mp2nx_boot_stage(void) { return g_bootStage.load(std::memory_order_acquire); }

extern "C" void mp2nx_load_boost(int reason, int on) {
  const uint32_t bit = 1u << reason;
  const uint32_t prev = on ? sReasons.fetch_or(bit) : sReasons.fetch_and(~bit);
  const uint32_t next = on ? (prev | bit) : (prev & ~bit);
  if (prev == next) {
    return;
  }
  const bool compileWas = (prev & kCompileBoostReasons) != 0;
  const bool compileNow = (next & kCompileBoostReasons) != 0;
  if (compileWas != compileNow) {
    aurora_shader_set_compile_boost(compileNow ? 1u : 0u);
  }
  const bool fastWas = (prev & kFastLoadReasons) != 0;
  const bool fastNow = (next & kFastLoadReasons) != 0;
  if (fastWas != fastNow) {
    appletSetCpuBoostMode(fastNow ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  }
  std::fprintf(stderr, "mp2-nx: load boost %s (reasons 0x%x) at %llu ms\n", next != 0 ? "on" : "off", next,
               static_cast<unsigned long long>(mp2nx_boot_ms()));
}

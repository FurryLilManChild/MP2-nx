// mp2-nx: more CPU for shader preparation while the game is loading, after Alek's Dusklight
// (aurora-aleks pipeline_cache.cpp:1099 and Dusklight shader_warmup_runtime.cpp, CC0/MIT).
// Reasons (bits), any of which turns the compile thread boost on (it may also use core 0, at the
// game's priority; aurora_shader_set_compile_boost):
//   0  boot: the title screen while the stored pipeline list is prepared again
//   1  CMFGameLoader: the black loading screen before the game starts
//   2  CWorldTransManager: an elevator or portal ride while the next world loads
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
constexpr uint32_t kFastLoadReasons = 1u << 1;
}  // namespace

extern "C" void mp2nx_load_boost(int reason, int on) {
  const uint32_t bit = 1u << reason;
  const uint32_t prev = on ? sReasons.fetch_or(bit) : sReasons.fetch_and(~bit);
  const uint32_t next = on ? (prev | bit) : (prev & ~bit);
  if (prev == next) {
    return;
  }
  if ((prev != 0) != (next != 0)) {
    aurora_shader_set_compile_boost(next != 0 ? 1u : 0u);
  }
  const bool fastWas = (prev & kFastLoadReasons) != 0;
  const bool fastNow = (next & kFastLoadReasons) != 0;
  if (fastWas != fastNow) {
    appletSetCpuBoostMode(fastNow ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  }
  std::fprintf(stderr, "mp2-nx: load boost %s (reasons 0x%x)\n", next != 0 ? "on" : "off", next);
}

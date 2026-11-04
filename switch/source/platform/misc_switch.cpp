// mp2-nx: small Horizon fixes from Alek's Dusklight Switch layer (CC0), each noted below.
#include <switch.h>

#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <thread>

extern "C" char __start__;  // libnx's start of the program image

// abort(): newlib's runs __appExit, which unmounts the SD card while other threads still use it,
// so the real cause was followed by a second, misleading crash and no log line (Dusklight
// switch_abort.cpp). Log the caller, then a panic break: Atmosphere writes a full crash report
// to sdmc:/atmosphere/crash_reports.
extern "C" [[noreturn]] void __wrap_abort(void) {
  const auto caller = reinterpret_cast<unsigned long long>(__builtin_return_address(0));
  const auto base = reinterpret_cast<unsigned long long>(&__start__);
  std::fprintf(stderr, "mp2-nx: abort() called from 0x%llx (program +0x%llx)\n", caller, caller - base);
  std::fflush(stdout);
  std::fflush(stderr);
  svcBreak(BreakReason_Panic, 0, 0);
  svcExitProcess();
  __builtin_unreachable();
}

// fchmod: libnx answers ENOSYS, which made std::filesystem::copy_file fail after copying every
// byte (Dusklight file_copy.hpp). Permissions mean nothing on FAT.
extern "C" int __wrap_fchmod(int, mode_t) { return 0; }

// Logs: line-buffered stdout and stderr made every log line an SD card write on whichever thread
// logged, and one write can stall 50 to 150 ms (Dusklight logging.cpp). Once the game is drawing,
// the logs are fully buffered and a background thread flushes them every 200 ms. Started by the
// Switch menu's first frame, so anything before it (and a crash at boot) is still written line by
// line.
extern "C" void mp2nx_start_log_flusher(void) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) {
    return;
  }
  std::fflush(stdout);
  std::fflush(stderr);
  std::setvbuf(stdout, nullptr, _IOFBF, 64 * 1024);
  std::setvbuf(stderr, nullptr, _IOFBF, 64 * 1024);
  std::thread([] {
    for (;;) {
      svcSleepThread(200'000'000LL);
      std::fflush(stdout);
      std::fflush(stderr);
    }
  }).detach();
}

// Suspension (HOME menu, sleep, a system dialog). The owner (test7 logs): an 83-second "no frame" stall
// and an 84-second "pipeline compile" were the console asleep or at the HOME menu, and they also
// went into the compile totals. Applet hooks run on the thread that calls appletMainLoop (SDL's
// event pump); each focus change or resume bumps an epoch, and the 3 s after one count as
// suspended too, so a watchdog tick that wakes first does not see a stall.
namespace {
AppletHookCookie sFocusCookie;
std::atomic<uint32_t> sSuspendEpoch{0};
std::atomic<uint64_t> sLastAppletEventTick{0};
void on_applet_event(AppletHookType hook, void*) {
  if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnResume ||
      hook == AppletHookType_OnOperationMode) {
    sSuspendEpoch.fetch_add(1, std::memory_order_relaxed);
    sLastAppletEventTick.store(armGetSystemTick(), std::memory_order_relaxed);
  }
}
}  // namespace

extern "C" void mp2nx_install_suspend_hook(void) { appletHook(&sFocusCookie, on_applet_event, nullptr); }

extern "C" uint32_t mp2nx_switch_suspend_epoch(void) { return sSuspendEpoch.load(std::memory_order_relaxed); }

extern "C" bool mp2nx_switch_suspended(void) {
  if (appletGetFocusState() != AppletFocusState_InFocus) {
    return true;
  }
  const uint64_t last = sLastAppletEventTick.load(std::memory_order_relaxed);
  return last != 0 && armGetSystemTick() - last < armNsToTicks(3'000'000'000ULL);
}

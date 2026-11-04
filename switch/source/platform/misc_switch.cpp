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

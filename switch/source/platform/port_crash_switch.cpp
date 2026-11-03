// mp2-nx: Echoes Unbound's crash reporter (platform/port_crash.cpp) is built on POSIX
// signals, ucontext and dladdr, none of which Horizon has. CPU exceptions are reported by
// the libnx handler in startup.cpp instead; this file keeps the same API so the rest of
// the port is unchanged, and turns an uncaught C++ exception into a log line and a clean
// exit (abort() would bring up Atmosphere's fatal screen).
#include "port_crash.h"

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <exception>
#include <string>
#include <typeinfo>

namespace PortCrash {
namespace {
void (*sAfterReport)(int, const void*) = nullptr;

[[noreturn]] void log_and_exit(const char* where) {
  std::string what = "not a std::exception";
  if (std::current_exception()) {
    try {
      throw;
    } catch (const std::exception& e) {
      what = e.what();
    } catch (...) {
    }
  }
  const char* type = "unknown type";
  char* demangled = nullptr;
  if (const std::type_info* info = abi::__cxa_current_exception_type(); info != nullptr) {
    int status = 0;
    demangled = abi::__cxa_demangle(info->name(), nullptr, nullptr, &status);
    type = demangled != nullptr ? demangled : info->name();
  }
  std::fprintf(stderr, "port: uncaught C++ exception in %s: %s: %s\n", where, type, what.c_str());
  std::fflush(stdout);
  std::fflush(stderr);
  if (sAfterReport != nullptr) {
    sAfterReport(0, nullptr);
  }
  svcExitProcess();
  __builtin_unreachable();
}
}  // namespace

void Install(const std::string& dumpFolder) {
  (void)dumpFolder;
  std::set_terminate([] { log_and_exit("std::terminate"); });
}

void SetAfterReport(void (*afterReport)(int signal, const void* address)) { sAfterReport = afterReport; }

bool RequestStack(long threadId) {
  (void)threadId;
  return false;  // no way to interrupt another thread for its stack on Horizon
}

void AbortOnException(const char* where) { log_and_exit(where); }

}  // namespace PortCrash

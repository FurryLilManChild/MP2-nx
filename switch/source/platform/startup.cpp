// mp2-nx: how the program starts and stops on Horizon.
//
// libnx runs userAppInit before main() and userAppExit after it, and reads a few
// symbols the program may define (__nx_applet_type, __nx_heap_size). This file defines
// them, opens the logs, points Echoes Unbound at its folders through the environment
// variables it already reads (MP2_USER_PATH, MP2_CACHE_PATH, MP2_DISC), and installs a
// crash handler that writes straight to the SD card.
#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../include/mp2nx_app.h"
#define MP2NX_DIR MP2NX_APP_DIR

// SQLite for the pipeline and Dawn caches on Horizon (sqlite_switch.cpp).
extern "C" void mp2nx_sqlite_setup(void);
extern "C" void mp2nx_install_suspend_hook(void);
extern "C" void mp2nx_load_boost(int reason, int on);                 // load_boost.cpp
extern "C" void mp2nx_boot_mark(const char* what, const char* stage);  // load_boost.cpp
extern "C" const char mp2nx_build_id[];                                 // <build>/mp2nx_build_id.c

#include <cerrno>
#include <ctime>

extern "C" {

// A full application (title takeover): the Album applet gets a fraction of the memory.
u32 __nx_applet_type = AppletType_Application;
// 0: libnx gives the heap everything the system leaves over.
size_t __nx_heap_size = 0;

// libnx's (weak, not in its headers). 1 makes its applet teardown end an application the way AM
// expects (ISelfController::Exit, then wait to be ended). Set only in userAppExit. Adapted from
// Alek's Dusklight switch_startup.cpp (V1-rc12, CC0): a bare svcExitProcess makes HOME show
// "The software was closed", and returning to hbloader with threads alive or code memory
// remapped (our REL arena, low_arena.cpp) crashed the loader (2001-0106) on his console.
u32 __nx_applet_exit_mode = 0;

}  // extern "C"

// Ends the process through AM (ISelfController::Exit); the kernel ends it if AM has not within 10 s.
extern "C" [[noreturn]] void mp2nx_end_process(void) {
  std::fflush(stdout);  // (the log flusher writes every 200 ms; this keeps the last lines)
  std::fflush(stderr);
  const AppletType type = appletGetAppletType();
  if (type == AppletType_Application || type == AppletType_SystemApplication) {
    Service* self = appletGetServiceSession_SelfController();
    if (self != nullptr && serviceIsActive(self) && R_SUCCEEDED(serviceDispatch(self, 0))) {
      for (int i = 0; i < 100; ++i) {
        svcSleepThread(100'000'000LL);
      }
    }
  }
  svcExitProcess();
  __builtin_unreachable();
}

namespace {
LoaderReturnFn sLoaderReturn = nullptr;
}

namespace {

// The log status (the build-stamp build). The owner (2026-11-08): two runs of the A/B build left logs that the
// A/B build cannot have written, and the old open_logs could not say why: it ignored every failure, so a
// log it could not replace stayed on the SD card looking like this run's. Now every step is checked; a
// log that cannot be replaced is deleted, a stream that cannot open its usual file goes to a fallback
// file, the result is on the boot screen and in the Switch menu (System > Logs), and last-run.txt
// (written and closed at once) records the build and each step even if the streams fail later.
char sLogStatus[160] = "not opened";
bool sLogsOk = false;
char sLogSteps[1024] = "";

void log_step(const char* what, int rc) {
  const size_t used = std::strlen(sLogSteps);
  if (used + 2 >= sizeof sLogSteps) return;
  if (rc == 0) {
    std::snprintf(sLogSteps + used, sizeof sLogSteps - used, "  %s: ok\n", what);
  } else {
    const int e = errno;
    std::snprintf(sLogSteps + used, sizeof sLogSteps - used, "  %s: FAILED (errno %d, %s)\n", what, e,
                  std::strerror(e));
  }
}

bool exists(const char* path) {
  struct stat st{};
  return stat(path, &st) == 0;
}

// Keeps the previous run's log as *.prev.log; a log that cannot be moved is deleted, so it cannot pass
// for this run's.
void rotate(const char* cur, const char* prev, const char* name) {
  char what[96];
  if (exists(prev)) {
    std::snprintf(what, sizeof what, "delete the old %s.prev.log", name);
    log_step(what, remove(prev));
  }
  if (exists(cur)) {
    std::snprintf(what, sizeof what, "keep the last %s.log as .prev.log", name);
    log_step(what, rename(cur, prev));
    if (exists(cur)) {
      std::snprintf(what, sizeof what, "delete the last %s.log (it could not be moved)", name);
      log_step(what, remove(cur));
    }
  }
}

// Points a stream at its log, or at the fallbacks; returns the file used, or nullptr.
const char* open_stream(FILE* stream, const char* const* paths, const char* name) {
  for (const char* const* p = paths; *p != nullptr; ++p) {
    char what[160];
    std::snprintf(what, sizeof what, "open %s for %s", *p, name);
    if (std::freopen(*p, "w", stream) != nullptr) {
      log_step(what, 0);
      std::setvbuf(stream, nullptr, _IOLBF, 0);
      return *p;
    }
    log_step(what, -1);
  }
  return nullptr;
}

// One file per stream: Horizon refuses a second write handle on an open file, so a
// shared file would silently lose one of them.
void open_logs() {
  log_step("make " MP2NX_DIR, mkdir(MP2NX_DIR, 0777) == 0 || errno == EEXIST ? 0 : -1);
  log_step("make " MP2NX_DIR "/logs", mkdir(MP2NX_DIR "/logs", 0777) == 0 || errno == EEXIST ? 0 : -1);
  rotate(MP2NX_DIR "/logs/mp2-nx.log", MP2NX_DIR "/logs/mp2-nx.prev.log", "mp2-nx");
  rotate(MP2NX_DIR "/logs/mp2-nx.err.log", MP2NX_DIR "/logs/mp2-nx.err.prev.log", "mp2-nx.err");
  static const char* const kOut[] = {MP2NX_DIR "/logs/mp2-nx.log", MP2NX_DIR "/mp2-nx.log",
                                     "sdmc:/" MP2NX_APP_NAME ".log", nullptr};
  static const char* const kErr[] = {MP2NX_DIR "/logs/mp2-nx.err.log", MP2NX_DIR "/mp2-nx.err.log",
                                     "sdmc:/" MP2NX_APP_NAME ".err.log", nullptr};
  const char* out = open_stream(stdout, kOut, "stdout");
  const char* err = open_stream(stderr, kErr, "stderr");
  sLogsOk = out == kOut[0] && err == kErr[0];
  if (sLogsOk) {
    std::snprintf(sLogStatus, sizeof sLogStatus, "OK (" MP2NX_DIR "/logs)");
  } else if (err != nullptr) {
    std::snprintf(sLogStatus, sizeof sLogStatus, "PROBLEM: the error log went to %s", err);
  } else {
    std::snprintf(sLogStatus, sizeof sLogStatus, "FAILED: no log file could be opened");
  }

  char when[64] = "unknown";
  const time_t now = time(nullptr);
  if (const tm* t = localtime(&now)) {
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S (Switch clock)", t);
  }
  // first lines of both logs: which build wrote them, and when
  for (FILE* f : {stdout, stderr}) {
    std::fprintf(f, "mp2-nx: %s; this run started %s\n", mp2nx_build_id, when);
    std::fprintf(f, "mp2-nx: logs %s\n", sLogStatus);
  }
  std::fprintf(stderr, "mp2-nx: the log steps:\n%s", sLogSteps);
  std::fflush(stdout);
  std::fflush(stderr);

  // last-run.txt: written and closed now, in the logs folder or the app folder
  for (const char* path : {MP2NX_DIR "/logs/last-run.txt", MP2NX_DIR "/last-run.txt"}) {
    if (FILE* f = std::fopen(path, "w")) {
      std::fprintf(f, "%s\nthis run started %s\nlogs: %s\nstdout: %s\nstderr: %s\nsteps:\n%s", mp2nx_build_id,
                   when, sLogStatus, out ? out : "(none)", err ? err : "(none)", sLogSteps);
      std::fclose(f);
      break;
    }
  }
}

bool has_suffix(const char* name, const char* suffix) {
  const size_t n = std::strlen(name);
  const size_t s = std::strlen(suffix);
  return n >= s && strcasecmp(name + n - s, suffix) == 0;
}

// The disc image in the app folder: an .iso or .gcm first, then .rvz, .wia, .gcz, .ciso.
std::string find_disc_in(const char* folder) {
  static const char* const kOrder[] = {".iso", ".gcm", ".rvz", ".wia", ".gcz", ".ciso"};
  for (const char* ext : kOrder) {
    DIR* dir = opendir(folder);
    if (dir == nullptr) {
      return {};
    }
    while (dirent* e = readdir(dir)) {
      if (e->d_name[0] != '.' && has_suffix(e->d_name, ext)) {
        std::string path = std::string{folder} + "/" + e->d_name;
        closedir(dir);
        return path;
      }
    }
    closedir(dir);
  }
  return {};
}

// The app's own folder first; a test build (MP2NX_APP_NAME other than mp2-nx) then uses the release's
// disc, so it needs no second copy on the SD card.
std::string find_disc() {
  std::string disc = find_disc_in(MP2NX_DIR);
  if (disc.empty() && std::strcmp(MP2NX_DIR, MP2NX_MAIN_DIR) != 0) {
    disc = find_disc_in(MP2NX_MAIN_DIR);
  }
  return disc;
}

void show_message_and_exit(const char* text) {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  std::printf("\n  Metroid Prime 2: Echoes (mp2-nx)\n\n%s\n\n  Press + to exit.\n", text);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
      break;
    }
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
  std::exit(0);
}

}  // namespace

// For the boot screen and the Switch menu (System > Logs).
extern "C" const char* mp2nx_log_status(void) { return sLogStatus; }
extern "C" bool mp2nx_logs_ok(void) { return sLogsOk; }

extern "C" void userAppInit(void) {
  romfsInit();
  // NVK refuses to start without this (Alek's Switch layer sets it the same way).
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
  nvInitialize();  // the GPU service the Vulkan driver talks to
  open_logs();
  mp2nx_boot_mark("userAppInit (the logs are open)", nullptr);

  const AppletType type = appletGetAppletType();
  u64 total = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  std::printf("mp2-nx: applet type %d, %llu MiB of memory\n", static_cast<int>(type),
              static_cast<unsigned long long>(total >> 20));
  if (type != AppletType_Application && type != AppletType_SystemApplication) {
    show_message_and_exit(
        "  This needs the memory of a full application.\n"
        "  Start it with title takeover: hold R while opening any game\n"
        "  from the HOME menu, then pick " MP2NX_APP_NAME ". Not from the Album.");
  }

  // mp2-nx (the boot-time build): the CPU at 1785 MHz from here to the Switch menu's first frame
  // (load_boost.cpp reason 3). The log of 2026-11-06 had the pipeline warm-up at stock 1020 MHz: the
  // boost only started at CMFGameLoader, after 10 s of black screen.
  mp2nx_load_boost(3, 1);
  mp2nx_sqlite_setup();
  mp2nx_install_suspend_hook(); // misc_switch.cpp
  // Not SDL_AUDIO_DEVICE_SAMPLE_FRAMES=4096 (Dusklight's crackle fix): Echoes keeps only 64 ms
  // queued (platform/ai_dma.cpp kTargetQueuedBytes) and a 4096-frame device pull is about 85 ms,
  // so it ran dry on every pull (test6c: choppy audio throughout).

  mkdir(MP2NX_DIR "/user", 0777);
  mkdir(MP2NX_DIR "/cache", 0777);
  if (std::getenv("MP2_USER_PATH") == nullptr) {
    setenv("MP2_USER_PATH", MP2NX_DIR "/user", 1);
  }
  if (std::getenv("MP2_CACHE_PATH") == nullptr) {
    setenv("MP2_CACHE_PATH", MP2NX_DIR "/cache", 1);
  }
  const std::string disc = find_disc();
  if (disc.empty()) {
    show_message_and_exit(
        "  No disc image found.\n"
        "  Copy your Metroid Prime 2: Echoes (USA) image (.iso or .rvz)\n"
        "  into " MP2NX_DIR "/ next to " MP2NX_APP_NAME ".nro.");
  }
  setenv("MP2_DISC", disc.c_str(), 1);
  std::printf("mp2-nx: disc %s\n", disc.c_str());
}

extern "C" void userAppExit(void) {
  // Always back to HOME: the REL arena's code-memory mappings and any thread Echoes, Dawn or
  // Aurora left running would crash hbloader on a return to it.
  std::fprintf(stderr, "mp2-nx: exit: ending the process through AM (back to HOME)\n");
  std::fflush(stdout);
  std::fflush(stderr);
  nvExit();
  romfsExit();
  sLoaderReturn = envGetExitFuncPtr();
  __nx_applet_exit_mode = 1;
}

// libnx's own is weak: the same steps. With __nx_applet_exit_mode 1, appletExit (in __appExit)
// points the exit function at libnx's _appletExitProcess (ISelfController::Exit). If the applet
// teardown did not run, the exit function is still the loader's: end the process ourselves.
extern "C" void __appExit(void);
extern "C" void NX_NORETURN __nx_exit(Result rc, LoaderReturnFn retaddr);
extern "C" void NX_NORETURN __libnx_exit(int rc) {
  (void)rc;
  __appExit();
  if (envGetExitFuncPtr() == sLoaderReturn) {
    svcExitProcess();
  }
  __nx_exit(0, envGetExitFuncPtr());
}

// ---------------------------------------------------------------------------------
// Crash handler. libnx calls __libnx_exception_handler on its own small stack for any
// CPU exception. It writes through fsFileWrite only (no malloc, no stdio), so a crash
// inside malloc or under a stdio lock still gets logged. Addresses are also given as
// offsets from the start of the program, which addr2line resolves against the ELF.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
extern char __start__;  // libnx's start of the program image
}

namespace {
int put_hex(char* out, u64 v) {
  static const char kHex[] = "0123456789abcdef";
  char tmp[16];
  int n = 0;
  do {
    tmp[n++] = kHex[v & 0xF];
    v >>= 4;
  } while (v != 0);
  out[0] = '0';
  out[1] = 'x';
  for (int i = 0; i < n; ++i) {
    out[2 + i] = tmp[n - 1 - i];
  }
  return n + 2;
}
int put_str(char* out, const char* s) {
  int n = 0;
  while (s[n] != '\0') {
    out[n] = s[n];
    ++n;
  }
  return n;
}
}  // namespace

// What the deko3d renderer was doing (switch/deko/dk_encoder.cpp), printed by the handler; null with Vulkan.
extern "C" {
const char* volatile mp2nx_crash_note = nullptr;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  char buf[2048];
  int n = 0;
  // the program's load address: the start of the code mapping this handler is in (&__start__ came out as
  // 0 in deko-3 and deko-4, so every "program +" offset was the raw address)
  u64 base = reinterpret_cast<u64>(&__start__);
  {
    MemoryInfo mi{};
    u32 pageInfo = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pageInfo, reinterpret_cast<u64>(&__libnx_exception_handler)))) {
      base = mi.addr;
    }
  }
  const auto line = [&](const char* label, u64 v, bool offset) {
    n += put_str(buf + n, label);
    n += put_hex(buf + n, v);
    if (offset && v >= base && v - base < 0x10000000ull) {
      n += put_str(buf + n, " (program +");
      n += put_hex(buf + n, v - base);
      n += put_str(buf + n, ")");
    }
    buf[n++] = '\n';
  };
  n += put_str(buf + n, "mp2-nx crash\n");
  if (const char* note = mp2nx_crash_note) {
    n += put_str(buf + n, "renderer: ");
    for (int i = 0; note[i] != '\0' && i < 120; ++i) buf[n++] = note[i];
    buf[n++] = '\n';
  }
  line("error_desc ", ctx->error_desc, false);
  line("pc  ", ctx->pc.x, true);
  line("lr  ", ctx->lr.x, true);
  line("sp  ", ctx->sp.x, false);
  line("far ", ctx->far.x, false);
  for (int i = 0; i < 29 && n < static_cast<int>(sizeof(buf)) - 64; ++i) {
    char label[8] = {'x', static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), ' ', 0};
    line(label, ctx->cpu_gprs[i].x, true);
  }
  // A short stack scan: words that point into the program are likely return addresses.
  n += put_str(buf + n, "stack:\n");
  const u64* sp = reinterpret_cast<const u64*>(ctx->sp.x);
  for (int i = 0, shown = 0; i < 256 && shown < 24 && n < static_cast<int>(sizeof(buf)) - 64; ++i) {
    const u64 v = sp[i];
    if (v >= base && v - base < 0x10000000ull) {
      line("  ", v, true);
      ++shown;
    }
  }

  FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
  if (sd != nullptr) {
    static char path[FS_MAX_PATH] = "/switch/" MP2NX_APP_NAME "/logs/crash.txt";
    fsFsDeleteFile(sd, path);
    // (created at its final size: a write past the end of a file opened without Append fails, which
    // left crash.txt empty until test deko-2)
    fsFsCreateFile(sd, path, static_cast<s64>(n), 0);
    FsFile f;
    if (R_SUCCEEDED(fsFsOpenFile(sd, path, FsOpenMode_Write, &f))) {
      fsFileWrite(&f, 0, buf, static_cast<u64>(n), FsWriteOption_Flush);
      fsFileClose(&f);
    }
  }
  svcExitProcess();
}

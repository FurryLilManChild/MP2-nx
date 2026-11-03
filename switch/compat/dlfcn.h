// mp2-nx: Horizon has no dynamic loader. Echoes Unbound uses dladdr only to name
// frames in its crash report; here it always fails, so the report keeps raw addresses
// (the Switch crash handler in switch/source/platform/startup.cpp gives program offsets).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  const char* dli_fname;
  void* dli_fbase;
  const char* dli_sname;
  void* dli_saddr;
} Dl_info;
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void*)0)
static inline int dladdr(const void* addr, Dl_info* info) { (void)addr; (void)info; return 0; }
static inline void* dlopen(const char* file, int mode) { (void)file; (void)mode; return 0; }
static inline void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return 0; }
static inline int dlclose(void* handle) { (void)handle; return 0; }
static inline const char* dlerror(void) { return "no dynamic loading on Horizon"; }
#ifdef __cplusplus
}
#endif

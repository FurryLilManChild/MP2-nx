// mp2-nx: no Linux system calls on Horizon. SYS_gettid is the only one Echoes Unbound
// asks for (its watchdog names the main thread); it maps to the kernel's thread id.
#pragma once
#include <switch.h>
#define SYS_gettid 186
#ifdef __cplusplus
extern "C" {
#endif
static inline long syscall(long number, ...) {
  if (number == SYS_gettid) {
    u64 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return (long)id;
  }
  return -1;
}
#ifdef __cplusplus
}
#endif

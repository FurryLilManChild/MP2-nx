// mp2-nx: two libnx pthread gaps that break ordinary C++ (both noted in the port base skill).
//  - pthread_detach returns ENOSYS, so std::thread::detach throws, and the still-joinable
//    std::thread then calls std::terminate. Detaching is reported as done; the thread runs on
//    and its resources are released when it exits.
//  - Threads get 128 KiB stacks by default. Game and Aurora code uses far bigger frames, so a
//    thread created without attributes gets 1 MiB.
// Linked with -Wl,--wrap=pthread_detach,--wrap=pthread_create.
#include <pthread.h>

extern "C" {
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
  if (attr != nullptr) {
    return __real_pthread_create(thread, attr, start, arg);
  }
  pthread_attr_t big;
  pthread_attr_init(&big);
  pthread_attr_setstacksize(&big, 1u << 20);
  const int rc = __real_pthread_create(thread, &big, start, arg);
  pthread_attr_destroy(&big);
  return rc;
}

int __wrap_pthread_detach(pthread_t thread) {
  (void)thread;
  return 0;
}
}

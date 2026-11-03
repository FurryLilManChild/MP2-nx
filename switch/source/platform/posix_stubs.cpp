// mp2-nx: POSIX calls that newlib on Horizon declares but libnx does not implement, used
// by code paths that cannot run on the console anyway (spawning a shell, terminal
// queries, file ownership). Each fails the way a POSIX system would when the call is
// not supported, so the callers take their existing error paths.
#include <cerrno>
#include <sys/types.h>

extern "C" {

int pipe(int fds[2]) {
  (void)fds;
  errno = ENOSYS;
  return -1;
}

pid_t waitpid(pid_t pid, int* status, int options) {
  (void)pid;
  (void)status;
  (void)options;
  errno = ECHILD;
  return -1;
}

int execvp(const char* file, char* const argv[]) {
  (void)file;
  (void)argv;
  errno = ENOSYS;
  return -1;
}

char* ttyname(int fd) {
  (void)fd;
  errno = ENOTTY;
  return nullptr;
}

uid_t geteuid(void) { return 0; }

int fchown(int fd, uid_t owner, gid_t group) {
  (void)fd;
  (void)owner;
  (void)group;
  return 0;
}

// Advisory locks: only this process ever opens the files.
int flock(int fd, int operation) {
  (void)fd;
  (void)operation;
  return 0;
}

}  // extern "C"

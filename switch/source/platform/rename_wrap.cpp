// mp2-nx: rename() that replaces an existing file, as POSIX requires and the port assumes.
// The SD card's FAT (through libnx's fsdev) refuses a rename onto an existing file, which broke
// every "write a temp file, rename it over the old one" save in the port: the memory card
// (test2: "Failed to write 8192 bytes to card"; test3: the bad save could not be moved into
// Card A/_deleted, "Failed to delete file at idx: 0"), settings, bindings and save states.
// Linked with -Wl,--wrap=rename, so std::filesystem::rename, std::rename and SDL_RenamePath
// all come here. The old file is moved aside first and removed only once the new one is in
// place, so a failure part way leaves the old file where it was.
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

extern "C" int __real_rename(const char* from, const char* to);

extern "C" int __wrap_rename(const char* from, const char* to) {
  if (__real_rename(from, to) == 0) {
    return 0;
  }
  const int firstErrno = errno;
  struct stat target{};
  struct stat source{};
  if (from == nullptr || to == nullptr || stat(to, &target) != 0 || stat(from, &source) != 0) {
    errno = firstErrno;
    std::fprintf(stderr, "mp2-nx: rename %s -> %s failed (errno %d)\n", from ? from : "?", to ? to : "?",
                 firstErrno);
    return -1;
  }
  if (S_ISDIR(target.st_mode)) {
    // POSIX replaces only an empty directory.
    if (rmdir(to) == 0 && __real_rename(from, to) == 0) {
      return 0;
    }
  } else {
    const std::string aside = std::string(to) + ".mp2nx-old";
    unlink(aside.c_str());
    if (__real_rename(to, aside.c_str()) == 0) {
      if (__real_rename(from, to) == 0) {
        unlink(aside.c_str());
        return 0;
      }
      const int moveErrno = errno;
      __real_rename(aside.c_str(), to);
      errno = moveErrno;
    }
  }
  const int err = errno;
  std::fprintf(stderr, "mp2-nx: rename %s -> %s failed with the target in place (errno %d, first %d)\n", from, to,
               err, firstErrno);
  errno = err;
  return -1;
}

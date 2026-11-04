// mp2-nx: SQLite on Horizon, for Aurora's two caches (the pipeline list and Dawn's compiled
// shaders). Adapted from Alek's Switch layer (aurora-aleks-nx lib/sqlite_switch_paths.cpp and
// lib/sqlite_cache_open.cpp, MIT), which found and measured all of this on the console:
//  - SQLite treats a path not starting with '/' as relative and prepends getcwd(); every path
//    here starts with a mount ("sdmc:/..."), so the caches answered "unable to open database
//    file". A copy of the "unix-none" VFS (unix without locking: libnx has no fcntl locks) with
//    xFullPathname replaced passes mount-qualified paths through, and is made the default.
//  - libnx file semantics differ from POSIX: a read starting past the end fails with EIO
//    (SQLite then reports the database as malformed), a write past the end leaves the gap
//    unfilled, and stat() of a file this process has open fails. SQLite's syscall table is
//    patched to clamp reads, zero-fill gaps, and report a busy file as a plain file.
#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {
sqlite3_vfs* g_base = nullptr;
sqlite3_vfs g_vfs{};

bool is_mount_qualified(const char* path) {
  if (path == nullptr) {
    return false;
  }
  const char* colon = std::strchr(path, ':');
  if (colon == nullptr || colon == path || colon[1] != '/') {
    return false;
  }
  for (const char* c = path; c < colon; ++c) {
    const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                    *c == '-' || *c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

int full_pathname(sqlite3_vfs* vfs, const char* path, int outLen, char* out) {
  if (!is_mount_qualified(path)) {
    return g_base->xFullPathname(vfs, path, outLen, out);
  }
  const size_t length = std::strlen(path);
  if (outLen <= 0 || length + 1 > static_cast<size_t>(outLen)) {
    return SQLITE_CANTOPEN;
  }
  std::memcpy(out, path, length + 1);
  return SQLITE_OK;
}

using ReadFn = ssize_t (*)(int, void*, size_t);
using WriteFn = ssize_t (*)(int, const void*, size_t);
using PreadFn = ssize_t (*)(int, void*, size_t, off_t);
using PwriteFn = ssize_t (*)(int, const void*, size_t, off_t);
using FstatFn = int (*)(int, struct stat*);
using StatFn = int (*)(const char*, struct stat*);
sqlite3_syscall_ptr g_realRead = nullptr;
sqlite3_syscall_ptr g_realWrite = nullptr;
sqlite3_syscall_ptr g_realPread = nullptr;
sqlite3_syscall_ptr g_realPwrite = nullptr;
sqlite3_syscall_ptr g_realFstat = nullptr;
sqlite3_syscall_ptr g_realStat = nullptr;

off_t file_size(int fd) {
  struct stat st{};
  return reinterpret_cast<FstatFn>(g_realFstat)(fd, &st) == 0 ? st.st_size : -1;
}

template <typename WriteAt>
bool fill_zeros(off_t from, off_t to, WriteAt write_at) {
  static const char zeros[4096] = {};
  while (from < to) {
    const size_t n = static_cast<size_t>(std::min<off_t>(to - from, sizeof(zeros)));
    const ssize_t w = write_at(zeros, n, from);
    if (w <= 0) {
      return false;
    }
    from += w;
  }
  return true;
}

ssize_t shim_pread(int fd, void* buf, size_t n, off_t off) {
  const off_t size = file_size(fd);
  if (size >= 0) {
    if (off >= size) {
      return 0;
    }
    if (static_cast<off_t>(off + n) > size) {
      n = static_cast<size_t>(size - off);
    }
  }
  return reinterpret_cast<PreadFn>(g_realPread)(fd, buf, n, off);
}

ssize_t shim_pwrite(int fd, const void* buf, size_t n, off_t off) {
  const off_t size = file_size(fd);
  if (size >= 0 && off > size) {
    fill_zeros(size, off, [fd](const void* b, size_t len, off_t at) {
      return reinterpret_cast<PwriteFn>(g_realPwrite)(fd, b, len, at);
    });
  }
  return reinterpret_cast<PwriteFn>(g_realPwrite)(fd, buf, n, off);
}

ssize_t shim_read(int fd, void* buf, size_t n) {
  const off_t off = ::lseek(fd, 0, SEEK_CUR);
  const off_t size = file_size(fd);
  if (off >= 0 && size >= 0) {
    if (off >= size) {
      return 0;
    }
    if (static_cast<off_t>(off + n) > size) {
      n = static_cast<size_t>(size - off);
    }
  }
  return reinterpret_cast<ReadFn>(g_realRead)(fd, buf, n);
}

ssize_t shim_write(int fd, const void* buf, size_t n) {
  const off_t off = ::lseek(fd, 0, SEEK_CUR);
  const off_t size = file_size(fd);
  if (off >= 0 && size >= 0 && off > size) {
    fill_zeros(size, off, [fd](const void* b, size_t len, off_t at) {
      if (::lseek(fd, at, SEEK_SET) != at) {
        return static_cast<ssize_t>(-1);
      }
      return reinterpret_cast<WriteFn>(g_realWrite)(fd, b, len);
    });
    ::lseek(fd, off, SEEK_SET);
  }
  return reinterpret_cast<WriteFn>(g_realWrite)(fd, buf, n);
}

int busy_tolerant_stat(const char* path, struct stat* out) {
  const int rc = reinterpret_cast<StatFn>(g_realStat)(path, out);
  if (rc == 0 || errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) {
    return rc;
  }
  std::memset(out, 0, sizeof(*out));
  out->st_mode = S_IFREG | 0644;
  out->st_nlink = 1;
  return 0;
}

void swap_in(sqlite3_vfs* vfs, const char* name, sqlite3_syscall_ptr& real, sqlite3_syscall_ptr shim) {
  if (real != nullptr) {
    return;
  }
  real = vfs->xGetSystemCall(vfs, name);
  if (real != nullptr) {
    vfs->xSetSystemCall(vfs, name, shim);
  }
}
}  // namespace

extern "C" void mp2nx_sqlite_setup(void) {
  g_base = sqlite3_vfs_find("unix-none");
  if (g_base == nullptr || g_base->iVersion < 3 || g_base->xSetSystemCall == nullptr) {
    std::fprintf(stderr, "mp2-nx: SQLite has no usable unix-none VFS; the caches will not open\n");
    return;
  }
  g_realFstat = g_base->xGetSystemCall(g_base, "fstat");
  if (g_realFstat != nullptr) {
    swap_in(g_base, "read", g_realRead, reinterpret_cast<sqlite3_syscall_ptr>(shim_read));
    swap_in(g_base, "write", g_realWrite, reinterpret_cast<sqlite3_syscall_ptr>(shim_write));
    swap_in(g_base, "pread", g_realPread, reinterpret_cast<sqlite3_syscall_ptr>(shim_pread));
    swap_in(g_base, "pwrite", g_realPwrite, reinterpret_cast<sqlite3_syscall_ptr>(shim_pwrite));
  }
  swap_in(g_base, "stat", g_realStat, reinterpret_cast<sqlite3_syscall_ptr>(busy_tolerant_stat));
  // pAppData stays SQLite's own: the unix VFS keeps its syscall table there.
  g_vfs = *g_base;
  g_vfs.zName = "mp2nx-unix-none";
  g_vfs.xFullPathname = full_pathname;
  const int rc = sqlite3_vfs_register(&g_vfs, 1);
  std::fprintf(stderr, "mp2-nx: SQLite VFS %s (rc %d)\n", rc == SQLITE_OK ? "mp2nx-unix-none is the default" : "failed", rc);
}

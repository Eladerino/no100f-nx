// POSIX calls newlib declares but libnx does not provide, or provides only as ENOSYS stubs.
#include <errno.h>
#include <reent.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

// The log (UserData/Logs/stderr.log) is written by every thread: newlib gives each thread its own
// stderr FILE (its own buffer and lock) over the same descriptor, and libnx's fsdev write reads the
// descriptor's offset, writes there and then advances it, with no lock. Two threads writing at once
// could both write at one offset (one line lost under the other) and advance it twice (a hole of stale
// bytes): the interleaved binary junk seen in the logs. Every newlib stdio write reaches the file
// through _write_r, which the NRO wraps (-Wl,--wrap=_write_r, cmake/FrightWorksSwitch.cmake): writes
// to the log's descriptor (fw_switch_set_log_fd, from userAppInit after the freopen) go one at a time.
// Other descriptors pass straight through.
static int g_logFd = -1;
static Mutex g_logWriteLock;  // libnx mutex: zero is unlocked, safe before the static constructors

// The debug server's log feed (fw_debug_server.cpp): every byte written to the log, in file order,
// handed over under the same lock. It replaces tailing the file, which Horizon refuses to open while
// the game has it open for writing (the server's log/logtext came back empty). Set only once the
// server's C++ objects exist; cleared before they go.
static void (*volatile g_logTap)(const void* data, size_t size);

void fw_switch_set_log_fd(int fd)
{
    g_logFd = fd;
}

void fw_switch_set_log_tap(void (*tap)(const void* data, size_t size))
{
    mutexLock(&g_logWriteLock);
    g_logTap = tap;
    mutexUnlock(&g_logWriteLock);
}

_ssize_t __real__write_r(struct _reent* r, int fd, const void* buf, size_t count);

_ssize_t __wrap__write_r(struct _reent* r, int fd, const void* buf, size_t count)
{
    if (fd < 0 || fd != g_logFd)
        return __real__write_r(r, fd, buf, count);
    mutexLock(&g_logWriteLock);
    const _ssize_t n = __real__write_r(r, fd, buf, count);
    void (*tap)(const void*, size_t) = g_logTap;
    if (tap != NULL && n > 0)
        tap(buf, (size_t)n);  // must not write to the log itself (it would deadlock here)
    mutexUnlock(&g_logWriteLock);
    return n;
}

// Horizon has no pipes. The runtime's process transcript (main.cpp) pipes stdout and stderr through
// a thread into console.log; when pipe() fails it puts the streams back and carries on, so stderr
// keeps going to UserData/Logs/stderr.log (source/spm_switch_init.cpp).
int pipe(int fds[2])
{
    (void)fds;
    errno = ENOSYS;
    return -1;
}

// The SD card has no permission bits, and libnx's fsdev chmod and fchmod fail with ENOSYS. libstdc++'s
// std::filesystem::copy_file calls fchmod on the new file to give it the source's mode and fails when
// that does, so every copy failed: the managed NAND's first-run WC24 files (nand_path.h) could not be
// created and the game stopped in NANDInit. Changing nothing is the right answer here, so these
// report success for a file or descriptor that exists.
int fchmod(int fd, mode_t mode)
{
    (void)mode;
    struct stat st;
    return fstat(fd, &st);
}

int chmod(const char* path, mode_t mode)
{
    (void)mode;
    struct stat st;
    return stat(path, &st);
}

// POSIX rename replaces an existing destination; Horizon's file system refuses it (the path already
// exists), so libnx's rename fails. The runtime saves through a temporary file renamed over the old
// one (hle/storage/nand_async.cpp AtomicReplaceHostFile), and NANDCreate makes the save file before
// the first write, so every save failed with "rename(... .nandsafe.tmp -> ...) failed" (boot 9). The
// NRO links with -Wl,--wrap=rename (cmake/SpmSwitch.cmake): when the plain rename fails and both
// paths are regular files, the old destination is removed and the rename tried again. That is not
// atomic: a kill between the two steps leaves only the temporary file, which holds the new data.
int __real_rename(const char* from, const char* to);

int __wrap_rename(const char* from, const char* to)
{
    if (__real_rename(from, to) == 0)
        return 0;
    const int err = errno;
    struct stat source;
    struct stat target;
    if (stat(from, &source) != 0 || !S_ISREG(source.st_mode) || stat(to, &target) != 0 ||
        !S_ISREG(target.st_mode)) {
        errno = err;
        return -1;
    }
    if (unlink(to) != 0)
        return -1;
    return __real_rename(from, to);
}

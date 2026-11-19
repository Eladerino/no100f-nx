// Adapted from SwitchWaker (MIT, switch/native/source/thread_wrap.c at 92307ff); see switch/THIRD_PARTY.md.
// Every pthread (and so every std::thread) of the NRO, which links with -Wl,--wrap=pthread_create
// (switch/cmake/SpmSwitch.cmake):
//
// - a stack of at least 4 MiB, page-aligned: libnx gives a pthread 128 KiB by default, which Tint
//   (Dawn's shader compiler) overflows. Boot 6 crashed that way on Aurora's pipeline compile worker
//   right after it was handed the 1156 seeded pipelines: a write one page below a thread's stack.
//   libnx also refuses a size that is not a multiple of 4 KiB.
// - the application cores: libnx creates pthreads on the process's default core with priority 0x3B,
//   below the main thread's 0x2C, so every helper would share core 0 with the guest scheduler and run
//   only when it waits (Horizon does not time-slice). Each new thread prefers core 1 or 2 in turn,
//   may run on any of the process's cores among 0-2, and gets the main thread's priority.
//
// - detached threads: libnx has no __syscall_thread_detach, so newlib's pthread_detach fails with
//   ENOSYS, std::thread::detach() throws, and the temporary std::thread's destructor, still joinable,
//   calls std::terminate(). Boot 8: the runtime's stall watchdog (hle/vi.cpp StartStallWatchdogOnce,
//   std::thread(...).detach()) ended the game at its first frame that way. pthread_detach hands
//   the thread to a reaper thread, which joins it once its handle signals that it ended (its stack
//   and kernel thread are freed then).
//
// SwitchWaker's version also keeps a thread registry for its performance lines; not needed yet.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "spm_switch_compat.h"  // FW_THREAD_* (also force-included; explicit for the reader)

#define MINIMUM_STACK_SIZE (4u * 1024u * 1024u)
#define THREAD_PRIORITY 0x2C

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);
int __wrap_pthread_detach(pthread_t thread);

static atomic_int g_next_core = 1;

// guestcore (dk_diag.txt, test build spec B2): with it on, helper threads are kept off the
// guest's core 0 entirely. An unpinned thread's mask today is every application core, so a
// pipeline compile (Tint holds a core for hundreds of ms on a cold pipeline) can land on the
// guest core and stall it; KartPadNX calls keeping compilers off the guest core its decisive
// fix. This file runs before the renderer's diag_init (threads start at process start), so it
// reads dk_diag.txt itself, once, on first use. What is bought is only "never on core 0":
// helpers get ideal core 2 and mask 1-2; the render threads (fw_switch_thread_mark
// FW_THREAD_RENDER: Aurora's frame worker and presenter, which also replays deko3d) ideal core 1.
// bgprio (dk_diag.txt): threads marked FW_THREAD_BACKGROUND (pipeline compile/cache writer, dk
// shader worker, DVD prefetch, stall watchdog, the reaper) drop to kBackgroundPriority, below the
// game thread and the render threads, so they only run when those wait.
// Both default off (the A/B on hardware picks the default); the main thread is the guest's.
#define DIAG_PATH "/switch/no100f-nx/UserData/dk_diag.txt"
#define BACKGROUND_PRIORITY 0x2F
static atomic_int g_guestcore = -1; // -1 unknown, 0 off, 1 on
static atomic_int g_bgprio = -1;

// One "key", "key=1" or "key=0/off/false" line of dk_diag.txt; the last mention wins.
static int diag_key(const char* name)
{
    const size_t len = strlen(name);
    int on = 0;
    FILE* f = fopen(DIAG_PATH, "r");
    if (f != NULL) {
        char line[256];
        while (fgets(line, sizeof line, f) != NULL) {
            const char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            const char* key = p;
            while ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
            if ((size_t)(p - key) != len || strncmp(key, name, len) != 0)
                continue;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\n' || *p == '\r') { // a bare key means on, as in device.cpp
                on = 1;
                continue;
            }
            if (*p != '=')
                continue;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            on = !(p[0] == '0' || strncasecmp(p, "off", 3) == 0 || strncasecmp(p, "false", 5) == 0);
        }
        fclose(f);
    }
    return on;
}

static int guestcore_enabled(void)
{
    int cached = atomic_load(&g_guestcore);
    if (cached >= 0)
        return cached;
    const int on = diag_key("guestcore");
    atomic_store(&g_guestcore, on);
    return on;
}

static int bgprio_enabled(void)
{
    int cached = atomic_load(&g_bgprio);
    if (cached >= 0)
        return cached;
    const int on = diag_key("bgprio");
    atomic_store(&g_bgprio, on);
    return on;
}

static u32 application_core_mask(void);

// The preferred core for a helper thread with guestcore on: core 2 (the render threads move to
// core 1 when they mark themselves), else the highest application core that is not core 0. -1
// when nothing is left (a one-core process), meaning keep the normal dealing.
static int guestcore_next_core(void)
{
    const u32 mask = application_core_mask() & ~1u;
    for (int c = 2; c >= 1; c--)
        if (mask & (1u << c))
            return c;
    return -1;
}

// Where the image starts (the code region holding this file's functions), for elf offsets.
static uintptr_t image_base(void)
{
    static uintptr_t base;
    if (base == 0) {
        MemoryInfo info;
        u32 page;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, (uintptr_t)&image_base)))
            base = (uintptr_t)info.addr;
    }
    return base;
}

// One boot line per thread: where it runs now and what it may use. `fn` (may be null) is its
// start function, printed as an offset into the ELF (aarch64-none-elf-addr2line -f -C -e).
void fw_switch_thread_log_current(const char* what, const void* fn)
{
    u64 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    s32 ideal = -1;
    u64 mask = 0;
    svcGetThreadCoreMask(&ideal, &mask, CUR_THREAD_HANDLE);
    s32 prio = -1;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const uintptr_t base = image_base();
    const uintptr_t at = (uintptr_t)fn;
    fprintf(stderr, "[switch] thread %llu %s: fn elf+0x%llx, on core %d, ideal %d mask 0x%x, prio 0x%X\n",
            (unsigned long long)tid, what ? what : "start",
            (unsigned long long)(at && base && at >= base ? at - base : at), (int)svcGetCurrentProcessorNumber(),
            (int)ideal, (unsigned)mask, (unsigned)prio);
}

// The lowest priority (0x3B) for a thread that must only run in everyone else's gaps (the debug
// server's threads, fw_debug_server.cpp). Such a thread can never preempt the game thread, so it keeps
// every application core, core 0 included, even under guestcore: on cores 1-2 alone, below the render
// and compile threads there, it starved (debug-server input injection stopped under guestcore).
void fw_switch_thread_lowest_priority(void)
{
    s32 ideal = 1;
    u64 mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&ideal, &mask, CUR_THREAD_HANDLE)) || ideal < 0)
        ideal = 1;
    const u32 app = application_core_mask();
    if ((app & (1u << ideal)) == 0)
        ideal = __builtin_ctz(app);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, app);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
}

// Called by a thread at its start (fw_switch_thread.h): FW_THREAD_RENDER moves it to core 1
// under guestcore; FW_THREAD_BACKGROUND lowers its priority under bgprio. Logged either way.
void fw_switch_thread_mark(int kind)
{
    static const char* const kNames[] = {"other", "render", "background"};
    if (kind == 1 && guestcore_enabled()) {
        const u32 mask = application_core_mask() & ~1u;
        if (mask & 2u)
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, mask);
    }
    if (kind == 2 && bgprio_enabled())
        svcSetThreadPriority(CUR_THREAD_HANDLE, BACKGROUND_PRIORITY);
    fw_switch_thread_log_current(kind >= 0 && kind <= 2 ? kNames[kind] : "marked", NULL);
}

struct Trampoline {
    void* (*start)(void*);
    void* arg;
    int core;
};

static u32 application_core_mask(void)
{
    static u32 mask;
    if (mask == 0) {
        u64 process_mask = 0;
        if (R_FAILED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
            process_mask = 0x7;
        mask = (u32)process_mask & 0x7;
        if (mask == 0)
            mask = 0x1;
    }
    return mask;
}

static void* trampoline(void* raw)
{
    struct Trampoline t = *(struct Trampoline*)raw;
    free(raw);
    u32 mask = application_core_mask();
    if (guestcore_enabled()) {
        const u32 no_guest = mask & ~1u;
        if (no_guest != 0)
            mask = no_guest;
    }
    int core = t.core;
    if ((mask & (1u << core)) == 0)
        core = __builtin_ctz(mask);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask);
    svcSetThreadPriority(CUR_THREAD_HANDLE, THREAD_PRIORITY);
    fw_switch_thread_log_current("start", (const void*)t.start);
    return t.start(t.arg);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg)
{
    pthread_attr_t sized;
    size_t requested = 0;
    if (attr != NULL) {
        sized = *attr;
        if (pthread_attr_getstacksize(attr, &requested) != 0)
            requested = 0;
    } else {
        pthread_attr_init(&sized);
    }
    size_t size = requested < MINIMUM_STACK_SIZE ? MINIMUM_STACK_SIZE : requested;
    size = (size + 0xFFF) & ~(size_t)0xFFF;
    pthread_attr_setstacksize(&sized, size);

    struct Trampoline* t = malloc(sizeof *t);
    if (t == NULL) {
        const int result = __real_pthread_create(thread, &sized, start, arg);
        if (attr == NULL)
            pthread_attr_destroy(&sized);
        return result;
    }
    t->start = start;
    t->arg = arg;
    const int guest_free = guestcore_enabled() ? guestcore_next_core() : -1;
    t->core = guest_free >= 0 ? guest_free : 1 + (atomic_fetch_add(&g_next_core, 1) - 1) % 2; // 1, 2, 1, 2...
    const int result = __real_pthread_create(thread, &sized, trampoline, t);
    if (result != 0)
        free(t);
    if (attr == NULL)
        pthread_attr_destroy(&sized);
    return result;
}

// ---- reaper of detached threads -------------------------------------------------------------------
// newlib's pthread_t is libnx's struct __pthread_t*, whose first member is the libnx Thread.
#define MAX_REAP 64 // svcWaitSynchronization's limit

static Mutex g_reap_lock;
static pthread_t g_reap[MAX_REAP];
static int g_reap_count;
static atomic_int g_reaper_started;

static Handle thread_handle(pthread_t t)
{
    return ((const Thread*)t)->handle;
}

static void reap_remove(pthread_t t)
{
    mutexLock(&g_reap_lock);
    for (int i = 0; i < g_reap_count; i++) {
        if (g_reap[i] == t) {
            g_reap[i] = g_reap[--g_reap_count];
            break;
        }
    }
    mutexUnlock(&g_reap_lock);
}

static void* reaper(void* unused)
{
    (void)unused;
    fw_switch_thread_mark(FW_THREAD_BACKGROUND);
    for (;;) {
        Handle handles[MAX_REAP];
        pthread_t threads[MAX_REAP];
        mutexLock(&g_reap_lock);
        const int n = g_reap_count;
        for (int i = 0; i < n; i++) {
            threads[i] = g_reap[i];
            handles[i] = thread_handle(g_reap[i]);
        }
        mutexUnlock(&g_reap_lock);
        if (n == 0) {
            svcSleepThread(100000000ull);
            continue;
        }
        s32 index = -1;
        // 100 ms: threads detached meanwhile are picked up at the next round
        const Result rc = svcWaitSynchronization(&index, handles, n, 100000000ull);
        if (R_VALUE(rc) == KERNELRESULT(InvalidHandle)) {
            // not a thread any more (should not happen): drop the ones that are not, keep the others
            for (int i = 0; i < n; i++) {
                s32 one = -1;
                if (R_VALUE(svcWaitSynchronization(&one, &handles[i], 1, 0)) == KERNELRESULT(InvalidHandle))
                    reap_remove(threads[i]);
            }
            continue;
        }
        if (R_FAILED(rc) || index < 0 || index >= n)
            continue;
        reap_remove(threads[index]);
        pthread_join(threads[index], NULL); // it has ended: frees its stack and closes its handle
    }
    return NULL;
}

int __wrap_pthread_detach(pthread_t thread)
{
    if (atomic_exchange(&g_reaper_started, 1) == 0) {
        pthread_t r;
        if (__wrap_pthread_create(&r, NULL, reaper, NULL) != 0)
            fprintf(stderr, "[switch] the reaper of detached threads could not start\n");
    }
    mutexLock(&g_reap_lock);
    bool listed = false;
    for (int i = 0; i < g_reap_count; i++)
        listed = listed || g_reap[i] == thread;
    const bool full = !listed && g_reap_count >= MAX_REAP;
    if (!listed && !full)
        g_reap[g_reap_count++] = thread;
    mutexUnlock(&g_reap_lock);
    if (full) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[switch] %d detached threads already wait to be joined: more are left unjoined\n",
                    MAX_REAP);
    }
    // Detached either way: the caller (std::thread::detach) must not see an error.
    return 0;
}

// Adapted for FrightWorks from spm-nx switch/source/spm_switch_init.cpp (12ec5e7).
// Process setup for the Switch NRO, outside the upstream runtime. libnx calls userAppInit() before
// main() and userAppExit() after it returns, so the runtime's own main (runtime/src/main.cpp) runs
// unchanged on top of what this file prepares:
//   - the SD card layout: FW_SWITCH_ROOT (sdmc:/switch/<NRO name>/) holds the NRO, the game/ folder
//     (the extracted disc) and UserData/ (settings, saves, caches, logs). The runtime finds both through the same environment
//     variables its Android build uses (runtime/src/platform/host_platform.cpp);
//   - stderr into UserData/Logs/stderr.log, because RT_LOG writes there and a log file is how a run
//     on the console reports back;
//   - BSD sockets (the debug server) when the network is up, and the debug server itself in dev
//     builds (FW_SWITCH_DEBUG_SERVER);
//   - a crash handler: a data abort or other exception writes its registers, the NRO's load address
//     and a backtrace to UserData/Logs/crash.txt.
#include <switch.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>

#include "guest_flat_memory.h"

namespace {
// Without the "sdmc:" prefix: fsdevMountSdmc makes the SD card the default device, so "/switch/..."
// is the same place, and it is an absolute path to std::filesystem and sqlite. With the prefix both
// treat the path as relative: std::filesystem::create_directories tries to make "sdmc:" and fails
// (the NAND folders), and sqlite's unix VFS looks for "/sdmc:" (Aurora's caches).
#ifndef FW_SWITCH_ROOT
#error "FW_SWITCH_ROOT comes from switch/cmake/FrightWorksSwitch.cmake"
#endif
#ifndef FW_SWITCH_ELF_NAME
#define FW_SWITCH_ELF_NAME "no100f-nx.elf"
#endif
constexpr const char* kRoot = FW_SWITCH_ROOT;
constexpr const char* kUserData = FW_SWITCH_ROOT "/UserData";
constexpr const char* kLogs = FW_SWITCH_ROOT "/UserData/Logs";
constexpr const char* kStderrLog = FW_SWITCH_ROOT "/UserData/Logs/stderr.log";

// The crash report goes to its own file through write(): through stderr it came out interleaved with,
// and overwritten by, the other threads' log lines (boot 6).
constexpr const char* kCrashLog = FW_SWITCH_ROOT "/UserData/Logs/crash.txt";

bool g_sockets = false;
u64 g_mainThreadId = 0;
std::atomic<bool> g_crashing{false};
std::atomic<u64> g_exitingThread{0};  // the thread in userAppExit(), which unmaps guest memory
int g_crashFd = -1;
char g_crashLine[512];

void Say(const char* format, ...) {
    if (g_crashFd < 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(g_crashLine, sizeof(g_crashLine), format, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    if (n >= static_cast<int>(sizeof(g_crashLine))) {
        n = sizeof(g_crashLine) - 1;
    }
    write(g_crashFd, g_crashLine, static_cast<size_t>(n));
}

const char* ExceptionName(u32 desc) {
    switch (desc) {
    case ThreadExceptionDesc_InstructionAbort: return "instruction abort";
    case ThreadExceptionDesc_MisalignedPC: return "misaligned PC";
    case ThreadExceptionDesc_MisalignedSP: return "misaligned SP";
    case ThreadExceptionDesc_SError: return "SError";
    case ThreadExceptionDesc_BadSVC: return "bad SVC";
    case ThreadExceptionDesc_Trap: return "trap";
    case ThreadExceptionDesc_Other: return "data abort or other";
    default: return "unknown";
    }
}

// The NRO's load address: the start of the code mapping this function is in.
uintptr_t ImageBase() {
    static uintptr_t base;
    if (base == 0) {
        MemoryInfo info{};
        u32 page = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<uintptr_t>(&ImageBase))) &&
            info.type != MemType_Unmapped) {
            base = info.addr;
        }
    }
    return base;
}

// An address in the NRO also as an offset for addr2line.
void SayAddress(const char* label, uint64_t address) {
    const uintptr_t base = ImageBase();
    if (base != 0 && address >= base && address - base < (1ull << 30)) {
        Say("%s0x%016" PRIx64 " (" FW_SWITCH_ELF_NAME "+0x%" PRIx64 ")", label, address, address - base);
    } else {
        Say("%s0x%016" PRIx64, label, address);
    }
}

bool ReadWord(uintptr_t address, uintptr_t* out) {
    if (address == 0 || (address & 7) != 0) {
        return false;
    }
    MemoryInfo info{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, address)) || info.type == MemType_Unmapped ||
        (info.perm & Perm_R) == 0 || address + 8 > info.addr + info.size) {
        return false;
    }
    *out = *reinterpret_cast<const uintptr_t*>(address);
    return true;
}

// The stack the frame pointer is in, and whether sp has left it: a stack overflow faults just below.
void SayStack(uint64_t sp, uint64_t fp) {
    MemoryInfo info{};
    u32 page = 0;
    if (fp == 0 || R_FAILED(svcQueryMemory(&info, &page, fp)) || info.type == MemType_Unmapped) {
        return;
    }
    Say("  stack 0x%016" PRIx64 "-0x%016" PRIx64 " (%" PRIu64 " KiB, memory type 0x%" PRIx32 ")", info.addr,
        info.addr + info.size, info.size / 1024, info.type);
    if (sp < info.addr) {
        Say(": sp is 0x%" PRIx64 " bytes below it, a stack overflow", info.addr - sp);
    }
    Say("\n");
}

void MakeDirectory(const char* path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "[switch] cannot create %s (errno %d)\n", path, errno);
    }
}
} // namespace

extern "C" {

// runtime/horizon_tls.c: the thread pointer clang's thread_locals use (TPIDR_EL0) for this thread.
void spm_switch_tls_init_main_thread(void);
void spm_switch_tls_check_main_thread(void);
// runtime/guest_flat_memory_horizon.cpp: unmaps the flat guest window, which hbloader needs gone.
void spm_switch_guest_memory_release(void);
// source/fw_debug_server.cpp: starts the debug server (dev builds, FW_SWITCH_DEBUG_SERVER), and
// stops it so its threads are gone before the socket service closes below. Stop is inert when the
// server never started. The boot itself is deferred: userAppInit only requests it
// (fw_debug_server_request_boot, plain data only, safe before the static constructors) and
// RuntimeMain in the runtime runs fw_debug_server_boot_pending once they have run.
void fw_debug_server_boot(const char* root);
void fw_debug_server_request_boot(const char* root);
void fw_debug_server_boot_pending(void);
void fw_debug_server_stop(void);
// switch/sdl3_shim/sdl3_shim_events.c: queues the SDL quit the frame loop turns into the
// normal shutdown (ExitForAuroraWindowClose -> userAppExit). Safe from any thread, once.
void sdl3_shim_request_quit(void);
// runtime/horizon_unistd.c: the log's descriptor, whose writes the _write_r wrap serializes.
void fw_switch_set_log_fd(int fd);
// source/pgo_instr_switch.c: the instrumented-PGO capture build's profile writer,
// linked only into that build (FrightWorksSwitch.cmake FW_PGO_INSTRUMENT); weak, so
// null in every other build.
void fw_pgo_instr_start(void) __attribute__((weak));
void fw_pgo_instr_dump_now(void) __attribute__((weak));

// The exception handler runs on its own stack: the faulting thread's may be the problem.
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

// Applet status changes, delivered by libnx's message processing (appletProcessMessage, reached
// from the frame loop's pump or the watcher thread below): the exit request asks the frame loop to
// quit. It only exists because userAppInit locks exit (appletLockExit); without the lock a HOME >
// Close ends the process outright and nothing of ours runs. With it, Close resumes the process with
// ExitRequest and gives it 15 s to exit before a forced kill.
void fw_switch_applet_status_hook(AppletHookType type, void* /*param*/) {
    if (type == AppletHookType_OnExitRequest) {
        // One line per exit so a log shows the applet close went through the
        // lock instead of a forced kill (a kill leaves no line at all).
        std::fprintf(stderr, "[switch] exit request received (HOME close)\n");
        sdl3_shim_request_quit();
    }
}

// The second applet-message consumer. The frame loop's pump (through the SDL
// shim) runs only while frames advance, and after a close the process is
// resumed out of focus, where nothing guarantees the frame loop keeps
// presenting. The watcher makes sure the ExitRequest is acted on either way.
// libnx tolerates two consumers: whichever thread receives a message runs it
// through the one processing routine (the loser's receive returns "no message").
// The receive is non-blocking; the thread sleeps between empty polls.
void fw_switch_applet_watcher(void) {
    for (;;) {
        u32 msg = 0;
        if (R_SUCCEEDED(appletGetMessage(&msg))) {
            appletProcessMessage(msg);
        } else {
            svcSleepThread(100000000ULL);  // 100 ms
        }
    }
}

AppletHookCookie g_appletHookCookie;

void userAppInit(void) {
    // Before anything that could touch a thread_local: libnx runs the static constructors after this.
    spm_switch_tls_init_main_thread();
    MakeDirectory(kRoot);
    MakeDirectory(kUserData);
    MakeDirectory(kLogs);
    if (std::freopen(kStderrLog, "w", stderr) != nullptr) {
        std::setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered: a crash keeps the last lines
        fw_switch_set_log_fd(fileno(stderr));      // every thread's writes to it, one at a time
    }
    spm_switch_tls_check_main_thread();
    // The PGO capture build starts its dump thread now that the SD layout and stderr exist.
    if (fw_pgo_instr_start) fw_pgo_instr_start();
    setenv("WIICOMPILED_RUNTIME_DIR", kRoot, 1);
    setenv("WIICOMPILED_DATA_DIR", kUserData, 1);

    g_sockets = R_SUCCEEDED(socketInitializeDefault());
    svcGetThreadId(&g_mainThreadId, CUR_THREAD_HANDLE);
    ImageBase();
    // The game thread's own core/affinity/priority line (horizon_pthread.c logs every other thread).
    fw_switch_thread_log_current("main (guest scheduler)", nullptr);

    // Lock exit, so a close from the HOME menu arrives as an ExitRequest (with
    // 15 s to act on it) instead of the system ending the process with nothing
    // of ours run: the frame loop then shuts down through userAppExit, which unlocks again at its end.
    // libnx itself never locks. A game that is hung when the player closes it
    // now takes up to those 15 s to be killed.
    const Result lockRc = appletLockExit();

    // Registered before main() so no status change of the run can precede it.
    appletHook(&g_appletHookCookie, fw_switch_applet_status_hook, nullptr);
    std::thread(fw_switch_applet_watcher).detach();

    AppletType type = appletGetAppletType();
    std::fprintf(stderr, "[switch] FrightWorks starting: %s, %s mode, sockets %s\n",
                 type == AppletType_Application || type == AppletType_SystemApplication
                     ? "title takeover"
                     : "applet mode (little memory: launch through a game instead)",
                 appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
                 g_sockets ? "up" : "off");
    if (R_FAILED(lockRc))
        std::fprintf(stderr, "[switch] appletLockExit failed (rc 0x%x): a HOME close will skip shutdown\n", lockRc);
#if FW_SWITCH_DEBUG_SERVER
    if (g_sockets) {
        // Do not boot the server here: libnx runs the static constructors after
        // userAppInit, so the server's C++ globals (its command map, log buffer
        // and strings) do not exist yet, and the first add_command crashed on a
        // zeroed std::map (crash-2, Nov 14, 2026). Request the boot instead: it
        // only writes a plain char buffer and a flag. RuntimeMain performs the
        // actual boot once main is running (fw_debug_server_boot_pending).
        fw_debug_server_request_boot(kRoot);
    } else {
        std::fprintf(stderr, "[switch] debug server not started: sockets are off\n");
    }
#endif
}

// The debug server keeps the console awake while it runs: a console under test is left alone for
// minutes at a time.
void fw_switch_keep_awake(bool on) {
    appletSetAutoSleepDisabled(on);
}

// The runtime's stall service (wiicompiled/runtime/src/hle/vi.cpp) reads this
// between burst rounds so it never delivers guest callbacks into a process that
// is already exiting: g_exitingThread itself has internal linkage, so this is
// its only window out of this file. Declared weak on the runtime side, so a
// frontend without this exit path leaves the check permanently false.
extern "C" bool fw_switch_exiting(void) {
    return g_exitingThread.load(std::memory_order_relaxed) != 0;
}

void userAppExit(void) {
    // After main() and the static destructors. A thread still running (the fatal exit leaves
    // Aurora's alive) may touch guest memory once it is unmapped; the crash handler parks it.
    u64 self = 0;
    svcGetThreadId(&self, CUR_THREAD_HANDLE);
    g_exitingThread = self;
    std::fprintf(stderr, "[switch] userAppExit: shutting down\n");
    // The debug server stops first: its threads touch nothing the release unmaps, but they
    // hold the listening socket the socketExit below is about to close under them.
    fw_debug_server_stop();
    // The PGO capture build's final dump comes before the guest memory release: its
    // counters live in instrumented code and data the release does not touch, but
    // nothing after this point is guaranteed to run.
    if (fw_pgo_instr_dump_now) fw_pgo_instr_dump_now();
    spm_switch_guest_memory_release();
    std::fprintf(stderr, "[switch] userAppExit: clean shutdown done\n");
    std::fflush(stderr);
    if (g_sockets) {
        socketExit();
    }
    // Last: libnx requires the exit lock (userAppInit) to be released before
    // the process ends, and appletExit runs right after this function returns.
    appletUnlockExit();
}

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    u64 threadId = 0;
    svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
    // During exit, another thread's fault is one that outlived the unmapped guest memory, not a
    // crash worth a report over the one that may have caused the exit: it waits, like a second
    // crash does. The exiting thread's own fault is still reported.
    const u64 exiting = g_exitingThread;
    bool expected = false;
    if ((exiting != 0 && exiting != threadId) || !g_crashing.compare_exchange_strong(expected, true)) {
        for (;;) {  // a second thread's exception waits for the first report to end the process
            svcSleepThread(1000000000ULL);
        }
    }
    g_crashFd = open(kCrashLog, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    Say("[switch] CRASH: %s (0x%" PRIx32 ") esr=0x%" PRIx32 " far=0x%016" PRIx64 " on thread %" PRIu64 "%s\n",
        ExceptionName(ctx->error_desc), ctx->error_desc, ctx->esr, ctx->far.x, threadId,
        threadId == g_mainThreadId ? " (the main thread: guest scheduler)" : " (not the main thread)");
    const uint64_t flatBase = reinterpret_cast<uint64_t>(RECOMP_FLAT_GUEST_BASE);
    if (flatBase != 0 && ctx->far.x >= flatBase && ctx->far.x - flatBase < GuestFlat::kGuestSpaceSize) {
        Say("  -> guest address 0x%08" PRIx64 " through the flat window, where nothing is mapped: a wild "
            "guest pointer or an MMIO read (the window has no page traps)\n",
            ctx->far.x - flatBase);
    }
    SayAddress("  pc=", ctx->pc.x);
    SayAddress(" lr=", ctx->lr.x);
    Say(" sp=0x%016" PRIx64 " fp=0x%016" PRIx64 "\n", ctx->sp.x, ctx->fp.x);
    SayStack(ctx->sp.x, ctx->fp.x);
    for (int i = 0; i < 29; i += 4) {
        Say(" ");
        for (int j = i; j < i + 4 && j < 29; ++j) {
            Say(" x%-2d=0x%016" PRIx64, j, ctx->cpu_gprs[j].x);
        }
        Say("\n");
    }
    Say("  image base=0x%016" PRIxPTR ": aarch64-none-elf-addr2line -f -C -i -e " FW_SWITCH_ELF_NAME " <offset>\n",
        ImageBase());
    Say("  backtrace (pc, lr, then the frame records' call sites):\n");
    SayAddress("    #0 ", ctx->pc.x);
    Say("\n");
    SayAddress("    #1 ", ctx->lr.x);
    Say("\n");
    uintptr_t fp = ctx->fp.x;
    for (int depth = 2; fp != 0 && depth < 48; ++depth) {
        uintptr_t next = 0;
        uintptr_t ret = 0;
        if (!ReadWord(fp, &next) || !ReadWord(fp + 8, &ret) || ret == 0) {
            break;
        }
        char label[16];
        std::snprintf(label, sizeof(label), "    #%d ", depth);
        SayAddress(label, ret - 4);
        Say("\n");
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    if (g_crashFd >= 0) {
        close(g_crashFd);
    }
    std::_Exit(EXIT_FAILURE);
}

// The host stack for the runtime's fatal paths (runtime/src/main.cpp FormatHostStackTrace), which
// has no walker on this platform: a std::terminate() reported only the guest's state (boot 7). The
// unwind tables first (C++ code has them), the frame records if they give out early. Offsets are
// for aarch64-none-elf-addr2line -f -C -i -e the NRO's ELF.
namespace {
struct UnwindFrames {
    uintptr_t pcs[48];
    unsigned count = 0;
    unsigned skip = 0;
};

_Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* argument) {
    auto* frames = static_cast<UnwindFrames*>(argument);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc == 0) {
        return _URC_END_OF_STACK;
    }
    if (frames->skip > 0) {
        --frames->skip;
        return _URC_NO_REASON;
    }
    frames->pcs[frames->count++] = pc;
    return frames->count < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
} // namespace

size_t fw_switch_host_backtrace(char* out, size_t size, unsigned skip) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    UnwindFrames frames;
    frames.skip = skip + 1;  // this function
    _Unwind_Backtrace(CollectFrame, &frames);
    const char* method = "unwind tables";
    if (frames.count < 3) {
        frames.count = 0;
        method = "frame records";
        uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        while (fp != 0 && frames.count < 48) {
            uintptr_t next = 0;
            uintptr_t ret = 0;
            if (!ReadWord(fp, &next) || !ReadWord(fp + 8, &ret) || ret == 0) {
                break;
            }
            frames.pcs[frames.count++] = ret;
            if (next <= fp) {
                break;
            }
            fp = next;
        }
    }

    u64 threadId = 0;
    svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
    const uintptr_t base = ImageBase();
    size_t used = 0;
    auto append = [&](const char* format, auto... args) {
        if (used + 1 >= size) {
            return;
        }
        const int n = std::snprintf(out + used, size - used, format, args...);
        if (n > 0) {
            used += static_cast<size_t>(n) < size - used ? static_cast<size_t>(n) : size - used - 1;
        }
    };
    append("[switch] host stack on thread %" PRIu64 "%s (%s; image base 0x%" PRIxPTR
           ", aarch64-none-elf-addr2line -f -C -i -e " FW_SWITCH_ELF_NAME " <offset>):\n",
           threadId, threadId == g_mainThreadId ? " (main)" : "", method, base);
    for (unsigned i = 0; i < frames.count; ++i) {
        // Return addresses: the call is the instruction before each.
        const uintptr_t pc = frames.pcs[i] - 4;
        if (base != 0 && pc >= base && pc - base < (1ull << 30)) {
            append("[switch]   #%u " FW_SWITCH_ELF_NAME "+0x%" PRIxPTR "\n", i, pc - base);
        } else {
            append("[switch]   #%u 0x%016" PRIxPTR "\n", i, pc);
        }
    }
    return used;
}

} // extern "C"

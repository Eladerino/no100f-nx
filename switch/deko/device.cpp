// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff: switch/deko/device.cpp
// (device, queue, swapchain, debug callback, queue checks, set-up cost), itself adapted from
// SwitchWakerHD's runtime/src/gfx/deko/backend.cpp.
// The deko3d device and the window (dk.h, dk_aurora.h): created from Aurora's webgpu::initialize right
// after Dawn's device, before Aurora creates any texture or buffer, so that every one of them gets
// its deko3d shadow (wgpu_objects.cpp). The swapchain is three RGBA8 images of 1280x720 on the
// default window; the present (wgpu_encode.cpp) scales Aurora's finished picture into one of them.
#include <switch.h>
#include <unistd.h>

#include "dk.h"
#include "dk_aurora.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" char* fake_heap_end;

namespace swdk {

Renderer R;
Diag g_diag;

// The diagnostic switches (dk.h): one "key=1" per line in UserData/dk_diag.txt, read once here.
// A line naming a key without "=0" turns it on. The active set is logged so a test log always
// says which switches the run used.
void diag_init() {
    FILE* f = std::fopen("/switch/no100f-nx/UserData/dk_diag.txt", "r");
    if (!f) {
        dklog("diag: no UserData/dk_diag.txt; all diagnostic switches off");
        return;
    }
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        const bool off = std::strstr(line, "=0") != nullptr;
        if (std::strstr(line, "depth_off")) g_diag.depthOff = !off;
        if (std::strstr(line, "cull_off")) g_diag.cullOff = !off;
        if (std::strstr(line, "magenta")) g_diag.magenta = !off;
        if (std::strstr(line, "picdump")) g_diag.picDump = !off;
        if (std::strstr(line, "efbclear")) g_diag.efbClear = !off;
        if (std::strstr(line, "bufdump")) g_diag.bufDump = !off;
        if (std::strstr(line, "texdump")) g_diag.texDump = !off;
        if (std::strstr(line, "shaderdump")) g_diag.shaderDump = !off;
        if (std::strstr(line, "nomerge")) g_diag.noMerge = !off;
        if (std::strstr(line, "uvlog")) g_diag.uvLog = !off;
        if (std::strstr(line, "mtxlog")) g_diag.mtxLog = !off;
        if (std::strstr(line, "perftimer")) g_diag.perfTimer = !off;
        if (std::strstr(line, "arrayup")) g_diag.arrayUp = !off;
        if (std::strstr(line, "cachedstaging")) g_diag.cachedStaging = !off;
        if (std::strstr(line, "storagecache")) g_diag.storageCache = !off;
        if (std::strstr(line, "usedrange")) g_diag.usedRange = !off;
        if (std::strstr(line, "fifofast")) g_diag.fifoFast = off ? 0 : (std::strstr(line, "=2") ? 2 : 1);
        if (std::strstr(line, "s0probe")) g_diag.s0Probe = !off;
        if (std::strstr(line, "stagetimers")) g_diag.stageTimers = !off;
        if (std::strstr(line, "nativescene")) g_diag.nativeScene = !off;
        if (std::strstr(line, "n8c2off")) g_diag.n8c2off = !off;
        if (std::strstr(line, "n9")) g_diag.n9 = !off;
        if (std::strstr(line, "dlcensus")) g_diag.dlCensus = !off;
        if (const char* wd = std::strstr(line, "watchdogat=")) g_diag.watchdogAt = std::atoi(wd + 11);
        if (std::strstr(line, "traceio")) g_diag.traceIo = !off;
        if (std::strstr(line, "tracecnt")) g_diag.traceCnt = !off;
        if (std::strstr(line, "tracewake")) g_diag.traceWake = !off;
        if (std::strstr(line, "parkwatch")) g_diag.parkWatch = !off;
        if (std::strstr(line, "seamfix")) g_diag.seamFix = !off;
        if (std::strstr(line, "padwatch")) g_diag.padWatch = !off;
        if (std::strstr(line, "fix2")) g_diag.fix2 = !off;
        if (std::strstr(line, "heapwatch")) g_diag.heapWatch = !off;
        if (std::strstr(line, "dispwatch")) g_diag.dispWatch = !off;
        if (std::strstr(line, "safepoint")) g_diag.safePoint = !off;
        if (std::strstr(line, "spinwatch")) g_diag.spinWatch = !off;
    if (std::strstr(line, "phasetime")) g_diag.phaseTime = !off;
        if (const char* ss = std::strstr(line, "stallsvc=")) g_diag.stallSvc = std::strncmp(ss + 9, "count", 5) == 0 ? 1 : (std::strncmp(ss + 9, "burst", 5) == 0 ? 2 : (std::strncmp(ss + 9, "probe", 5) == 0 ? 3 : 0));
        if (std::strstr(line, "pipering")) g_diag.pipeRing = !off;
    }
    std::fclose(f);
    // Aurora's command processor reads NSMBW_NO_MERGE once, on the first draw it handles; diag_init
    // runs from webgpu::initialize, before any texture, buffer or draw exists, so setting it here
    // lands in time. In-process libc env: Aurora's getenv sees what this setenv wrote.
    if (g_diag.noMerge) setenv("NSMBW_NO_MERGE", "1", 1);
    // Same handshake for NSMBW_UV_LOG: build_uniform reads it once, on the first draw it stages.
    if (g_diag.uvLog) setenv("NSMBW_UV_LOG", "1", 1);
    // Same handshake for NSMBW_MTX_LOG.
    if (g_diag.mtxLog) setenv("NSMBW_MTX_LOG", "1", 1);
    // Same handshake for NSMBW_STAGE_TIMERS.
    if (g_diag.perfTimer) setenv("NSMBW_STAGE_TIMERS", "1", 1);
    // Same handshake for NSMBW_ARRAY_REUPLOAD: handle_draw reads it once, on the first draw it handles.
    if (g_diag.arrayUp) setenv("NSMBW_ARRAY_REUPLOAD", "1", 1);
    // Same handshake for NO100F_STORAGE_CACHE: the command processor's storage_cached_upload reads it
    // once, on the first unmerged draw (aurora lib/gx/command_processor.cpp).
    if (g_diag.storageCache) setenv("NO100F_STORAGE_CACHE", "1", 1);
    // Same handshake for NO100F_USED_RANGE: the command processor's used-range upload reads it
    // once, on the first unmerged draw (aurora lib/gx/command_processor.cpp).
    if (g_diag.usedRange) setenv("NO100F_USED_RANGE", "1", 1);
    if (g_diag.s0Probe) setenv("NO100F_S0PROBE", "1", 1);
    if (g_diag.stageTimers) setenv("NO100F_STAGE_TIMERS", "1", 1);
    if (g_diag.nativeScene) setenv("NO100F_NATIVESCENE", "1", 1);
    if (g_diag.n8c2off) setenv("NO100F_N8C2OFF", "1", 1);
    if (g_diag.n9) setenv("NO100F_N9", "1", 1);
    if (g_diag.dlCensus) setenv("NO100F_DLCENSUS", "1", 1);
    // Same handshake for the runtime's stall watchdog (runtime hle/vi.cpp): one extra mark at
    // the given uptime in seconds, read once by the watchdog thread.
    if (g_diag.watchdogAt > 0) {
        char wdBuf[16];
        std::snprintf(wdBuf, sizeof(wdBuf), "%d", g_diag.watchdogAt);
        setenv("NO100F_WATCHDOG_AT", wdBuf, 1);
    }
    // Same handshake for the runtime's IO completion trace (runtime hle/storage/dvd.cpp), read once by QueueDvdCallback.
    if (g_diag.traceIo) setenv("SCOOBY_TRACE_IO", "1", 1);
    // Same handshake for the runtime's wedge counter sampler (runtime hle/vi.cpp), read once by the watchdog thread.
    if (g_diag.traceCnt) setenv("SCOOBY_TRACE_CNT", "1", 1);
    // Same handshake for the runtime's park seam trace (runtime hle/os/os_sleep.cpp + os_scheduler.cpp), read once at the seam.
    if (g_diag.traceWake) setenv("SCOOBY_TRACE_WAKE", "1", 1);
    // Same handshake for the runtime's park rescue (runtime hle/vi.cpp), read once by the stall-service thread.
    if (g_diag.parkWatch) setenv("SCOOBY_PARKWATCH", "1", 1);
    // Same handshake for the runtime seam fix (hle/os/os_sleep.cpp seam pass +
    // hle/storage/dvd.cpp held GX queue), read once at process start.
    if (g_diag.seamFix) setenv("SCOOBY_SEAMFIX", "1", 1);
    // Same handshake for the runtime's pad visibility instrument (runtime hle/input/pad.cpp), read once at the first PADRead.
    if (g_diag.padWatch) setenv("SCOOBY_PADWATCH", "1", 1);
    // Same handshake for Fix 2 (runtime hle/vi.cpp present hook), read once at process start.
    if (g_diag.fix2) setenv("SCOOBY_FIX2", "1", 1);
    // Same handshake for the heap-list canary (runtime hle/vi.cpp), read once by the watchdog thread.
    if (g_diag.heapWatch) setenv("SCOOBY_HEAPWATCH", "1", 1);
    // Same handshake for the display-state watch (runtime hle/vi.cpp), read once by the watchdog thread.
    if (g_diag.dispWatch) setenv("SCOOBY_DISPWATCH", "1", 1);
    // Same handshake for the executor safepoint (runtime hle/vi.cpp), read once by the stall-service thread.
    if (g_diag.safePoint) setenv("SCOOBY_SAFEPOINT", "1", 1);
    // Same handshake for the spinner PC watch (runtime hle/vi.cpp), read once by the stall-service thread.
    if (g_diag.spinWatch) setenv("SCOOBY_SPINWATCH", "1", 1);
    // Same handshake for the per-phase frame telemetry (runtime hle/vi.cpp + gx_fifo + os_scheduler + audio), read once by each site.
    if (g_diag.phaseTime) setenv("SCOOBY_PHASETIME", "1", 1);
    // Same handshake for the runtime's stall service (runtime hle/vi.cpp): delivery mode, read once by the service thread.
    if (g_diag.stallSvc) setenv("NO100F_STALLSVC", g_diag.stallSvc == 1 ? "count" : (g_diag.stallSvc == 2 ? "burst" : "probe"), 1);
    // Same handshake for the runtime's gather-pipe parser (runtime hle/gx/gx_fifo.cpp): fifofast (1, or 2
    // with the self-check) and the per-word diagnostic ring, both read once at the first pipe write.
    if (g_diag.fifoFast) setenv("NO100F_FIFO_FAST", g_diag.fifoFast == 2 ? "2" : "1", 1);
    if (g_diag.pipeRing) setenv("NO100F_PIPE_RING", "1", 1);
    dklog("diag: depth_off=%d cull_off=%d magenta=%d picdump=%d efbclear=%d bufdump=%d texdump=%d shaderdump=%d nomerge=%d uvlog=%d mtxlog=%d perftimer=%d arrayup=%d cachedstaging=%d storagecache=%d fifofast=%d pipering=%d usedrange=%d watchdogat=%d traceio=%d tracecnt=%d stallsvc=%s tracewake=%d parkwatch=%d seamfix=%d padwatch=%d fix2=%d heapwatch=%d dispwatch=%d safepoint=%d spinwatch=%d phasetime=%d dlcensus=%d n9=%d n8c2off=%d",
          g_diag.depthOff ? 1 : 0, g_diag.cullOff ? 1 : 0, g_diag.magenta ? 1 : 0, g_diag.picDump ? 1 : 0,
          g_diag.efbClear ? 1 : 0, g_diag.bufDump ? 1 : 0, g_diag.texDump ? 1 : 0, g_diag.shaderDump ? 1 : 0,
          g_diag.noMerge ? 1 : 0, g_diag.uvLog ? 1 : 0, g_diag.mtxLog ? 1 : 0, g_diag.perfTimer ? 1 : 0,
          g_diag.arrayUp ? 1 : 0, g_diag.cachedStaging ? 1 : 0, g_diag.storageCache ? 1 : 0, g_diag.fifoFast,
          g_diag.pipeRing ? 1 : 0, g_diag.usedRange ? 1 : 0, g_diag.watchdogAt, g_diag.traceIo ? 1 : 0,
          g_diag.traceCnt ? 1 : 0, g_diag.stallSvc == 1 ? "count" : (g_diag.stallSvc == 2 ? "burst" : (g_diag.stallSvc == 3 ? "probe" : "off")), g_diag.traceWake ? 1 : 0, g_diag.parkWatch ? 1 : 0, g_diag.seamFix ? 1 : 0, g_diag.padWatch ? 1 : 0, g_diag.fix2 ? 1 : 0, g_diag.heapWatch ? 1 : 0, g_diag.dispWatch ? 1 : 0, g_diag.safePoint ? 1 : 0, g_diag.spinWatch ? 1 : 0, g_diag.phaseTime ? 1 : 0, g_diag.dlCensus ? 1 : 0, g_diag.n9 ? 1 : 0, g_diag.n8c2off ? 1 : 0);
}

namespace {

constexpr uint32_t kSwapImages = 3;
constexpr uint32_t kQueueCommandMemory = 1u << 20;

DkSwapchain g_swapchain = nullptr;
DkImage g_swapImages[kSwapImages];
ImageAlloc g_swapMem[kSwapImages];
int g_debugMessages = 0;

// the heap malloc has never grown into, in MiB (libnx's heap end against malloc's break): deko3d's
// memory blocks come from it, so the difference across the set-up is its cost
size_t heap_never_used_mib() {
    char* top = static_cast<char*>(sbrk(0));
    if (!fake_heap_end || !top || top == reinterpret_cast<char*>(-1)) return 0;
    return size_t(fake_heap_end - top) >> 20;
}

// deko3d's messages: only the debug library calls this (the release library aborts with 2359-xxxx)
void debug_message(void*, const char* context, DkResult result, const char* message) {
    if (result != DkResult_Success) {
        fatal("deko3d error in %s: result %d: %s", context ? context : "?", int(result), message ? message : "");
    }
    const int n = g_debugMessages++;
    if (n < 200) {
        dklog("deko3d %s: %s", context ? context : "?", message ? message : "");
    } else if (n == 200) {
        dklog("deko3d: more messages, not logged");
    }
}

void init_swapchain() {
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) {
        DkImageLayoutMaker m;
        dkImageLayoutMakerDefaults(&m, R.device);
        m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
        m.format = DkImageFormat_RGBA8_Unorm;
        m.dimensions[0] = R.width;
        m.dimensions[1] = R.height;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &m);
        g_swapMem[i] = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
        dkImageInitialize(&g_swapImages[i], &layout, g_swapMem[i].block, g_swapMem[i].offset);
        images[i] = &g_swapImages[i];
    }
    DkSwapchainMaker m;
    dkSwapchainMakerDefaults(&m, R.device, nwindowGetDefault(), images, kSwapImages);
    dklog("creating the swapchain on the default window");
    log_flush();
    g_swapchain = dkSwapchainCreate(&m);
    dkSwapchainSetSwapInterval(g_swapchain, 1);
    dklog("swapchain: %u RGBA8 images of %ux%u, swap interval 1", kSwapImages, R.width, R.height);
}

}  // namespace

int swapchain_acquire(DkFence* fence) {
    int slot = 0;
    dkSwapchainAcquireImage(g_swapchain, &slot, fence);
    return slot;
}

void swapchain_wait(DkFence* fence) {
    check_queue("acquiring a swapchain image");
    dkQueueWaitFence(R.queue, fence);
}

const DkImage& swapchain_image(int slot) { return g_swapImages[slot]; }

void swapchain_present(int slot) {
    check_queue("the present");
    dkQueuePresentImage(R.queue, g_swapchain, slot);
    R.presents++;
}

bool env_flag(const char* name, bool def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}

void dklog(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    fprintf(stderr, "[dk] %s\n", line);
}

void log_flush() { fflush(stderr); }

// ---- clocks: clkrst readback (ported from spm-nx switch/deko/device.cpp spm_log_clocks) -----------
// Three clkrst sessions (CPU, GPU, memory), opened lazily on the first call and kept for the process.
// A rate that cannot be read prints as ERR(0x........) in its own slot: a readback that cannot read is
// itself a finding, never a silent gap. A number without its clocks is void (switch/H3-PLAN.md).
namespace {
constexpr PcvModuleId kClockModules[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
ClkrstSession g_clockSessions[3];
Result g_clockOpenRc[3] = {};
bool g_clockOpen[3] = {};
bool g_clockInitDone = false;
std::mutex g_clockMutex;

void clocks_init_once() {
    if (g_clockInitDone) return;
    g_clockInitDone = true;
    const Result initRc = clkrstInitialize();
    for (int i = 0; i < 3; ++i) {
        if (R_FAILED(initRc)) {
            g_clockOpenRc[i] = initRc;
            continue;
        }
        g_clockOpenRc[i] = clkrstOpenSession(&g_clockSessions[i], kClockModules[i], 3);
        g_clockOpen[i] = R_SUCCEEDED(g_clockOpenRc[i]);
    }
}
}  // namespace

void clocks_format(char* buf, size_t size) {
    std::lock_guard<std::mutex> lock(g_clockMutex);
    clocks_init_once();
    u32 hz[3] = {};
    bool ok[3] = {};
    char slot[3][24];
    for (int i = 0; i < 3; ++i) {
        ok[i] = g_clockOpen[i] && R_SUCCEEDED(clkrstGetClockRate(&g_clockSessions[i], &hz[i]));
        if (ok[i]) {
            std::snprintf(slot[i], sizeof slot[i], "%u.%u", unsigned(hz[i] / 1000000u),
                          unsigned(hz[i] / 100000u % 10u));
        } else {
            std::snprintf(slot[i], sizeof slot[i], "ERR(0x%08X)", unsigned(g_clockOpenRc[i]));
        }
    }
    // the certified handheld clocks: CPU 1020 MHz, GPU 384 MHz, memory 1331.2 MHz
    const bool cpuStock = ok[0] && hz[0] == 1020000000u;
    const bool memStock = ok[2] && hz[2] / 1000000u == 1331u;
    const char* verdict = "NONSTOCK";
    if (cpuStock && memStock && ok[1] && hz[1] == 384000000u)
        verdict = "STOCK";
    else if (cpuStock && memStock && ok[1] && hz[1] == 307200000u)
        verdict = "GPU-307-LOTTERY";
    std::snprintf(buf, size, "clk cpu/gpu/mem %s/%s/%s MHz %s", slot[0], slot[1], slot[2], verdict);
}

void clocks_log(const char* tag) {
    char clk[96];
    clocks_format(clk, sizeof clk);
    dklog("clocks %s: %s", tag, clk);
}

void fatal(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    fprintf(stderr, "[dk] FATAL: %s\n", line);
    log_flush();
    abort();
}

void check_queue(const char* before) {
    if (!dkQueueIsInErrorState(R.queue)) return;
    fatal("the deko3d queue is in an error state (a GPU fault) before %s, after %llu presents; deko3d messages %d",
          before, (unsigned long long)R.presents, g_debugMessages);
}

}  // namespace swdk

using namespace swdk;

// The live clocks for the debug server's "clocks" command (switch/source/fw_debug_server.cpp, which
// references this weakly: a build without the renderer answers that it has no readback).
extern "C" void aurora_switch_dk_clocks(char* buf, size_t size) { clocks_format(buf, size); }

extern "C" void aurora_switch_dk_init(void) {
    if (R.device) return;
    diag_init();
    const uint64_t t0 = now_ns();
    clocks_log("boot");
    const size_t heapBefore = heap_never_used_mib();
    dklog("deko3d renderer: Aurora's WebGPU calls replayed on deko3d; heap never used before the set-up %zu MiB",
          heapBefore);
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_message;
    // window origin top left, depth 0 to 1, clip-space y up: WebGPU's conventions (dk.h)
    dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
    dklog("creating the device");
    log_flush();
    R.device = dkDeviceCreate(&dm);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, R.device);
    // zcull off: SwitchWakerHD saw depth-biased decals lose whole tiles with it. Compute stays on:
    // Aurora's depth peek (GXPeekZ) is a compute pass.
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_MediumPrio | DkQueueFlags_DisableZcull;
    qm.commandMemorySize = kQueueCommandMemory;
    qm.flushThreshold = qm.commandMemorySize / 8;
    dklog("creating the queue");
    log_flush();
    R.queue = dkQueueCreate(&qm);

    memory_init();
    init_swapchain();
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        empty_image_init();
    }
    shaders_init();
    const size_t heapAfter = heap_never_used_mib();
    dklog("set-up cost: %zu MiB of heap (never used: %zu MiB before, %zu MiB after), %.0f ms",
          heapBefore > heapAfter ? heapBefore - heapAfter : 0, heapBefore, heapAfter, double(now_ns() - t0) / 1e6);
    log_flush();
}

extern "C" void aurora_switch_dk_shutdown(void) {
    if (R.queue == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        flush();
    }
    dkQueueWaitIdle(R.queue);
    if (g_swapchain != nullptr) {
        dkSwapchainDestroy(g_swapchain);  // gives the window's buffers back
        g_swapchain = nullptr;
    }
    dklog("shut down after %llu presents", (unsigned long long)R.presents);
}

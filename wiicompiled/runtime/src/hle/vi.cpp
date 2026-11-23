#include <bit>
#include <vector>
#include "frame_input.h"
#include "det_hash.h"
#include "timebase_contract.h"
#include "det_clock.h"
#include "hle_stubs.h"
#include "runtime_config.h"
#include "memory.h"
#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "settings_overlay.h"
#include "fiber_manager.h"
#include "platform/host_platform.h"
#include "runtime_log.h"
#include "guest_profiler.h"
#include "no100f_native_scene.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <aurora/gfx.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#include <aurora/aurora.h>

// Forward declaration for OSWakeupThread - used to wake threads on VI retrace queue
extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx);

// Forward declaration for OSSleepThread - used by VIWaitForRetrace HLE
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* cpu);
extern "C" int g_gxFrameCount;
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);

// Aurora frame cycle tracking - needs external linkage for the GX HLE
// (declared in gx_internal.h, consumed by gx_frame.cpp). We need to call
// aurora_begin_frame() before GX commands and aurora_end_frame() after.
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// N8 C7: vi clock reads (one counter for the present/wait path, one for the
// SleepPreciselyUntil spin loop). The 9 design-passes table lines are all
// inside env-gated diagnostic watchdog threads (off in flights); the real
// per-present reads are the Clock::now() sites below. Gated on the
// nativescene dk_diag key (No100fNativeSceneEnabled), one branch per site
// when off. Counters ride the measurement flight only, never a price build.
static std::atomic<uint64_t> s_n8ClockVi{0};
static std::atomic<uint64_t> s_n8ClockViSpin{0};
static Clock::time_point n8_vi_now() noexcept {
  if (No100fNativeSceneEnabled()) s_n8ClockVi.fetch_add(1, std::memory_order_relaxed);
  return Clock::now();
}
static Clock::time_point n8_vi_spin_now() noexcept {
  if (No100fNativeSceneEnabled()) s_n8ClockViSpin.fetch_add(1, std::memory_order_relaxed);
  return Clock::now();
}

constexpr uint32_t kGuestGxRenderModeCopyBytes = 0x39;

bool ReadGuestRenderModeObj(uint32_t guestPtr, GXRenderModeObj& out) {
    if (guestPtr == 0 || !Memory::Contains(guestPtr, kGuestGxRenderModeCopyBytes)) {
        return false;
    }

    try {
        out.viTVmode = static_cast<VITVMode>(Memory::Read32(guestPtr + 0x00));
        out.fbWidth = Memory::Read16(guestPtr + 0x04);
        out.efbHeight = Memory::Read16(guestPtr + 0x06);
        out.xfbHeight = Memory::Read16(guestPtr + 0x08);
        out.viXOrigin = Memory::Read16(guestPtr + 0x0a);
        out.viYOrigin = Memory::Read16(guestPtr + 0x0c);
        out.viWidth = Memory::Read16(guestPtr + 0x0e);
        out.viHeight = Memory::Read16(guestPtr + 0x10);
        out.xFBmode = static_cast<VIXFBMode>(Memory::Read32(guestPtr + 0x14));
        out.field_rendering = Memory::Read8(guestPtr + 0x18);
        out.aa = Memory::Read8(guestPtr + 0x19);

        for (size_t i = 0; i < 12; ++i) {
            out.sample_pattern[i][0] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2));
            out.sample_pattern[i][1] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2) + 1);
        }
        for (size_t i = 0; i < 7; ++i) {
            out.vfilter[i] = Memory::Read8(guestPtr + 0x32 + static_cast<uint32_t>(i));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    return true;
}

struct ViState {
    bool initialized = false;
    uint32_t tvFormat = 0; // VIGetTvFormat returns VI_NTSC before VIInit
    uint32_t nextFrameBuffer = 0;
    uint32_t currentFrameBuffer = 0;
    uint32_t retraceCount = 0;
    bool black = false;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t renderWidth = 640;
    uint32_t renderHeight = 480;
    uint32_t viXOrigin = 0;
    uint32_t viYOrigin = 0;
    uint32_t xfbWidth = 640;
    uint32_t xfbHeight = 480;
    bool fieldOdd = false;
    Clock::time_point lastRetrace = Clock::now();
    std::chrono::microseconds retraceInterval{16666us}; // ~60 Hz
    // Deterministic mode: the virtual tick of the last retrace (see det_clock.h); lastRetrace above
    // keeps real time for presentation only.
    uint64_t detLastRetraceTicks = 0;
    bool hasValidXfb = false; // True once we've received at least one GXCopyDisp
    uint32_t readyXfb = 0;    // XFB address from the most recent GXCopyDisp

    // VIConfigure/VISetNextFrameBuffer/VISetBlack only write pending values below; VIFlush arms them but
    // the commit happens at the next retrace, matching real VI hardware. A VIFlush called from a
    // pre-retrace callback therefore misses the imminent field and lands one field late.

    // Pending values (written by VISetNextFrameBuffer, VISetBlack, VIConfigure)
    uint32_t pendingNextFrameBuffer = 0;
    bool pendingBlack = false;
    // Matches the active-state default (NTSC before VIInit) and the pending
    // 16666us interval below; a flush before any VIConfigure must not commit
    // PAL timing onto an NTSC-interval state.
    uint32_t pendingTvFormat = 0;
    uint32_t pendingRenderWidth = 640;
    uint32_t pendingRenderHeight = 480;
    uint32_t pendingViXOrigin = 0;
    uint32_t pendingViYOrigin = 0;
    uint32_t pendingXfbWidth = 640;
    uint32_t pendingXfbHeight = 480;
    std::chrono::microseconds pendingRetraceInterval{16666us};
    // V-Sync: the interval the display lock wants for the next retrace (0 = the game's own interval).
    std::chrono::nanoseconds lockedInterval{0};
    
    // Set by VIFlush(); cleared after commit in AdvanceRetrace
    bool flushArmed = false;
};

std::mutex g_viMutex;
ViState g_vi;


// Guest-side state addresses used by the SDK's VI globals.
constexpr uint32_t kViInitializedFlagAddr   = 0x802713B3;  // NO100F: no IsInitialized in this SDK; encoderType (r13-0x6ED0) low byte, VIInit sets it to 1
constexpr uint32_t kViTvFormatAddr          = 0x802713DC;  // CurrTvMode (NO100F r13-0x6EA4)
constexpr uint32_t kViRenderWidthAddr       = 0x8026DE34;  // HorVer.DispSizeX (NO100F HorVer 0x8026DE30 + 4)
constexpr uint32_t kViRenderHeightAddr      = 0x8026DE36;  // HorVer.DispSizeY (+6)
constexpr uint32_t kViXfbWidthAddr          = 0x8026DE42;  // HorVer.FBSizeX (+0x12)
constexpr uint32_t kViXfbHeightAddr         = 0x8026DE4C;  // HorVer+0x1C (PanSizeY)
constexpr uint32_t kViRetraceCountAddr      = 0x80271398;  // retraceCount (NO100F r13-0x6EE8); // matches VIWaitForRetrace/handler
constexpr uint32_t kViTimingGuardAddr       = 0x8056dd0c;  // IsInitialized+0xC (offset-matched)
constexpr uint32_t kViPreRetraceCallback    = 0x802713A8;  // PreCB (NO100F r13-0x6ED8)
constexpr uint32_t kViPostRetraceCallback   = 0x802713AC;  // PostCB (NO100F r13-0x6ED4)
constexpr uint32_t kViNextFrameBufferAddr   = 0x8026DE60;  // NO100F: no CurrBufAddr in this SDK; VISetNextFrameBuffer writes HorVer.bufAddr
constexpr uint32_t kViNextFrameBufferHwAddr = 0x8026DE60;  // HorVer.bufAddr (+0x30)
constexpr uint32_t kViRetraceQueueAddr      = 0x802713A0;  // retraceQueue (NO100F r13-0x6EE0); // Thread queue for VIWaitForRetrace



// ============================================================================
// V-Sync display lock (Windows)
// ============================================================================
// With V-Sync on, presents wait for vblank. A game clock of exactly 60.000 Hz against a display that
// really runs at 59.94 (or 179.96 / 3) drops or repeats a frame every few seconds, and a full flip
// queue makes Present block. So the VI clock runs at exactly k display periods per retrace (k = 1 on a
// 60 Hz display, 3 at 180 Hz), with its phase pulled gently toward the middle of a vblank interval so
// each frame lands on the same vblank every time. The compositor reports the display's exact period
// and the time of its last vblank. Displays with no whole fraction near 60 (75, 144 Hz) keep the
// game's own clock.

struct DisplayLock {
    bool valid = false;
    int k = 0;
    std::chrono::nanoseconds period{0};
    Clock::time_point vblank{};
    Clock::time_point lastQuery{};
    bool logged = false;
};
DisplayLock g_displayLock;  // under g_viMutex

void RefreshDisplayLockLocked(Clock::time_point now) {
    DisplayLock& d = g_displayLock;
    if (!settings_overlay::VSync()) {
        d.valid = false;
        d.logged = false;
        return;
    }
    if (d.lastQuery != Clock::time_point{} && now - d.lastQuery < 250ms) {
        return;
    }
    d.lastQuery = now;
#if defined(_WIN32)
    using GetTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
    static const GetTimingFn getTiming = [] {
        HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        return dwm != nullptr ? reinterpret_cast<GetTimingFn>(
                                    reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmGetCompositionTimingInfo")))
                              : nullptr;
    }();
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    LARGE_INTEGER frequency{}, qpcNow{};
    const HRESULT result = getTiming != nullptr ? getTiming(nullptr, &info) : E_NOTIMPL;
    if (FAILED(result) || info.qpcRefreshPeriod == 0 || !::QueryPerformanceFrequency(&frequency) ||
        !::QueryPerformanceCounter(&qpcNow)) {
        if (!d.logged) {
            d.logged = true;
            RT_LOGF(RT_TAG_VI, "V-Sync: no display timing (0x%08lX); the game keeps its own clock\n",
                    static_cast<unsigned long>(result));
        }
        d.valid = false;
        return;
    }
    const auto steadyNow = Clock::now();
    const double nsPerTick = 1e9 / static_cast<double>(frequency.QuadPart);
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(info.qpcRefreshPeriod * nsPerTick));
    const double hz = 1e9 / static_cast<double>(period.count());
    const int k = static_cast<int>(std::lround(hz / 60.0));
    d.valid = k >= 1 && k <= 8 && std::fabs(hz / k - 60.0) < 60.0 * 0.02;
    d.k = k;
    d.period = period;
    d.vblank = steadyNow - std::chrono::nanoseconds(static_cast<int64_t>(
                               static_cast<double>(qpcNow.QuadPart - static_cast<LONGLONG>(info.qpcVBlank)) * nsPerTick));
    if (!d.logged) {
        d.logged = true;
        RT_LOGF(RT_TAG_VI, "V-Sync: display %.3f Hz, %s\n", hz,
                d.valid ? (std::string("game clock locked at ") + std::to_string(k) + " refresh(es) per frame").c_str()
                        : "no whole fraction near 60 Hz; the game keeps its own clock");
    }
#else
    d.valid = false;
#endif
}

// The interval to the retrace after `retrace`: k display periods, nudged toward the target phase.
std::chrono::nanoseconds LockedIntervalAfter(Clock::time_point retrace) {
    const DisplayLock& d = g_displayLock;
    if (!d.valid || d.period.count() <= 0) {
        return std::chrono::nanoseconds(0);
    }
    const int64_t period = d.period.count();
    // Presents go out about 6.5 ms after their retrace (aurora's kNativePresentOffsetNanos): put that
    // moment halfway between two vblanks, the most room either way.
    constexpr int64_t kPresentOffset = 6'500'000;
    int64_t target = (period / 2 - kPresentOffset) % period;
    if (target < 0) target += period;
    int64_t phase = (std::chrono::duration_cast<std::chrono::nanoseconds>(retrace - d.vblank).count()) % period;
    if (phase < 0) phase += period;
    int64_t error = phase - target;
    if (error > period / 2) error -= period;
    if (error < -period / 2) error += period;
    const int64_t correction = std::clamp<int64_t>(-error / 20, -100'000, 100'000);
    return std::chrono::nanoseconds(period * d.k + correction);
}

std::chrono::microseconds IntervalForFormat(uint32_t tvFormat) {
    // NTSC-ish defaults to 60 Hz; PAL uses 50 Hz.
    return tvFormat == 1 ? 20000us : 16666us;
}

// Shared busy-wait budget for deadline-precise sleeps (matches Aurora's
// presenter spin window). Larger windows burn a core for no visible gain.
constexpr std::chrono::microseconds kFinalSpinWindow{500};

void SleepPreciselyUntil(Clock::time_point deadline, bool finishWithSpin = false,
                         std::chrono::microseconds spinWindow = 750us) {
    const auto now = n8_vi_now();  // N8 C7: SleepPreciselyUntil entry read
    if (now >= deadline) {
        return;
    }
    const auto timerDeadline =
        finishWithSpin && deadline - now > spinWindow ? deadline - spinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static thread_local HighResolutionTimer timer;
    if (timer.handle != nullptr) {
        const auto remaining100ns =
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
            if (!finishWithSpin) {
                return;
            }
        }
    }
#endif
    if (!finishWithSpin) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    if (n8_vi_now() < timerDeadline) {  // N8 C7: pre-spin check read
        std::this_thread::sleep_until(timerDeadline);
    }
    // This runs only for the final fraction of a VI interval. Do not yield the
    // host thread here: a scheduler quantum is larger than the remaining
    // budget and would recreate the 17-18 ms sawtooth this path removes.
    while (n8_vi_spin_now() < deadline) {  // N8 C7: spin-loop iteration read
#if defined(_WIN32)
        YieldProcessor();
#endif
    }
}

void ViSetR3(CpuContext* ctx, uint32_t value)
{
    if (ctx) {
        ctx->gpr[3] = value;
    }
}

void WriteGuestStateLocked() {
    try {
        Memory::Write8(kViInitializedFlagAddr, 1);
        // (kViTimingGuardAddr: Wii __VIDimming flag, not in this SDK)
        Memory::Write32(kViTvFormatAddr, g_vi.tvFormat);
        Memory::Write16(kViRenderWidthAddr, static_cast<uint16_t>(g_vi.renderWidth));
        Memory::Write16(kViRenderHeightAddr, static_cast<uint16_t>(g_vi.renderHeight));
        Memory::Write16(kViXfbWidthAddr, static_cast<uint16_t>(g_vi.xfbWidth));
        Memory::Write16(kViXfbHeightAddr, static_cast<uint16_t>(g_vi.xfbHeight));
        Memory::Write32(kViRetraceCountAddr, g_vi.retraceCount);
        Memory::Write32(kViPreRetraceCallback, g_vi.preRetraceCallback);
        Memory::Write32(kViPostRetraceCallback, g_vi.postRetraceCallback);
        // Write PENDING frame buffer to guest memory so SDK code sees the queued value
        Memory::Write32(kViNextFrameBufferAddr, g_vi.pendingNextFrameBuffer);
        Memory::Write32(kViNextFrameBufferHwAddr, g_vi.pendingNextFrameBuffer);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "WriteGuestStateLocked", e);
    }
}

// The interval to the next retrace: the display lock's while V-Sync holds one (NTSC only).
std::chrono::nanoseconds ActiveIntervalLocked() {
    if (g_vi.lockedInterval.count() > 0 && g_vi.tvFormat != 1) {
        return g_vi.lockedInterval;
    }
    return g_vi.retraceInterval;
}

uint64_t IntervalTicks(std::chrono::microseconds interval) {
    return TimeBaseContract::NanosecondsToTicks(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count()));
}

void EnsureInitializedLocked() {
    if (g_vi.initialized) {
        return;
    }
    g_vi.initialized = true;
    g_vi.retraceInterval = IntervalForFormat(g_vi.tvFormat);
    g_vi.lastRetrace = Clock::now();
    g_vi.detLastRetraceTicks = DetClock::Now();
    WriteGuestStateLocked();
}

// GXRenderModeObj::viTVmode encodes the output family (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60) in bits [4:2].
uint32_t ExtractTvFormat(uint32_t tvMode) {
    return (tvMode >> 2) & 0x7;
}

// Re-entry guard to prevent AdvanceRetrace calling itself via OSWakeupThread -> SelectThread
static std::atomic<bool> s_inAdvanceRetrace{false};

// Set while VI_HLE_PresentFrame runs its seal/pace/pre-warm sequence. Guest callbacks
// serviced during that window (pace-loop alarms, GX timing polls) still see the stale
// hasValidXfb/g_auroraFrameActive flags; a retrace-context present fired from them would
// end the freshly pre-warmed empty frame and show it as a black frame group.
static std::atomic<bool> s_presentSequenceActive{false};

// Bring-up: frames the guest actually handed to the presenter (an XFB was ready), and a one-shot
// watchdog that dumps every guest thread's state if none arrived after 12 s and again after 30 s, so
// a boot that stalls in a wait loop shows where each thread is parked.
static std::atomic<uint32_t> s_presentedXfbFrames{0};
// Heapwatch trip wire (JUNCTION-CALLBACK-DESIGN J4.2, Nov 17, 2026): the
// heap-list canary's sampler thread sets this at its first detected
// out-of-window link. The sampler otherwise keeps its trip state private;
// this single atomic is the minimum host-visible surface, and its only
// consumer is the stall service's probe mode (stallsvc=probe), whose stop
// checks must see a trip the moment the canary does. Nothing else reads or
// writes it.
static std::atomic<bool> s_heapWatchTripped{false};
static bool HeapWatchTripped() { return s_heapWatchTripped.load(std::memory_order_relaxed); }

// Dispwatch (COPYDISP-STUDY D4, Nov 17, 2026): the display-state instrument
// the CopyDisp study specced to decide its one residue - whether the display
// module's op dispatcher (guest func_80175FC4) is ever entered post-cliff,
// with which ops, and what the display pool / current-mode state reads at
// each movie boundary. Two halves, both read-only:
//   - DispWatch_HLE_OnDispatcherEntry, called from the dispatcher's own
//     prologue in the generated shard (the dispatcher has exactly one
//     definition and every entry path - indirect dispatch, the raw dispatch
//     tables, registration dispatch - lands on its first instruction, so a
//     prologue call is complete by construction where a hook inside
//     InvokeIndirectCpu would cover only the indirect path). It reads the
//     op index (ctx r3) and the argument pointer (ctx r4) off the host
//     context and snapshots the display words below via TryRead32. It
//     executes no guest code, writes no guest memory, and holds no lock.
//   - a sampler thread in StartStallWatchdogOnce on the heapwatch cadence
//     (250 ms) that change-logs the same state plus the current-mode
//     struct's geometry halfwords.
// Armed by dk_diag dispwatch (SCOOBY_DISPWATCH, plumbed from
// switch/deko/device.cpp). Unarmed, the sampler thread does not exist and
// the entry helper returns on its arm check: behaviour identical to the
// durability build.
// Display words (guest SDA base r13 = 0x80278280; derivations in
// COPYDISP-STUDY D2 and ADVANCER-INVOKE-DESIGN A1): the framebuffer pool
// the setup op (loc_80176204) is the only writer of - base 0x802716BC,
// token 0x802716F4, F8 0x802716F8 (the presenter's GXCopyDisp destination,
// read at shard loc_80176C64 from r13 - 27528), P0 0x802716FC, P1
// 0x80271700, fb0 0x80271704 - the presenter site-2 flag 0x802716D0, the
// current-mode pointer 0x80271710 (one of the static 60-byte mode structs
// at 0x801ED290 / 0x801ED2CC / 0x801ED308; +4 = fbWidth, +6 = efbHeight,
// +8 = xfbHeight), and the config pointer the setup op stores at
// 0x80271708.
static bool DispWatchArmed() {
    static const bool armed = std::getenv("SCOOBY_DISPWATCH") != nullptr;
    return armed;
}
// One epoch for every [dispwatch] line's t= field, the entry log's and the
// sampler's alike, so timestamps compare across lines (the Fix2ElapsedMs
// convention).
static long long DispWatchElapsedMs() {
    static const Clock::time_point epoch = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count();
}
static std::atomic<uint32_t> s_dispWatchEntries{0};
static std::atomic<bool> s_dispWatchCapLogged{false};
constexpr uint32_t kDispWatchEntryLogCap = 20000u;
// The nine watched words, in log order: pool base, token, F8, P0, P1, fb0,
// the site-2 flag, the current-mode pointer, the config pointer. Every read
// is a non-faulting TryRead32; any failure fails the whole snapshot and the
// caller skips silently (the heapwatch discipline - guest memory not yet up
// or being torn down is not a state worth logging).
static bool DispWatchTryReadWords(uint32_t w[9]) {
    constexpr uint32_t kAddrs[9] = {0x802716BCu, 0x802716F4u, 0x802716F8u, 0x802716FCu,
                                    0x80271700u, 0x80271704u, 0x802716D0u, 0x80271710u,
                                    0x80271708u};
    for (int i = 0; i < 9; ++i) {
        if (!Memory::TryRead32(kAddrs[i], w[i])) {
            return false;
        }
    }
    return true;
}
extern "C" void DispWatch_HLE_OnDispatcherEntry(CpuContext* ctx) {
    if (!DispWatchArmed()) {
        return;
    }
    const uint32_t n = s_dispWatchEntries.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > kDispWatchEntryLogCap) {
        // Log every entry up to the cap, then say so once; suppressed
        // entries stay counted in s_dispWatchEntries, which the sampler's
        // steady lines carry, so nothing is silently sampled away.
        if (!s_dispWatchCapLogged.exchange(true, std::memory_order_relaxed)) {
            RT_LOGF(RT_TAG_VI,
                    "[dispwatch] entry log cap reached t=%lldms entries=%u (further entries counted, not logged)\n",
                    DispWatchElapsedMs(), n);
        }
        return;
    }
    uint32_t w[9];
    if (!DispWatchTryReadWords(w)) {
        return;
    }
    RT_LOGF(RT_TAG_VI,
            "[dispwatch] op=%u arg=0x%08X t=%lldms frames=%u pool=[bc=%08X tok=%08X f8=%08X p0=%08X p1=%08X fb0=%08X] flag=%u mode=0x%08X cfg=0x%08X\n",
            ctx ? ctx->gpr[3] : 0, ctx ? ctx->gpr[4] : 0, DispWatchElapsedMs(),
            s_presentedXfbFrames.load(std::memory_order_relaxed), w[0], w[1], w[2], w[3], w[4],
            w[5], w[6], w[7], w[8]);
}
extern "C" { extern int g_gxDrawDump; extern int g_gxCullOff; extern unsigned g_gxDrawIdx; extern unsigned g_gxDrawLimit; }  // aurora command_processor (Diagnostic)
// switch/source/fw_switch_init.cpp: true once userAppExit has begun and guest memory is
// about to be unmapped. Weak: a frontend without that exit path leaves it undefined, and
// the stall service's burst stop check then treats the process as never exiting.
extern "C" bool fw_switch_exiting(void) __attribute__((weak));
static uint32_t GxPresentedFramesLocal() { return s_presentedXfbFrames.load(std::memory_order_relaxed); }
static uint32_t (*const g_gxFramesFn)() = &GxPresentedFramesLocal;

// Executor safepoint (SAFEPOINT-DESIGN, Nov 17, 2026; Route B proving
// build): the junction wait (JUNCTION-CALLBACK-DESIGN J3) is a guest
// thread spinning in a call-free poll on a callback-maintained flag, and
// every foreign-thread delivery of the callbacks it waits for is
// convicted of planting the wild heap cell 0x389dfff4 (v1, v2, burst,
// probe - F-J1). The one executor the evidence has never touched is the
// spinner itself. Route B hand-emits a check at the two call-free
// back-edges of the convicted poll (guest func_800D8164, shard_9e6d: the
// wait-clear cycle at 0x800D859C and the wait-set cycle at 0x800D85B4):
//     if (GuestSafepointRequested()) GuestSafepointServe(ctx, <target>);
// immediately before each loop's goto. No register write-back or reload
// brackets the call (study S1): the delivery entries seed their private
// callback context from the ambient ctx's r1/r2/r13/FP mode, which are
// ABI-invariant inside a function body, and the spinner's remaining
// registers are host locals an ordinary host call preserves.
// The gate lives in the stall-service thread (below): it replicates the
// probe's gate and, on completion, sets the request atomic and records
// the episode bases - it delivers nothing itself (the count mode's
// discipline). The handler below runs the probe's dose programme from
// the spinner's own code, under the deferred-callback scope, so a wake
// and everything it releases are strictly ordered after the delivery on
// the one thread that was already executing guest code (study S2/S4).
// Armed by dk_diag safepoint (SCOOBY_SAFEPOINT, plumbed from
// switch/deko/device.cpp). Unarmed, the gate never sets the request and
// the handler never runs; the emitted checks remain compiled into the
// shard regardless (one relaxed atomic load + one predicted branch per
// poll-loop iteration), which the build log states plainly.
// Route B-2 (Nov 17, 2026): spinwatch named the junction spinner -
// func_800D875C, a calibrated delay loop on stream counter 0x80271024,
// spinning inside AdvanceRetrace's pre-callback housekeeping chain
// (SPINWATCH2-BOOT-LOG). Checks now also sit at the two delay loops'
// back-edges (func_800D875C inner 0x800D8784 / outer 0x800D877C,
// func_800D85EC inner 0x800D86AC / outer 0x800D86A0 - each serve target
// is its branch's own goto target, the as-built rule). A trip there
// reads the retrace guard SET, so the dose programme is guard-aware
// (fw-legality-delayloop): audio-first while the guard stands (the
// audio poll is the only class that can advance the stream counter),
// retrace doses held unspent and uncounted until a guard-clear serve,
// and a polls-exhausted guard-set close records the retrace class as
// GUARD-BLOCKED/untested rather than F-SP2. The witness set gains the
// stream counter and the op counter: the counter advancing is the
// delay loops' release, attributable to the dose that caused it.
// Route B-3 (Nov 18, 2026): episode re-arm. The B-2 proving boot woke
// its spinner on dose 1 - and the released chain wedged again ~2 s
// later at the chain's next callback-maintained wait, refused by the
// one-episode-per-boot rule (B2-BOOT-LOG). Policy only; the mechanism
// is unchanged: after an episode closes (wake, no-wake, guard-blocked
// close) the gate returns to watching, and a new episode opens only
// on a FRESH silence window - the gate's conditions re-evaluated, the
// episode bases recorded at that open, the feeders frozen together
// for the gate's >= 2 s silence. Per-episode dose counters reset at
// each open (the handler keys its dose machine off the gate's episode
// number); the per-episode caps (8 pairs / 16 polls) are unchanged.
// Boot cap: at most 16 episodes; the close of the 16th logs the cap
// line and dosing stops for the boot. Spinwatch's shot re-arms with
// the gate (one shot per episode), so every wedge is named. The
// permanent stops are unchanged: a heapwatch trip suppresses all
// dosing for the boot, and the refusal conditions stand.
// Route B-4 (Nov 18, 2026): dose-depth continuation. The B-3 proving
// boot ran sixteen episodes, every one a dose-1 wake on the counter
// witness - and the counter reached only 80 of the delay's 140 ticks
// (B3-BOOT-LOG): the wedge is ONE delay loop (func_800D875C),
// re-entered per delivered retrace, and the wake check was closing
// the episode on the first tick movement, buying one retrace per
// ~2 s silence window. Now, when a dose's wake witness is the stream
// counter specifically and frames remain frozen at the post-dose
// observation, the episode does NOT close: the handler stays in the
// episode and keeps delivering pairs at the programme's >= 1 s
// observation spacing (never the 100 ms multiplicity rate),
// re-witnessing after each dose, until frames advance (a wake, as
// before), a dose moves nothing (the continuation ends and the
// standard programme proceeds from that point), or the per-episode
// pair cap is reached with the counter still short (a distinct
// dose-depth cap close). Counter movement seen between doses in
// continuation (a released pass feeding the delay) is likewise not
// a wake; the counter witness base re-bases so each dose is measured
// on its own movement. The continuation delivers pairs only: a
// guard-set serve holds the pair unspent and uncounted, per the
// Route B-2 rule, and the continuation waits for a guard-clear
// serve. The boot cap rises to 32 episodes as a backstop; the caps
// per episode (8 pairs / 16 polls) are unchanged.
// One epoch for every [safepoint] handler line's t= field (the
// DispWatchElapsedMs convention; the gate's lines use the service
// thread's own epoch, as the probe's do).
static long long SafepointElapsedMs() {
    static const Clock::time_point epoch = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count();
}
// The gate's spinner: the guest main thread (the park machinery's
// kParkMainThreadAddr in the service thread; the record address is the
// identity the handler checks GetCurrentGuestThread() against).
constexpr uint32_t kSafepointSpinnerThread = 0x8026D568u;
// The J3 candidate flag func_800D8164 polls (the probe's kProbeFlagAddr).
constexpr uint32_t kSafepointFlagAddr = 0x80271020u;
// The stream counter the delay loops wait on (SPINNER-PC Q2; the
// spinwatch witness word) and func_800D8164's op counter (Route B-2
// witness words; same addresses the spinwatch block reads below).
constexpr uint32_t kSafepointCounterAddr = 0x80271024u;
constexpr uint32_t kSafepointOpCounterAddr = 0x8027101Cu;
// Boot cap on safepoint episodes (Route B-3; raised 16 -> 32 by
// Route B-4 as a backstop behind dose-depth continuation): the gate
// opens at most this many episodes per boot, then dosing stops.
constexpr uint32_t kSafepointMaxEpisodes = 32u;
// Request + episode state shared between the gate (service thread, the
// only writer of the bases, the episode number and the request) and the
// handler (scheduler thread, the only writer of the close). The handler
// clears the request exactly once, at the episode's close; the gate
// watches for the close and re-arms (Route B-3), storing the next
// episode's number below before it sets that episode's request.
static std::atomic<bool> s_safepointRequested{false};
static std::atomic<bool> s_safepointSuppressed{false};  // heapwatch trip: never serve again this boot
static std::atomic<uint32_t> s_safepointBaseFrames{0};
static std::atomic<uint32_t> s_safepointBaseEnqueues{0};
static std::atomic<uint32_t> s_safepointBaseFlag{0};
static std::atomic<uint32_t> s_safepointEpisode{0};  // current episode number (1-based once the gate opens one)
extern "C" bool GuestSafepointRequested() {
    return s_safepointRequested.load(std::memory_order_acquire);
}
// One full safepoint snapshot: the four feeders plus the J3 flag and the
// display-sync words, read host-side only (the probe's probeSample set).
struct SafepointSnap {
    uint32_t frames = 0, retrace = 0, aiCb = 0, enqueues = 0, flag = 0;
    uint32_t counter = 0, opCounter = 0;
    TraceWakeSync sync{};
};
static SafepointSnap SafepointSample() {
    SafepointSnap s;
    s.frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        s.retrace = g_vi.retraceCount;
    }
    s.aiCb = Audio_HLE_TraceAiCallbacks();
    s.enqueues = Dvd_HLE_TraceEnqueues();
    MemoryInline::TryReadGuestScalar(kSafepointFlagAddr, s.flag);
    MemoryInline::TryReadGuestScalar(kSafepointCounterAddr, s.counter);
    MemoryInline::TryReadGuestScalar(kSafepointOpCounterAddr, s.opCounter);
    s.sync = OS_HLE_TraceWakeReadSync();
    return s;
}
// Close the episode from the handler side: drop the request so the
// emitted checks go quiet, whatever the close reason.
static void SafepointCloseEpisode() {
    s_safepointRequested.store(false, std::memory_order_release);
}
extern "C" void GuestSafepointServe(CpuContext* ctx, uint32_t backEdgeTarget) {
    (void)ctx;  // the delivery entries seed from the ambient context themselves (study S1)
    // A callback that itself enters a call-free spin while a dose is in
    // flight must not nest a second dose inside the first (study S4's one
    // new guard). The deferred-callback counter the dose runs under is a
    // count, re-entrant by design; this flag guards the dose machine.
    static thread_local bool inDelivery = false;
    if (inDelivery) {
        return;
    }
    if (!s_safepointRequested.load(std::memory_order_acquire)) {
        return;  // stale trip: the episode already closed
    }
    // Identity (study S3): the tripping thread must be the gate's
    // spinner, and (proving build) the back-edge must be one of the
    // convicted cycles - the 8164 flag poll's pair, or a delay loop's
    // back-edge (Route B-2). Anything else is logged once, and the
    // request stands for the real spinner.
    static std::atomic<bool> wrongThreadLogged{false};
    const uint32_t currentThread = Fiber::GuestFiberManager::GetCurrentGuestThread();
    if (currentThread != kSafepointSpinnerThread) {
        if (!wrongThreadLogged.exchange(true, std::memory_order_relaxed)) {
            RT_LOGF(RT_TAG_VI,
                    "[safepoint] trip ignored: thread=0x%08X target=0x%08X (not the gate's spinner)\n",
                    currentThread, backEdgeTarget);
        }
        return;
    }
    static std::atomic<bool> wrongTargetLogged{false};
    if (backEdgeTarget != 0x800D859Cu && backEdgeTarget != 0x800D85B4u &&
        backEdgeTarget != 0x800D8784u && backEdgeTarget != 0x800D877Cu &&
        backEdgeTarget != 0x800D86ACu && backEdgeTarget != 0x800D86A0u) {
        if (!wrongTargetLogged.exchange(true, std::memory_order_relaxed)) {
            RT_LOGF(RT_TAG_VI,
                    "[safepoint] trip ignored: target=0x%08X (not a convicted poll cycle)\n",
                    backEdgeTarget);
        }
        return;
    }
    // Dose machine state. Only the spinner's own trips reach here, on
    // the one scheduler thread, so plain statics are single-threaded
    // state. Phases mirror the probe's: 0 awaiting dose 1, 1 observing
    // after dose 1, 2 observing after dose 2, 3 capped multiplicity.
    static int spPhase = 0;
    static uint32_t spPairs = 0, spPolls = 0;  // cumulative deliveries this episode
    static uint32_t spDose = 0;                // dose number of the latest delivery (1, 2 or 3)
    static Clock::time_point spLastDose{};     // host time of the latest delivery
    static Clock::time_point spLastSample{};   // last witness sample (10 ms throttle)
    static Clock::time_point spLastSummary{};  // last 1 s observation summary
    static bool spHaveSample = false;
    static bool spSawGuardClear = false;   // a dose-decision serve read the retrace guard clear
    static bool spHoldLogged = false;      // the polls-capped hold line (once)
    static bool spDepthContinue = false;   // Route B-4: dose-depth continuation is feeding the delay loop
    static uint32_t spBaseCounter = 0, spBaseOpCounter = 0;  // handler-side bases for the B-2 witness words
    static bool spHaveBase = false;
    static uint32_t spEpisodeServed = 0;  // episode the dose machine's state belongs to
    // Route B-3: a fresh episode restarts the dose machine from zero -
    // per-episode counters, phase, guard/hold latches and handler-side
    // bases reset on the first serve of each new episode. The gate
    // stores the episode number before it sets the request, so the
    // first trip of an episode always observes the new number.
    const uint32_t spEpisode = s_safepointEpisode.load(std::memory_order_relaxed);
    if (spEpisode != spEpisodeServed) {
        spEpisodeServed = spEpisode;
        spPhase = 0;
        spPairs = 0;
        spPolls = 0;
        spDose = 0;
        spHaveSample = false;
        spSawGuardClear = false;
        spHoldLogged = false;
        spDepthContinue = false;
        spHaveBase = false;
    }
    const auto now = Clock::now();
    // Stop checks before anything else: a canary trip suppresses the
    // safepoint permanently for the boot (the probe's abort, mirrored);
    // process exit closes the episode. Neither ever delivers again.
    if (HeapWatchTripped() || s_safepointSuppressed.load(std::memory_order_relaxed)) {
        s_safepointSuppressed.store(true, std::memory_order_relaxed);
        RT_LOGF(RT_TAG_VI, "[safepoint] abort: heapwatch trip ep=%u dose=%u pairs=%u polls=%u\n",
                spEpisodeServed, spDose, spPairs, spPolls);
        SafepointCloseEpisode();
        return;
    }
    if (fw_switch_exiting && fw_switch_exiting()) {
        RT_LOGF(RT_TAG_VI, "[safepoint] closed: process exiting\n");
        SafepointCloseEpisode();
        return;
    }
    // Witness sampling, throttled to 10 ms of host time: the handler is
    // entered at spin rate while the request stands, and sampling the VI
    // lock on every trip would contend with the scheduler for no
    // information - every timescale in the programme (100 ms rate limit,
    // 1 s observation windows) is far above the throttle, and the loop's
    // own flag re-read sees a release the instant the handler returns.
    // The post-dose sample below always runs.
    if (spHaveSample && now - spLastSample < std::chrono::milliseconds(10)) {
        return;
    }
    spHaveSample = true;
    spLastSample = now;
    const SafepointSnap s = SafepointSample();
    if (!spHaveBase) {
        spHaveBase = true;
        spBaseCounter = s.counter;
        spBaseOpCounter = s.opCounter;
    }
    // The retrace guard at this serve (fw-legality-delayloop): a trip
    // from a delay loop fires inside AdvanceRetrace's own callback
    // chain and reads the guard set; the dose programme below is
    // chosen by it.
    const bool spGuardSet = VI_HLE_IsAdvancingRetrace();
    // Wake witnesses, against the gate's episode bases (the probe's
    // three): frames, enqueues, the J3 flag. Movement seen before a dose
    // is the wake-before-dose (the F-J3 shape, benign here: the guest
    // released itself); movement after a dose attributes to that dose.
    const char* spWitness = nullptr;
    bool spWitnessIsCounter = false;
    if (s.frames != s_safepointBaseFrames.load(std::memory_order_relaxed)) {
        spWitness = "frames";
    } else if (s.enqueues != s_safepointBaseEnqueues.load(std::memory_order_relaxed)) {
        spWitness = "enqueues";
    } else if (s.flag != s_safepointBaseFlag.load(std::memory_order_relaxed)) {
        spWitness = "flag";
    } else if (s.counter != spBaseCounter) {
        spWitness = "counter71024";
        spWitnessIsCounter = true;
    } else if (s.opCounter != spBaseOpCounter) {
        spWitness = "op7101C";
    }
    if (spWitness && spDepthContinue && spWitnessIsCounter) {
        // Route B-4: in dose-depth continuation, counter movement with
        // frames still frozen (frames is the chain's first witness and
        // did not fire) is the delay being fed between doses - a
        // released pass - not a wake. Re-base so the next dose is
        // measured on its own movement, and fall through to the dose
        // decisions below.
        spBaseCounter = s.counter;
        spWitness = nullptr;
    }
    if (spWitness) {
        if (spDose == 0) {
            RT_LOGF(RT_TAG_VI,
                    "[safepoint] wake-before-dose ep=%u witness=%s frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u counter71024=%u op7101C=%u\n",
                    spEpisodeServed, spWitness, s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                    s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch, s.counter, s.opCounter);
        } else {
            RT_LOGF(RT_TAG_VI,
                    "[safepoint] wake ep=%u dose=%u witness=%s pairs=%u polls=%u frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u counter71024=%u op7101C=%u\n",
                    spEpisodeServed, spDose, spWitness, spPairs, spPolls, s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                    s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch, s.counter, s.opCounter);
        }
        SafepointCloseEpisode();
        return;
    }
    // Observation summaries, 1 s cadence (the probe's obs lines).
    if (spDose != 0 && now - spLastSummary >= std::chrono::seconds(1)) {
        spLastSummary = now;
        RT_LOGF(RT_TAG_VI,
                "[safepoint] obs t=%lldms ep=%u dose=%u frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u counter71024=%u op7101C=%u\n",
                SafepointElapsedMs(), spEpisodeServed, spDose, s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch, s.counter, s.opCounter);
    }
    // Dose decisions, guard-aware (Route B-2; fw-legality-delayloop).
    // A guard-clear serve runs the probe's programme as built: dose 1
    // fires on the first trip that reaches here (a retrace pair), dose
    // 2 after a >= 1 s observation (an audio poll), then dose 3 one
    // delivery per >= 100 ms (the study's rate limit), pairs first to
    // the cumulative 8, then polls to 16 (doses 1-2 count toward the
    // caps, as in the probe). Alarms are excluded, matching the probe:
    // the junction's alarm queue was empty in every observation.
    // A guard-set serve fired inside AdvanceRetrace's own callback
    // chain (the delay loops' context): a retrace pair there is a
    // provable no-op under the re-entry guard, so it is HELD UNSPENT -
    // never attempted, never counted - and the serve delivers an audio
    // poll instead (the only class that can advance the delay loops'
    // stream counter). Multiplicity in a guard-set context is polls
    // only; if a later serve reads the guard clear, the standard
    // programme resumes from the counters as they stand.
    bool deliver = false;
    bool retraceClass = false;
    uint32_t thisDose = 0;
    if (!spGuardSet) {
        spSawGuardClear = true;
    }
    // Route B-4: a continuation delivery does not advance the standard
    // programme's phase - if the continuation ends (a dose moves
    // nothing), the programme proceeds from the phase as it stands.
    const bool depthDelivery = spDepthContinue;
    if (spDepthContinue) {
        // Dose-depth continuation (Route B-4): the last dose fed the
        // delay loop and the guest is still frozen, so the episode
        // stays open and keeps delivering pairs at the programme's
        // >= 1 s observation spacing, bounded by the per-episode pair
        // cap. A guard-set serve holds the pair unspent and uncounted
        // (the Route B-2 rule); the continuation waits for a
        // guard-clear serve.
        if (spPairs >= 8) {
            RT_LOGF(RT_TAG_VI, "[safepoint] ep=%u dose-depth cap: counter71024=%u frames frozen\n",
                    spEpisodeServed, s.counter);
            SafepointCloseEpisode();
            return;
        }
        if (!spGuardSet && now - spLastDose >= std::chrono::seconds(1)) {
            deliver = true; thisDose = spDose + 1; retraceClass = true;
        }
    } else if (spPhase == 0) {
        deliver = true; thisDose = 1;
        retraceClass = !spGuardSet;
    } else if (spPhase == 1) {
        if (now - spLastDose >= std::chrono::seconds(1)) {
            deliver = true; thisDose = 2;
            // The standard dose 2 is the audio poll. If dose 1 was
            // spent as a guard-set poll (pairs still 0), the pair the
            // programme leads with is still owed: a guard-clear serve
            // delivers it here, and the programme resumes.
            retraceClass = !spGuardSet && spPairs == 0 && spPolls != 0;
        }
    } else if (spPhase == 2) {
        if (now - spLastDose >= std::chrono::seconds(1)) {
            spPhase = 3;  // multiplicity begins on this same trip (below)
        }
    }
    if (!spDepthContinue && spPhase == 3 && now - spLastDose >= std::chrono::milliseconds(100)) {
        if (spPairs >= 8 && spPolls >= 16) {
            // Full dose, no wake: F-SP2's shape (both classes tested).
            // The episode closes; the gate re-arms on a fresh silence
            // window (Route B-3).
            RT_LOGF(RT_TAG_VI, "[safepoint] no-wake at full dose ep=%u pairs=%u polls=%u\n", spEpisodeServed, spPairs, spPolls);
            SafepointCloseEpisode();
            return;
        }
        if (spPolls >= 16 && spPairs == 0 && !spSawGuardClear) {
            // Polls exhausted with the guard set throughout: the
            // retrace class was never testable in this episode, so the
            // close records it GUARD-BLOCKED/untested - F-SP2 must not
            // be claimable against an untested class.
            RT_LOGF(RT_TAG_VI,
                    "[safepoint] no-wake: retrace class guard-blocked (untested) ep=%u pairs=%u polls=%u\n",
                    spEpisodeServed, spPairs, spPolls);
            SafepointCloseEpisode();
            return;
        }
        if (spGuardSet) {
            if (spPolls < 16) {
                deliver = true; thisDose = 3; retraceClass = false;
            } else if (!spHoldLogged) {
                // Polls capped, pairs still owed, guard still set:
                // hold for a guard-clear serve (no delivery, no close).
                spHoldLogged = true;
                RT_LOGF(RT_TAG_VI,
                        "[safepoint] polls capped ep=%u pairs=%u polls=%u: pairs held for a guard-clear serve\n",
                        spEpisodeServed, spPairs, spPolls);
            }
        } else {
            deliver = true; thisDose = 3;
            retraceClass = spPairs < 8;
        }
    }
    if (!deliver) {
        return;
    }
    // Refuse at the delivery point if interrupts are disabled: the
    // delivery entries early-return on that anyway (hardware would pend,
    // and the guest's re-enable edge delivers through the existing path),
    // so the episode closes rather than spending doses into a no-op.
    if (!OS_HLE_InterruptsEnabled()) {
        RT_LOGF(RT_TAG_VI,
                "[safepoint] refused: interrupts disabled ep=%u dose=%u pairs=%u polls=%u frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u counter71024=%u op7101C=%u\n",
                spEpisodeServed, thisDose, spPairs, spPolls, s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch, s.counter, s.opCounter);
        SafepointCloseEpisode();
        return;
    }
    // Deliver one dose, under the deferred-callback scope (the seam
    // pass's discipline): no fiber switch can occur inside, and wake
    // effects land as pending-mask state the guest honours at its own
    // next switch point. The entries themselves also hold the scope; the
    // counter is a count, re-entrant by design.
    const SafepointSnap pre = s;
    inDelivery = true;
    OS_HLE_BeginDeferredGuestCallbacks();
    if (retraceClass) {
        VI_HLE_ProcessRetracesDeferred(1);
        ++spPairs;
    } else {
        Audio_HLE_PollDeferred();
        ++spPolls;
    }
    OS_HLE_EndDeferredGuestCallbacks();
    inDelivery = false;
    spDose = thisDose;
    spLastDose = now;
    if (!depthDelivery) {
        if (spPhase == 0) {
            spPhase = 1;
        } else if (spPhase == 1) {
            spPhase = 2;
        }
    }
    const SafepointSnap post = SafepointSample();
    RT_LOGF(RT_TAG_VI,
            "[safepoint] dose %u ep=%u class=%s pairs=%u polls=%u guard=%d pre: frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X counter71024=%u op7101C=%u A=%u B=%u arm=%u latch=%u post: frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X counter71024=%u op7101C=%u A=%u B=%u arm=%u latch=%u\n",
            thisDose, spEpisodeServed, retraceClass ? "retrace" : "audio", spPairs, spPolls, spGuardSet ? 1 : 0,
            pre.frames, pre.retrace, pre.aiCb, pre.enqueues, pre.flag, pre.counter, pre.opCounter,
            pre.sync.counterA, pre.sync.counterB, pre.sync.arm, pre.sync.latch,
            post.frames, post.retrace, post.aiCb, post.enqueues, post.flag, post.counter, post.opCounter,
            post.sync.counterA, post.sync.counterB, post.sync.arm, post.sync.latch);
    // Post-dose witness check (study S2 step 3): movement against the
    // bases is the wake, attributed to this dose.
    const char* postWitness = nullptr;
    bool postWitnessIsCounter = false;
    if (post.frames != s_safepointBaseFrames.load(std::memory_order_relaxed)) {
        postWitness = "frames";
    } else if (post.enqueues != s_safepointBaseEnqueues.load(std::memory_order_relaxed)) {
        postWitness = "enqueues";
    } else if (post.flag != s_safepointBaseFlag.load(std::memory_order_relaxed)) {
        postWitness = "flag";
    } else if (post.counter != spBaseCounter) {
        postWitness = "counter71024";
        postWitnessIsCounter = true;
    } else if (post.opCounter != spBaseOpCounter) {
        postWitness = "op7101C";
    }
    if (postWitness && postWitnessIsCounter) {
        // Route B-4 dose-depth continuation: this dose fed the delay
        // loop - the stream counter moved - but frames remain frozen
        // (frames is the chain's first witness and did not fire), so
        // this is not a wake. The episode stays open: re-base the
        // counter witness so the next dose is measured on its own
        // movement, and the dose decisions above deliver the next
        // pair after the >= 1 s observation interval.
        spDepthContinue = true;
        spBaseCounter = post.counter;
        spLastSummary = now;
        return;
    }
    if (postWitness) {
        RT_LOGF(RT_TAG_VI,
                "[safepoint] wake ep=%u dose=%u witness=%s pairs=%u polls=%u frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u counter71024=%u op7101C=%u\n",
                spEpisodeServed, spDose, postWitness, spPairs, spPolls, post.frames, post.retrace, post.aiCb, post.enqueues, post.flag,
                post.sync.counterA, post.sync.counterB, post.sync.arm, post.sync.latch, post.counter, post.opCounter);
        SafepointCloseEpisode();
        return;
    }
    if (spDepthContinue) {
        // Route B-4: a continuation dose that moved nothing ends the
        // continuation here - the standard programme proceeds from
        // this point on later serves (the audio poll dose and
        // multiplicity, as built).
        spDepthContinue = false;
    }
    spLastSummary = now;
}

// Spinwatch (SPINNER-PC-STUDY Q3, Nov 17, 2026): name the code the junction
// spinner actually executes. The safepoint proving boot's F-SP5 verdict - the
// gate requested and the spinner never tripped either hand-emitted check -
// left the spinner's identity open: the J3 poll identification came from
// other boots' traces, and no boot had ever sampled this wedge's PC.
// Spinwatch re-points the runtime's own Switch guest profiler
// (guest_profiler.cpp: svcSetThreadActivity pause, svcGetThreadContext3 PC/LR
// read, registry resolution) at the safepoint gate's open. It is a
// count-mode-class instrument: it delivers nothing, executes no guest code,
// and takes no guest lock; its sampler thread only pauses the game thread
// (which resumes with its state intact - the profiler pauses it ~1000x/s in
// healthy flow on this build line) and reads host-side witness words around
// the shot. One shot per safepoint episode (Route B-3: the gate resets
// the latch below each time it re-arms after an episode close), fired
// from the gate block below at the same point that sets the safepoint
// request; the gate's conditions, the request and the handler are
// untouched by its presence.
// Armed by dk_diag spinwatch (SCOOBY_SPINWATCH, plumbed from
// switch/deko/device.cpp). Unarmed, nothing here runs: no thread, no reads,
// no log lines - behaviour identical to the safepoint build.
constexpr uint32_t kSpinwatchCounterAddr = 0x80271024u;    // stream counter the study Q2 delay waits poll (r13 - 29276)
constexpr uint32_t kSpinwatchOpCounterAddr = 0x8027101Cu;  // func_800D8164's op counter (r13 - 29284)
constexpr uint32_t kSpinwatchSamples = 64u;
constexpr uint32_t kSpinwatchSpacingMs = 10u;
constexpr int kSpinwatchSettleMs = 250;
// Per-episode latch (Route B-3): the shot fires once per safepoint
// episode - the gate's request branch exchanges it at each episode
// open, and the gate resets it when it re-arms after an episode close,
// so the next episode's wedge is shot and named too.
static std::atomic<bool> s_spinwatchFired{false};
// The witness set, read host-side only (TryReadGuestScalar + the trace
// snapshot; the heapwatch discipline): the J3 flag, the Q2 stream counter,
// the op counter, and the display-sync words the gate snapshots.
struct SpinwatchWitness {
    uint32_t flag = 0, counter = 0, opCounter = 0;
    TraceWakeSync sync{};
};
static SpinwatchWitness SpinwatchReadWitness() {
    SpinwatchWitness w;
    MemoryInline::TryReadGuestScalar(kSafepointFlagAddr, w.flag);
    MemoryInline::TryReadGuestScalar(kSpinwatchCounterAddr, w.counter);
    MemoryInline::TryReadGuestScalar(kSpinwatchOpCounterAddr, w.opCounter);
    w.sync = OS_HLE_TraceWakeReadSync();
    return w;
}
// The shot itself, on its own detached thread so the stall-service loop
// never waits on it: settle, witness, sample, witness, resolve, log.
static void SpinwatchShotThread(uint32_t handle) {
    RT_LOGF(RT_TAG_VI,
            "[spinwatch] shot handle=0x%08X source=present-path samples=%u spacing_ms=%u settle_ms=%d\n",
            handle, kSpinwatchSamples, kSpinwatchSpacingMs, kSpinwatchSettleMs);
    std::this_thread::sleep_for(std::chrono::milliseconds(kSpinwatchSettleMs));
    const SpinwatchWitness pre = SpinwatchReadWitness();
    // Caller-owned fixed buffer, sized before the window: the sampler
    // allocates nothing, and nothing runs while the game thread is paused
    // beyond the context read and the resume (the profiler's discipline).
    GuestProfiler::SpinwatchPcSample samples[kSpinwatchSamples];
    uint32_t failCount = 0, firstFailRc = 0;
    const char* firstFailStage = "";
    const uint32_t taken = GuestProfiler::SpinwatchSampleGameThread(
        handle, samples, kSpinwatchSamples, kSpinwatchSpacingMs, &failCount, &firstFailRc,
        &firstFailStage);
    const SpinwatchWitness post = SpinwatchReadWitness();
    if (failCount != 0) {
        RT_LOGF(RT_TAG_VI, "[spinwatch] failed rc=0x%X stage=%s\n", firstFailRc, firstFailStage);
    }
    if (taken == 0) {
        return;
    }
    GuestProfiler::SpinwatchResolution res;
    if (!GuestProfiler::SpinwatchResolve(samples, taken, 4, &res)) {
        return;
    }
    RT_LOGF(RT_TAG_VI,
            "[spinwatch] pc fn=0x%08X %s samples=%u/%u lr fn=0x%08X %s pc_raw=elf+0x%llX lr_raw=elf+0x%llX\n",
            res.modalPc.guest, res.modalPc.name, res.modalPc.samples, res.total, res.modalLr.guest,
            res.modalLr.name, static_cast<unsigned long long>(res.modalPc.elfOffset),
            static_cast<unsigned long long>(res.modalLr.elfOffset));
    for (uint32_t i = 0; i < res.minorityCount; ++i) {
        RT_LOGF(RT_TAG_VI, "[spinwatch] minority fn=0x%08X %s samples=%u\n", res.minorities[i].guest,
                res.minorities[i].name, res.minorities[i].samples);
    }
    RT_LOGF(RT_TAG_VI,
            "[spinwatch] witnesses pre flag=0x%08X counter71024=%u op7101C=%u A=%u B=%u arm=%u latch=%u post flag=0x%08X counter71024=%u op7101C=%u A=%u B=%u arm=%u latch=%u\n",
            pre.flag, pre.counter, pre.opCounter, pre.sync.counterA, pre.sync.counterB, pre.sync.arm,
            pre.sync.latch, post.flag, post.counter, post.opCounter, post.sync.counterA,
            post.sync.counterB, post.sync.arm, post.sync.latch);
}
// Fired from the gate block at the request point (the gate's open). Logs the
// gate-open witnesses in the study's form - the gate's own sample plus the
// two stream words the gate does not read - then either names the missing
// handle (a named failure, not a silent skip) or spawns the shot thread.
static void SpinwatchGateOpen(long long gateMs, const SafepointSnap& gateSnap) {
    uint32_t counter = 0, opCounter = 0;
    MemoryInline::TryReadGuestScalar(kSpinwatchCounterAddr, counter);
    MemoryInline::TryReadGuestScalar(kSpinwatchOpCounterAddr, opCounter);
    RT_LOGF(RT_TAG_VI,
            "[spinwatch] gate-open t=%lldms frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X counter71024=%u op7101C=%u A=%u B=%u arm=%u latch=%u\n",
            gateMs, gateSnap.frames, gateSnap.retrace, gateSnap.aiCb, gateSnap.enqueues,
            gateSnap.flag, counter, opCounter, gateSnap.sync.counterA, gateSnap.sync.counterB,
            gateSnap.sync.arm, gateSnap.sync.latch);
    const uint32_t handle = GuestProfiler::SpinwatchGameThreadHandle();
    if (handle == 0) {
        RT_LOGF(RT_TAG_VI, "[spinwatch] no game-thread handle\n");
        return;
    }
    std::thread(SpinwatchShotThread, handle).detach();
}

void StartStallWatchdogOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Unattended bring-up: autoexit.txt beside the executable holds a run length in seconds.
        if (const auto dir = RuntimeConfigFile::ExecutableDirectory()) {
            std::ifstream in(*dir / "autoexit.txt");
            int seconds = 0;
            if (in >> seconds && seconds > 0) {
                std::thread([seconds] {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    std::cerr << "[runtime] autoexit after " << seconds << " s" << std::endl;
                    std::_Exit(0);
                }).detach();
            }
        }
        std::thread([] {
            const auto start = n8_vi_now();  // N8 C7: autoexit watchdog read
            for (const int seconds : {12, 30}) {
                std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
                const uint32_t frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                const bool black = g_vi.black;  // racy read; diagnostics only
                RT_LOGF(RT_TAG_VI, "[watchdog] %d s: %u guest frame(s) presented, VI black=%d\n", seconds, frames,
                        black ? 1 : 0);
                if (frames != 0 && !black && seconds != 30) {  // Bring-up: always dump at 30 s
                    continue;
                }
                RT_LOGF(RT_TAG_VI, "[watchdog] nothing visible yet; guest threads:\n");
                Fiber::GuestFiberManager::DumpGuestThreads(std::cerr);
            }
            // Extra operator-timed mark (NO100F_WATCHDOG_AT, seconds since boot; plumbed from
            // the watchdogat dk_diag key). It exists for the PGO wedge decider (Nov 16, 2026):
            // the capture build wedges minutes into a scene transition, long after the two
            // bring-up marks above have fired and this thread has exited, so no guest dump
            // ever landed at a wedge. The 12 s / 30 s marks above are unchanged.
            //
            // The extra mark is deliberately not an unconditional dump. It samples the
            // presented-frame count 10 s before the mark and again at the mark, and dumps
            // only if the count did not advance. SPM's copy of this watchdog learned the hard
            // way that the dump's stack walk can hold the guest fiber mutex long enough to
            // freeze a running game mid-load; an armed mark must be harmless on a healthy
            // run, so a live game gets one log line and no walk. The cumulative-frames
            // predicate used at 12 s cannot express this: at a late mark the count reads in
            // the thousands even when the guest has been wedged for minutes.
            if (const char* env = std::getenv("NO100F_WATCHDOG_AT")) {
                const int mark = std::atoi(env);
                if (mark > 40) {  // past the 30 s mark, with the full 10 s delta window ahead
                    std::this_thread::sleep_until(start + std::chrono::seconds(mark - 10));
                    const uint32_t before = s_presentedXfbFrames.load(std::memory_order_relaxed);
                    std::this_thread::sleep_until(start + std::chrono::seconds(mark));
                    const uint32_t after = s_presentedXfbFrames.load(std::memory_order_relaxed);
                    if (after != before) {
                        RT_LOGF(RT_TAG_VI, "[watchdog] extra mark at %ds: frames advancing (%u -> %u), no dump\n",
                                mark, before, after);
                    } else {
                        const bool black = g_vi.black;  // racy read; diagnostics only
                        RT_LOGF(RT_TAG_VI, "[watchdog] %d s: %u guest frame(s) presented, VI black=%d\n", mark,
                                after, black ? 1 : 0);
                        RT_LOGF(RT_TAG_VI, "[watchdog] nothing visible yet; guest threads:\n");
                        Fiber::GuestFiberManager::DumpGuestThreads(std::cerr);
                    }
                }
            }
        }).detach();
        // tracecnt sampler (SCOOBY_TRACE_CNT, plumbed from the tracecnt dk_diag key): a
        // watchdog-image thread that watches the PGO wedge's candidate feeders from the
        // host side (WEDGE-COUNTER-STUDY Q4, Nov 16, 2026). It only reads: presented
        // frames, the VI retrace count and its guest word, the AI DMA delivery counters,
        // the __AID guest words, the OS alarm queue head and the current retrace
        // callbacks. It never invokes guest code and never pumps anything; with the env
        // unset the thread does not exist and behaviour is unchanged.
        if (std::getenv("SCOOBY_TRACE_CNT")) {
            std::thread([] {
                const auto start = n8_vi_now();  // N8 C7: tracecnt watchdog read
                uint32_t lastFrames = 0;
                auto lastPresent = start;
                auto lastLog = start;
                bool haveLast = false;
                uint32_t pRetrace = 0, pGuest = 0, pAiCb = 0, pAiBlocks = 0, pAidCb = 0;
                uint32_t pAiBusy = 0, pAlarmHead = 0, pPre = 0, pPost = 0;
                uint32_t pGxBp = 0, pGx47 = 0, pGx45 = 0, pGx48 = 0, pGxOther = 0, pGxHw = 0;
                uint64_t pAlarmFire = 0;
                bool pAlarmOk = false;
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const auto now = n8_vi_now();  // N8 C7: tracecnt watchdog read
                    const uint32_t frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                    if (frames != lastFrames) {
                        lastFrames = frames;
                        lastPresent = now;
                    }
                    uint32_t retrace = 0, pre = 0, post = 0;
                    {
                        std::lock_guard<std::mutex> lock(g_viMutex);
                        retrace = g_vi.retraceCount;
                        pre = g_vi.preRetraceCallback;
                        post = g_vi.postRetraceCallback;
                    }
                    uint32_t guest = 0;
                    const bool guestOk = Memory::TryRead32(kViRetraceCountAddr, guest);
                    uint32_t aidCb = 0, aiBusy = 0;
                    Memory::TryRead32(0x802710FCu, aidCb);   // __AID_Callback (audio.cpp kAIDmaCallbackAddr)
                    Memory::TryRead32(0x8027110Cu, aiBusy);  // __AID_Active (audio.cpp kAICallbackBusyAddr)
                    const uint32_t aiCb = Audio_HLE_TraceAiCallbacks();
                    const uint32_t aiBlocks = Audio_HLE_TraceAiBlocks();
                    uint32_t alarmHead = 0;
                    uint64_t alarmFire = 0, alarmNow = 0;
                    const bool alarmOk = OS_HLE_TraceAlarmHead(&alarmHead, &alarmFire, &alarmNow);
                    const bool changed = !haveLast || retrace != pRetrace || guest != pGuest || aiCb != pAiCb ||
                                         aiBlocks != pAiBlocks || aidCb != pAidCb || aiBusy != pAiBusy ||
                                         alarmHead != pAlarmHead || alarmFire != pAlarmFire || pre != pPre ||
                                         post != pPost || alarmOk != pAlarmOk;
                    // [gxcnt] instrument reads (SEAMFIX-FOLLOWUP-STUDY): the GX
                    // completion-site counters, host-side atomic loads like the
                    // feeders above; printed as a companion line below.
                    const uint32_t gxBp = GxFifo_HLE_BreakPtCalls();
                    const uint32_t gx47 = GxFifo_HLE_PeHitsToken();
                    const uint32_t gx45 = GxFifo_HLE_PeHitsDrawDone();
                    const uint32_t gx48 = GxFifo_HLE_PeHitsSync();
                    const uint32_t gxOther = GxFifo_HLE_PeHitsOther();
                    const uint32_t gxHw = Dvd_HLE_GxHeldHighWater();
                    const bool gxChanged = gxBp != pGxBp || gx47 != pGx47 || gx45 != pGx45 ||
                                           gx48 != pGx48 || gxOther != pGxOther || gxHw != pGxHw;
                    const bool stalled = now - lastPresent > std::chrono::seconds(1);
                    if (changed || (stalled && now - lastLog >= std::chrono::seconds(5))) {
                        const long long tms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                        RT_LOGF(RT_TAG_VI,
                                "[cnt] t=%lldms frames=%u retrace=%u guest=%u guest_ok=%d ai_cb=%u ai_blocks=%u aid_cb=0x%08X ai_busy=%u alarm_head=0x%08X alarm_fire=%llu alarm_now=%llu pre=0x%08X post=0x%08X\n",
                                tms, frames, retrace, guest, guestOk ? 1 : 0, aiCb, aiBlocks, aidCb, aiBusy,
                                alarmHead, static_cast<unsigned long long>(alarmFire),
                                static_cast<unsigned long long>(alarmNow), pre, post);
                        // [gxcnt] companion line: printed when the GX counters moved,
                        // and on the stall cadence regardless, so a breakpt count that
                        // has stopped moving stays visible against the [cnt] timeline.
                        if (gxChanged || stalled) {
                            RT_LOGF(RT_TAG_VI,
                                    "[gxcnt] t=%lldms breakpt=%u pe47=%u pe45=%u pe48=%u pe_other=%u gxheld_hw=%u\n",
                                    tms, gxBp, gx47, gx45, gx48, gxOther, gxHw);
                            pGxBp = gxBp; pGx47 = gx47; pGx45 = gx45;
                            pGx48 = gx48; pGxOther = gxOther; pGxHw = gxHw;
                        }
                        lastLog = now;
                    }
                    haveLast = true;
                    pRetrace = retrace; pGuest = guest; pAiCb = aiCb; pAiBlocks = aiBlocks; pAidCb = aidCb;
                    pAiBusy = aiBusy; pAlarmHead = alarmHead; pAlarmFire = alarmFire; pPre = pre; pPost = post;
                    pAlarmOk = alarmOk;
                }
            }).detach();
        }
        // Heap-list canary (SCOOBY_HEAPWATCH, plumbed from the heapwatch dk_diag
        // key; BURST-FORENSICS-STUDY F4, Nov 17, 2026): a read-only sampler on the
        // tracecnt thread's 250 ms cadence that walks the guest OS heap's
        // allocated and free lists and timestamps the first appearance of an
        // out-of-window link - the corruption signature (a bogus cell at
        // 0x389dfff0, fatal field at +4) that killed the stall-service v2
        // episode through OSFreeToHeap and the eager junction boot's burst
        // through OSAllocFromHeap. It only reads, via Memory::TryRead32; a wild
        // link terminates that list's walk and is never dereferenced beyond the
        // failing read. No guest code, no guest writes, no guest lock held
        // across the walk. With the env unset the thread does not exist and
        // behaviour is unchanged.
        //
        // Guest heap layout, derived from the generated shards (CANARY-BUILD-LOG
        // has the full derivation): the OSAlloc/OSFree wrappers (func_80015FA0 /
        // func_80015F74) load __OSCurrHeap from (r13 - 31480) and pass it as the
        // heap handle to the workers (func_80104724 / func_80104820); a worker
        // indexes a 12-byte descriptor table whose base pointer it loads from
        // (r13 - 28536). The guest SDA base r13 = 0x80278280 is anchored by the
        // VI retrace word (r13 - 0x6EE8 = 0x80271398) and the display A word
        // (r13 - 27584 = 0x802716C0), so __OSCurrHeap sits at 0x80270788 and the
        // table pointer at 0x80271308. Descriptor: +4 = free-list head, +8 =
        // allocated-list head. Cell: +0 prev, +4 next, +8 size.
        if (std::getenv("SCOOBY_HEAPWATCH")) {
            std::thread([] {
                constexpr uint32_t kOsCurrHeapAddr = 0x80270788u;
                constexpr uint32_t kOsHeapTablePtrAddr = 0x80271308u;
                constexpr uint32_t kRamLo = 0x80000000u;
                constexpr uint32_t kRamHi = 0x81800000u;  // cached RAM window end (exclusive)
                const auto start = n8_vi_now();  // N8 C7: heapwatch watchdog read
                auto lastHeartbeat = start;
                uint32_t lastFrames = 0;
                bool haveFrames = false;
                uint32_t loggedBad[16] = {};
                uint32_t loggedBadCount = 0;
                const auto inWindow = [](uint32_t a) { return a >= kRamLo && a < kRamHi; };
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const auto now = n8_vi_now();  // N8 C7: heapwatch watchdog read
                    const long long tms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                    const uint32_t frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                    const bool framesFrozen = haveFrames && frames == lastFrames;
                    lastFrames = frames;
                    haveFrames = true;
                    // Resolve the current heap descriptor. Any failed read, an
                    // unset/implausible handle, or an out-of-window table means
                    // the heap is not up (or not current): skip silently.
                    uint32_t handleRaw = 0, tableBase = 0;
                    if (!Memory::TryRead32(kOsCurrHeapAddr, handleRaw) ||
                        !Memory::TryRead32(kOsHeapTablePtrAddr, tableBase)) {
                        continue;
                    }
                    const int32_t handle = static_cast<int32_t>(handleRaw);
                    if (handle < 0 || handle > 127 || !inWindow(tableBase)) {
                        continue;
                    }
                    const uint32_t desc = tableBase + static_cast<uint32_t>(handle) * 12u;
                    if (!inWindow(desc) || !inWindow(desc + 8u)) {
                        continue;
                    }
                    uint32_t heads[2] = {0, 0};
                    if (!Memory::TryRead32(desc + 8u, heads[0]) ||  // allocated list head
                        !Memory::TryRead32(desc + 4u, heads[1])) {  // free list head
                        continue;
                    }
                    const auto reportTrip = [&](const char* list, uint32_t depth, uint32_t cell,
                                                uint32_t bad) {
                        // Probe trip wire: any detected trip is host-visible
                        // from this instant (see s_heapWatchTripped above).
                        s_heapWatchTripped.store(true, std::memory_order_relaxed);
                        for (uint32_t i = 0; i < loggedBadCount; ++i) {
                            if (loggedBad[i] == bad) {
                                return;  // this bad value is already on record
                            }
                        }
                        uint32_t retrace = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_viMutex);
                            retrace = g_vi.retraceCount;
                        }
                        RT_LOGF(RT_TAG_VI,
                                "[heapwatch] list=%s depth=%u cell=0x%08X link=0x%08X t=%lldms frames=%u retrace=%u ai_cb=%u enqueues=%u\n",
                                list, depth, cell, bad, tms, frames, retrace,
                                Audio_HLE_TraceAiCallbacks(), Dvd_HLE_TraceEnqueues());
                        if (loggedBadCount < 16u) {
                            loggedBad[loggedBadCount++] = bad;
                        }
                    };
                    static const char* kListNames[2] = {"alloc", "free"};
                    bool tripped = false;
                    uint32_t walked[2] = {0, 0};
                    for (int list = 0; list < 2; ++list) {
                        uint32_t cell = heads[list];
                        uint32_t prevCell = desc + (list == 0 ? 8u : 4u);
                        for (uint32_t depth = 0; depth < 64u; ++depth) {
                            if (cell == 0) {
                                break;  // clean end of list
                            }
                            if (!inWindow(cell)) {
                                // The link in prevCell points out of the window:
                                // the wild cell the next heap operation would die
                                // on. In the log line, cell= is that pointing cell
                                // (the descriptor head field at depth 0) and
                                // link= is the out-of-window value itself.
                                reportTrip(kListNames[list], depth, prevCell, cell);
                                tripped = true;
                                break;
                            }
                            uint32_t prev = 0, next = 0;
                            if (!Memory::TryRead32(cell, prev) ||
                                !Memory::TryRead32(cell + 4u, next)) {
                                break;  // in-window but unreadable: stop this walk
                            }
                            walked[list] = depth + 1;
                            if (prev != 0 && !inWindow(prev)) {
                                reportTrip(kListNames[list], depth, cell, prev);
                                tripped = true;
                                break;
                            }
                            prevCell = cell;
                            cell = next;
                        }
                    }
                    // Clean-walk heartbeat: at most once per 5 s, and only while
                    // the presented-frames counter is frozen, so a clean junction
                    // is on record too (not just corrupt ones).
                    if (!tripped && framesFrozen && now - lastHeartbeat >= std::chrono::seconds(5)) {
                        RT_LOGF(RT_TAG_VI, "[heapwatch] clean walk t=%lldms frames=%u alloc_cells=%u free_cells=%u\n",
                                tms, frames, walked[0], walked[1]);
                        lastHeartbeat = now;
                    }
                }
            }).detach();
        }

        // Dispwatch sampler (COPYDISP-STUDY D4, Nov 17, 2026): the state half
        // of the dispwatch instrument whose entry half sits above
        // (DispWatch_HLE_OnDispatcherEntry). On the heapwatch cadence it
        // reads the display pool / flag / mode / config words and
        // change-logs them with the feeder values, so each movie boundary's
        // display state is on record against the dispatcher-entry lines;
        // the current-mode struct's geometry halfwords (+4 fbWidth,
        // +6 efbHeight, +8 xfbHeight) are read through the mode pointer
        // only when it lands in the cached RAM window, and the pointer is
        // never dereferenced otherwise. The first successful sample logs
        // as a change (the boot's initial state is the baseline the
        // readout compares against). A steady heartbeat (at most once per
        // 5 s, only while frames are frozen) positively evidences a static
        // post-cliff state and carries the dispatcher-entry total, so
        // entries suppressed past the entry log's cap stay accounted.
        // Read-only throughout: TryRead32 only, no guest code, no guest
        // writes, no guest lock held across the reads (g_viMutex is taken
        // only briefly for the retrace count, as the canary does). With
        // the env unset the thread does not exist and behaviour is
        // unchanged.
        if (std::getenv("SCOOBY_DISPWATCH")) {
            std::thread([] {
                constexpr uint32_t kRamLo = 0x80000000u;
                constexpr uint32_t kRamHi = 0x81800000u;  // cached RAM window end (exclusive)
                const auto start = n8_vi_now();  // N8 C7: dispwatch watchdog read
                auto lastHeartbeat = start;
                uint32_t lastFrames = 0;
                bool haveFrames = false;
                bool havePrev = false;
                uint32_t prev[12] = {};  // the 9 watched words + fbw / efbh / xfbh
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const auto now = n8_vi_now();  // N8 C7: dispwatch watchdog read
                    const uint32_t frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                    const bool framesFrozen = haveFrames && frames == lastFrames;
                    lastFrames = frames;
                    haveFrames = true;
                    uint32_t w[9];
                    if (!DispWatchTryReadWords(w)) {
                        continue;  // guest memory not up (or going down): skip silently
                    }
                    uint32_t fbw = 0, efbh = 0, xfbh = 0;
                    const uint32_t mode = w[7];
                    if (mode >= kRamLo && mode < kRamHi) {
                        uint32_t w4 = 0, w8 = 0;
                        if (Memory::TryRead32(mode + 4u, w4) && Memory::TryRead32(mode + 8u, w8)) {
                            fbw = (w4 >> 16) & 0xFFFFu;
                            efbh = w4 & 0xFFFFu;
                            xfbh = (w8 >> 16) & 0xFFFFu;
                        }
                    }
                    const uint32_t cur[12] = {w[0], w[1], w[2], w[3], w[4], w[5],
                                              w[6], w[7], w[8], fbw, efbh, xfbh};
                    bool changed = !havePrev;
                    for (int i = 0; i < 12 && !changed; ++i) {
                        changed = cur[i] != prev[i];
                    }
                    if (changed) {
                        uint32_t retrace = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_viMutex);
                            retrace = g_vi.retraceCount;
                        }
                        RT_LOGF(RT_TAG_VI,
                                "[dispwatch] change t=%lldms frames=%u retrace=%u ai_cb=%u enqueues=%u pool=[bc=%08X tok=%08X f8=%08X p0=%08X p1=%08X fb0=%08X] flag=%u mode=0x%08X modehw=[fbw=%u efbh=%u xfbh=%u] cfg=0x%08X\n",
                                DispWatchElapsedMs(), frames, retrace, Audio_HLE_TraceAiCallbacks(),
                                Dvd_HLE_TraceEnqueues(), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                                w[7], fbw, efbh, xfbh, w[8]);
                        for (int i = 0; i < 12; ++i) {
                            prev[i] = cur[i];
                        }
                        havePrev = true;
                    }
                    // Steady heartbeat: at most once per 5 s, and only while
                    // the presented-frames counter is frozen, so a static
                    // post-cliff display state is on record too (not just
                    // changing ones).
                    if (framesFrozen && now - lastHeartbeat >= std::chrono::seconds(5)) {
                        uint32_t retrace = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_viMutex);
                            retrace = g_vi.retraceCount;
                        }
                        RT_LOGF(RT_TAG_VI,
                                "[dispwatch] steady t=%lldms frames=%u retrace=%u ai_cb=%u enqueues=%u pool=[bc=%08X tok=%08X f8=%08X p0=%08X p1=%08X fb0=%08X] flag=%u mode=0x%08X modehw=[fbw=%u efbh=%u xfbh=%u] cfg=0x%08X entries=%u\n",
                                DispWatchElapsedMs(), frames, retrace, Audio_HLE_TraceAiCallbacks(),
                                Dvd_HLE_TraceEnqueues(), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                                w[7], fbw, efbh, xfbh, w[8],
                                s_dispWatchEntries.load(std::memory_order_relaxed));
                        lastHeartbeat = now;
                    }
                }
            }).detach();
        }

        // Stall service (WEDGE-COUNTER-STUDY Q5, Nov 16, 2026): every deferred
        // pump in this runtime is guest-activity-driven, so a guest that
        // busy-waits on a retrace/AX/alarm counter without making calls (the
        // PGO capture build's scene-transition wedge: presents frozen, the one
        // guest thread spinning in a pure memory poll) starves the very
        // delivery it waits for. On hardware the VI/AI/decrementer interrupts
        // keep firing underneath such a poll. This thread restores that floor:
        // once at least one frame has presented, while presents have stalled
        // for > 250 ms, interrupts are enabled and deferred work is actually
        // due, it runs the same deferred entries the guest-activity pumps run
        // (private interrupt context, rescheduling suppressed, each entry
        // delivering only what is due). In every healthy state it is a no-op:
        // presents advancing, nothing due, or interrupts disabled leave it
        // silent. Universal, stall-gated; not env-gated.
        //
        // Gate narrowed to FEEDER SILENCE (GATE-DISCRIMINATION-STUDY Q4, Nov 16, 2026):
        // the present-stall test this thread shipped with fired during boot-phase
        // ACTIVE-WORK stalls - the guest streaming boot.HIP and pumping its own
        // callbacks, feeders advancing at every sampler tick - and delivering guest
        // callbacks from this thread concurrently with that active execution crashed
        // both launches of the first proving boot. A present stall is not a wait: a
        // healthy boot sits at frames=2 for 4.05 s with every feeder moving. The wedge
        // is the opposite texture - presents, retrace count, AI callbacks and DVD/CARD
        // enqueues ALL frozen at once, for minutes - so an episode now opens only after
        // all four feeders have been unchanged for >= 2000 ms with the frame worker
        // idle, and closes on resumed presents or after 600 firings. Feeder change
        // times refresh on every observed change, including changes the service's own
        // firings caused, so re-arming always needs a fresh silence window.
        // Risk-bounded, not proof-safe: the claim is the empirical one that no healthy
        // state of this title produces 2 s of total feeder silence.
        //
        // Delivery is now two-stage and mode-selected (EPISODE-FORENSICS-STUDY Q4,
        // Nov 16, 2026): the v2 episode fired the deferred trio unconditionally every
        // tick, the guest woke mid-episode and began freeing heap blocks while later
        // firings (plus nested re-entrant trios via interrupt restores) kept running
        // guest callback code from this thread, and the OS heap's free list tore
        // within ~5 firings. Mode count (dk_diag stallsvc=count) delivers ONLY the
        // retrace count word - no wake, no callbacks, no guest code - which cannot
        // tear anything and decides whether the wait reads the raw word. Mode burst
        // (stallsvc=burst) delivers ONE bounded burst of retrace+audio rounds per
        // silence window, with the alarm step dropped (the junction's alarm queue is
        // empty in every wedge boot; the step's only effect there was the network
        // pump walking its guest-thread-only lock-free state from this thread).
        // No key: the service never delivers. Mode probe (stallsvc=probe) is
        // the JUNCTION-CALLBACK-DESIGN J4.2 instrument: it shares this gate
        // (conditions replicated in its own block below, plus heapwatch-armed
        // and the spinner signature) and delivers single-class doses - one
        // retrace pair, then one audio poll, then capped multiplicity -
        // under the heapwatch canary's trip wire. Its state and block sit
        // beside parkwatch's below.
        //
        // Safepoint gate (SAFEPOINT-DESIGN S3, Nov 17, 2026): NOT a delivery
        // mode. When dk_diag safepoint is set (SCOOBY_SAFEPOINT), this
        // thread runs the probe's gate conditions in its own block below -
        // feeder silence, frame worker idle, work due, heapwatch armed,
        // the spinner signature - and on completion SETS the safepoint
        // request atomic and records the episode bases instead of
        // delivering. The delivery runs in GuestSafepointServe (above),
        // called from the spinning thread's own emitted back-edge check
        // (Route B: hand-emitted in guest func_800D8164), on the scheduler
        // thread, under the deferred-callback scope. This thread still
        // executes no guest code; its role is the count mode's discipline
        // with a different consumer.
        std::thread([] {
            const bool traceCnt = std::getenv("SCOOBY_TRACE_CNT") != nullptr;
            // Delivery mode, plumbed from the stallsvc dk_diag key (device.cpp):
            // 1 = count-only, 2 = bounded burst, 3 = probe (single-class doses,
            // JUNCTION-CALLBACK-DESIGN J4.2), 0/absent = never deliver. Read
            // once; the feeder bookkeeping below runs in every mode.
            const char* svcEnv = std::getenv("NO100F_STALLSVC");
            const int svcMode = svcEnv && std::string(svcEnv) == "count" ? 1
                                : (svcEnv && std::string(svcEnv) == "burst" ? 2
                                : (svcEnv && std::string(svcEnv) == "probe" ? 3 : 0));
            uint32_t bursts = 0;  // burst mode: bursts delivered this boot (cap 3)
            uint32_t lastFrames = s_presentedXfbFrames.load(std::memory_order_relaxed);
            auto lastPresent = n8_vi_now();  // N8 C7: stallsvc watchdog read
            bool inEpisode = false;
            uint32_t firings = 0;
            // Feeder-silence bookkeeping: last observed value and last-change time for
            // the three feeders not already tracked (frames are lastFrames/lastPresent).
            // Seeded on the first tick.
            uint32_t lastRetrace = 0, lastAiCb = 0, lastEnqueues = 0;
            auto retraceChanged = lastPresent, aiCbChanged = lastPresent, enqueuesChanged = lastPresent;
            bool haveFeeders = false;
            // Count mode witnesses: feeder values at episode entry. In pure
            // counter-only delivery nothing but the retrace count may move; an
            // ai_cb or enqueue move during an episode is the mode's falsifier
            // signal (something else delivered, or the guest woke) and is logged.
            uint32_t episodeAiBase = 0, episodeEnqBase = 0;
            bool episodeAiMoved = false, episodeEnqMoved = false;
            // Parkwatch (PARK-STUDY-2 Q4(c), Nov 16, 2026): the mirror image of
            // the feeder-silence gate below. At an FMV boundary the presenter
            // thread can park legitimately on the display-sync queue in the
            // pipeline-full state, waiting for an advance whose setter chain
            // is a closed loop once the only submitter sleeps: presents freeze
            // while the retrace and AI feeders keep advancing (the post-retrace
            // callback runs throughout and gates out on arm == 0). The waiter's
            // post-sleep path presents unconditionally, so one targeted wake
            // restarts the chain. This block only samples and requests: the
            // wake itself is fired by SelectThread's idle loop
            // (OS_HLE_RequestParkWake), never from this thread, and no guest
            // code runs here. Armed by dk_diag parkwatch (SCOOBY_PARKWATCH);
            // with the env unset the block is inert and behaviour is unchanged.
            const bool parkWatch = std::getenv("SCOOBY_PARKWATCH") != nullptr;
            constexpr uint32_t kParkMainThreadAddr = 0x8026D568u;  // the presenter / default thread
            uint32_t pwFrames = lastFrames;        // last frame count this block observed
            auto pwFrameMoveAt = lastPresent;      // when pwFrames last changed
            uint32_t pwRetraceBase = 0, pwAiCbBase = 0;  // feeder values when the freeze window opened
            bool pwSigHeld = false;                // signature continuously true right now
            bool pwSigLogged = false;              // signature line emitted for this freeze
            auto pwSigSince = lastPresent;         // signature continuously true since
            bool pwFiredThisEpisode = false;       // the one fire for this signature formation
            bool pwStopLogged = false;             // wake-not-taking line emitted for this formation
            bool pwOutcomePending = false;         // a fire is awaiting its outcome
            uint32_t pwFireFrame = 0;              // frame count at the pending fire
            auto pwFireAt = lastPresent;           // when the pending fire was requested
            uint32_t pwFires = 0;                  // fires requested this boot (serial in the outcome lines)
            uint32_t pwDryFires = 0;               // consecutive fires with no frame movement (stop at 3)
            // The per-boot fire budget counts dry fires only: a fire whose
            // outcome is "frames resumed" spends nothing. Stage B attempt 2
            // (Nov 16, 2026) proved the wake on hardware - eight fires, eight
            // cures - and the boot then died at a ninth park because the old
            // total-fires cap had been spent by the successes. The
            // 3-consecutive-dry stop below stays as the runaway guard.
            uint32_t pwDryTotal = 0;               // dry fires this boot in total (fire budget cap 8)
            bool pwCapLogged = false;              // fire-budget exhaustion line emitted (once per boot)
            bool pwStopped = false;                // dry-fire stop latched for the rest of the boot
            bool pwBaselined = false;              // feeder baseline captured for the current freeze window
            bool pwDiagLogged = false;             // arm diagnostic emitted (once per boot)
            // Probe mode (stallsvc=probe; JUNCTION-CALLBACK-DESIGN J4.2, Nov 17,
            // 2026): the deciding instrument for the junction wait. The tested
            // matrix cells are the combined trio at uncontrolled multiplicity
            // from this thread (v1/v2/burst - fatal) and one class (count) at
            // 2,630 deliveries (safe, no wake). The probe fills the untested
            // cell: ONE class, ONE delivery, canary watching, long observation.
            // Dose 1 is a single retrace pair; dose 2 (only if no wake) a
            // single audio poll; dose 3 is capped multiplicity - retrace pairs
            // to a cumulative 8, then audio polls to a cumulative 16, one
            // delivery per 2 s. One episode per boot; the probe never re-arms.
            // Gate: the feeder-silence gate below, conditions unchanged, PLUS
            // heapwatch armed (without it the probe refuses and never
            // delivers) PLUS the spinner signature (main thread RUNNING,
            // state==2, queue word 0x0, on two consecutive ticks). Stop checks
            // run before every dose and through every observation window: a
            // heapwatch trip aborts the probe for the boot, process exit
            // closes it, and feeder movement is the wake it watches for.
            const bool probeHeapArmed = std::getenv("SCOOBY_HEAPWATCH") != nullptr;
            constexpr uint32_t kProbeFlagAddr = 0x80271020u;  // func_800D8164's completion flag (study J3)
            const auto probeStart = lastPresent;  // epoch for the probe lines' t= values
            int probePhase = 0;  // 0 armed, 1 observe dose 1, 2 observe dose 2, 3 dose 3, 4 done
            bool probeRefusedLogged = false;   // heapwatch-refusal line emitted (once per boot)
            bool probeEpisodeRan = false;      // an episode actually opened this boot
            bool probeSecondLogged = false;    // second-silence-window line emitted
            uint32_t probeSigTicks = 0;        // consecutive ticks the spinner signature has held
            bool probeGateReady = false;       // the v2 gate held at the previous tick (armed phase)
            uint32_t probeGateFrames = 0, probeGateEnq = 0;  // feeder bases at gate-ready
            uint32_t probeEpFrames = 0, probeEpEnq = 0, probeEpFlag = 0;  // wake-detection bases
            uint32_t probePairs = 0, probePolls = 0;  // cumulative deliveries this episode
            uint32_t probeDose = 0;            // dose number of the latest delivery (1, 2 or 3)
            auto probeWindowEnd = lastPresent;   // end of the current 10 s observation window
            auto probeNextDoseAt = lastPresent;  // dose-3 cadence (one delivery per 2 s)
            auto probeLastSummary = lastPresent;  // last 1 s observation summary
            // Safepoint gate state (SAFEPOINT-DESIGN S3, Nov 17, 2026):
            // the probe's gate re-pointed. When dk_diag safepoint is set,
            // the block below replicates the probe gate's conditions
            // (feeder silence >= 2000 ms, frame worker idle, work due, the
            // same DetClock / interrupt guards) PLUS heapwatch armed PLUS
            // the spinner signature - and on completion SETS the request
            // atomic (s_safepointRequested) and records the episode bases
            // (frames / enqueues / flag) for GuestSafepointServe, instead
            // of delivering. This thread executes no guest code.
            // Route B-3: the block RE-ARMS after each handler close -
            // a new episode opens only on a fresh silence window, at
            // most kSafepointMaxEpisodes per boot.
            const bool spArmed = std::getenv("SCOOBY_SAFEPOINT") != nullptr;
            const bool swArmed = std::getenv("SCOOBY_SPINWATCH") != nullptr;
            const auto spStart = lastPresent;  // epoch for the gate lines' t= values
            int spGateState = 0;  // 0 watching, 1 episode open (request set), 2 spent (permanent stop), 3 cap spent (episode cap used)
            bool spRefusedLogged = false;   // heapwatch-refusal line emitted (once per boot)
            uint32_t spEpisodesOpened = 0;  // episodes opened this boot (Route B-3 cap accounting)
            bool spCapIgnoredLogged = false;  // cap-state ignored-window line emitted
            uint32_t spSigTicks = 0;        // consecutive ticks the spinner signature has held
            bool spGateReady = false;       // the gate held at the previous tick (armed state)
            uint32_t spGateFrames = 0, spGateEnq = 0;  // feeder bases at gate-ready
            // One full probe snapshot: the four feeders plus the study J3
            // candidate flag and the display-sync words, read host-side only
            // (the trace accessors / TryReadGuestScalar; no guest code runs).
            struct ProbeSnap {
                uint32_t frames = 0, retrace = 0, aiCb = 0, enqueues = 0, flag = 0;
                TraceWakeSync sync{};
            };
            const auto probeSample = [&]() {
                ProbeSnap s;
                s.frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(g_viMutex);
                    s.retrace = g_vi.retraceCount;
                }
                s.aiCb = Audio_HLE_TraceAiCallbacks();
                s.enqueues = Dvd_HLE_TraceEnqueues();
                MemoryInline::TryReadGuestScalar(kProbeFlagAddr, s.flag);
                s.sync = OS_HLE_TraceWakeReadSync();
                return s;
            };
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const auto now = n8_vi_now();  // N8 C7: tracewake watchdog read
                const uint32_t frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                uint32_t retraceNow = 0;
                {
                    std::lock_guard<std::mutex> lock(g_viMutex);
                    retraceNow = g_vi.retraceCount;
                }
                const uint32_t aiCbNow = Audio_HLE_TraceAiCallbacks();
                const uint32_t enqueuesNow = Dvd_HLE_TraceEnqueues();
                if (!haveFeeders) {
                    lastRetrace = retraceNow; lastAiCb = aiCbNow; lastEnqueues = enqueuesNow;
                    retraceChanged = aiCbChanged = enqueuesChanged = now;
                    haveFeeders = true;
                } else {
                    if (retraceNow != lastRetrace) { lastRetrace = retraceNow; retraceChanged = now; }
                    if (aiCbNow != lastAiCb) { lastAiCb = aiCbNow; aiCbChanged = now; }
                    if (enqueuesNow != lastEnqueues) { lastEnqueues = enqueuesNow; enqueuesChanged = now; }
                }
                // Probe block (stallsvc=probe only; inert in every other
                // mode). Runs on the fresh feeder samples, ahead of the
                // frame-change handling, so a wake is attributed on the tick
                // it lands. Its gate replicates the feeder-silence gate
                // below condition for condition (same change stamps, same
                // due test, same DetClock / interrupt guards that precede
                // the gate in this loop) because the probe's episode spans
                // minutes of ticks and cannot live inside that one-shot
                // block; the gate block itself skips mode 3 (see its guard).
                if (svcMode == 3) {
                    const long long probeNowMs = static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - probeStart).count());
                    const auto probeLogDose = [&](uint32_t dose, const char* cls, const ProbeSnap& pre,
                                                  const ProbeSnap& post) {
                        RT_LOGF(RT_TAG_VI,
                                "[stallsvc-probe] dose %u class=%s pairs=%u polls=%u pre: frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u post: frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u\n",
                                dose, cls, probePairs, probePolls,
                                pre.frames, pre.retrace, pre.aiCb, pre.enqueues, pre.flag,
                                pre.sync.counterA, pre.sync.counterB, pre.sync.arm, pre.sync.latch,
                                post.frames, post.retrace, post.aiCb, post.enqueues, post.flag,
                                post.sync.counterA, post.sync.counterB, post.sync.arm, post.sync.latch);
                    };
                    // Spinner signature, through the same thread-record
                    // access parkwatch uses (state word at +0x2C8, queue word
                    // at +0x2DC of the main thread's record): RUNNING
                    // (state==2) with queue word 0x0.
                    uint16_t probeThrState = 0;
                    uint32_t probeThrQueue = 0;
                    const bool probeStateOk =
                        MemoryInline::TryReadGuestScalar(kParkMainThreadAddr + 0x2C8u, probeThrState);
                    const bool probeQueueOk =
                        MemoryInline::TryReadGuestScalar(kParkMainThreadAddr + 0x2DCu, probeThrQueue);
                    probeSigTicks = (probeStateOk && probeQueueOk && probeThrState == 2 && probeThrQueue == 0)
                                        ? probeSigTicks + 1
                                        : 0;
                    if (probePhase == 0) {
                        // The v2 gate, replicated: all four feeders unchanged
                        // for >= 2000 ms, the frame worker idle, work due.
                        bool probeRetraceDue = false;
                        {
                            std::lock_guard<std::mutex> lock(g_viMutex);
                            probeRetraceDue = g_vi.initialized && now >= g_vi.lastRetrace + ActiveIntervalLocked();
                        }
                        uint32_t probeAlarmHead = 0;
                        uint64_t probeAlarmFire = 0, probeAlarmNow = 0;
                        const bool probeAlarmDue =
                            OS_HLE_TraceAlarmHead(&probeAlarmHead, &probeAlarmFire, &probeAlarmNow) &&
                            probeAlarmFire <= probeAlarmNow;
                        const auto probeSilence = std::chrono::milliseconds(2000);
                        const bool probeV2Gate = frames != 0 && now - lastPresent >= probeSilence &&
                                                 now - retraceChanged >= probeSilence &&
                                                 now - aiCbChanged >= probeSilence &&
                                                 now - enqueuesChanged >= probeSilence &&
                                                 aurora_frame_worker_idle() &&
                                                 (probeRetraceDue || probeAlarmDue) &&
                                                 !DetClock::Enabled() && OS_HLE_InterruptsEnabled();
                        if (probeGateReady && (frames != probeGateFrames || enqueuesNow != probeGateEnq)) {
                            // The guest moved on its own after the v2 gate
                            // completed but before dose 1 (the signature was
                            // still confirming): F-J3's shape, logged in the
                            // act. The episode never opens; the probe is spent.
                            const ProbeSnap s = probeSample();
                            RT_LOGF(RT_TAG_VI,
                                    "[stallsvc-probe] wake dose=0 witness=spontaneous pairs=0 polls=0 frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u\n",
                                    s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                                    s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch);
                            probePhase = 4;
                            probeGateReady = false;
                        } else if (probeV2Gate && probeSigTicks >= 2) {
                            if (!probeHeapArmed) {
                                // Refuses once per boot and never delivers in
                                // this state (study J4.2 gate condition (i)).
                                if (!probeRefusedLogged) {
                                    probeRefusedLogged = true;
                                    RT_LOGF(RT_TAG_VI, "[stallsvc-probe] refused: heapwatch not armed\n");
                                }
                                probePhase = 4;
                            } else if (HeapWatchTripped()) {
                                RT_LOGF(RT_TAG_VI, "[stallsvc-probe] abort: heapwatch trip dose=0 pairs=0 polls=0\n");
                                probePhase = 4;
                            } else if (fw_switch_exiting && fw_switch_exiting()) {
                                RT_LOGF(RT_TAG_VI, "[stallsvc-probe] closed: process exiting\n");
                                probePhase = 4;
                            } else {
                                // Episode open: log the gate state with the
                                // feeder values at this instant (the burst
                                // forensics' gate-arithmetic caveat), then
                                // dose 1 - exactly one retrace pair through
                                // the existing deferred entry, the same call
                                // the burst loop makes per round.
                                const ProbeSnap pre = probeSample();
                                probeEpFrames = pre.frames;
                                probeEpEnq = pre.enqueues;
                                probeEpFlag = pre.flag;
                                probeEpisodeRan = true;
                                RT_LOGF(RT_TAG_VI,
                                        "[stallsvc-probe] episode open t=%lldms frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u\n",
                                        probeNowMs, pre.frames, pre.retrace, pre.aiCb, pre.enqueues, pre.flag,
                                        pre.sync.counterA, pre.sync.counterB, pre.sync.arm, pre.sync.latch);
                                VI_HLE_ProcessRetracesDeferred(1);
                                probePairs = 1;
                                probeDose = 1;
                                const ProbeSnap post = probeSample();
                                probeLogDose(1, "retrace", pre, post);
                                probePhase = 1;
                                probeWindowEnd = now + std::chrono::seconds(10);
                                probeLastSummary = now;
                            }
                            probeGateReady = false;
                        }
                        if (probePhase == 0) {
                            probeGateReady = probeV2Gate;
                            if (probeV2Gate) {
                                probeGateFrames = frames;
                                probeGateEnq = enqueuesNow;
                            }
                        }
                    } else if (probePhase == 1 || probePhase == 2 || probePhase == 3) {
                        // Stop checks before anything else, every tick: a
                        // canary trip aborts the probe for the boot; process
                        // exit closes it. Neither ever delivers again.
                        if (HeapWatchTripped()) {
                            RT_LOGF(RT_TAG_VI, "[stallsvc-probe] abort: heapwatch trip dose=%u pairs=%u polls=%u\n",
                                    probeDose, probePairs, probePolls);
                            probePhase = 4;
                        } else if (fw_switch_exiting && fw_switch_exiting()) {
                            RT_LOGF(RT_TAG_VI, "[stallsvc-probe] closed: process exiting\n");
                            probePhase = 4;
                        } else {
                            const ProbeSnap s = probeSample();
                            const char* probeWitness = nullptr;
                            if (s.frames != probeEpFrames) {
                                probeWitness = "frames";
                            } else if (s.enqueues != probeEpEnq) {
                                probeWitness = "enqueues";
                            } else if (s.flag != probeEpFlag) {
                                probeWitness = "flag";
                            }
                            if (probeWitness) {
                                RT_LOGF(RT_TAG_VI,
                                        "[stallsvc-probe] wake dose=%u witness=%s pairs=%u polls=%u frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u\n",
                                        probeDose, probeWitness, probePairs, probePolls,
                                        s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                                        s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch);
                                probePhase = 4;
                            } else {
                                if (now - probeLastSummary >= std::chrono::seconds(1)) {
                                    probeLastSummary = now;
                                    RT_LOGF(RT_TAG_VI,
                                            "[stallsvc-probe] obs t=%lldms dose=%u frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u\n",
                                            probeNowMs, probeDose, s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                                            s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch);
                                }
                                if (probePhase == 1 && now >= probeWindowEnd) {
                                    // Dose 2: exactly one audio poll, the burst
                                    // loop's per-round audio call.
                                    Audio_HLE_PollDeferred();
                                    probePolls = 1;
                                    probeDose = 2;
                                    const ProbeSnap post = probeSample();
                                    probeLogDose(2, "audio", s, post);
                                    probePhase = 2;
                                    probeWindowEnd = now + std::chrono::seconds(10);
                                } else if (probePhase == 2 && now >= probeWindowEnd) {
                                    probePhase = 3;
                                    probeNextDoseAt = now;
                                } else if (probePhase == 3 && now >= probeNextDoseAt) {
                                    // Dose 3: capped multiplicity, one delivery
                                    // per 2 s - retrace pairs first to the
                                    // cumulative cap of 8, then audio polls to
                                    // 16 (study J4.2). The caps exceed the v2
                                    // outer dose that released the guest, so
                                    // exhausting them awake-less is F-J2.
                                    if (probePairs >= 8 && probePolls >= 16) {
                                        RT_LOGF(RT_TAG_VI, "[stallsvc-probe] no-wake at full dose pairs=%u polls=%u\n",
                                                probePairs, probePolls);
                                        probePhase = 4;
                                    } else if (probePairs < 8) {
                                        VI_HLE_ProcessRetracesDeferred(1);
                                        ++probePairs;
                                        probeDose = 3;
                                        const ProbeSnap post = probeSample();
                                        probeLogDose(3, "retrace", s, post);
                                        probeNextDoseAt = now + std::chrono::seconds(2);
                                    } else {
                                        Audio_HLE_PollDeferred();
                                        ++probePolls;
                                        probeDose = 3;
                                        const ProbeSnap post = probeSample();
                                        probeLogDose(3, "audio", s, post);
                                        probeNextDoseAt = now + std::chrono::seconds(2);
                                    }
                                }
                            }
                        }
                    } else if (probeEpisodeRan && !probeSecondLogged) {
                        // One episode per boot (study J4.2): a second full
                        // silence window with the signature is logged and
                        // ignored.
                        const auto probeSilence = std::chrono::milliseconds(2000);
                        if (frames != 0 && now - lastPresent >= probeSilence &&
                            now - retraceChanged >= probeSilence && now - aiCbChanged >= probeSilence &&
                            now - enqueuesChanged >= probeSilence && probeSigTicks >= 2) {
                            probeSecondLogged = true;
                            RT_LOGF(RT_TAG_VI,
                                    "[stallsvc-probe] second silence window ignored (one episode per boot) t=%lldms frames=%u\n",
                                    probeNowMs, frames);
                        }
                    }
                }
                // Safepoint gate block (armed only by dk_diag safepoint;
                // inert otherwise). Runs on the fresh feeder samples,
                // beside the probe block, ahead of the frame-change
                // handling. It never delivers: on gate completion it sets
                // the request atomic the spinner's emitted checks test,
                // and records the episode bases the handler witness-checks
                // against.
                if (spArmed) {
                    const long long spNowMs = static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - spStart).count());
                    // Spinner signature, through the same thread-record
                    // access the probe uses (state word at +0x2C8, queue
                    // word at +0x2DC of the main thread's record): RUNNING
                    // (state==2) with queue word 0x0, on two consecutive
                    // 50 ms ticks.
                    uint16_t spThrState = 0;
                    uint32_t spThrQueue = 0;
                    const bool spStateOk =
                        MemoryInline::TryReadGuestScalar(kParkMainThreadAddr + 0x2C8u, spThrState);
                    const bool spQueueOk =
                        MemoryInline::TryReadGuestScalar(kParkMainThreadAddr + 0x2DCu, spThrQueue);
                    spSigTicks = (spStateOk && spQueueOk && spThrState == 2 && spThrQueue == 0)
                                     ? spSigTicks + 1
                                     : 0;
                    if (spGateState == 0) {
                        // The probe's gate, replicated condition for
                        // condition (same change stamps, same due test,
                        // same guards).
                        bool spRetraceDue = false;
                        {
                            std::lock_guard<std::mutex> lock(g_viMutex);
                            spRetraceDue = g_vi.initialized && now >= g_vi.lastRetrace + ActiveIntervalLocked();
                        }
                        uint32_t spAlarmHead = 0;
                        uint64_t spAlarmFire = 0, spAlarmNow = 0;
                        const bool spAlarmDue =
                            OS_HLE_TraceAlarmHead(&spAlarmHead, &spAlarmFire, &spAlarmNow) &&
                            spAlarmFire <= spAlarmNow;
                        const auto spSilence = std::chrono::milliseconds(2000);
                        const bool spGate = frames != 0 && now - lastPresent >= spSilence &&
                                            now - retraceChanged >= spSilence &&
                                            now - aiCbChanged >= spSilence &&
                                            now - enqueuesChanged >= spSilence &&
                                            aurora_frame_worker_idle() &&
                                            (spRetraceDue || spAlarmDue) &&
                                            !DetClock::Enabled() && OS_HLE_InterruptsEnabled();
                        if (spGateReady && (frames != spGateFrames || enqueuesNow != spGateEnq)) {
                            // The guest moved on its own after the gate
                            // completed but before the signature confirmed:
                            // F-J3's shape (the study's wake-before-dose,
                            // gate side). The episode never opens; the
                            // safepoint is spent.
                            const SafepointSnap s = SafepointSample();
                            RT_LOGF(RT_TAG_VI,
                                    "[safepoint] wake-before-dose witness=spontaneous frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u\n",
                                    s.frames, s.retrace, s.aiCb, s.enqueues, s.flag,
                                    s.sync.counterA, s.sync.counterB, s.sync.arm, s.sync.latch);
                            spGateState = 2;
                            spGateReady = false;
                        } else if (spGate && spSigTicks >= 2) {
                            if (!probeHeapArmed) {
                                // Refuses once per boot and never requests
                                // in this state (the probe's gate condition,
                                // mirrored).
                                if (!spRefusedLogged) {
                                    spRefusedLogged = true;
                                    RT_LOGF(RT_TAG_VI, "[safepoint] refused: heapwatch not armed\n");
                                }
                                spGateState = 2;
                            } else if (HeapWatchTripped()) {
                                RT_LOGF(RT_TAG_VI, "[safepoint] abort: heapwatch trip dose=0 pairs=0 polls=0\n");
                                spGateState = 2;
                            } else if (fw_switch_exiting && fw_switch_exiting()) {
                                RT_LOGF(RT_TAG_VI, "[safepoint] closed: process exiting\n");
                                spGateState = 2;
                            } else {
                                // Request: record the episode bases, log
                                // the gate state with the feeder values at
                                // this instant, then set the request. The
                                // spinner's next back-edge trip serves the
                                // first dose; this thread delivers nothing.
                                const SafepointSnap pre = SafepointSample();
                                ++spEpisodesOpened;
                                s_safepointEpisode.store(spEpisodesOpened, std::memory_order_relaxed);
                                s_safepointBaseFrames.store(pre.frames, std::memory_order_relaxed);
                                s_safepointBaseEnqueues.store(pre.enqueues, std::memory_order_relaxed);
                                s_safepointBaseFlag.store(pre.flag, std::memory_order_relaxed);
                                RT_LOGF(RT_TAG_VI,
                                        "[safepoint] request ep=%u t=%lldms frames=%u retrace=%u ai_cb=%u enqueues=%u flag=0x%08X A=%u B=%u arm=%u latch=%u\n",
                                        spEpisodesOpened, spNowMs, pre.frames, pre.retrace, pre.aiCb, pre.enqueues, pre.flag,
                                        pre.sync.counterA, pre.sync.counterB, pre.sync.arm, pre.sync.latch);
                                s_safepointRequested.store(true, std::memory_order_release);
                                // Spinwatch trigger (SPINNER-PC-STUDY Q3): the
                                // shot fires at the gate's open, beside the
                                // request - read-only, one shot per episode
                                // (the latch is reset at re-arm, below).
                                // The gate's conditions, the request and
                                // the handler are unchanged.
                                if (swArmed &&
                                    !s_spinwatchFired.exchange(true, std::memory_order_relaxed)) {
                                    SpinwatchGateOpen(spNowMs, pre);
                                }
                                spGateState = 1;
                            }
                            spGateReady = false;
                        }
                        if (spGateState == 0) {
                            spGateReady = spGate;
                            if (spGate) {
                                spGateFrames = frames;
                                spGateEnq = enqueuesNow;
                            }
                        }
                    } else if (spGateState == 1 &&
                               !s_safepointRequested.load(std::memory_order_acquire)) {
                        // Route B-3 re-arm: the handler has closed the
                        // episode (it clears the request exactly once,
                        // at the close). While the request still stands
                        // the handler owns the episode and this stays
                        // silent. On the close: a heapwatch suppression
                        // is a permanent stop (the handler's abort line
                        // already logged it); the close of the boot's
                        // last allowed episode logs the cap line and
                        // spends the gate; otherwise the gate returns
                        // to watching - the next episode needs a FRESH
                        // silence window (the gate conditions above,
                        // re-evaluated, with the bases recorded at that
                        // open), and spinwatch's latch resets so the
                        // next wedge is shot too.
                        if (s_safepointSuppressed.load(std::memory_order_relaxed)) {
                            spGateState = 2;
                        } else if (spEpisodesOpened >= kSafepointMaxEpisodes) {
                            RT_LOGF(RT_TAG_VI, "[safepoint] episode cap reached episodes=%u\n",
                                    spEpisodesOpened);
                            spGateState = 3;
                        } else {
                            spGateState = 0;
                            spGateReady = false;
                            s_spinwatchFired.store(false, std::memory_order_relaxed);
                        }
                    } else if (spGateState == 3 && !spCapIgnoredLogged) {
                        // Cap spent: dosing has stopped for the boot.
                        // A further full silence window with the
                        // signature is logged once, in the ignored-
                        // window form with the cap named as the reason.
                        const auto spSilence = std::chrono::milliseconds(2000);
                        if (frames != 0 && now - lastPresent >= spSilence &&
                            now - retraceChanged >= spSilence && now - aiCbChanged >= spSilence &&
                            now - enqueuesChanged >= spSilence && spSigTicks >= 2) {
                            spCapIgnoredLogged = true;
                            RT_LOGF(RT_TAG_VI,
                                    "[safepoint] silence window ignored (episode cap reached) t=%lldms frames=%u\n",
                                    spNowMs, frames);
                        }
                    }
                }
                // Parkwatch sampling (state declared beside the service state
                // above). Runs before the frame-change handling so a resumed
                // frame resolves a pending fire's outcome on the same tick.
                if (parkWatch) {
                    if (frames != pwFrames) {
                        if (pwOutcomePending) {
                            RT_LOGF(RT_TAG_VI, "[parkwatch] outcome: frames resumed (%u -> %u, %lld ms after fire #%u)\n",
                                    pwFireFrame, frames,
                                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - pwFireAt).count()),
                                    pwFires);
                            pwOutcomePending = false;
                            pwDryFires = 0;
                        }
                        pwFrames = frames;
                        pwFrameMoveAt = now;
                        pwSigHeld = false;
                        pwSigLogged = false;
                        pwFiredThisEpisode = false;
                        pwStopLogged = false;
                        pwBaselined = false;
                    }
                    const bool pwFrozen =
                        frames != 0 && now - pwFrameMoveAt >= std::chrono::milliseconds(2000);
                    if (pwFrozen && !pwBaselined) {
                        // Freeze window opens this tick: baseline the feeders the
                        // signature requires to have advanced across the window.
                        // The baseline is captured ONCE per freeze window. The first
                        // version re-baselined on every tick until the signature held,
                        // so the advance comparisons below compared the feeders against
                        // the values sampled on the same tick and could never be true:
                        // the signature was unreachable (proving boot d64ed2b9 logged
                        // zero [parkwatch] lines across a ~250 s textbook park).
                        pwRetraceBase = retraceNow;
                        pwAiCbBase = aiCbNow;
                        pwBaselined = true;
                    }
                    bool pwSigNow = false;
                    TraceWakeSync pwSnap{};
                    if (pwFrozen && !DetClock::Enabled()) {
                        uint16_t mainState = 0;  // OSThread state word (+0x2C8, kThreadStateOffset)
                        uint32_t mainQueue = 0;  // OSThread queue word (+0x2DC, kThreadQueueOffset)
                        const bool stateOk =
                            MemoryInline::TryReadGuestScalar(kParkMainThreadAddr + 0x2C8u, mainState);
                        const bool queueOk =
                            MemoryInline::TryReadGuestScalar(kParkMainThreadAddr + 0x2DCu, mainQueue);
                        pwSigNow = stateOk && queueOk && mainState == 4 &&
                                   mainQueue == kTraceWakeQueueAddr && retraceNow != pwRetraceBase &&
                                   aiCbNow != pwAiCbBase;
                        if (pwSigNow) {
                            pwSnap = OS_HLE_TraceWakeReadSync();
                        }
                        if (!pwDiagLogged) {
                            // Arm diagnostic (once per boot, at the first freeze window):
                            // the detector's own sampled values, read through the same
                            // accessors the signature check uses, so a silent detector
                            // explains itself in the log - sane values here with no
                            // signature line after them convict the signature logic;
                            // wrong values convict the read path; no line at all means
                            // the freeze window itself never formed.
                            pwDiagLogged = true;
                            RT_LOGF(RT_TAG_VI,
                                "[parkwatch] diag: first freeze window: frames=%u frozen %lld ms, stateOk=%d state=%u queueOk=%d queue=0x%08X retrace=%u ai_cb=%u\n",
                                frames,
                                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - pwFrameMoveAt).count()),
                                stateOk ? 1 : 0, static_cast<unsigned>(mainState),
                                queueOk ? 1 : 0, mainQueue,
                                retraceNow, aiCbNow);
                        }
                    }
                    if (pwSigNow) {
                        if (!pwSigHeld) {
                            pwSigHeld = true;
                            pwSigSince = now;
                            if (!pwSigLogged) {
                                pwSigLogged = true;
                                RT_LOGF(RT_TAG_VI,
                                        "[parkwatch] signature: frames=%u frozen %lld ms, retrace %u->%u, ai_cb %u->%u, thread 0x8026D568 state=4 queue=0x802716E4 A=%u B=%u arm=%u latch=%u\n",
                                        frames,
                                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - pwFrameMoveAt).count()),
                                        pwRetraceBase, retraceNow, pwAiCbBase, aiCbNow,
                                        pwSnap.counterA, pwSnap.counterB, pwSnap.arm, pwSnap.latch);
                            }
                        }
                        // Confirmation: the signature must hold continuously for
                        // 500 ms (the study's two samples at the service's nominal
                        // 250 ms cadence; this loop ticks at 50 ms) before the one
                        // fire of this episode is requested.
                        if (!pwOutcomePending && now - pwSigSince >= std::chrono::milliseconds(500)) {
                            if (!pwStopped && !pwFiredThisEpisode && pwDryTotal < 8) {
                                ++pwFires;
                                pwFiredThisEpisode = true;
                                pwOutcomePending = true;
                                pwFireFrame = frames;
                                pwFireAt = now;
                                // Parkwatch-cure (EARLY-CLIFF-STUDY Q3(a), Nov 19, 2026):
                                // on a confirmed park the relay is dead because the
                                // flag word at 0x802716D0 stands at 1 (set by the
                                // presenter's site-2 call, cleared only by the
                                // advancer's catch-up branch, which cannot run while
                                // the relay is dead). Clear it before waking the
                                // waiter so the next rescued present re-arms the
                                // breakpoint (GXEnableBreakPt) and the AA4 stream,
                                // the advancer and the in-flow rate resume. Safe:
                                // the signature guarantees A is exactly 2 behind B
                                // and no AA4 is in flight, so the slot is empty
                                // and the flag cannot double-arm a live breakpoint.
                                uint32_t pwFlagBefore = 0;
                                MemoryInline::TryReadGuestScalar(0x802716D0u, pwFlagBefore);
                                MemoryInline::TryWriteGuestScalar(0x802716D0u, 0u);
                                RT_LOGF(RT_TAG_VI,
                                        "[parkwatch] cure: flag 0x802716D0 %u -> 0 before fire #%u (frames=%u)\n",
                                        pwFlagBefore, pwFires, frames);
                                OS_HLE_RequestParkWake();
                            } else if (pwStopped && !pwStopLogged) {
                                // The dry-fire stop is latched: keep the falsifier
                                // loud at each fresh signature instead of firing.
                                pwStopLogged = true;
                                RT_LOGF(RT_TAG_VI,
                                        "[parkwatch] wake not taking: 3 fires with no frame movement; signature still present, no further fires this boot\n");
                            }
                        }
                    } else {
                        pwSigHeld = false;
                        pwFiredThisEpisode = false;
                        pwStopLogged = false;
                    }
                    // A fire whose frames never move resolves dry after 5 s;
                    // closing the episode lets a still-parked guest re-form the
                    // signature (fresh 500 ms confirmation) and earn the next
                    // capped fire, until 3 dry fires latch the stop.
                    if (pwOutcomePending && now - pwFireAt >= std::chrono::milliseconds(5000)) {
                        pwOutcomePending = false;
                        ++pwDryFires;
                        ++pwDryTotal;
                        RT_LOGF(RT_TAG_VI, "[parkwatch] outcome: no frame movement 5 s after fire #%u\n", pwFires);
                        if (pwDryTotal >= 8 && !pwCapLogged) {
                            // The budget cap engages at this fire: say so in the
                            // log. The old total-fires cap withheld the ninth
                            // fire of the Stage B attempt-2 boot in silence
                            // (signature logged, no reason given).
                            pwCapLogged = true;
                            RT_LOGF(RT_TAG_VI, "[parkwatch] fire budget exhausted: %u dry fires this boot; no further fires\n", pwDryTotal);
                        }
                        if (pwDryFires >= 3) {
                            pwStopped = true;
                        }
                        pwFiredThisEpisode = false;
                        pwSigHeld = false;
                        pwSigLogged = false;
                        pwStopLogged = false;
                    }
                }
                if (frames != lastFrames) {
                    lastFrames = frames;
                    lastPresent = now;
                    if (inEpisode) {
                        RT_LOGF(RT_TAG_VI, "[stallsvc] presents resumed (frame %u) after %u firing(s); episode closed\n",
                                frames, firings);
                        inEpisode = false;
                        firings = 0;
                    }
                    continue;
                }
                if (DetClock::Enabled() || !OS_HLE_InterruptsEnabled()) {
                    continue;
                }
                // Due checks: retrace and alarm have exact cheap host-side
                // tests (the same comparisons their deferred entries make
                // first). Audio has none: its elapsed time lives in a
                // thread_local poll cursor a query must not consume, so
                // Audio_HLE_PollDeferred rides along whenever the gate fires
                // and its own gating decides (a tick with no elapsed block
                // completes nothing).
                bool retraceDue = false;
                {
                    std::lock_guard<std::mutex> lock(g_viMutex);
                    retraceDue = g_vi.initialized && now >= g_vi.lastRetrace + ActiveIntervalLocked();
                }
                uint32_t alarmHead = 0;
                uint64_t alarmFire = 0, alarmNow = 0;
                const bool alarmDue =
                    OS_HLE_TraceAlarmHead(&alarmHead, &alarmFire, &alarmNow) && alarmFire <= alarmNow;
                if (!retraceDue && !alarmDue) {
                    continue;
                }
                if (!inEpisode) {
                    // Feeder-silence gate (study Q4): open only when all four feeders
                    // have been unchanged for >= 2000 ms and the frame worker has no
                    // job in flight. The entry line logs the four silence durations
                    // so a proving boot can audit the decision from the log.
                    const auto silence = std::chrono::milliseconds(2000);
                    if (frames == 0 || now - lastPresent < silence || now - retraceChanged < silence ||
                        now - aiCbChanged < silence || now - enqueuesChanged < silence ||
                        !aurora_frame_worker_idle()) {
                        continue;
                    }
                    if (svcMode == 0 || svcMode == 3 || (svcMode == 2 && bursts >= 3)) {
                        // Delivery off; the probe (mode 3 runs its own gate
                        // and episode in its block above and never enters
                        // this one); or the boot's burst budget is spent
                        // (the spend is logged when the third burst
                        // completes): the gate keeps auditing, nothing is
                        // delivered.
                        continue;
                    }
                    RT_LOGF(RT_TAG_VI,
                            "[stallsvc] mode=%s: feeder silence >=2000 ms (frames %lld ms, retrace %lld ms, ai_cb %lld ms, enqueues %lld ms) with work due (retrace=%d alarm=%d)\n",
                            svcMode == 1 ? "count" : "burst",
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPresent).count()),
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - retraceChanged).count()),
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - aiCbChanged).count()),
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - enqueuesChanged).count()),
                            retraceDue ? 1 : 0, alarmDue ? 1 : 0);
                    if (svcMode == 2) {
                        // Bounded burst (forensics Q4 stage B): one burst per silence
                        // window - 18 rounds of 2 retrace deliveries + 1 audio poll,
                        // no alarm step at all. At every round start the burst aborts
                        // the moment the guest shows life (new async work queued, a
                        // present landed) or the process starts exiting, so the
                        // post-wake exposure is the burst's own tail, never an open
                        // episode. No episode persists after a burst; re-arming needs
                        // a fresh full silence window, which the burst's own deliveries
                        // make automatic by refreshing the feeder change times on the
                        // next samples.
                        ++bursts;
                        const uint32_t burstEnqBase = enqueuesNow;
                        const uint32_t burstFramesBase = frames;
                        int roundsDone = 0;
                        const char* stopReason = "all 18 rounds delivered";
                        for (int round = 0; round < 18; ++round) {
                            if (Dvd_HLE_TraceEnqueues() != burstEnqBase) {
                                stopReason = "enqueue counter moved (guest awake)";
                                break;
                            }
                            if (s_presentedXfbFrames.load(std::memory_order_relaxed) != burstFramesBase) {
                                stopReason = "presented frames moved (guest awake)";
                                break;
                            }
                            if (fw_switch_exiting && fw_switch_exiting()) {
                                stopReason = "process exiting";
                                break;
                            }
                            VI_HLE_ProcessRetracesDeferred(1);
                            VI_HLE_ProcessRetracesDeferred(1);
                            Audio_HLE_PollDeferred();
                            ++roundsDone;
                            if (traceCnt) {
                                RT_LOGF(RT_TAG_VI, "[stallsvc] burst %u round %d done\n", bursts, roundsDone);
                            }
                        }
                        RT_LOGF(RT_TAG_VI, "[stallsvc] burst %u complete: %d round(s), stopped: %s\n",
                                bursts, roundsDone, stopReason);
                        if (bursts >= 3) {
                            RT_LOGF(RT_TAG_VI,
                                    "[stallsvc] burst cap reached (3 bursts this boot); service quiet for the rest of the boot\n");
                        }
                        continue;
                    }
                    inEpisode = true;
                    episodeAiBase = aiCbNow;
                    episodeEnqBase = enqueuesNow;
                    episodeAiMoved = episodeEnqMoved = false;
                }
                // Count mode only below: an open episode delivers the retrace count
                // word once per tick and nothing else (forensics Q4 stage A). First,
                // the witnesses: any ai_cb or enqueue movement during the episode is
                // logged once per feeder, as the mode's falsifier signal.
                if (!episodeAiMoved && aiCbNow != episodeAiBase) {
                    episodeAiMoved = true;
                    RT_LOGF(RT_TAG_VI,
                            "[stallsvc] count-mode witness: ai_cb moved during the episode (%u -> %u); in counter-only delivery nothing but the retrace count moves\n",
                            episodeAiBase, aiCbNow);
                }
                if (!episodeEnqMoved && enqueuesNow != episodeEnqBase) {
                    episodeEnqMoved = true;
                    RT_LOGF(RT_TAG_VI,
                            "[stallsvc] count-mode witness: enqueue counter moved during the episode (%u -> %u); the guest is issuing work\n",
                            episodeEnqBase, enqueuesNow);
                }
                if (firings >= 600) {
                    // Episode cap (study Q4): 600 firings is 30 s of service, far
                    // beyond the healthy transition's needs (~30 retraces). Closing
                    // restarts the silence clock, so re-arming requires a fresh full
                    // window even if the guest stays frozen.
                    RT_LOGF(RT_TAG_VI,
                            "[stallsvc] mode=count: episode cap reached (600 firings); episode closed, re-arming requires a fresh 2000 ms feeder silence\n");
                    inEpisode = false;
                    firings = 0;
                    lastPresent = now;
                    retraceChanged = aiCbChanged = enqueuesChanged = now;
                    continue;
                }
                ++firings;
                if (traceCnt) {
                    RT_LOGF(RT_TAG_VI, "[stallsvc] firing #%u (stalled %lld ms, frames=%u)\n", firings,
                            static_cast<long long>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPresent).count()),
                            frames);
                }
                VI_HLE_AdvanceRetraceCountOnly();
            }
        }).detach();
    });
}

// Report where the game's own loader places its boot RELs. The translated build expects fixed
// heap addresses; a mismatch means the
// guest heap layout differs from hardware and translated REL code would run against wrong data.
// Walks __OSModuleInfoList (0x800030C8) once per retrace until all have been reported.
void LogGuestModulesOnce() {
    struct ExpectedModule { uint32_t id; const char* name; uint32_t image; uint32_t bss; };
    static constexpr ExpectedModule kExpected[] = {
        {1, "relF", 0x80BF6840u, 0x80F12640u},  // id, name, image and bss addresses
    };
    static uint32_t s_reported = 0;  // one bit per module id
    if (s_reported == 0x2u) {
        return;
    }
    uint32_t module = 0;
    if (!Memory::TryRead32(0x800030C8u, module)) {
        return;
    }
    for (int guard = 0; module != 0 && guard < 16; ++guard) {
        uint32_t id = 0, next = 0, sectionInfo = 0, sectionBytes = 0, bss = 0;
        if (!Memory::TryRead32(module + 0x00u, id) || !Memory::TryRead32(module + 0x04u, next) ||
            !Memory::TryRead32(module + 0x10u, sectionInfo) || !Memory::TryRead32(module + 0x30u, sectionBytes)) {
            return;
        }
        const uint32_t bssSection = sectionBytes & 0xFFu;  // OSModuleHeader.bssSection (+0x33)
        if (bssSection != 0) {
            Memory::TryRead32(sectionInfo + bssSection * 8u, bss);  // absolute after OSLink
        }
        if (id < 32 && (s_reported & (1u << id)) == 0) {
            s_reported |= 1u << id;
            const ExpectedModule* expected = nullptr;
            for (const auto& candidate : kExpected) {
                if (candidate.id == id) {
                    expected = &candidate;
                }
            }
            const bool match = expected != nullptr && expected->image == module &&
                               (bss == 0 || (bss & ~1u) == expected->bss);
            RT_LOGF(RT_TAG_OS, "REL module %u (%s) linked at image 0x%08X bss 0x%08X: %s\n", id,
                    expected ? expected->name : "unexpected", module, bss,
                    match ? "MATCH" : "MISMATCH vs the pre-linked layout");
        }
        module = next;
    }
}

// Log every sequence switch (logo, title, game, ...), so automated bring-up runs show how far the
// game got without screenshots.
void LogSceneChanges() {
    // Guest words read as the current and next sequence (.sdata, r13-0x7E48 / -0x7E44) and the stage.
    static constexpr const char* kSeqNames[] = {"logo", "title", "game", "mapchange", "gameover", "load"};
    static uint32_t s_last = 0xFFFFFFFFu;
    uint32_t now = 0, next = 0, stage = 0;
    if (!Memory::TryRead32(0x8056D0D8u, now) || !Memory::TryRead32(0x8056D0DCu, next) ||
        !Memory::TryRead32(0x804CF364u, stage)) {
        return;
    }
    const uint32_t key = (now & 0xFFu) << 16 | (next & 0xFFu) << 8 | (stage & 0xFFu);
    if (key == s_last) {
        return;
    }
    s_last = key;
    const auto name = [](uint32_t v) { return v < 6 ? kSeqNames[v] : "none"; };
    RT_LOGF(RT_TAG_VI, "seq -> %s (next %s) stage %u (frame %u)\n", name(now), name(next), stage,
            s_presentedXfbFrames.load(std::memory_order_relaxed));
}

// Drain-at-retrace (RETRACE-HALF-STUDY R5): shared state for the retrace
// drain in AdvanceRetrace below and the Fix 2 present hook further down this
// file. The latch is set once, at the boot's first [fix2] engaged edge (the
// hook sets it): fix2 is armed and the guest's frame-swap relay is already
// dead, so live-flow delivery timing is untouched - outside stranded
// episodes the held GX deque keeps exactly its built drains (the seam pass
// and the idle pump). Once latched it stays latched for the rest of the
// boot: strandings recur at every movie boundary, and re-latching per
// episode adds nothing. The two counters are the drain's report fields,
// surfaced on the hook's escalated/adv-dead/revived lines as drains=%u
// (retrace passes that delivered at least one callback) and drained=%u
// (callbacks those passes delivered).
static std::atomic<bool> s_fix2DrainLatched{false};
static std::atomic<uint32_t> s_fix2DrainPasses{0};
static std::atomic<uint32_t> s_fix2DrainedCallbacks{0};
// One epoch for every [fix2] line's t= field, the present hook's and the
// retrace drain's alike, so timestamps compare across lines.
static long long Fix2ElapsedMs() {
    static const Clock::time_point epoch = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count();
}

void AdvanceRetrace(CpuContext* ctx, Clock::time_point retraceStamp, bool serviceAurora) {
    // Prevent re-entry - this can happen if OSWakeupThread triggers SelectThread
    // which goes idle and calls ProcessTimerEvents again
    if (s_inAdvanceRetrace.exchange(true)) {
        return;
    }
    StartStallWatchdogOnce();
    LogGuestModulesOnce();
    LogSceneChanges();

    uint32_t preCb = 0;
    uint32_t postCb = 0;
    uint32_t retraceValue = 0;
    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    bool isBlack = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        
        // Commit pending state if VIFlush armed it (see ViState).
        if (g_vi.flushArmed) {
            // Commit pending -> active
            g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
            g_vi.black = g_vi.pendingBlack;
            g_vi.tvFormat = g_vi.pendingTvFormat;
            g_vi.renderWidth = g_vi.pendingRenderWidth;
            g_vi.renderHeight = g_vi.pendingRenderHeight;
            g_vi.viXOrigin = g_vi.pendingViXOrigin;
            g_vi.viYOrigin = g_vi.pendingViYOrigin;
            g_vi.xfbWidth = g_vi.pendingXfbWidth;
            g_vi.xfbHeight = g_vi.pendingXfbHeight;
            g_vi.retraceInterval = g_vi.pendingRetraceInterval;
            
            // Clear flush armed flag
            g_vi.flushArmed = false;
            
        }
        
        g_vi.retraceCount++;
        g_vi.fieldOdd = !g_vi.fieldOdd;
        g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
        currentFb = g_vi.currentFrameBuffer;
        g_vi.lastRetrace = retraceStamp;
        RefreshDisplayLockLocked(n8_vi_now());  // N8 C7: retrace lock read
        g_vi.lockedInterval = LockedIntervalAfter(retraceStamp);
        retraceValue = g_vi.retraceCount;
        preCb = g_vi.preRetraceCallback;
        postCb = g_vi.postRetraceCallback;
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        isBlack = g_vi.black;
        WriteGuestStateLocked();
    }

    // Deterministic mode: this retrace starts a frame. Fingerprint guest memory as the previous
    // frame left it, then latch every channel's input for the new one before any guest code reads it.
    if (DetClock::Enabled()) {
        DetHash::OnRetrace(retraceValue, ctx ? ctx->gpr[1] : 0);
        FrameInput::Latch(retraceValue);
    }

    // Wake up threads sleeping on the VI retrace queue (VIWaitForRetrace).
    // The retrace count has been incremented and written to guest memory.
    if (ctx) {
        // Bring-up trace: the first wakeups show whether a VIWaitForRetrace sleeper was queued and
        // whether waking it made the OS see a runnable thread.
        static int s_wakeLogs = 0;
        const bool logWake = s_wakeLogs < 6;
        uint32_t queueHead = 0;
        if (logWake) {
            Memory::TryRead32(kViRetraceQueueAddr, queueHead);
        }
        ctx->gpr[3] = kViRetraceQueueAddr;
        OSWakeupThread_HLE_801aaaa4(ctx);
        if (logWake) {
            ++s_wakeLogs;
            uint32_t runQueueBits = 0;
            Memory::TryRead32(0x80271370u, runQueueBits);  // RunQueueBits (NO100F)
            RT_LOGF(RT_TAG_VI, "[retrace %u] woke retrace queue (head 0x%08X) -> RunQueueBits 0x%08X\n",
                    retraceValue, queueHead, runQueueBits);
        }
    }

    if (serviceAurora) {
        // Process window events (but don't present - that happens in GXCopyDisp)
        UpdateAuroraAndProcessEvents();

        // Start a new Aurora frame if one isn't already active
        if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
            if (BeginAuroraFrame()) {
                g_auroraFrameActive.store(true, std::memory_order_release);
            }
        }
    }

    { static int n = 0; if (n < 6 && (preCb || postCb || !ctx)) { ++n; RT_LOGF(RT_TAG_VI, "[retrace %u] callbacks pre 0x%08X post 0x%08X ctx %d\n", retraceValue, preCb, postCb, ctx != nullptr); } }
    if (ctx) {
        ctx->gpr[3] = retraceValue;
        // DRAIN-AT-RETRACE (RETRACE-HALF-STUDY R5): deliver the held GX
        // deque here, at retrace cadence, immediately before the pre-retrace
        // callback - the per-pass order is drain -> AF0 -> BB0 by
        // construction of this call site. Episode 2 of the escalation boot
        // starved on a one-delivery lag: a D4 charge's latch pulse is
        // consumable in exactly one retrace pass, and arm's recharge (an
        // AA4 match delivery) lagged one seam/idle drain behind it, so each
        // drain bought ~1 genuine advancer run and the chain died between
        // drains. Draining here puts the AA4 queued by pass N-1's advancer
        // run into the pass that tests that run's D4 charge: match ->
        // arm := 1, AF0 -> latch := 1, BB0 (arm and latch) -> the advancer
        // runs again, on the guest's own callbacks. The delivery runs under
        // the seam pass's deferred-callback scope
        // (OS_HLE_Begin/EndDeferredGuestCallbacks), on this retrace's
        // context - a context that already executes AF0 and BB0 guest code
        // microseconds later; the drain invokes each callback on a copy of
        // it. Never on the count-only path: VI_HLE_AdvanceRetraceCountOnly
        // stays delivery-free, so the stall service's thread never executes
        // guest code.
        if (s_fix2DrainLatched.load(std::memory_order_relaxed) &&
            Dvd_HLE_GxPendingCount() > 0) {
            const uint32_t deliveredBefore = Dvd_HLE_GxDeliveredTotal();
            OS_HLE_BeginDeferredGuestCallbacks();
            const bool drainDelivered = DvdProcessPendingGxCallbacks(ctx);
            OS_HLE_EndDeferredGuestCallbacks();
            if (drainDelivered) {
                s_fix2DrainPasses.fetch_add(1, std::memory_order_relaxed);
                s_fix2DrainedCallbacks.fetch_add(
                    Dvd_HLE_GxDeliveredTotal() - deliveredBefore, std::memory_order_relaxed);
                static std::atomic<bool> s_drainLogged{false};
                if (!s_drainLogged.exchange(true, std::memory_order_relaxed)) {
                    RT_LOGF(RT_TAG_VI, "[fix2] drain-engaged t=%lldms breakpt=%u presents=%u\n",
                            Fix2ElapsedMs(), GxFifo_HLE_BreakPtCalls(),
                            s_presentedXfbFrames.load(std::memory_order_relaxed));
                }
            }
        }
        if (preCb) {
            InvokeIndirectCpu(preCb, ctx);
        }
        if (postCb) {
            // Follow the SDK: call whatever callback is set.
            InvokeIndirectCpu(postCb, ctx);
        }
    }

    // VISetBlack(TRUE) keeps frame submission running but shows only the clear color: GX render work is
    // skipped and Aurora's end_frame() clears to black, matching the hardware manual's "signal continues,
    // pixels go black" behavior. When not black, submission waits for hasXfbReady (GXCopyDisp done).
    if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire)) {
        const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
        const bool xfbMatches = (readyXfb != 0 && readyXfb == currentFb);
        const bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
        const bool shouldPresentBlack = isBlack && frameActive;
        const bool shouldSubmit = frameActive && (shouldPresentXfb || shouldPresentBlack);

        if (shouldSubmit) {
            if (!isBlack) {
                // Normal presentation: draw overlay on top of GX content
                settings_overlay::Draw();
            }
            // VI black remains a pure black presentation.
            // Unpaced: this present already runs in retrace context.
            VI_HLE_PresentFrame(shouldPresentXfb, false);
        } else if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !shouldPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            // Don't call aurora_end_frame() as that would present incomplete work
            UpdateAuroraAndProcessEvents();
        }
    }

    // Clear re-entry guard
    s_inAdvanceRetrace.store(false);
}

bool AdvanceDueRetraces(CpuContext* ctx, int maxToProcess, bool serviceAurora)
{
    bool advancedAny = false;

    if (DetClock::Enabled()) {
        for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                if (!g_vi.initialized) {
                    return advancedAny;
                }
                const uint64_t due = g_vi.detLastRetraceTicks + IntervalTicks(g_vi.retraceInterval);
                if (DetClock::Now() < due) {
                    return advancedAny;
                }
                g_vi.detLastRetraceTicks = due;
            }
            AdvanceRetrace(ctx, n8_vi_now(), serviceAurora);  // N8 C7: AdvanceRetrace read
            advancedAny = true;
        }
        return advancedAny;
    }

    for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
        Clock::time_point target;
        auto now = n8_vi_now();  // N8 C7: AdvanceRetrace read
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            if (!g_vi.initialized) {
                return advancedAny;
            }
            target = g_vi.lastRetrace + ActiveIntervalLocked();
            if (now < target) {
                return advancedAny;
            }
        }


        AdvanceRetrace(ctx, target, serviceAurora);
        advancedAny = true;
    }

    return advancedAny;
}

} // namespace

// Force one retrace boundary to pass, whether or not its wall-clock deadline
// has arrived, so the guest's retrace callbacks (AsyncDisplay's counters and
// friends) run. VI_HLE_PollRetrace below is the time-driven counterpart.
void VI_HLE_ForceRetrace(CpuContext* ctx) {
    Clock::time_point target;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        target = g_vi.lastRetrace + ActiveIntervalLocked();
    }
    AdvanceRetrace(ctx, target, true);
}

bool VI_HLE_IsAdvancingRetrace() {
    return s_inAdvanceRetrace.load(std::memory_order_acquire);
}

// Advance every retrace whose interval has already elapsed. Safe to call from
// busy loops (GX drawing, the scheduler's idle spin) to keep VBlank ticking.
void VI_HLE_PollRetrace(CpuContext* ctx) {
    AdvanceDueRetraces(ctx, 8, true);
}

void VI_HLE_ProcessRetracesDeferred(int maxToProcess) {
    // A wall-clock pump (GX work, host frame waits); deterministic mode delivers retraces only from
    // the scheduler and the guest's own VI calls.
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled() || DetClock::Enabled()) {
        return;
    }

    // This entry point runs from the middle of an arbitrary translated function
    // (GX__Begin's timing service, the host frame loop). Retrace callbacks are
    // an interrupt from that function's point of view, so they get a private
    // copy of its register file exactly as the hardware interrupt would.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    // Host renderer ownership is not a guest critical section. Deliver due VI
    // callbacks at wall-clock cadence while suppressing thread switches and
    // all recursive Aurora/event work until the native GX call has unwound.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        AdvanceDueRetraces(cpu, maxToProcess, false);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

// Counter-only twin of AdvanceRetrace's locked section (EPISODE-FORENSICS-STUDY
// Q4 stage A): advance the retrace count and mirror it to guest memory exactly
// as a real retrace does - the flush commit, the count, the field parity, the
// framebuffer latch, the retrace stamp and the display lock - but deliver
// nothing: no retrace-queue wake, no pre/post callbacks, no guest code at all.
// A guest waiting on the raw count word is released by this alone; a guest
// waiting on a callback-maintained counter (AsyncDisplay, the AX fade) is not,
// which is precisely the question stage A exists to answer.
void VI_HLE_AdvanceRetraceCountOnly() {
    const auto stamp = Clock::now();
    std::lock_guard<std::mutex> lock(g_viMutex);
    EnsureInitializedLocked();
    // Commit pending state if VIFlush armed it (see ViState): the same commit
    // AdvanceRetrace performs under this lock before it counts.
    if (g_vi.flushArmed) {
        g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
        g_vi.black = g_vi.pendingBlack;
        g_vi.tvFormat = g_vi.pendingTvFormat;
        g_vi.renderWidth = g_vi.pendingRenderWidth;
        g_vi.renderHeight = g_vi.pendingRenderHeight;
        g_vi.viXOrigin = g_vi.pendingViXOrigin;
        g_vi.viYOrigin = g_vi.pendingViYOrigin;
        g_vi.xfbWidth = g_vi.pendingXfbWidth;
        g_vi.xfbHeight = g_vi.pendingXfbHeight;
        g_vi.retraceInterval = g_vi.pendingRetraceInterval;
        g_vi.flushArmed = false;
    }
    g_vi.retraceCount++;
    g_vi.fieldOdd = !g_vi.fieldOdd;
    g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
    g_vi.lastRetrace = stamp;
    RefreshDisplayLockLocked(stamp);
    g_vi.lockedInterval = LockedIntervalAfter(stamp);
    WriteGuestStateLocked();
}

uint64_t VI_HLE_DetNextRetraceTicks() {
    std::lock_guard<std::mutex> lock(g_viMutex);
    if (!g_vi.initialized) {
        return DetClock::kNever;
    }
    return g_vi.detLastRetraceTicks + IntervalTicks(g_vi.retraceInterval);
}

void VI_HLE_WaitForNextRetracePoll() {
    Clock::time_point retraceDeadline;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        retraceDeadline = g_vi.lastRetrace + ActiveIntervalLocked();
    }

    const auto now = n8_vi_now();  // N8 C7: WaitForNextRetracePoll read
    if (now >= retraceDeadline) {
        return;
    }
    // Audio DMA and alarm queues still need regular service even when the VI
    // deadline is farther away. The high-resolution wait removes the 1 ms
    // scheduler overshoot when retrace is the next event.
    const bool retraceIsNext = retraceDeadline <= now + 1ms;
    SleepPreciselyUntil(retraceIsNext ? retraceDeadline : now + 1ms, retraceIsNext,
                        kFinalSpinWindow);
}

namespace {

// Retrace count consumed by the most recent paced present. This is a slot memo
// against the VI timeline, not a second clock: it only answers "did a retrace
// already elapse while this frame was being produced?". Producer-thread only.
uint32_t s_lastPacedRetraceCount = ~0u;

// Last presentation anchor handed to Aurora, in nanoseconds on the VI retrace
// grid. Guarantees consecutive sealed frames never share an anchor (see the
// comment at the stamping site). Producer-thread only, like the memo above.
uint64_t s_lastPresentAnchorNanos = 0;

// Sleeps to the same VI retrace boundary VIWaitForRetrace targets, servicing alarms every 1 ms so audio
// DMA and timers keep running, then delivers that retrace so guest logic starts exactly on the grid.
void PaceToRetraceBoundary(Clock::time_point deadline) {
    constexpr auto kServiceSlice = 1ms;
    for (;;) {
        const auto now = n8_vi_now();  // N8 C7: PaceToRetraceBoundary read
        if (now >= deadline) {
            break;
        }
        if (deadline - now > kServiceSlice + kFinalSpinWindow) {
            SleepPreciselyUntil(now + kServiceSlice);
            OS_HLE_ProcessAlarmsDeferred(8);
            // Audio DMA is a 3 ms cadence and this wait is up to a full display
            // period long. Without a pump here the blocks that came due during
            // the wait are all delivered at once when the guest next reaches
            // idle, which the guest observes as AI slack jitter.
            Audio_HLE_PollDeferred();
            continue;
        }
        SleepPreciselyUntil(deadline, true, kFinalSpinWindow);
        break;
    }
    VI_HLE_ProcessRetracesDeferred(1);
}

} // namespace

// Single owner of the Aurora frame presentation sequence: seals the active frame, optionally paces the
// producer to the VI retrace boundary, and pre-warms the next frame. Paced from GXCopyDisp; unpaced for
// the retrace-context black/boot present path in AdvanceRetrace.
// NO100F_LOG_FPS (debug.no100f.logfps on Android): every two seconds, the frames presented and where
// each frame's time went: the game (guest code and GX command building), aurora_end_frame (encoding,
// submission, and waiting on the GPU when it falls behind) and pacing to the retrace. A game share near
// the frame budget is CPU-bound; a large end_frame share is GPU-bound.
struct FrameTimes {
    Clock::time_point windowStart{};
    Clock::time_point lastPresentEnd{};
    double gameMs = 0, endFrameMs = 0, paceMs = 0, worstMs = 0, sumSq = 0;
    int slow = 0;
    int frames = 0;
};

static bool LogFpsEnabled() {
    static const bool on = std::getenv("NO100F_LOG_FPS") != nullptr;
    return on;
}

// phasetime (PHASETIME design, Nov 19, 2026): per-phase frame telemetry. Armed by dk_diag
// phasetime (SCOOBY_PHASETIME); unarmed, the sites below cost one static-bool check each and
// log nothing. The presenting thread is the main thread; captured at the first XFB present.
static bool PhaseTimeArmed() {
    static const bool armed = std::getenv("SCOOBY_PHASETIME") != nullptr;
    return armed;
}
static uint32_t s_phaseMainThread = 0;

// dvd.cpp: time in the game's disc reads since start, and their count.
uint64_t DvdReadNanosTotal();
uint32_t DvdReadCount();
// The game thread's waits and side costs, each a total since start (see their definitions).
uint64_t GuestSchedulerIdleNanosTotal();                                           // os/os_scheduler.cpp
uint64_t AxMixJoinNanosTotal();                                                    // audio/ax_mix.cpp
void CardOpenTotals(uint64_t* calls, uint64_t* nanos);                             // storage/card_gc.cpp
void GxCopyTotals(uint64_t* texCopies, uint64_t* dispCopies, uint64_t* overlayNanos);  // gx/gx_copy.cpp
void GxFifoWriteTotals(uint64_t* calls, uint64_t* bytes, uint64_t* dlCalls, uint64_t* burstCalls,
                       uint64_t* burstBytes);                                      // gx/gx_fifo.cpp
void GxBeginFrameWaitTotals(uint64_t* nanos, uint64_t* sleeps);                    // gx/gx_frame.cpp
void GxFifoFastTotals(uint64_t* fastWords, uint64_t* mismatches);                  // gx/gx_fifo.cpp
void GxL2Totals(uint64_t* sampledCalls, uint64_t* dispatchNanos, uint64_t* storeNanos,
                uint64_t* wakeNanos, uint64_t* runWords, uint64_t* runs1, uint64_t* runs2_4,
                uint64_t* runs5_16, uint64_t* runs17);                             // gx/gx_fifo.cpp
namespace aurora::gfx {
uint64_t staging_split_count() noexcept;  // gfx/common.cpp
}  // namespace aurora::gfx
uint64_t GxBBoxClearCount();                                                           // gx/gx_stubs.cpp
uint64_t GxBBoxReadCount();                                                            // gx/gx_stubs.cpp
uint64_t PhaseTimeAudioDelivNanosTotal();                                      // audio/audio.cpp
uint64_t PhaseTimeFirstPacketNanos();                                          // gx/gx_fifo.cpp
void PhaseTimeClearFirstPacket();                                              // gx/gx_fifo.cpp
uint32_t PhaseTimeThreadSnapshot(uint32_t* addrs, uint64_t* nanos, uint64_t* switchesOut);  // os/os_scheduler.cpp
// N8 counters (Step 2 measurement build): per-site steady_clock reads and the
// C2/C3/C6 totals, each a total since start (see their definitions). The
// nativescene getters (No100fDlSiteTotals, No100fGeomTotals,
// No100fGeomPrintTable) come via no100f_native_scene.h.
void aurora_n8_totals(uint64_t* xUnmovedBytes, uint64_t* xMovedBytes, uint64_t* xUploads,
                      uint64_t* processCalls, uint64_t* pipeDistinct,
                      uint64_t* clockProcTimer, uint64_t* processCallsDl,
                      uint64_t* processCallsDrain);                      // aurora/command_processor.cpp
void aurora_n8_clock_totals(uint64_t* waits, uint64_t* present);         // aurora/aurora.cpp
uint64_t gx_n8_deferred_clock_total();                                   // gx/gx_vertex.cpp
uint64_t gxframe_n8_clock_total();                                       // gx/gx_frame.cpp
uint64_t ossched_n8_idle_clock_total();                                  // os/os_scheduler.cpp
uint64_t ppchelp_n8_timebase_clock_total();                              // ppc_helpers.cpp
uint64_t ossleep_n8_clock_total();                                       // os/os_sleep.cpp
uint64_t gxcopy_n8_overlay_clock_total();                                // gx/gx_copy.cpp
uint64_t osintr_n8_clock_total();                                        // os/os_interrupt.cpp
uint64_t pad_n8_other_clock_total();                                     // pad.cpp

namespace {
// A snapshot of those totals; frames take differences.
struct FrameSplit {
    uint64_t idle = 0, gxLock = 0, waitSealed = 0, waitDone = 0, mixJoin = 0, card = 0, cardOpens = 0, overlay = 0,
             texCopies = 0, dispCopies = 0, storageCalls = 0, storageBytes = 0, cacheHits = 0, cacheMisses = 0,
             cacheSkipped = 0, urUploads = 0, urFallbacks = 0, urSaved = 0, beginWait = 0, beginSleeps = 0,
             fifoCalls = 0, fifoBytes = 0, fifoDlCalls = 0,
             burstCalls = 0, burstBytes = 0, fastWords = 0, fastMismatches = 0,
             bboxClears = 0, bboxReads = 0, bboxDisarms = 0, bboxArmedVerts = 0, bboxAccumCalls = 0,
             pipeBumps = 0, memoHits = 0, memoMisses = 0, tableHits = 0, tableMisses = 0, gxProcess = 0,
             stageAdmit = 0, stageResolve = 0, stageUploads = 0, stageUniform = 0, stageRecord = 0,
             stageBBox = 0, audioDeliv = 0,  // phasetime: cumulative host audio-delivery nanos (audio/audio.cpp)
             l2Sampled = 0, l2DispatchNs = 0, l2StoreNs = 0, l2WakeNs = 0, l2RunWords = 0, l2Runs1 = 0,
             l2Runs2_4 = 0, l2Runs5_16 = 0, l2Runs17 = 0, stagingSplits = 0;  // L2 counters
    static FrameSplit Now() {
        FrameSplit s;
        GxFifoFastTotals(&s.fastWords, &s.fastMismatches);
        GxBeginFrameWaitTotals(&s.beginWait, &s.beginSleeps);
        GxFifoWriteTotals(&s.fifoCalls, &s.fifoBytes, &s.fifoDlCalls, &s.burstCalls, &s.burstBytes);
        aurora_push_storage_totals(&s.storageCalls, &s.storageBytes);
        aurora_storage_cache_totals(&s.cacheHits, &s.cacheMisses, &s.cacheSkipped);
        aurora_used_range_totals(&s.urUploads, &s.urFallbacks, &s.urSaved);
        aurora_bbox_totals(&s.bboxArmedVerts, &s.bboxAccumCalls, &s.bboxDisarms);
        aurora_pipeline_memo_totals(&s.pipeBumps, &s.memoHits, &s.memoMisses, &s.tableHits, &s.tableMisses);
        s.bboxClears = GxBBoxClearCount();
        s.bboxReads = GxBBoxReadCount();
        s.gxProcess = AuroraGxProcessNanosTotal();
        s.audioDeliv = PhaseTimeAudioDelivNanosTotal();
        GxL2Totals(&s.l2Sampled, &s.l2DispatchNs, &s.l2StoreNs, &s.l2WakeNs, &s.l2RunWords, &s.l2Runs1,
                   &s.l2Runs2_4, &s.l2Runs5_16, &s.l2Runs17);
        s.stagingSplits = aurora::gfx::staging_split_count();
        AuroraStageTimerTotals(&s.stageAdmit, &s.stageResolve, &s.stageUploads, &s.stageUniform,
                               &s.stageRecord, &s.stageBBox);
        s.idle = GuestSchedulerIdleNanosTotal();
        s.gxLock = AuroraGxLockNanosTotal();
        uint64_t sealedWaits = 0, doneWaits = 0;
        aurora_frame_worker_wait_totals(&s.waitSealed, &s.waitDone, &sealedWaits, &doneWaits);
        s.mixJoin = AxMixJoinNanosTotal();
        CardOpenTotals(&s.cardOpens, &s.card);
        GxCopyTotals(&s.texCopies, &s.dispCopies, &s.overlay);
        return s;
    }
    FrameSplit operator-(const FrameSplit& o) const {
        FrameSplit r;
        r.idle = idle - o.idle;
        r.gxLock = gxLock - o.gxLock;
        r.waitSealed = waitSealed - o.waitSealed;
        r.waitDone = waitDone - o.waitDone;
        r.mixJoin = mixJoin - o.mixJoin;
        r.card = card - o.card;
        r.cardOpens = cardOpens - o.cardOpens;
        r.overlay = overlay - o.overlay;
        r.texCopies = texCopies - o.texCopies;
        r.dispCopies = dispCopies - o.dispCopies;
        r.storageCalls = storageCalls - o.storageCalls;
        r.storageBytes = storageBytes - o.storageBytes;
        r.cacheHits = cacheHits - o.cacheHits;
        r.cacheMisses = cacheMisses - o.cacheMisses;
        r.cacheSkipped = cacheSkipped - o.cacheSkipped;
        r.urUploads = urUploads - o.urUploads;
        r.urFallbacks = urFallbacks - o.urFallbacks;
        r.urSaved = urSaved - o.urSaved;
        r.beginWait = beginWait - o.beginWait;
        r.beginSleeps = beginSleeps - o.beginSleeps;
        r.fifoCalls = fifoCalls - o.fifoCalls;
        r.fifoBytes = fifoBytes - o.fifoBytes;
        r.fifoDlCalls = fifoDlCalls - o.fifoDlCalls;
        r.burstCalls = burstCalls - o.burstCalls;
        r.burstBytes = burstBytes - o.burstBytes;
        r.fastWords = fastWords - o.fastWords;
        r.fastMismatches = fastMismatches - o.fastMismatches;
        r.bboxClears = bboxClears - o.bboxClears;
        r.bboxReads = bboxReads - o.bboxReads;
        r.bboxDisarms = bboxDisarms - o.bboxDisarms;
        r.bboxArmedVerts = bboxArmedVerts - o.bboxArmedVerts;
        r.bboxAccumCalls = bboxAccumCalls - o.bboxAccumCalls;
        r.pipeBumps = pipeBumps - o.pipeBumps;
        r.memoHits = memoHits - o.memoHits;
        r.memoMisses = memoMisses - o.memoMisses;
        r.tableHits = tableHits - o.tableHits;
        r.tableMisses = tableMisses - o.tableMisses;
        r.gxProcess = gxProcess - o.gxProcess;
        r.stageAdmit = stageAdmit - o.stageAdmit;
        r.stageResolve = stageResolve - o.stageResolve;
        r.stageUploads = stageUploads - o.stageUploads;
        r.stageUniform = stageUniform - o.stageUniform;
        r.stageRecord = stageRecord - o.stageRecord;
        r.stageBBox = stageBBox - o.stageBBox;
        r.audioDeliv = audioDeliv - o.audioDeliv;
        r.l2Sampled = l2Sampled - o.l2Sampled;
        r.l2DispatchNs = l2DispatchNs - o.l2DispatchNs;
        r.l2StoreNs = l2StoreNs - o.l2StoreNs;
        r.l2WakeNs = l2WakeNs - o.l2WakeNs;
        r.l2RunWords = l2RunWords - o.l2RunWords;
        r.l2Runs1 = l2Runs1 - o.l2Runs1;
        r.l2Runs2_4 = l2Runs2_4 - o.l2Runs2_4;
        r.l2Runs5_16 = l2Runs5_16 - o.l2Runs5_16;
        r.l2Runs17 = l2Runs17 - o.l2Runs17;
        r.stagingSplits = stagingSplits - o.stagingSplits;
        return r;
    }
};

// N8 counters snapshot (Step 2 measurement build): the same Now()/operator-
// shape as FrameSplit, but only the N8 counters. All fields cumulative since
// start; the window takes differences. The C5 per-identity table is printed
// separately (No100fGeomPrintTable) once per window from Log().
struct N8Split {
    uint64_t xUnmoved, xMoved, xUploads, processCalls, pipeDistinct, clockProcTimer;
    uint64_t processCallsDl, processCallsDrain;  // L2: C3 split by caller (DL path vs drain path)
    uint64_t clockAuroraWait, clockAuroraPresent, clockGxDeferred, clockGxFrame, clockOsSchedIdle;
    uint64_t clockPpcTimeBase, clockOsSleep, clockGxCopy, clockOsIntr, clockPadOther, clockVi;
    uint64_t clockViSpin, dlAtomicSubs, dlAtomicDraws, dlSectorSubs, dlSectorDraws, dlIm3dSubs;
    uint64_t dlIm3dDraws, dlOtherSubs, dlOtherDraws, dlUnnotedSubs, dlUnnotedDraws, dlHostSubs;
    uint64_t geomRenders, geomAllowRenders, geomUncatRenders, geomUnknownRenders, geomSiteDraws, geomDistinct;
    static N8Split Now() {
        N8Split s;
        aurora_n8_totals(&s.xUnmoved, &s.xMoved, &s.xUploads, &s.processCalls, &s.pipeDistinct,
                         &s.clockProcTimer, &s.processCallsDl, &s.processCallsDrain);
        aurora_n8_clock_totals(&s.clockAuroraWait, &s.clockAuroraPresent);
        s.clockGxDeferred = gx_n8_deferred_clock_total();
        s.clockGxFrame = gxframe_n8_clock_total();
        s.clockOsSchedIdle = ossched_n8_idle_clock_total();
        s.clockPpcTimeBase = ppchelp_n8_timebase_clock_total();
        s.clockOsSleep = ossleep_n8_clock_total();
        s.clockGxCopy = gxcopy_n8_overlay_clock_total();
        s.clockOsIntr = osintr_n8_clock_total();
        s.clockPadOther = pad_n8_other_clock_total();
        s.clockVi = s_n8ClockVi.load(std::memory_order_relaxed);
        s.clockViSpin = s_n8ClockViSpin.load(std::memory_order_relaxed);
        No100fDlSiteTotals(&s.dlAtomicSubs, &s.dlAtomicDraws, &s.dlSectorSubs, &s.dlSectorDraws,
                           &s.dlIm3dSubs, &s.dlIm3dDraws, &s.dlOtherSubs, &s.dlOtherDraws,
                           &s.dlUnnotedSubs, &s.dlUnnotedDraws, &s.dlHostSubs);
        No100fGeomTotals(&s.geomRenders, &s.geomAllowRenders, &s.geomUncatRenders,
                         &s.geomUnknownRenders, &s.geomSiteDraws, &s.geomDistinct);
        return s;
    }
    N8Split operator-(const N8Split& o) const {
        N8Split r;
        r.xUnmoved = xUnmoved - o.xUnmoved;
        r.xMoved = xMoved - o.xMoved;
        r.xUploads = xUploads - o.xUploads;
        r.processCalls = processCalls - o.processCalls;
        r.processCallsDl = processCallsDl - o.processCallsDl;
        r.processCallsDrain = processCallsDrain - o.processCallsDrain;
        r.pipeDistinct = pipeDistinct - o.pipeDistinct;
        r.clockProcTimer = clockProcTimer - o.clockProcTimer;
        r.clockAuroraWait = clockAuroraWait - o.clockAuroraWait;
        r.clockAuroraPresent = clockAuroraPresent - o.clockAuroraPresent;
        r.clockGxDeferred = clockGxDeferred - o.clockGxDeferred;
        r.clockGxFrame = clockGxFrame - o.clockGxFrame;
        r.clockOsSchedIdle = clockOsSchedIdle - o.clockOsSchedIdle;
        r.clockPpcTimeBase = clockPpcTimeBase - o.clockPpcTimeBase;
        r.clockOsSleep = clockOsSleep - o.clockOsSleep;
        r.clockGxCopy = clockGxCopy - o.clockGxCopy;
        r.clockOsIntr = clockOsIntr - o.clockOsIntr;
        r.clockPadOther = clockPadOther - o.clockPadOther;
        r.clockVi = clockVi - o.clockVi;
        r.clockViSpin = clockViSpin - o.clockViSpin;
        r.dlAtomicSubs = dlAtomicSubs - o.dlAtomicSubs;
        r.dlAtomicDraws = dlAtomicDraws - o.dlAtomicDraws;
        r.dlSectorSubs = dlSectorSubs - o.dlSectorSubs;
        r.dlSectorDraws = dlSectorDraws - o.dlSectorDraws;
        r.dlIm3dSubs = dlIm3dSubs - o.dlIm3dSubs;
        r.dlIm3dDraws = dlIm3dDraws - o.dlIm3dDraws;
        r.dlOtherSubs = dlOtherSubs - o.dlOtherSubs;
        r.dlOtherDraws = dlOtherDraws - o.dlOtherDraws;
        r.dlUnnotedSubs = dlUnnotedSubs - o.dlUnnotedSubs;
        r.dlUnnotedDraws = dlUnnotedDraws - o.dlUnnotedDraws;
        r.dlHostSubs = dlHostSubs - o.dlHostSubs;
        r.geomRenders = geomRenders - o.geomRenders;
        r.geomAllowRenders = geomAllowRenders - o.geomAllowRenders;
        r.geomUncatRenders = geomUncatRenders - o.geomUncatRenders;
        r.geomUnknownRenders = geomUnknownRenders - o.geomUnknownRenders;
        r.geomSiteDraws = geomSiteDraws - o.geomSiteDraws;
        r.geomDistinct = geomDistinct - o.geomDistinct;
        return r;
    }
    N8Split& operator+=(const N8Split& o) {
        xUnmoved += o.xUnmoved;
        xMoved += o.xMoved;
        xUploads += o.xUploads;
        processCalls += o.processCalls;
        processCallsDl += o.processCallsDl;
        processCallsDrain += o.processCallsDrain;
        pipeDistinct += o.pipeDistinct;
        clockProcTimer += o.clockProcTimer;
        clockAuroraWait += o.clockAuroraWait;
        clockAuroraPresent += o.clockAuroraPresent;
        clockGxDeferred += o.clockGxDeferred;
        clockGxFrame += o.clockGxFrame;
        clockOsSchedIdle += o.clockOsSchedIdle;
        clockPpcTimeBase += o.clockPpcTimeBase;
        clockOsSleep += o.clockOsSleep;
        clockGxCopy += o.clockGxCopy;
        clockOsIntr += o.clockOsIntr;
        clockPadOther += o.clockPadOther;
        clockVi += o.clockVi;
        clockViSpin += o.clockViSpin;
        dlAtomicSubs += o.dlAtomicSubs;
        dlAtomicDraws += o.dlAtomicDraws;
        dlSectorSubs += o.dlSectorSubs;
        dlSectorDraws += o.dlSectorDraws;
        dlIm3dSubs += o.dlIm3dSubs;
        dlIm3dDraws += o.dlIm3dDraws;
        dlOtherSubs += o.dlOtherSubs;
        dlOtherDraws += o.dlOtherDraws;
        dlUnnotedSubs += o.dlUnnotedSubs;
        dlUnnotedDraws += o.dlUnnotedDraws;
        dlHostSubs += o.dlHostSubs;
        geomRenders += o.geomRenders;
        geomAllowRenders += o.geomAllowRenders;
        geomUncatRenders += o.geomUncatRenders;
        geomUnknownRenders += o.geomUnknownRenders;
        geomSiteDraws += o.geomSiteDraws;
        geomDistinct += o.geomDistinct;
        return *this;
    }
};

// 600 presented frames of FrameSplit and the frame buckets, logged as averages per frame.
struct FrameWindow {
    int frames = 0;
    double frameMs = 0, worstMs = 0, gameMs = 0, gxMs = 0, discMs = 0, endFrameMs = 0, paceMs = 0;
    FrameSplit sum;
    N8Split n8sum;
    // phasetime: per-window exact-span + thread accumulators (added to, never reset mid-window).
    double preSubmitMs = 0, submitMs = 0;
    double threadMainMs = 0, threadOtherMs = 0, threadTopMs = 0;
    uint32_t threadTopAddr = 0;
    uint64_t threadSwitches = 0;
    uint32_t threadWinAddrs[16] = {};
    double threadWinMs[16] = {};
    int threadWinCount = 0;
    void Add(double frame, double game, double gx, double disc, double endFrame, double pace, const FrameSplit& d,
             double preSubmit = 0, double submit = 0) {
        ++frames;
        frameMs += frame;
        preSubmitMs += preSubmit;
        submitMs += submit;
        worstMs = std::max(worstMs, frame);
        gameMs += game;
        gxMs += gx;
        discMs += disc;
        endFrameMs += endFrame;
        paceMs += pace;
        sum.idle += d.idle;
        sum.gxLock += d.gxLock;
        sum.waitSealed += d.waitSealed;
        sum.waitDone += d.waitDone;
        sum.mixJoin += d.mixJoin;
        sum.card += d.card;
        sum.cardOpens += d.cardOpens;
        sum.overlay += d.overlay;
        sum.texCopies += d.texCopies;
        sum.dispCopies += d.dispCopies;
        sum.storageCalls += d.storageCalls;
        sum.storageBytes += d.storageBytes;
        sum.cacheHits += d.cacheHits;
        sum.cacheMisses += d.cacheMisses;
        sum.cacheSkipped += d.cacheSkipped;
        sum.urUploads += d.urUploads;
        sum.urFallbacks += d.urFallbacks;
        sum.urSaved += d.urSaved;
        sum.beginWait += d.beginWait;
        sum.beginSleeps += d.beginSleeps;
        sum.fifoCalls += d.fifoCalls;
        sum.fifoBytes += d.fifoBytes;
        sum.fifoDlCalls += d.fifoDlCalls;
        sum.burstCalls += d.burstCalls;
        sum.burstBytes += d.burstBytes;
        sum.fastWords += d.fastWords;
        sum.fastMismatches += d.fastMismatches;
        sum.bboxClears += d.bboxClears;
        sum.bboxReads += d.bboxReads;
        sum.bboxDisarms += d.bboxDisarms;
        sum.bboxArmedVerts += d.bboxArmedVerts;
        sum.bboxAccumCalls += d.bboxAccumCalls;
        sum.pipeBumps += d.pipeBumps;
        sum.memoHits += d.memoHits;
        sum.memoMisses += d.memoMisses;
        sum.tableHits += d.tableHits;
        sum.tableMisses += d.tableMisses;
        sum.gxProcess += d.gxProcess;
        sum.audioDeliv += d.audioDeliv;  // phasetime
        sum.stageAdmit += d.stageAdmit;
        sum.stageResolve += d.stageResolve;
        sum.stageUploads += d.stageUploads;
        sum.stageUniform += d.stageUniform;
        sum.stageRecord += d.stageRecord;
        sum.stageBBox += d.stageBBox;
        sum.l2Sampled += d.l2Sampled;
        sum.l2DispatchNs += d.l2DispatchNs;
        sum.l2StoreNs += d.l2StoreNs;
        sum.l2WakeNs += d.l2WakeNs;
        sum.l2RunWords += d.l2RunWords;
        sum.l2Runs1 += d.l2Runs1;
        sum.l2Runs2_4 += d.l2Runs2_4;
        sum.l2Runs5_16 += d.l2Runs5_16;
        sum.l2Runs17 += d.l2Runs17;
        sum.stagingSplits += d.stagingSplits;
        // Aurora's staging use of the last sealed frame (bytes copied to the GPU buffers per frame)
        if (const AuroraStats* st = aurora_get_stats()) {
            vertBytes += st->lastVertSize;
            uniformBytes += st->lastUniformSize;
            indexBytes += st->lastIndexSize;
            storageStaged += st->lastStorageSize;
            textureBytes += st->lastTextureUploadSize;
            drawCalls += st->drawCallCount;
            mergedDraws += st->mergedDrawCallCount;
        }
    }
    void AddN8(const N8Split& d) { n8sum += d; }
    double vertBytes = 0, uniformBytes = 0, indexBytes = 0, storageStaged = 0, textureBytes = 0, drawCalls = 0,
           mergedDraws = 0;
    // phasetime: accumulate one frame's per-thread wall time into the window. mainThread is the
    // presenting (main) thread captured at the first XFB present; top is the top NON-main thread
    // (top-overall would just repeat main).
    void AddThreads(const uint32_t* addrs, const double* ms, uint32_t count, uint32_t mainThread,
                    uint64_t switches) {
        threadSwitches += switches;
        for (uint32_t i = 0; i < count; ++i) {
            if (ms[i] == 0.0) continue;
            if (addrs[i] == mainThread) {
                threadMainMs += ms[i];
                continue;
            }
            threadOtherMs += ms[i];
            int idx = -1;
            for (int j = 0; j < threadWinCount; ++j) {
                if (threadWinAddrs[j] == addrs[i]) { idx = j; break; }
            }
            if (idx < 0 && threadWinCount < 16) {
                idx = threadWinCount++;
                threadWinAddrs[idx] = addrs[i];
                threadWinMs[idx] = 0;
            }
            if (idx >= 0) {
                threadWinMs[idx] += ms[i];
                if (threadWinMs[idx] > threadTopMs) {
                    threadTopMs = threadWinMs[idx];
                    threadTopAddr = addrs[i];
                }
            }
        }
    }

    void Log(uint32_t lastFrame) const {
        const double n = frames, ns = 1e6 * n;
        // begin wait: the 1 ms sleeps in BeginNextAuroraFrameWithRetry (gx_frame.cpp) while Aurora's frame
        // pipeline is not ready for a new frame; a wait on the render side, so it counts with the waits.
        const double waits =
            (sum.idle + sum.gxLock + sum.waitSealed + sum.waitDone + sum.mixJoin + sum.beginWait) / ns;
        RT_LOGF(RT_TAG_VI,
                "frames %u-%u: %.2f ms/frame (%.1f fps, worst %.1f); game %.2f = busy %.2f + waits %.2f [idle %.2f, gx "
                "lock %.2f, worker sealed %.2f done %.2f, mix join %.2f, begin wait %.2f (%.1f sleeps)]; in busy: gx "
                "%.2f, disc %.2f, card %.3f (%.2f opens), overlay %.3f; end_frame %.2f, pace %.2f; copies/frame tex "
                "%.2f disp %.2f\n",
                lastFrame - static_cast<uint32_t>(frames) + 1, lastFrame, frameMs / n, 1000.0 * n / frameMs, worstMs,
                gameMs / n, gameMs / n - waits, waits, sum.idle / ns, sum.gxLock / ns, sum.waitSealed / ns,
                sum.waitDone / ns, sum.mixJoin / ns, sum.beginWait / ns, sum.beginSleeps / n,
                gxMs / n - sum.gxLock / ns, discMs / n, sum.card / ns, sum.cardOpens / n, sum.overlay / ns,
                endFrameMs / n, paceMs / n, sum.texCopies / n, sum.dispCopies / n);
        // phasetime: the five-way split's exact spans, beside the census line. Only while armed.
        // tail = busy - gx - named - audeliv: the unaccounted remainder (guest update-side work
        // not otherwise named). named = disc + card + overlay, all in ms per frame.
        if (PhaseTimeArmed()) {
            const double namedMs = discMs / n + sum.card / ns + sum.overlay / ns;
            const double gxMsPerFrame = gxMs / n - sum.gxLock / ns;
            const double busyMs = gameMs / n - waits;
            const double tailMs = busyMs - gxMsPerFrame - namedMs - sum.audioDeliv / ns;
            RT_LOGF(RT_TAG_VI,
                    "[phase] frames %u-%u: game %.2f = pre-submit %.2f + submit %.2f (aurora process %.2f) + named "
                    "%.2f; threads main %.2f other %.2f (top 0x%08X %.2f, %u switches); audeliv %.2f; tail %.2f\n",
                    lastFrame - static_cast<uint32_t>(frames) + 1, lastFrame, gameMs / n, preSubmitMs / n,
                    submitMs / n, sum.gxProcess / ns, namedMs, threadMainMs / n, threadOtherMs / n, threadTopAddr,
                    threadTopMs / n, static_cast<unsigned>(threadSwitches), sum.audioDeliv / ns, tailMs);
        }
        // Gather-pipe traffic per frame: single-word HleFifoWrite calls (each translated store to the
        // pipe), how many of them were display-list recording, and the burst path. The L2 time split
        // (sampled) and run histogram ride the same line.
        // L2: the sampled time-split buckets scale by total/sampled calls
        // (exactly ~64 when nativescene is on; 0 when off, and the buckets read 0).
        const double l2Scale = sum.l2Sampled > 0 ? static_cast<double>(sum.fifoCalls) / sum.l2Sampled : 0;
        RT_LOGF(RT_TAG_VI,
                "gxfifo frames %u-%u: per frame %.0f pipe writes (%.0f KiB, %.0f into display lists), %.0f bursts "
                "(%.0f KiB); fifofast %.0f words appended, %llu mismatches; l2 dispatch %.3f ms store %.3f ms "
                "wake %.3f ms (sampled %.0f calls); runs 1:%.0f 2-4:%.0f 5-16:%.0f 17+:%.0f words %.0f\n",
                lastFrame - static_cast<uint32_t>(frames) + 1, lastFrame, sum.fifoCalls / n,
                sum.fifoBytes / (1024.0 * n), sum.fifoDlCalls / n, sum.burstCalls / n, sum.burstBytes / (1024.0 * n),
                sum.fastWords / n, static_cast<unsigned long long>(sum.fastMismatches),
                sum.l2DispatchNs * l2Scale / 1e6 / n, sum.l2StoreNs * l2Scale / 1e6 / n,
                sum.l2WakeNs * l2Scale / 1e6 / n, sum.l2Sampled / n,
                sum.l2Runs1 / n, sum.l2Runs2_4 / n, sum.l2Runs5_16 / n, sum.l2Runs17 / n, sum.l2RunWords / n);
        // Staging per frame (KiB): what Aurora copied into its GPU buffers, and the vertex-array
        // re-uploads behind the storage share (push_storage: one per indexed array per unmerged draw).
        const double kib = 1024.0 * n;
        RT_LOGF(RT_TAG_VI,
                "staging frames %u-%u: per frame vert %.0f KiB, uniform %.0f, index %.0f, storage %.0f, texture %.0f; "
                "push_storage %.0f calls %.0f KiB; draws %.0f (+%.0f merged); storagecache hits %.0f misses %.0f "
                "skipped %.0f KiB; usedrange uploads %.0f fallbacks %.0f saved %.0f KiB; bbox clears %.0f reads %.0f "
                "disarms %.0f, armed %.0f verts in %.0f calls; pipeline bumps %.0f, memo hits %.0f misses %.0f, "
                "table misses %.0f, splits %.0f\n",
                lastFrame - static_cast<uint32_t>(frames) + 1, lastFrame, vertBytes / kib, uniformBytes / kib,
                indexBytes / kib, storageStaged / kib, textureBytes / kib, sum.storageCalls / n,
                sum.storageBytes / kib, drawCalls / n, mergedDraws / n, sum.cacheHits / n, sum.cacheMisses / n,
                sum.cacheSkipped / kib, sum.urUploads / n, sum.urFallbacks / n, sum.urSaved / kib,
                sum.bboxClears / n, sum.bboxReads / n, sum.bboxDisarms / n, sum.bboxArmedVerts / n,
                sum.bboxAccumCalls / n, sum.pipeBumps / n, sum.memoHits / n, sum.memoMisses / n,
                sum.tableMisses / n, sum.stagingSplits / n);
        // N8 counters per frame (Step 2 measurement build): gated on the
        // nativescene key, so the toggle set reads zero. The C5 per-identity
        // table follows on its own lines.
        if (No100fNativeSceneEnabled()) {
            RT_LOGF(RT_TAG_VI,
                    "[n8counters] frames %u-%u: xframe uploads %.0f unmoved %.0f KiB moved %.0f KiB; process %.0f "
                    "calls (dl %.0f drain %.0f); dlsite subs atomic %.0f sector %.0f im3d %.0f other %.0f unnoted %.0f host %.0f | "
                    "draws atomic %.0f sector %.0f im3d %.0f other %.0f unnoted %.0f; geom renders %.0f allow %.0f "
                    "uncat %.0f unknown %.0f distinct %.0f siteDraws %.0f; pipeDistinct %.0f; clock proctimer %.0f "
                    "deferred %.0f gxframe %.0f aurwait %.0f aurpresent %.0f vi %.0f vispin %.0f idle %.0f "
                    "timebase %.0f ossleep %.0f gxcopy %.0f osintr %.0f other %.0f\n",
                    lastFrame - static_cast<uint32_t>(frames) + 1, lastFrame, n8sum.xUploads / n,
                    n8sum.xUnmoved / kib, n8sum.xMoved / kib, n8sum.processCalls / n,
                    n8sum.processCallsDl / n, n8sum.processCallsDrain / n,
                    n8sum.dlAtomicSubs / n, n8sum.dlSectorSubs / n, n8sum.dlIm3dSubs / n,
                    n8sum.dlOtherSubs / n, n8sum.dlUnnotedSubs / n, n8sum.dlHostSubs / n,
                    n8sum.dlAtomicDraws / n, n8sum.dlSectorDraws / n, n8sum.dlIm3dDraws / n,
                    n8sum.dlOtherDraws / n, n8sum.dlUnnotedDraws / n,
                    n8sum.geomRenders / n, n8sum.geomAllowRenders / n, n8sum.geomUncatRenders / n,
                    n8sum.geomUnknownRenders / n, n8sum.geomDistinct / n, n8sum.geomSiteDraws / n,
                    n8sum.pipeDistinct / n,
                    n8sum.clockProcTimer / n, n8sum.clockGxDeferred / n, n8sum.clockGxFrame / n,
                    n8sum.clockAuroraWait / n, n8sum.clockAuroraPresent / n, n8sum.clockVi / n,
                    n8sum.clockViSpin / n, n8sum.clockOsSchedIdle / n,
                    n8sum.clockPpcTimeBase / n, n8sum.clockOsSleep / n, n8sum.clockGxCopy / n,
                    n8sum.clockOsIntr / n, n8sum.clockPadOther / n);
            No100fGeomPrintTable();
        }
        // Per-stage gx split (NO100F_STAGE_TIMERS; nothing prints while the timers are off): the
        // remainder is process minus the timed spans, computed here so the packet loop itself
        // needs no clock reads.
        if (sum.stageAdmit + sum.stageResolve + sum.stageUploads + sum.stageUniform + sum.stageRecord +
                sum.stageBBox >
            0) {
            const double procMs = sum.gxProcess / ns, admitMs = sum.stageAdmit / ns,
                         resolveMs = sum.stageResolve / ns, uploadsMs = sum.stageUploads / ns,
                         uniformMs = sum.stageUniform / ns, recordMs = sum.stageRecord / ns,
                         bboxMs = sum.stageBBox / ns;
            RT_LOGF(RT_TAG_VI,
                    "gxstage frames %u-%u: per frame process %.2f ms = admit %.2f (resolve %.2f) + uploads %.2f + "
                    "uniform %.2f + bind/record %.2f + bbox %.2f + remainder %.2f\n",
                    lastFrame - static_cast<uint32_t>(frames) + 1, lastFrame, procMs, admitMs, resolveMs,
                    uploadsMs, uniformMs, recordMs, bboxMs,
                    procMs - (admitMs + uploadsMs + uniformMs + recordMs + bboxMs));
        }
    }
};
}  // namespace


// Fix 2 (FIX2-DESIGN-STUDY Part B): present-time completion synthesis. Called
// from VI_HLE_PresentFrame immediately after the presented-frames counter
// advances - the runtime's frame-present point, which this title's guest
// present path matches 1:1 (frames == pe47 == pe48 at every [gxcnt] sample on
// record). When the guest's frame-swap completion relay has died (the cliff:
// the advancer stopped running, so GXEnableBreakPt is never called again and
// the flag word its presenter site waits on is never cleared), hardware would
// still deliver a draw completion for the presented frame; this hook
// synthesizes that one event - the guest's own registered callback, queued
// with the genuine (0, 0) signature into the held GX deque - gated by the
// study's dead-detector. The hook executes no guest code and writes no guest
// memory: counter loads, read-only guest reads, the host completion flag, and
// one mutex-guarded deque push. Context: PresentFrame's two presenting
// callers (GXCopyDisp on the guest thread; the retrace-context present),
// serialized by the present-sequence guard, before any g_viMutex section.
//
// Escalation (ADVANCER-INVOKE-DESIGN Part B, the F1 follow-up): the AA4
// payload's exit is computed from live guest memory and is absorbing in the
// zero-pool stranding this front-end actually produces (the display teardown
// op zeroes the framebuffer-address pool the token/F8 rotations draw from, so
// token == table[A].word0 == 0 at every delivery and AA4 match-exits forever -
// F1: 1,334 syntheses, zero mismatch). After N = 10 fruitless AA4 syntheses in
// an episode, the payload therefore switches one-way to the guest's advancer
// itself (func_801759DC, queued (0, 0) through the same held deque): its
// advance is unconditional arithmetic on A/B plus the scheduler wake, the one
// event the dead relay is missing. If M = 30 advancer queues still leave
// breakpt frozen with A unchanged across the whole advancer phase, F-ADV-DEAD
// trips and all synthesis stands down for the rest of the episode. The hook's
// shape is unchanged: it queues a different guest address; the payload still
// executes only at the existing delivery points (seam pass, idle pump).
//
// Sustain test (RETRACE-HALF-STUDY R4a): breakpt movement under invocation is
// tautological - an invoked advancer calls GXEnableBreakPt itself - so the
// revived line no longer prints at the first movement. Movement in PRIME_ADV
// instead opens a sustain window: K = 24 consecutive presents in which the
// hook queues nothing (the drain-at-retrace, seam-wake and parkwatch are not
// synthesis and keep running), counting breakpt's total movement and A's
// total advance across the window. Only a window with both >= 2 prints
// revived (extended with the sustain result and the drain counters); a
// failed window returns to PRIME_ADV with escalation retained, and three
// failures close the episode unrevived via sustain-giveup. Episode state
// (R4b) zeroes ONLY at the closing edges - durable close, adv-dead,
// sustain-giveup - never at the engaged edge, so every episode re-earns
// escalation from SYNTH_AA4 by construction. The adv-dead and giveup
// stand-downs outlive their episode as an admission hold (v2, below).
//
// Durability (DRAIN-DURABILITY-STUDY Q1-Q4): the drain boot's census
// showed the sustain bar certifying burst tails, not durability - every
// priming burst terminates in an advancer catch-up run, whose bookkeeping
// charges D4 but queues no AA4 and clears the presenter flag, so AF0
// spends the charge into an arm-less gate and site-2's present-paced
// recharge lands after the charge is gone: the state (arm=1, d4=0,
// latch=0) is absorbing (the catch-up phase-slip - R3's one-delivery lag
// rotated onto the presenter's AA4). Three changes follow. (1) A sustain
// pass no longer closes the episode: it enters WATCH, 72 further presents
// with the hook queueing nothing; a K = 3 breakpt freeze or a final third
// (presents 49-72) short of >= 2 breakpt moves and >= 2 A advances fails
// the watch and re-opens PRIME_ADV in the same episode, and only a
// freeze-free watch with the final-third movement prints [fix2] durable
// and closes the episode. (2) The admission hold is v2: it clears on a
// fresh cliff (breakpt moved off the hold value) or, failing that, after
// a backed-off interval (240 presents, doubling per persistent cycle to
// 960) with the episode admitted as resumed on a one-fail sustain budget
// - the v1 clear-on-movement rule deadlocked against a relay that cannot
// move. (3) The standing service: with the drain latched and no episode
// open, a re-phase primer watches for the slip signature itself (arm=1,
// d4=0, A != B, breakpt frozen across 3 presents) and queues ONE advancer
// - the escalation's payload through the same held deque - whose run
// charges D4 while arm stands, re-phasing both supplies. A 240-present
// health line is the service's standing output. The detector, the
// completion flag, the drain, the seam machinery, parkwatch and the
// stall service are untouched.
static void Fix2PresentHook() {
    if (!OS_HLE_Fix2Armed()) return;
    // The per-present completion flag closes the interval that ends at this
    // present: read + cleared exactly once per present, here, as the input
    // to the synthesis decision (set at the gx_fifo queue sites; see
    // hle_stubs.h). A set flag means a genuine completion was queued in the
    // interval - suppress synthesis; the flag is re-armed by the next
    // genuine queueing or by a synthesis below.
    const bool genuineQueued = GxFifo_HLE_TakePresentCompletion();
    const uint32_t presents = s_presentedXfbFrames.load(std::memory_order_relaxed);
    const uint32_t breakpt = GxFifo_HLE_BreakPtCalls();
    const long long tms = Fix2ElapsedMs();
    // Dead-detector state: the run length of consecutive presents showing the
    // same breakpt sample. Detector state, not episode state: the episode
    // machine below never zeroes it.
    static uint32_t lastBreakPt = 0;
    static uint32_t sameRun = 0;
    static uint32_t synthTotal = 0;  // boot-total AA4 queues; numbers the synth lines
    // Episode state (RETRACE-HALF-STUDY R4b): zeroed ONLY at the episode-
    // closing edges - durable close, adv-dead, sustain-giveup - via
    // ResetEpisode below, never at the engaged edge, so every episode
    // re-earns escalation from SYNTH_AA4 by construction. (The escalation
    // build's episode 2 inherited episodeSynths=10 across its revived edge,
    // bypassed SYNTH_AA4 and never logged engaged.) engaged marks an open
    // episode.
    static bool engaged = false;
    static uint32_t episodeSynths = 0;
    // Escalation state (ADVANCER-INVOKE-DESIGN B2): the episode's payload mode
    // (false = SYNTH_AA4, the slot callback; true = SYNTH_ADV, the advancer
    // itself), the advancer queues this episode, the F-ADV-DEAD trip flag,
    // and the A sample taken at the escalation edge with its moved flag (A
    // counts as moved if any later advancer-phase sample differs from it).
    static uint32_t episodeAdvs = 0;
    static bool advancing = false;
    static bool advDead = false;
    static uint32_t advBaseA = 0;
    static bool advAMoved = false;
    // Sustain state (RETRACE-HALF-STUDY R4a): whether a sustain window is
    // open, its present count and accumulated movement (breakpt total, A
    // total), the episode's failed-window count, the A sample the next
    // window present diffs against, and the re-entry gate (after a failed
    // window, sustain re-enters only on a breakpt movement that follows at
    // least one further advancer queue).
    static bool sustaining = false;
    static uint32_t sustainPresents = 0;
    static uint32_t sustainMoves = 0;
    static uint32_t sustainAAdv = 0;
    static uint32_t sustainFails = 0;
    static uint32_t sustainPrevA = 0;
    static bool sustainNeedsQueue = false;
    // Watch state (DRAIN-DURABILITY-STUDY Q3): after a sustain pass the
    // episode stays open in WATCH for 72 presents - the hook queues
    // nothing - tracking breakpt movement and A advance overall and in
    // the final third (presents 49-72, the certifying window). watchPrevA
    // is the A sample the next watch present diffs against, seeded from
    // the sustain window's last sample at the pass edge.
    static bool watching = false;
    static uint32_t watchPresents = 0;
    static uint32_t watchMoves = 0;
    static uint32_t watchAAdv = 0;
    static uint32_t watchTailMoves = 0;
    static uint32_t watchTailAAdv = 0;
    static uint32_t watchPrevA = 0;
    // Admission hold v2 (DRAIN-DURABILITY-STUDY Q2): set at the adv-dead
    // and sustain-giveup closing edges with holdValue = the frozen
    // breakpt and holdStartPresent = the closing present. The closing
    // zeroed the episode, but the freeze that produced it must not
    // immediately re-admit a fresh episode - the detector alone cannot
    // tell a continuing freeze from a fresh cliff. The v1 rule (clear on
    // first breakpt movement) deadlocked in its first boot: after a
    // giveup the relay cannot produce the movement that clears it. v2
    // clears two ways, evaluated per present while held, before the
    // detector: a FRESH clear (breakpt != holdValue - a later boundary's
    // burst moved it, so the coming freeze is a fresh cliff for the
    // unchanged detector) resets the backoff to 240; a PERSISTENT clear
    // (breakpt still == holdValue, frozen across K = 3 presents, and
    // holdBackoff presents elapsed since the hold set) admits the next
    // episode as resumed (resumedPending, below) on a one-fail sustain
    // budget and doubles the backoff for the next persistent cycle
    // (240 -> 480 -> 960; a durable close also resets it to 240). While
    // presents advance one of the two always fires, so the hold cannot
    // deadlock; F-ADV-DEAD remains a per-episode verdict, and the
    // doubling backoff drives the long-run synthesis rate in a
    // permanently dead relay toward zero. episodeResumed is the open
    // episode's resumed flag, captured at its engaged edge.
    static bool admissionHold = false;
    static uint32_t holdValue = 0;
    static uint32_t holdStartPresent = 0;
    static uint32_t holdBackoff = 240;
    static bool resumedPending = false;
    static bool episodeResumed = false;
    // Re-phase primer (DRAIN-DURABILITY-STUDY Q3/Q4): the standing
    // service's active half. rephaseCount is the boot total of primer
    // fires (the rephase lines' total and the health line's rephases
    // field); the cooldown markers record the last fire's breakpt and
    // present, and whether breakpt has moved since. The primer stops
    // for the boot at 600 fires (rephaseCapped).
    static uint32_t rephaseCount = 0;
    static bool rephaseCapped = false;
    static bool rephaseCooldown = false;
    static bool rephaseSawMove = false;
    static uint32_t rephaseFireBreakpt = 0;
    static uint32_t rephaseFirePresent = 0;
    // Standing counters for the health line: episodes admitted (engaged
    // edges), durable closes, and the present count toward the next
    // health line (240 presents, counted from the boot's first present).
    static uint32_t episodesTotal = 0;
    static uint32_t durablesTotal = 0;
    static uint32_t healthClock = 0;
    const auto ResetEpisode = [&]() {
        engaged = false;
        episodeSynths = 0;
        episodeAdvs = 0;
        advancing = false;
        advDead = false;
        advBaseA = 0;
        advAMoved = false;
        sustaining = false;
        sustainPresents = 0;
        sustainMoves = 0;
        sustainAAdv = 0;
        sustainFails = 0;
        sustainNeedsQueue = false;
        watching = false;
        watchPresents = 0;
        watchMoves = 0;
        watchAAdv = 0;
        watchTailMoves = 0;
        watchTailAAdv = 0;
        episodeResumed = false;
    };
    const bool moved = (breakpt != lastBreakPt);
    // Health line (DRAIN-DURABILITY-STUDY Q3/Q4): the standing service's
    // standing output - one line every 240 presents while fix2 is armed,
    // counted from the boot's first present, ahead of every early return
    // below so no phase can swallow a block. seamwakes reads the seam
    // pass's own fire counter through its accessor (os_sleep.cpp).
    if (++healthClock >= 240) {
        healthClock = 0;
        RT_LOGF(RT_TAG_VI,
                "[fix2] health t=%lldms breakpt=%u presents=%u episodes=%u durables=%u rephases=%u seamwakes=%u\n",
                tms, breakpt, presents, episodesTotal, durablesTotal, rephaseCount,
                OS_HLE_SeamWakeFires());
    }
    if (engaged && sustaining) {
        // SUSTAIN (RETRACE-HALF-STUDY R4a): the hook queues nothing for
        // K = 24 consecutive presents - no AA4, no advancer, no
        // NotePresentCompletion; genuine guest completions set the flag as
        // usual and are consumed at the top as usual. The drain-at-retrace,
        // seam-wake and parkwatch are not synthesis and keep running: the
        // window measures whether the guest relay drives itself given the
        // standing nets. Per present, accumulate breakpt's movement and
        // A's advance, (new - old) mod 3 - A is written only by the
        // advancer, so any movement is a guest-driver advance. The entry
        // present is window present 1 with zero deltas by construction
        // (its bases were recorded at entry), and the pass is evaluated
        // when the count reaches K, on the accumulated totals.
        const TraceWakeSync sync = OS_HLE_TraceWakeReadSync();
        ++sustainPresents;
        if (moved) {
            sustainMoves += breakpt - lastBreakPt;
            lastBreakPt = breakpt;
            sameRun = 1;
        } else {
            ++sameRun;
        }
        const int32_t aDiff = static_cast<int32_t>(sync.counterA) -
                              static_cast<int32_t>(sustainPrevA);
        sustainAAdv += static_cast<uint32_t>(((aDiff % 3) + 3) % 3);
        sustainPrevA = sync.counterA;
        if (sustainPresents >= 24) {
            if (sustainMoves >= 2 && sustainAAdv >= 2) {
                // PASS: the only path that prints revived. The episode
                // does NOT close (DRAIN-DURABILITY-STUDY Q3): revived now
                // means "sustain passed, watch pending" - the episode
                // enters WATCH (72 unaided presents) and only a
                // WATCH-certified durable close ends it. The watch's A
                // base is the sustain window's last sample, and its
                // freeze run starts at this pass present.
                RT_LOGF(RT_TAG_VI,
                        "[fix2] revived t=%lldms breakpt=%u presents=%u synths=%u advs=%u sustain=pass moves=%u aadv=%u drains=%u drained=%u\n",
                        tms, breakpt, presents, episodeSynths, episodeAdvs,
                        sustainMoves, sustainAAdv,
                        s_fix2DrainPasses.load(std::memory_order_relaxed),
                        s_fix2DrainedCallbacks.load(std::memory_order_relaxed));
                sustaining = false;
                watching = true;
                watchPresents = 0;
                watchMoves = 0;
                watchAAdv = 0;
                watchTailMoves = 0;
                watchTailAAdv = 0;
                watchPrevA = sustainPrevA;
                sameRun = 1;
            } else {
                ++sustainFails;
                RT_LOGF(RT_TAG_VI,
                        "[fix2] sustain-fail t=%lldms breakpt=%u presents=%u moves=%u aadv=%u fails=%u\n",
                        tms, breakpt, presents, sustainMoves, sustainAAdv, sustainFails);
                if (sustainFails >= (episodeResumed ? 1u : 3u)) {
                    // The episode closes unrevived - host-paced is its
                    // honest label - and the admission hold (v2) keeps the
                    // same freeze from re-admitting a fresh episode. A
                    // resumed episode's sustain budget is one fail.
                    RT_LOGF(RT_TAG_VI,
                            "[fix2] sustain-giveup t=%lldms breakpt=%u presents=%u moves=%u aadv=%u fails=%u drains=%u drained=%u\n",
                            tms, breakpt, presents, sustainMoves, sustainAAdv, sustainFails,
                            s_fix2DrainPasses.load(std::memory_order_relaxed),
                            s_fix2DrainedCallbacks.load(std::memory_order_relaxed));
                    ResetEpisode();
                    admissionHold = true;
                    holdValue = breakpt;
                    holdStartPresent = presents;
                } else {
                    // Back to PRIME_ADV with escalation retained; sustain
                    // re-enters on the next breakpt movement after at
                    // least one further advancer queue.
                    sustaining = false;
                    sustainPresents = 0;
                    sustainMoves = 0;
                    sustainAAdv = 0;
                    sustainNeedsQueue = true;
                }
            }
        }
        return;
    }
    if (engaged && watching) {
        // WATCH (DRAIN-DURABILITY-STUDY Q3): 72 further presents with the
        // hook queueing nothing - no synthesis of either payload; the
        // drain-at-retrace, seam-wake and parkwatch are not synthesis and
        // keep running. Movement accounting matches the sustain window's
        // (breakpt deltas; A's advance as (new - old) mod 3 per present),
        // accumulated overall and over the final third (presents 49-72).
        // A K = 3 breakpt freeze fails the watch on the spot; reaching 72
        // freeze-free with the final third short of >= 2 / >= 2 fails it
        // too. Either failure re-opens PRIME_ADV in the SAME episode -
        // escalation retained, sustain re-entry behind the usual
        // queue-then-movement gate - and is not a sustain fail (the
        // episode's fail budget is untouched). Only a freeze-free watch
        // whose final third moved >= 2 / >= 2 closes the episode, as
        // durable (its moves/aadv fields are the certifying final-third
        // totals); the durable close also resets the admission backoff.
        const TraceWakeSync sync = OS_HLE_TraceWakeReadSync();
        ++watchPresents;
        uint32_t moveDelta = 0;
        if (moved) {
            moveDelta = breakpt - lastBreakPt;
            watchMoves += moveDelta;
            lastBreakPt = breakpt;
            sameRun = 1;
        } else {
            ++sameRun;
        }
        const int32_t aDiff = static_cast<int32_t>(sync.counterA) -
                              static_cast<int32_t>(watchPrevA);
        const uint32_t aDelta = static_cast<uint32_t>(((aDiff % 3) + 3) % 3);
        watchAAdv += aDelta;
        watchPrevA = sync.counterA;
        if (watchPresents > 48) {
            watchTailMoves += moveDelta;
            watchTailAAdv += aDelta;
        }
        if (sameRun >= 3) {
            RT_LOGF(RT_TAG_VI,
                    "[fix2] watch-fail t=%lldms breakpt=%u presents=%u moves=%u aadv=%u reason=freeze\n",
                    tms, breakpt, presents, watchMoves, watchAAdv);
            watching = false;
            watchPresents = 0;
            watchMoves = 0;
            watchAAdv = 0;
            watchTailMoves = 0;
            watchTailAAdv = 0;
            sustainNeedsQueue = true;
        } else if (watchPresents >= 72) {
            if (watchTailMoves >= 2 && watchTailAAdv >= 2) {
                RT_LOGF(RT_TAG_VI,
                        "[fix2] durable t=%lldms breakpt=%u presents=%u synths=%u advs=%u moves=%u aadv=%u drains=%u drained=%u\n",
                        tms, breakpt, presents, episodeSynths, episodeAdvs,
                        watchTailMoves, watchTailAAdv,
                        s_fix2DrainPasses.load(std::memory_order_relaxed),
                        s_fix2DrainedCallbacks.load(std::memory_order_relaxed));
                ++durablesTotal;
                holdBackoff = 240;
                ResetEpisode();
            } else {
                RT_LOGF(RT_TAG_VI,
                        "[fix2] watch-fail t=%lldms breakpt=%u presents=%u moves=%u aadv=%u reason=movement\n",
                        tms, breakpt, presents, watchMoves, watchAAdv);
                watching = false;
                watchPresents = 0;
                watchMoves = 0;
                watchAAdv = 0;
                watchTailMoves = 0;
                watchTailAAdv = 0;
                sustainNeedsQueue = true;
            }
        }
        return;
    }
    if (moved) {
        if (engaged && advancing && !sustainNeedsQueue) {
            // Priming confirmed (RETRACE-HALF-STUDY R4a): the first breakpt
            // movement in PRIME_ADV opens the sustain window at this same
            // present; record the A base the window diffs against. The
            // movement itself is the primer's and does not count toward
            // the window's totals.
            const TraceWakeSync sync = OS_HLE_TraceWakeReadSync();
            sustaining = true;
            sustainPresents = 1;
            sustainMoves = 0;
            sustainAAdv = 0;
            sustainPrevA = sync.counterA;
            lastBreakPt = breakpt;
            sameRun = 1;
            return;
        }
        if (engaged && !advancing) {
            // Movement in PRIME_AA4: the relay showed life before
            // escalation was needed, so the episode's premise is gone.
            // Close it silently - revived prints only on a sustain pass.
            ResetEpisode();
        }
        lastBreakPt = breakpt;
        sameRun = 1;
    } else {
        ++sameRun;
    }
    if (!engaged && s_fix2DrainLatched.load(std::memory_order_relaxed)) {
        // Re-phase primer (DRAIN-DURABILITY-STUDY Q3/Q4): the standing
        // service. The catch-up phase-slip (Q1) leaves a stable,
        // fully-named state - arm charged, D4 spent, headroom in the ring
        // (A != B), breakpt frozen - and one advancer run from it
        // re-phases both supplies: its bookkeeping charges D4 while arm
        // stands, and nothing clears arm except the gate-open pass the
        // charge enables. The primer watches for exactly that signature
        // at each present with no episode open (the episode machine owns
        // its own strandings) and queues ONE advancer through the
        // escalation's own held-deque path. The payload is the guest's
        // own callback, delivered only at the existing delivery points;
        // the hook executes no guest code and writes no guest memory,
        // and the queue is not a draw completion, so the present
        // completion flag is not noted. The gate cannot fire in live
        // flow: breakpt moves ~1/present there and the conjunction never
        // holds. The admission hold below does not suppress the service -
        // a primer burst that moves breakpt is also what fresh-clears a
        // hold. Cooldown after a fire: no re-fire until breakpt has
        // moved and re-frozen across 3 presents, or 48 presents pass.
        // Per-boot cap 600 fires, logged once at the cap.
        if (rephaseCount >= 600) {
            if (!rephaseCapped) {
                rephaseCapped = true;
                RT_LOGF(RT_TAG_VI,
                        "[fix2] rephase-capped t=%lldms breakpt=%u presents=%u total=%u\n",
                        tms, breakpt, presents, rephaseCount);
            }
        } else {
            if (rephaseCooldown && breakpt != rephaseFireBreakpt) {
                rephaseSawMove = true;
            }
            const bool cooldownElapsed = !rephaseCooldown ||
                (rephaseSawMove && sameRun >= 3) ||
                (presents - rephaseFirePresent >= 48);
            if (cooldownElapsed && breakpt != 0 && sameRun >= 3) {
                const TraceWakeSync sync = OS_HLE_TraceWakeReadSync();
                if (sync.arm == 1 && sync.d4 == 0 && sync.counterA != sync.counterB) {
                    QueueGxInterruptCallback(0x801759DCu, 0, 0);
                    ++rephaseCount;
                    rephaseCooldown = true;
                    rephaseSawMove = false;
                    rephaseFireBreakpt = breakpt;
                    rephaseFirePresent = presents;
                    RT_LOGF(RT_TAG_VI,
                            "[fix2] rephase #%u t=%lldms breakpt=%u presents=%u A=%u B=%u arm=%u latch=%u d4=%u flag=%u total=%u\n",
                            rephaseCount, tms, breakpt, presents, sync.counterA,
                            sync.counterB, sync.arm, sync.latch, sync.d4, sync.flag,
                            rephaseCount);
                    return;
                }
            }
        }
    }
    if (admissionHold) {
        // Admission hold v2 (DRAIN-DURABILITY-STUDY Q2): the clear
        // conditions, evaluated per present while held, before the
        // detector. Fresh: breakpt moved off the hold value - the freeze
        // that follows is a fresh cliff and the unchanged detector below
        // admits it as a normal episode (resumedPending stays clear).
        // Persistent: breakpt never moved, but the backed-off interval
        // elapsed with the value frozen across K = 3 presents - admit
        // the next episode as resumed on a one-fail sustain budget and
        // double the backoff for the next persistent cycle. Neither
        // firing means the hold stands and admission stays closed this
        // present.
        if (breakpt != holdValue) {
            RT_LOGF(RT_TAG_VI,
                    "[fix2] hold-cleared t=%lldms reason=fresh breakpt=%u presents=%u backoff=%u\n",
                    tms, breakpt, presents, holdBackoff);
            admissionHold = false;
            resumedPending = false;
            holdBackoff = 240;
        } else if (sameRun >= 3 && presents - holdStartPresent >= holdBackoff) {
            RT_LOGF(RT_TAG_VI,
                    "[fix2] hold-cleared t=%lldms reason=persistent breakpt=%u presents=%u backoff=%u\n",
                    tms, breakpt, presents, holdBackoff);
            admissionHold = false;
            resumedPending = true;
            holdBackoff = holdBackoff * 2u > 960u ? 960u : holdBackoff * 2u;
        } else {
            return;
        }
    }
    // Detector (study B3): (b) generation has lived at least once this boot -
    // breakpt is the monotonic GXEnableBreakPt call counter, so a nonzero
    // sample is itself the proof, and boot-phase presents (counter still 0)
    // are excluded by construction; (d) breakpt unchanged across the last
    // K = 3 consecutive presents counting this one - the generating flow
    // calls ~1 per present, and K = 3 absorbs its known double-present
    // bursts; (e) presents are advancing - this hook is the present. A
    // genuine completion queued in the interval suppresses synthesis
    // outright (the completion-flag discipline).
    if (breakpt == 0 || sameRun < 3 || genuineQueued) return;
    // (c) a completion routine is registered: the BreakPointCB slot, else
    // the DrawDoneCB slot; both zero means nothing is registered to deliver.
    uint32_t cb = 0;
    if (!Memory::TryRead32(0x802712A4u, cb) || cb == 0) {
        cb = 0;
        if (!Memory::TryRead32(0x802712B4u, cb) || cb == 0) return;
    }
    // Synthesis. The snapshot (A/B/arm/latch via the tracewake sync read; the
    // token word, table[A].word0 - the exact comparison AA4 makes at delivery)
    // is read-only, taken at queue time so the delivery's exit distribution is
    // countable from the log (study B2).
    const TraceWakeSync sync = OS_HLE_TraceWakeReadSync();
    // Escalation edge (ADVANCER-INVOKE-DESIGN B2): N = 10 AA4 syntheses with
    // breakpt still frozen - this synthesis switches payload one-way to the
    // advancer for the rest of the episode. A is sampled here, at the first
    // advancer-phase synthesis point, as the phase's reference sample.
    if (!advancing && episodeSynths >= 10) {
        advancing = true;
        advBaseA = sync.counterA;
        advAMoved = false;
        RT_LOGF(RT_TAG_VI,
                "[fix2] escalated t=%lldms breakpt=%u presents=%u synths=%u A=%u B=%u arm=%u latch=%u drains=%u drained=%u\n",
                tms, breakpt, presents, episodeSynths, sync.counterA, sync.counterB, sync.arm, sync.latch,
                s_fix2DrainPasses.load(std::memory_order_relaxed),
                s_fix2DrainedCallbacks.load(std::memory_order_relaxed));
    }
    if (advancing) {
        // SYNTH_ADV: queue the guest advancer itself (func_801759DC - a title
        // constant of the display module, in the style of the callback slots
        // above; it reads no argument registers, and (0, 0) is the genuine
        // AA4 signature) through the same held deque. The hook still executes
        // no guest code: the payload runs only at the existing delivery
        // points. Detector condition (c) above stays the episode admission
        // gate for both payloads, unchanged (design B1). engaged is already
        // set here by construction: escalation is reachable only after the
        // AA4 path's first synthesis of the episode set it.
        if (sync.counterA != advBaseA) advAMoved = true;
        QueueGxInterruptCallback(0x801759DCu, 0, 0);
        GxFifo_HLE_NotePresentCompletion();  // this interval's one completion is now the synthesized one
        ++episodeAdvs;
        sustainNeedsQueue = false;  // a further advancer queue: sustain may re-enter on the next movement
        static uint32_t advLogged = 0;
        if (advLogged++ < 100000) {
            RT_LOGF(RT_TAG_VI,
                    "[fix2] adv #%u t=%lldms breakpt=%u presents=%u A=%u B=%u arm=%u latch=%u d4=%u b8=%u e0=%u flag=%u\n",
                    episodeAdvs, tms, breakpt, presents, sync.counterA, sync.counterB, sync.arm, sync.latch,
                    sync.d4, sync.b8, sync.e0, sync.flag);
        }
        // Advancer cap (design B2; closing edge per RETRACE-HALF-STUDY
        // R4b): M = 30 advancer queues with breakpt still frozen (frozen by
        // construction here - any movement leaves this path for the sustain
        // window above) and A unchanged across the whole advancer phase
        // trip F-ADV-DEAD. The trip closes the episode (state zeroed at
        // this edge) and the admission hold stands synthesis down until
        // breakpt moves. If A moved, each invocation is producing the
        // advance the guest lacks (the host-paced-advancer shape): keep
        // invoking and let the snapshots speak.
        if (episodeAdvs >= 30 && !advAMoved) {
            advDead = true;
            RT_LOGF(RT_TAG_VI, "[fix2] adv-dead t=%lldms breakpt=%u presents=%u advs=%u A=%u drains=%u drained=%u\n",
                    tms, breakpt, presents, episodeAdvs, sync.counterA,
                    s_fix2DrainPasses.load(std::memory_order_relaxed),
                    s_fix2DrainedCallbacks.load(std::memory_order_relaxed));
            ResetEpisode();
            admissionHold = true;
            holdValue = breakpt;
            holdStartPresent = presents;
        }
        return;
    }
    uint32_t token = 0, tableA = 0;
    Memory::TryRead32(0x802716F4u, token);
    Memory::TryRead32(0x80270150u + sync.counterA * 8u, tableA);
    QueueGxInterruptCallback(cb, 0, 0);
    GxFifo_HLE_NotePresentCompletion();  // this interval's one completion is now the synthesized one
    ++synthTotal;
    ++episodeSynths;
    if (!engaged) {
        // Engaged edge: the episode's first synthesis. It resets NOTHING
        // (R4b: state zeroes at the closing edges only; episodeSynths was
        // zeroed there, so the increment above already counts this queue
        // as the episode's first). The boot's first engaged edge also sets
        // the drain-at-retrace latch (RETRACE-HALF-STUDY R5), sticky for
        // the rest of the boot.
        engaged = true;
        episodeResumed = resumedPending;
        resumedPending = false;
        ++episodesTotal;
        s_fix2DrainLatched.store(true, std::memory_order_relaxed);
        RT_LOGF(RT_TAG_VI, "[fix2] engaged t=%lldms breakpt=%u presents=%u\n", tms, breakpt, presents);
    }
    static uint32_t synthLogged = 0;
    if (synthLogged++ < 100000) {
        RT_LOGF(RT_TAG_VI,
                "[fix2] synth #%u t=%lldms cb=0x%08X breakpt=%u presents=%u A=%u token=0x%08X tableA=0x%08X\n",
                synthTotal, tms, cb, breakpt, presents, sync.counterA, token, tableA);
    }
}

void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    if (s_presentSequenceActive.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    GuestProfiler::OnFramePresented();
    static FrameTimes times;
    const Clock::time_point presentStart = n8_vi_now();  // N8 C7: present start read
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    struct SequenceGuard {
        ~SequenceGuard() { s_presentSequenceActive.store(false, std::memory_order_release); }
    } sequenceGuard;
    if (presentedXfb) {
        {
            const uint32_t pf = s_presentedXfbFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            static const int32_t dumpTarget = [] { std::ifstream in("drawdump.txt"); int32_t f = -1; in >> f; return f; }();
            static const bool cullTest = std::ifstream("depth_test.txt").good();
            if (g_gxDrawDump) std::fflush(nullptr);  // games are killed by test scripts: get the dump to disk
            g_gxDrawDump = (dumpTarget >= 0 && pf == static_cast<uint32_t>(dumpTarget)) ? 1 : 0;
            static const int envTest = [] { const char* e = std::getenv("NO100F_CULL_TEST"); return e ? std::atoi(e) : 0; }();  // 1 = no culling, 2 = no depth test (diagnostic)
            static const unsigned drawLimit = [] { const char* e = std::getenv("NO100F_DRAW_LIMIT"); return e ? static_cast<unsigned>(std::atoi(e)) : ~0u; }();
            g_gxDrawLimit = drawLimit;
            g_gxDrawIdx = 0;
            g_gxCullOff = envTest ? envTest : ((cullTest && pf >= 6900u) ? 2 : 0);
        }
        Fix2PresentHook();
        // Bring-up: with drawdump.txt present, report any guest write into the DOL's
        // read-only sections (.rodata/.sdata2) once the game has been running a while.
        if (s_presentedXfbFrames.load(std::memory_order_relaxed) == 3000u && std::ifstream("drawdump.txt").good()) {
            std::ifstream dol(RuntimeConfigFile::ResolvedDvdRoot() / "sys" / "main.dol", std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(dol)), {});
            auto be = [&](size_t o) { return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3]; };
            for (int sec = 7; d.size() > 0xE4 && sec < 18; ++sec) {
                const uint32_t off = be(sec * 4), addr = be(0x48 + sec * 4), size = be(0x90 + sec * 4);
                if (addr != 0x80319200u && addr != 0x8056E280u) continue;  // .rodata, .sdata2
                int reported = 0;
                for (uint32_t i = 0; i + 4 <= size && off + i + 4 <= d.size() && reported < 40; i += 4) {
                    uint32_t v = 0;
                    if (Memory::TryRead32(addr + i, v) && v != be(off + i)) {
                        RT_LOGF(RT_TAG_VI, "[rocheck] 0x%08X dol %08X mem %08X\n", addr + i, be(off + i), v);
                        ++reported;
                    }
                }
                RT_LOGF(RT_TAG_VI, "[rocheck] section 0x%08X checked, %d diffs shown\n", addr, reported);
            }
        }
        // Log the game's first visible frame.
        static std::once_flag firstVisibleFrame;
        if (!g_vi.black) {
            std::call_once(firstVisibleFrame, [] {
                RT_LOGF(RT_TAG_VI, "first visible guest frame presented\n");
            });
        }
    }
    Clock::time_point paceDeadline{};
    bool paceThisFrame = false;
    // Deterministic mode paces in the scheduler's idle wait instead, and must not deliver the
    // retrace from here (PaceToRetraceBoundary does), so frames present as soon as they are sealed.
    if (DetClock::Enabled()) {
        paceToRetrace = false;
    }
    if (paceToRetrace) {
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            paceDeadline = g_vi.lastRetrace + ActiveIntervalLocked();
            retraceCount = g_vi.retraceCount;
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                ActiveIntervalLocked().count());
        }
        // Hold the frame to its boundary only when no retrace elapsed during
        // its production. A frame that missed its boundary presents
        // immediately: hardware would quantize down to the next vblank here,
        // but free-running late frames matches the previous pacer and keeps a
        // heavy scene at e.g. 50 fps instead of hard 30.
        const uint32_t retracesElapsed = retraceCount - s_lastPacedRetraceCount;
        paceThisFrame = retracesElapsed == 0;
        s_lastPacedRetraceCount = retraceCount;
        // aurora_report_producer_paced needs a different signal than the pace-wait above: the guest
        // self-paces via VIWaitForRetrace, so one retrace per produced frame is the healthy locked-60
        // cadence, and zero only happens when production outruns VI. "Kept up" means <=1 retrace
        // elapsed; 2+ means a boundary was missed, so Aurora seals that frame without its interpolated
        // slots (a windowed backstop lowers the slot target only under sustained overload).
        aurora_report_producer_paced(retracesElapsed <= 1);
        // Stamp the sealed frame's presentation schedule so Aurora paces interpolated slots against
        // this same VI timeline. Anchor to the NEXT retrace boundary, not the period just produced,
        // since slots anchored to the current period would already be expired by seal time. Encoding
        // overruns are corrected by sliding the whole slot group forward onto a later boundary of this
        // same grid, so this stays the single cadence authority. Anchors must also be strictly
        // monotonic: two frames sealed before lastRetrace advances would collide on one boundary and
        // burst-present, so a colliding anchor steps onto the next grid point instead of repeating it.
        uint64_t anchorNanos = baseNanos + intervalNanos;
        if (s_lastPresentAnchorNanos != 0 && anchorNanos <= s_lastPresentAnchorNanos) {
            anchorNanos = s_lastPresentAnchorNanos + intervalNanos;
        }
        s_lastPresentAnchorNanos = anchorNanos;
        aurora_set_present_schedule(anchorNanos, intervalNanos);
    } else {
        // Retrace-context presents (VI black, boot) have no display period of
        // their own to subdivide; present as soon as the frame is ready. The
        // schedule grid is gone, so the anchor cursor must not constrain the
        // next paced frame.
        s_lastPresentAnchorNanos = 0;
        aurora_set_present_schedule(0, 0);
    }

    const Clock::time_point endFrameStart = n8_vi_now();  // N8 C7: endFrame start read
    aurora_end_frame();
    const Clock::time_point paceStart = n8_vi_now();  // N8 C7: pace start read
    if (paceThisFrame) {
        PaceToRetraceBoundary(paceDeadline);
        std::lock_guard<std::mutex> lock(g_viMutex);
        s_lastPacedRetraceCount = g_vi.retraceCount;
    }
    // Always on: a frame that took over 20 ms (a dropped frame at 60) goes to the log with where the
    // time went, so a report of drops on someone's machine comes with its cause. At most 30 a minute.
    if (presentedXfb) {
        // phasetime: the presenting thread is the main thread; capture it once for the thread split.
        if (s_phaseMainThread == 0 && Fiber::GuestFiberManager::IsInitialized()) {
            s_phaseMainThread = Fiber::GuestFiberManager::GetCurrentGuestThread();
        }
        static Clock::time_point s_lastEnd{}, s_windowStart{};
        static int s_logged = 0;
        static uint64_t s_lastGx = 0, s_lastDvd = 0;
        static uint32_t s_lastDvdReads = 0;
        const uint64_t gxTotal = AuroraGxProcessNanosTotal();
        const double gxMs = (gxTotal - s_lastGx) / 1e6;
        s_lastGx = gxTotal;
        const uint64_t dvdTotal = DvdReadNanosTotal();
        const uint32_t dvdReads = DvdReadCount();
        const double dvdMs = (dvdTotal - s_lastDvd) / 1e6;
        const uint32_t frameReads = dvdReads - s_lastDvdReads;
        s_lastDvd = dvdTotal;
        s_lastDvdReads = dvdReads;
        // Where the game bucket goes (all since start; this frame's share is the difference): the guest
        // scheduler idle (no guest thread runnable), GX decoding's renderer-lock wait, waits for Aurora's
        // frame worker (SEALED: FIFO drain; DONE: GXCopyDisp/end_frame), AX mix-worker joins, CARDOpen,
        // settings_overlay::Draw, and the EFB/display copies made.
        const FrameSplit split = FrameSplit::Now();
        static FrameSplit s_lastSplit = split;
        const FrameSplit d = split - s_lastSplit;
        s_lastSplit = split;
        // N8 counters: same snapshot/diff shape as the FrameSplit above.
        const N8Split n8split = N8Split::Now();
        static N8Split s_lastN8 = n8split;
        const N8Split n8d = n8split - s_lastN8;
        s_lastN8 = n8split;
        const Clock::time_point now = n8_vi_now();  // N8 C7: present read
        // phasetime: take + clear the first-packet stamp for this interval. Taken once per
        // present even when the frame is skipped below, so a boot packet never leaks into the
        // first real frame.
        uint64_t phasePktNanos = 0;
        if (PhaseTimeArmed()) {
            phasePktNanos = PhaseTimeFirstPacketNanos();
            PhaseTimeClearFirstPacket();
        }
        if (s_lastEnd != Clock::time_point{}) {
            const double frame = ms(now - s_lastEnd);
            const double game = ms(presentStart - s_lastEnd);
            // phasetime spans: pre-submit = first packet - previous present end; submit =
            // present start - first packet. No packet this frame: the whole game span is pre-submit.
            double preSubmitMs = 0.0, submitMs = 0.0;
            if (PhaseTimeArmed()) {
                const int64_t prevEndNs = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(s_lastEnd.time_since_epoch()).count());
                const int64_t presentNs = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(presentStart.time_since_epoch()).count());
                const int64_t pktNs = static_cast<int64_t>(phasePktNanos);
                if (phasePktNanos != 0 && pktNs >= prevEndNs && pktNs <= presentNs) {
                    preSubmitMs = static_cast<double>(pktNs - prevEndNs) / 1e6;
                    submitMs = static_cast<double>(presentNs - pktNs) / 1e6;
                } else {
                    preSubmitMs = game;
                    submitMs = 0.0;
                }
            }
            if (now - s_windowStart > std::chrono::minutes(1)) {
                s_windowStart = now;
                s_logged = 0;
            }
            if (frame > 20.0 && frame < 2000.0 && s_logged++ < 30) {
                uint32_t sceneWord = 0;
                Memory::TryRead32(0x80428730u, sceneWord);
                // phasetime: RT_LOGF prepends "[" tag "] " to the format by literal
                // concatenation, so a ternary format cannot go through it - this line uses
                // fprintf directly with the "[vi] " prefix in both branches.
                std::fprintf(stderr,
                             PhaseTimeArmed()
                                 ? "[vi] slow frame %.1f ms (scene 0x%03X): game %.2f (gx %.2f, disc %.2f in %u reads), present %.2f, "
                                   "end_frame %.2f, pace %.2f; game split: idle %.2f, gx lock %.2f, worker wait sealed %.2f "
                                   "done %.2f, mix join %.2f, card %.2f (%llu opens), overlay %.2f; copies tex %llu disp %llu; "
                                   "phase pre %.2f submit %.2f\n"
                                 : "[vi] slow frame %.1f ms (scene 0x%03X): game %.2f (gx %.2f, disc %.2f in %u reads), present %.2f, "
                                   "end_frame %.2f, pace %.2f; game split: idle %.2f, gx lock %.2f, worker wait sealed %.2f "
                                   "done %.2f, mix join %.2f, card %.2f (%llu opens), overlay %.2f; copies tex %llu disp %llu\n",
                        frame, sceneWord & 0xFFFFu, game, gxMs, dvdMs, frameReads,
                        ms(endFrameStart - presentStart),
                        ms(paceStart - endFrameStart), ms(now - paceStart), d.idle / 1e6, d.gxLock / 1e6,
                        d.waitSealed / 1e6, d.waitDone / 1e6, d.mixJoin / 1e6, d.card / 1e6,
                        static_cast<unsigned long long>(d.cardOpens), d.overlay / 1e6,
                        static_cast<unsigned long long>(d.texCopies), static_cast<unsigned long long>(d.dispCopies),
                        preSubmitMs, submitMs);
            }
            // Always on, every 600 presented frames: the same split as averages per frame, and what is
            // left of the game bucket after the waits ("busy": translated game code, HLE and GX decoding).
            static FrameWindow s_win;
            s_win.Add(frame, game, gxMs, dvdMs, ms(paceStart - endFrameStart), ms(now - paceStart), d,
                      preSubmitMs, submitMs);
            s_win.AddN8(n8d);
            // phasetime: per-thread wall time for this frame, from the scheduler snapshot diff.
            // The first snapshot after arming only seeds the baseline (the cumulative totals
            // since boot are not this frame's time), so nothing is added for that frame.
            if (PhaseTimeArmed()) {
                static uint32_t s_phasePrevAddrs[16] = {};
                static uint64_t s_phasePrevNanos[16] = {};
                static uint32_t s_phasePrevCount = 0;
                static uint64_t s_phasePrevSwitches = 0;
                uint32_t curAddrs[16] = {};
                uint64_t curNanos[16] = {};
                uint64_t curSwitches = 0;
                const uint32_t curCount = PhaseTimeThreadSnapshot(curAddrs, curNanos, &curSwitches);
                if (s_phasePrevCount == 0 && s_phasePrevSwitches == 0) {
                    // First snapshot: seed only.
                } else {
                    uint32_t frameAddrs[16] = {};
                    double frameMs[16] = {};
                    uint32_t frameCount = 0;
                    for (uint32_t i = 0; i < curCount; ++i) {
                        uint64_t prev = 0;
                        for (uint32_t j = 0; j < s_phasePrevCount; ++j) {
                            if (s_phasePrevAddrs[j] == curAddrs[i]) { prev = s_phasePrevNanos[j]; break; }
                        }
                        const double diffMs = static_cast<double>(curNanos[i] - prev) / 1e6;
                        if (diffMs > 0.0 && frameCount < 16) {
                            frameAddrs[frameCount] = curAddrs[i];
                            frameMs[frameCount] = diffMs;
                            ++frameCount;
                        }
                    }
                    s_win.AddThreads(frameAddrs, frameMs, frameCount, s_phaseMainThread,
                                     curSwitches - s_phasePrevSwitches);
                }
                s_phasePrevCount = curCount;
                for (uint32_t i = 0; i < curCount; ++i) {
                    s_phasePrevAddrs[i] = curAddrs[i];
                    s_phasePrevNanos[i] = curNanos[i];
                }
                s_phasePrevSwitches = curSwitches;
            }
            if (s_win.frames == 600) {
                s_win.Log(s_presentedXfbFrames.load(std::memory_order_relaxed));
                s_win = FrameWindow{};
            }
        }
        s_lastEnd = now;
    }
    if (LogFpsEnabled() && presentedXfb) {
        const Clock::time_point now = n8_vi_now();  // N8 C7: present read
        if (times.lastPresentEnd != Clock::time_point{}) {
            const double game = ms(presentStart - times.lastPresentEnd) + ms(endFrameStart - presentStart);
            const double frame = ms(now - times.lastPresentEnd);
            times.gameMs += game;
            times.endFrameMs += ms(paceStart - endFrameStart);
            times.paceMs += ms(now - paceStart);
            times.worstMs = std::max(times.worstMs, frame);
            times.sumSq += frame * frame;
            times.slow += frame > 20.0 ? 1 : 0;
            ++times.frames;
        } else {
            times.windowStart = now;
        }
        times.lastPresentEnd = now;
        const double window = ms(now - times.windowStart);
        if (window >= 2000.0 && times.frames > 0) {
            const double n = times.frames;
            uint64_t gxNanos = 0, lockNanos = 0, draws = 0;
            AuroraTakeGxStats(&gxNanos, &lockNanos, &draws);
            uint64_t copies = 0, creates = 0;
            AuroraTakeCopyStats(&copies, &creates);
            RT_LOGF(RT_TAG_VI,
                    "fps %.1f: game %.2f ms (gx %.2f ms, lock wait %.2f ms, %.0f draws, %.1f copies, %.1f new copy "
                    "textures), end_frame %.2f ms, pace %.2f ms, worst frame %.1f ms, jitter %.2f ms, %d over 20 ms\n",
                    n * 1000.0 / window, times.gameMs / n, gxNanos / 1e6 / n, lockNanos / 1e6 / n, draws / n,
                    copies / n, creates / n, times.endFrameMs / n, times.paceMs / n, times.worstMs,
                    std::sqrt(std::max(0.0, times.sumSq / n - (window / n) * (window / n))), times.slow);
            if (aurora_get_frame_interpolation_fps() != 0) {  // what the screen shows vs the 60 Hz game
                AuroraPresentTiming present{};
                aurora_get_present_timing(&present);
                AuroraFrameInterpolationDiagnostics fi{};
                aurora_get_frame_interpolation_diagnostics(&fi);
                RT_LOGF(RT_TAG_VI, "interp: displayed %.1f fps (motion %.1f), target %u, slots %u/%u, matched %u/%u of %u draws, "
                        "eligible %u, low-match %llu/%llu frames, reductions %llu, late drops %llu\n",
                        present.framesPerSecond, present.effectiveFramesPerSecond, fi.targetFps, fi.activeSamples,
                        fi.targetSamples, fi.matches, fi.matchable, fi.candidates, fi.eligible,
                        (unsigned long long)fi.framesLowMatch, (unsigned long long)fi.framesSealed,
                        (unsigned long long)fi.slotReductions, (unsigned long long)fi.lateSealDrops);
            }
            times = FrameTimes{};
            times.windowStart = now;
            times.lastPresentEnd = now;
        }
    }
    g_auroraFrameActive.store(false, std::memory_order_release);
    g_auroraFrameHadWork.store(false, std::memory_order_release);
    if (presentedXfb) {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.hasValidXfb = false;
        g_vi.readyXfb = 0;
    }
    // Pre-warm the next frame so subsequent GX work has a valid frame context.
    {
        UpdateAuroraAndProcessEvents();
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
    }
}

// -----------------------------------------------------------------------------
// VI_HLE_SetXfbReady - Called by GXCopyDisp to signal EFB->XFB copy completed.
// This marks that we now have valid framebuffer data to present.
// -----------------------------------------------------------------------------
void VI_HLE_SetXfbReady(uint32_t xfbAddr) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_vi.hasValidXfb = true;
    g_vi.readyXfb = xfbAddr;
    if (g_vi.currentFrameBuffer == 0 && g_vi.nextFrameBuffer == 0) {
        g_vi.currentFrameBuffer = xfbAddr;
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
        WriteGuestStateLocked();
    } else if (g_vi.nextFrameBuffer != xfbAddr) {
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
    }
}

// VIInit (0x801BC7A0) and its lower-level helper __VIInit (0x801BC5A0) both
// program MMIO at 0xCC0020xx on hardware. We skip all hardware access and seed
// the same defaults instead, so the two entry points share one body.
static void SeedViStateForInit(CpuContext* ctx, const char* who)
{
    RT_LOG(RT_TAG_VI) << who << " called: seeding VI state (HLE)" << std::endl;
    GuestProfiler::OnFramePresented();  // diagnostic: NO100F_PROFILE can sample boot hangs before the first frame
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    ViSetR3(ctx, 0);
}

extern "C" void VIInit_HLE_801b94a4(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "VIInit_801b94a4");
}
PPC_NATIVE_OVERRIDE_VOID(8010AB28, VIInit_HLE_801b94a4, (CpuContext* ctx), (ctx));

extern "C" void __VIInit_HLE_801b9294(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "__VIInit_801b9294");
}
PPC_NATIVE_OVERRIDE_VOID(8010A930, __VIInit_HLE_801b9294, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// Helper stubs referenced by VIInit switch cases (case D variants).
// These are hardware-specific; treat as no-ops to keep control flow intact.
// -----------------------------------------------------------------------------
extern "C" void VIInit_caseD_0_HLE_801b9934(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_0_801b9934 stubbed" << std::endl;
}

extern "C" void VIInit_caseD_1_HLE_801b993c(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_1_801b993c stubbed" << std::endl;
}

extern "C" void VIInit_caseD_2_HLE_801b9944(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_2_801b9944 stubbed" << std::endl;
}

// -----------------------------------------------------------------------------
// VISetPreRetraceCallback (0x801BC3E0)
// -----------------------------------------------------------------------------
extern "C" void VISetPreRetraceCallback_HLE_801b90f4(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.preRetraceCallback;
        g_vi.preRetraceCallback = newCb;
        RT_LOGF(RT_TAG_VI, "VISetPreRetraceCallback 0x%08X (was 0x%08X) lr 0x%08X\n", newCb, prev, ctx ? ctx->lr : 0u);
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(8010A818, VISetPreRetraceCallback_HLE_801b90f4, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPostRetraceCallback (0x801BC430)
// -----------------------------------------------------------------------------
extern "C" void VISetPostRetraceCallback_HLE_801b9138(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.postRetraceCallback;
        g_vi.postRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(8010A85C, VISetPostRetraceCallback_HLE_801b9138, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIGetDTVStatus (0x801BE070)
// Reads DTV status from VI hardware (MMIO 0xCC00206E). Stub to "not ready".
// -----------------------------------------------------------------------------
extern "C" void VIGetDTVStatus_HLE_801bad38(CpuContext* ctx)
{
    ViSetR3(ctx, 0); // return 0 -> not ready / disabled
}

// -----------------------------------------------------------------------------
// VIConfigure & related helpers: translate GXRenderModeObj into guest globals.
// -----------------------------------------------------------------------------
extern "C" void GX__SetDispCopySrc_8016f438(uint32_t l, uint32_t t, uint32_t w, uint32_t h);  // hle/gx/gx_copy.cpp
extern "C" void GX__SetDispCopyDst_8016f4b8(uint32_t w, uint32_t h);
struct CopyMode { uint16_t width = 0, efbHeight = 0, xfbHeight = 0; };
static CopyMode g_viCopyMode;  // last VIConfigure'd mode's display-copy size (VI_HLE_ApplyDisplayCopyForMode)
void VI_HLE_ApplyDisplayCopyForMode();
extern "C" void VIConfigure_HLE_801b9f6c(CpuContext* ctx)
{
    // One read of the guest GXRenderModeObj serves both the VI pending state and
    // aurora, rather than unpacking the same 0x39 bytes twice.
    const uint32_t renderModePtr = ctx ? ctx->gpr[3] : 0;
    GXRenderModeObj renderMode{};
    if (!ReadGuestRenderModeObj(renderModePtr, renderMode)) {
        RT_LOG(RT_TAG_VI) << "VIConfigure: invalid GXRenderModeObj pointer 0x"
                  << std::hex << renderModePtr << std::dec << std::endl;
        ViSetR3(ctx, 0);
        return;
    }

    { static int n = 0; if (n++ < 4) RT_LOGF(RT_TAG_VI, "[mode] VIConfigure tv %u fb %ux%u xfbH %u vi %u,%u %ux%u\n", static_cast<unsigned>(renderMode.viTVmode), renderMode.fbWidth, renderMode.efbHeight, renderMode.xfbHeight, renderMode.viXOrigin, renderMode.viYOrigin, renderMode.viWidth, renderMode.viHeight); }
    const uint32_t decodedTvFormat =
        ExtractTvFormat(static_cast<uint32_t>(renderMode.viTVmode));
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingTvFormat = decodedTvFormat;
        g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        g_vi.pendingRenderWidth =
            renderMode.viWidth != 0 ? renderMode.viWidth : renderMode.fbWidth;
        g_vi.pendingRenderHeight =
            renderMode.viHeight != 0 ? renderMode.viHeight : renderMode.xfbHeight;
        g_vi.pendingViXOrigin = renderMode.viXOrigin;
        g_vi.pendingViYOrigin = renderMode.viYOrigin;
        g_vi.pendingXfbWidth = renderMode.fbWidth;
        g_vi.pendingXfbHeight =
            renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight;
    }

    ::VIConfigure(&renderMode);
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_viCopyMode = {renderMode.fbWidth, renderMode.efbHeight,
                        static_cast<uint16_t>(renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight)};
    }
    VI_HLE_ApplyDisplayCopyForMode();

    ViSetR3(ctx, 0);
}

// The VI only scans out viHeight lines of the XFB, but the presenter shows the whole display copy. A game can
// configure a 448-line VI mode BEFORE GXInit, whose __GXInitGX defaults the copy to 640x480, then
// never set it again (the console's VI just drops the extra lines): 32 undrawn lines showed as a band at the
// bottom. Copy (and size the XFB) as the TV shows it: on VIConfigure and again after GXInit (gx_init.cpp).
void VI_HLE_ApplyDisplayCopyForMode() {
    CopyMode mode{};
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        mode = g_viCopyMode;
    }
    if (mode.width != 0 && mode.efbHeight != 0) {
        GX__SetDispCopySrc_8016f438(0, 0, mode.width, mode.efbHeight);
        GX__SetDispCopyDst_8016f4b8(mode.width, mode.xfbHeight);
    }
}
PPC_NATIVE_OVERRIDE_VOID(8010B468, VIConfigure_HLE_801b9f6c, (CpuContext* ctx), (ctx));

extern "C" void VIFlush_HLE_801ba9a4(CpuContext* ctx)
{
    uint32_t guestNextFb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        if (g_vi.pendingNextFrameBuffer == 0) {
            try {
                guestNextFb = Memory::Read32(kViNextFrameBufferAddr);
            } catch (const Memory::AccessViolation&) {
                guestNextFb = 0;
            }
            if (guestNextFb == 0) {
                try {
                    guestNextFb = Memory::Read32(kViNextFrameBufferHwAddr);
                } catch (const Memory::AccessViolation&) {
                    guestNextFb = 0;
                }
            }
            if (guestNextFb != 0) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
            }
        }
        
        // Arm only; the commit happens at the next retrace (see ViState).
        g_vi.flushArmed = true;
    }

    // NOTE: We do NOT set hasValidXfb here. Frame readiness is signaled ONLY by
    // GXCopyDisp (which sets hasValidXfb = true), as that's when the EFB->XFB
    // copy is complete and we have a valid frame to present.

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8010BC04, VIFlush_HLE_801ba9a4, (CpuContext* ctx), (ctx));

extern "C" void VISetNextFrameBuffer_HLE_801baab8(CpuContext* ctx)
{
    const uint32_t fbPtr = ctx ? ctx->gpr[3] : 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingNextFrameBuffer = fbPtr;
        // Also update guest memory for SDK code that reads this directly
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8010BD20, VISetNextFrameBuffer_HLE_801baab8, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextFrameBuffer_HLE_801bab24(CpuContext* ctx)
{
    uint32_t fb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Return PENDING value - what was set by VISetNextFrameBuffer
        fb = g_vi.pendingNextFrameBuffer;
    }
    ViSetR3(ctx, fb);
    VI_HLE_PollRetrace(ctx);
}

extern "C" void VISetBlack_HLE_801bab2c(CpuContext* ctx)
{
    const bool makeBlack = ctx ? (ctx->gpr[3] != 0) : false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingBlack = makeBlack;
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8010BD8C, VISetBlack_HLE_801bab2c, (CpuContext* ctx), (ctx));

extern "C" void VIGetRetraceCount_HLE_801baba4(CpuContext* ctx)
{
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        count = g_vi.retraceCount;
    }
    // NO100F 60 fps ([video] fps60): the main loop (0x800AD68C) waits until two retraces passed since the last
    // frame; counting double for that caller makes it wait one. The game steps by the measured frame time.
    if (static const bool fps60 = RuntimeConfigFile::Fps60(); fps60 && ctx->lr == 0x800AD690u) count *= 2;
    ViSetR3(ctx, count);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(8010BE08, VIGetRetraceCount_HLE_801baba4, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextField_HLE_801babac(CpuContext* ctx)
{
    bool fieldOdd = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        fieldOdd = g_vi.fieldOdd;
    }
    ViSetR3(ctx, fieldOdd ? 1 : 0);
    VI_HLE_PollRetrace(ctx);
}

extern "C" void VIGetCurrentLine_HLE_801bac48(CpuContext* ctx)
{
    uint32_t height = 480;
    std::chrono::nanoseconds interval{16666us};
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        height = g_vi.xfbHeight;
        interval = ActiveIntervalLocked();
        last = g_vi.lastRetrace;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(n8_vi_now() - last);  // N8 C7: VIGetCurrentLine read
    if (DetClock::Enabled()) {
        uint64_t lastTicks = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            lastTicks = g_vi.detLastRetraceTicks;
        }
        const uint64_t now = DetClock::Now();
        elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            TimeBaseContract::TicksToDuration(now > lastTicks ? now - lastTicks : 0));
    }
    uint32_t line = 0;
    if (interval.count() > 0 && height > 0) {
        const uint64_t scaled = static_cast<uint64_t>(elapsed.count()) * height;
        const uint64_t intervalUs = static_cast<uint64_t>(std::max<int64_t>(1, interval.count() / 1000));
        line = static_cast<uint32_t>(std::min<uint64_t>(height - 1, scaled / intervalUs));
    }
    ViSetR3(ctx, line);
}
PPC_NATIVE_OVERRIDE_VOID(8010BF20, VIGetCurrentLine_HLE_801bac48, (CpuContext* ctx), (ctx));

extern "C" void VIWaitForRetrace_HLE_801b99ec(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (Fiber::GuestFiberManager::IsInitialized()) {
        const int32_t irqState = OS__DisableInterrupts_801a65ac();
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            retraceCount = g_vi.retraceCount;
        }

        do {
            cpu->gpr[3] = kViRetraceQueueAddr;
            OSSleepThread_HLE_801aa9b8(cpu);

            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                EnsureInitializedLocked();
                if (g_vi.retraceCount != retraceCount) {
                    break;
                }
            }
        } while (true);

        OS__RestoreInterrupts_801a65d4(irqState);
    } else {
        std::chrono::nanoseconds interval{16666us};
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            interval = ActiveIntervalLocked();
            target = g_vi.lastRetrace + interval;
        }

        const auto now = n8_vi_now();  // N8 C7: VIWaitForRetrace read
        if (now < target) {
            SleepPreciselyUntil(target, true);
        }
        AdvanceRetrace(cpu, target, true);
    }
    ViSetR3(cpu, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8010AFA0, VIWaitForRetrace_HLE_801b99ec, (CpuContext* ctx), (ctx));

// VIGetScanMode: reads the VI clock/DTV registers on hardware. The host has no
// cable, so report interlaced (VI_INTERLACE = 0), the mode every NTSC console supports.
extern "C" uint32_t VIGetScanMode_HLE() { return 0; }


uint32_t GxPresentedFrames() { return g_gxFramesFn(); }

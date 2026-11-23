// SelectThread scheduler, OSWakeupThread and the OSMutex primitives.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "timebase_contract.h"
#include "det_clock.h"
#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "fiber_manager.h"
#include "runtime_log.h"
#include "os_internal.h"

#include "../no100f_native_scene.h"

// Time the guest scheduler spent in its idle loop (no runnable guest thread) since start.
static std::atomic<uint64_t> g_guestIdleNanos{0};

// N8 C7: IdleTimer clock reads (2 per idle entry). Gated on the nativescene
// dk_diag key (No100fNativeSceneEnabled), one branch per site when off.
// Counters ride the measurement flight only, never a price build.
static std::atomic<uint64_t> s_n8ClockOsSchedIdle{0};
static std::chrono::steady_clock::time_point n8_ossched_now() noexcept {
  if (No100fNativeSceneEnabled()) s_n8ClockOsSchedIdle.fetch_add(1, std::memory_order_relaxed);
  return std::chrono::steady_clock::now();
}
uint64_t ossched_n8_idle_clock_total() {
  return s_n8ClockOsSchedIdle.load(std::memory_order_relaxed);
}

// phasetime (PHASETIME design, Nov 19, 2026): wall time per guest thread, from SelectThread's
// commit points. The running word has exactly two writers inside SelectThread (the idle entry
// and the commit below); OSSleepThread's early-boot default-thread fallback and the fiber
// exit cleanup can move it outside SelectThread, in which case the interval closes against
// the last committed thread - rare, boot/exit only, noted not fixed. The open interval (last
// commit -> now) is folded into snapshots at read time and never mutated there.
// Up to 16 distinct threads are tracked; beyond that, further threads are dropped (the game
// runs a handful; unreachable in practice).
static bool PhaseTimeArmed() {
    static const bool armed = std::getenv("SCOOBY_PHASETIME") != nullptr;
    return armed;
}
struct PhaseThreadSlot {
    uint32_t addr = 0;
    uint64_t nanos = 0;
};
static PhaseThreadSlot s_phaseSlots[16];
static int s_phaseSlotCount = 0;
static uint32_t s_phaseCurrent = 0;
static std::chrono::steady_clock::time_point s_phaseStamp{};
static uint64_t s_phaseSwitches = 0;
static void PhaseTimeAccrue(uint32_t thread, uint64_t nanos) {
    for (int i = 0; i < s_phaseSlotCount; ++i) {
        if (s_phaseSlots[i].addr == thread) {
            s_phaseSlots[i].nanos += nanos;
            return;
        }
    }
    if (s_phaseSlotCount < 16) {
        s_phaseSlots[s_phaseSlotCount].addr = thread;
        s_phaseSlots[s_phaseSlotCount].nanos = nanos;
        ++s_phaseSlotCount;
    }
    // Over the cap: the interval is dropped (documented above).
}
static void PhaseTimeCommit(uint32_t incoming) {
    if (!PhaseTimeArmed()) return;
    const auto now = std::chrono::steady_clock::now();
    if (s_phaseCurrent != 0 && s_phaseStamp != std::chrono::steady_clock::time_point{}) {
        PhaseTimeAccrue(s_phaseCurrent, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_phaseStamp).count()));
    }
    if (incoming != s_phaseCurrent) ++s_phaseSwitches;
    s_phaseCurrent = incoming;
    s_phaseStamp = now;
}
uint32_t PhaseTimeThreadSnapshot(uint32_t* addrs, uint64_t* nanos, uint64_t* switchesOut) {
    if (switchesOut) *switchesOut = s_phaseSwitches;
    if (!PhaseTimeArmed()) return 0;
    int n = s_phaseSlotCount;
    for (int i = 0; i < n; ++i) {
        addrs[i] = s_phaseSlots[i].addr;
        nanos[i] = s_phaseSlots[i].nanos;
    }
    // Fold the open interval into the current thread's entry (snapshot only; no state mutated).
    // The current thread may have no slot yet (slots are created at the NEXT commit), so it is
    // appended when missing.
    if (s_phaseCurrent != 0 && s_phaseStamp != std::chrono::steady_clock::time_point{}) {
        const uint64_t open = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - s_phaseStamp).count());
        int idx = -1;
        for (int i = 0; i < n; ++i) {
            if (addrs[i] == s_phaseCurrent) { idx = i; break; }
        }
        if (idx >= 0) {
            nanos[idx] += open;
        } else if (n < 16) {
            addrs[n] = s_phaseCurrent;
            nanos[n] = open;
            ++n;
        }
    }
    return static_cast<uint32_t>(n);
}
static void NoteGuestIdle(std::chrono::steady_clock::duration d)
{
    g_guestIdleNanos.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()),
        std::memory_order_relaxed);
}
uint64_t GuestSchedulerIdleNanosTotal() { return g_guestIdleNanos.load(std::memory_order_relaxed); }

namespace {
void LinkMutexToThread(uint32_t threadPtr, uint32_t mutexPtr)
{
    const uint32_t tail = ::Memory::Read32(threadPtr + kThreadMutexTailOffset);
    if (tail == 0) {
        ::Memory::Write32(threadPtr + kThreadMutexQueueOffset, mutexPtr);
    } else {
        ::Memory::Write32(tail + kMutexThreadNextOffset, mutexPtr);
    }

    ::Memory::Write32(mutexPtr + kMutexThreadPrevOffset, tail);
    ::Memory::Write32(mutexPtr + kMutexThreadNextOffset, 0);
    ::Memory::Write32(threadPtr + kThreadMutexTailOffset, mutexPtr);
}

void UnlinkMutexFromThread(uint32_t threadPtr, uint32_t mutexPtr)
{
    UnlinkGuestListNode(mutexPtr, kMutexThreadNextOffset, kMutexThreadPrevOffset,
                        threadPtr + kThreadMutexQueueOffset, threadPtr + kThreadMutexTailOffset);
}

// Tail-enqueue `thread` on the run queue for `priority` and record which queue
// it is linked into. Marking the priority pending is a separate step because
// OSWakeupThread runs its fiber bookkeeping between the two.
void LinkThreadOnRunQueue(uint32_t thread, int32_t priority)
{
    const uint32_t queueEntry = kThreadQueueArrayAddr + static_cast<uint32_t>(priority) * 8u;
    const uint32_t tail = ::Memory::Read32(queueEntry + 4u);
    if (tail == 0) {
        ::Memory::Write32(queueEntry, thread);
    } else {
        ::Memory::Write32(tail + kThreadNextOffset, thread);
    }
    ::Memory::Write32(thread + kThreadPrevOffset, tail);
    ::Memory::Write32(thread + kThreadNextOffset, 0);
    ::Memory::Write32(queueEntry + 4u, thread);
    ::Memory::Write32(thread + kThreadQueueOffset, queueEntry);
}

// Publish `priority` in the scheduler pending mask and request a reschedule.
void MarkRunQueuePending(int32_t priority)
{
    const uint32_t pending = ::Memory::Read32(kSchedulerPendingFlagAddr);
    ::Memory::Write32(kSchedulerPendingFlagAddr, pending | (1u << (31 - priority)));
    ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
}

// Pop `head` off the front of the thread queue whose head/tail slots are
// `queueAddr`/`queueAddr + 4`, returning the new head.
uint32_t PopThreadQueueHead(uint32_t queueAddr, uint32_t head)
{
    const uint32_t next = ::Memory::Read32(head + kThreadNextOffset);
    if (next == 0) {
        ::Memory::Write32(queueAddr + 4u, 0);
    } else {
        ::Memory::Write32(next + kThreadPrevOffset, 0);
    }
    ::Memory::Write32(queueAddr, next);
    return next;
}

void PromoteThreadPriority(uint32_t threadPtr, int32_t priority)
{
    while (threadPtr != 0) {
        if (static_cast<int32_t>(::Memory::Read32(threadPtr + kThreadSuspendOffset)) > 0) {
            return;
        }

        const int32_t currentPriority =
            static_cast<int32_t>(::Memory::Read32(threadPtr + kThreadPriorityOffset));
        if (currentPriority <= priority) {
            return;
        }

        threadPtr = SetThreadEffectivePriority(threadPtr, priority);
    }
}
} // namespace


// tracewake (park seam trace, Nov 16, 2026; PARK-STUDY.md Q4(d)): shared seam state
// for the [wake] loggers in this file and os_sleep.cpp (declared in hle_stubs.h).
// The snapshot reads guest memory host-side only - TryReadGuestScalar never faults
// the host - and the armed check reads SCOOBY_TRACE_WAKE once. With the env unset
// nothing is captured or logged and no behaviour changes.
bool OS_HLE_TraceWakeArmed()
{
    static const bool armed = std::getenv("SCOOBY_TRACE_WAKE") != nullptr;
    return armed;
}

// seamfix (SEAMFIX-DESIGN.md): arm flag for the OSSleepThread seam pass and the
// GX PE-interrupt class deferral. Read once, like the tracewake arm above.
bool OS_HLE_SeamFixArmed()
{
    static const bool armed = std::getenv("SCOOBY_SEAMFIX") != nullptr;
    return armed;
}

// Fix 2 (FIX2-DESIGN-STUDY Part B): arm flag for the present-time completion
// synthesis hook in hle/vi.cpp. A separate key from seamfix so the seamfix
// A/B (the CLIFF-STUDY decider boot) stays uncontaminated and the two
// mechanisms remain separately falsifiable. Read once, like the arms above.
bool OS_HLE_Fix2Armed()
{
    static const bool armed = std::getenv("SCOOBY_FIX2") != nullptr;
    return armed;
}

TraceWakeSync OS_HLE_TraceWakeReadSync()
{
    TraceWakeSync s;
    MemoryInline::TryReadGuestScalar(0x802716C0u, s.counterA);
    MemoryInline::TryReadGuestScalar(0x802716C4u, s.counterB);
    MemoryInline::TryReadGuestScalar(0x802716D8u, s.arm);
    MemoryInline::TryReadGuestScalar(0x802716DCu, s.latch);
    MemoryInline::TryReadGuestScalar(0x80271398u, s.guest);
    MemoryInline::TryReadGuestScalar(0x802716D4u, s.d4);
    MemoryInline::TryReadGuestScalar(0x802716B8u, s.b8);
    MemoryInline::TryReadGuestScalar(0x802716E0u, s.e0);
    MemoryInline::TryReadGuestScalar(0x802716D0u, s.flag);
    return s;
}


// parkwatch (park rescue, Nov 16, 2026; PARK-STUDY-2.md Q4(c)): one-shot wake
// request for the display-sync queue. The detector in hle/vi.cpp sets it when
// the park signature confirms; SelectThread's idle loop below consumes it and
// fires the wake there, on the scheduler thread - where every other wake in
// the system already happens - and never on the service thread. The wake is a
// bare queue drain (WakeupThreadQueue): it runs no guest code.
void OS_HLE_WakeupThreadNoReschedule(CpuContext* ctx, uint32_t waitQueue);
static std::atomic<bool> g_parkWakeRequest{false};
void OS_HLE_RequestParkWake()
{
    g_parkWakeRequest.store(true, std::memory_order_release);
}
extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx);
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx);

// Deterministic idle (det_clock.h): deliver everything due at the current virtual instant, in a
// fixed order, and if that made no thread runnable, wait for real time to reach the next event and
// move the timeline there. Every device therefore wakes its threads at the same points.
static void DeterministicIdleStep(CpuContext* cpu)
{
    ProcessSleepTimers(cpu);
    Audio_HLE_Poll(cpu);
    if (::Memory::Read32(kSchedulerPendingFlagAddr) != 0) {
        return;
    }
    VI_HLE_PollRetrace(cpu);
    ProcessAlarmQueue(cpu, 8);
    // seamfix: the held GX PE-interrupt class is part of this pump's delivery
    // set too, so deterministic mode can never starve it (the seam pass
    // itself stays DetClock-gated off; order stays fixed: general, then GX).
    if (OS_HLE_SeamFixArmed()) {
        DvdProcessPendingGxCallbacks(cpu);
    }
    if (::Memory::Read32(kSchedulerPendingFlagAddr) != 0) {
        return;
    }

    const uint64_t now = DetClock::Now();
    uint64_t next = std::min({VI_HLE_DetNextRetraceTicks(), Audio_HLE_DetNextEventTicks(), NextSleepTimerTicks(),
                              NextAlarmTicks()});
    // Before VIInit nothing may be scheduled yet: creep forward a millisecond at a time. An event
    // that is due but could not be delivered this pass still moves time on by a tick.
    constexpr uint64_t kMaxStep = TimeBaseContract::kTicksPerSecond / 1000;
    if (next == DetClock::kNever) {
        next = now + kMaxStep;
    } else if (next <= now) {
        next = now + 1;
    }
    DetClock::PaceTo(next);
    DetClock::AdvanceTo(next);
}

static void TryInvokeSwitchCallback(uint32_t oldCtx, uint32_t newCtx, CpuContext* cpu)
{
    try {
        if (kSwitchThreadCallbackPtrAddr == 0) return;
        const uint32_t callbackAddr = ::Memory::Read32(kSwitchThreadCallbackPtrAddr);
        if (callbackAddr == 0) {
            return;
        }

        if (TranslatedFunctionRegistry::FindByAddressPtr(callbackAddr)) {
            CpuContext* ctx = cpu ? cpu : &GetPersistentCpuContext();
            ctx->gpr[3] = oldCtx;
            ctx->gpr[4] = newCtx;
            InvokeIndirectCpu(callbackAddr, ctx);
            return;
        }

        RT_LOG(RT_TAG_OS) << "__OSThreadInit: switchThreadCallback 0x" << std::hex << callbackAddr
                  << std::dec << " not registered; skipping" << std::endl;
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "__OSThreadInit: failed to read callback pointer (0x" << std::hex
                  << e.address() << std::dec << ")" << std::endl;
    }
}

// Forward declare for SelectThread to use
extern "C" void OS__SetCurrentContext_801a1e70(uint32_t contextAddr);
extern "C" void func_8010533C(CpuContext* ctx);

// ============================================================================
// SelectThread HLE - Thread Scheduler (Fiber-Aware)
// Address: 0x801B4EA0. Picks the next runnable thread; switches via Windows
// Fibers instead of blocking InvokeIndirectJump.
// ============================================================================

extern "C" void SelectThread_801a9c08(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t forceSwitch = cpu->gpr[3];

    ProcessSleepTimers(cpu);

    // Read scheduler state
    const uint32_t idleFlag = ::Memory::Read32(kSchedulerIdleFlagAddr);
    if (idleFlag >= 1) {
        // Scheduler is idle/disabled, return 0
        cpu->gpr[3] = 0;
        return;
    }

    // Get current context
    const uint32_t currentContext = ::Memory::Read32(kOSCurrentContextAddr);
    const uint32_t runningContext = ::Memory::Read32(kOSRunningContextAddr);
    
    // Early exit if no thread system is initialized yet
        // (both current and running contexts are 0)
        if (currentContext == 0 && runningContext == 0) {
            cpu->gpr[3] = 0;
            return;
        }

    if (currentContext != runningContext) {
        // Not in the running thread's context, return 0
        cpu->gpr[3] = 0;
        return;
    }

    // Check if we have a running thread
    if (runningContext != 0) {
        // Read thread state at offset 0x2C8 (state field)
        const uint16_t threadState = ::Memory::Read16(runningContext + 0x2C8u);
        
        if (threadState == 2) { // RUNNING state
            // Check if we need to reschedule
            const uint32_t reschedPending = ::Memory::Read32(kSchedulerPendingFlagAddr);
            
            if (forceSwitch == 0) {
                // Get thread priority from offset 0x2D0
                const int32_t threadPriority = static_cast<int32_t>(::Memory::Read32(runningContext + 0x2D0u));
                
                // countLeadingZeros equivalent - find highest priority pending
                uint32_t pendingClz = 32;
                if (reschedPending != 0) {
                    pendingClz = PPC_Cntlzw(reschedPending);
                }
                
                if (threadPriority <= static_cast<int32_t>(pendingClz)) {
                    // Current thread has higher or equal priority, don't switch
                    cpu->gpr[3] = 0;
                    return;
                }
            }
            
            // Mark thread as READY (state = 1)
            ::Memory::Write16(runningContext + 0x2C8u, 1);
            
            // Add running thread back to its priority queue
            const int32_t prio = static_cast<int32_t>(::Memory::Read32(runningContext + 0x2D0u));
            LinkThreadOnRunQueue(runningContext, prio);
            MarkRunQueuePending(prio);
        }

        // If the context is not preemptible, save it and return when it resumes.
        // This matches OSSaveContext semantics used by the original scheduler.
        try {
            const uint16_t modeFlags = ::Memory::Read16(runningContext + 0x1A2u);
            if ((modeFlags & 0x0002u) == 0) {
                cpu->gpr[3] = runningContext;
                func_8010533C(cpu);
                if (cpu->gpr[3] != 0) {
                    cpu->gpr[3] = 0;
                    return;
                }
            }
        } catch (const ::Memory::AccessViolation& e) {
            RT_LOG(RT_TAG_OS) << "SelectThread: failed to read mode flags @0x"
                      << std::hex << e.address() << std::dec << std::endl;
        }
    }

    // Check if there are any runnable threads
    uint32_t reschedPending = ::Memory::Read32(kSchedulerPendingFlagAddr);
    if (reschedPending == 0) {
        // No threads to run - enter idle loop
        TryInvokeSwitchCallback(runningContext, 0, cpu);
        PhaseTimeCommit(0);
        ::Memory::Write32(kOSRunningContextAddr, 0);
        
        // Set current context to idle thread context
        OS__SetCurrentContext_801a1e70(kIdleThreadContextAddr);
        // Idle time (no guest thread runnable: VIWaitForRetrace sleepers, audio/alarm waits), so the
        // frame lines in hle/vi.cpp can split the guest's time into work and waiting.
        const auto idleStart = n8_ossched_now();  // N8 C7: IdleTimer start read
        struct IdleTimer {
            std::chrono::steady_clock::time_point start;
            ~IdleTimer() { const auto end = n8_ossched_now(); NoteGuestIdle(end - start); }  // N8 C7: IdleTimer end read
        } idleTimer{idleStart};

        while (true) {
            // Enable interrupts and idle until something becomes runnable.
            OS__EnableInterrupts_801a65c0();

            while (::Memory::Read32(kSchedulerPendingFlagAddr) == 0) {
                if (DetClock::Enabled()) {
                    DeterministicIdleStep(cpu);
                    continue;
                }
                ProcessSleepTimers(cpu);
                // Dolphin models DSP audio DMA as an independent 4 kHz timing
                // event.  Poll it from the guest scheduler instead of batching
                // completed 3 ms DMA blocks at VI retrace cadence.  A completed
                // block wakes SoundThread and sets the scheduler pending mask.
                Audio_HLE_Poll(cpu);
                if (::Memory::Read32(kSchedulerPendingFlagAddr) != 0) {
                    break;
                }
                // parkwatch actuation (PARK-STUDY-2.md Q4(c)): the detector
                // in hle/vi.cpp confirmed the display-sync park signature and
                // set the request flag; fire exactly one no-reschedule wake
                // on that queue, here on the scheduler thread.
                if (g_parkWakeRequest.exchange(false, std::memory_order_acq_rel)) {
                    static uint32_t parkFireCount = 0;
                    RT_LOGF(RT_TAG_OS, "[parkwatch] fire #%u (idle-loop actuation, queue 0x802716E4)\n",
                            ++parkFireCount);
                    OS_HLE_WakeupThreadNoReschedule(cpu, kTraceWakeQueueAddr);
                }
                VI_HLE_PollRetrace(cpu);
                if (Fiber::GuestFiberManager::IsInitialized()) {
                    Fiber::GuestFiberManager::ProcessTimerEvents(cpu);
                }
                ProcessAlarmQueue(cpu, 8);
                // seamfix: held GX PE-interrupt class, delivered at the
                // idle pump (an interrupt-shaped point) when armed.
                if (OS_HLE_SeamFixArmed()) {
                    DvdProcessPendingGxCallbacks(cpu);
                }
                if (::Memory::Read32(kSchedulerPendingFlagAddr) != 0) {
                    break;
                }
                VI_HLE_WaitForNextRetracePoll();
            }

            OS__DisableInterrupts_801a65ac();
            reschedPending = ::Memory::Read32(kSchedulerPendingFlagAddr);
            if (reschedPending != 0) {
                break;
            }
        }

        OS__ClearContext_801a2098(kIdleThreadContextAddr);
    }

    // Clear reschedule counter
    ::Memory::Write32(kSchedulerReschedCounterAddr, 0);

    // Note: When the current thread is already in WAITING state (not RUNNING),
    // we skip the preemptibility check entirely, matching the original OS behavior.
    // The preemptibility check only applies when the thread is RUNNING and we're
    // deciding whether to preempt it.

    // Find highest priority thread
    uint32_t pendingMask = ::Memory::Read32(kSchedulerPendingFlagAddr);
    
    if (pendingMask == 0) {
        // No runnable threads in the run queue at this point
        cpu->gpr[3] = 0;
        return;
    }
    
    uint32_t priorityLevel = 0;
    uint32_t queueEntry = 0;
    uint32_t nextThread = 0;
    while (true) {
        priorityLevel = PPC_Cntlzw(pendingMask);
        queueEntry = kThreadQueueArrayAddr + (priorityLevel * 8);
        nextThread = ::Memory::Read32(queueEntry);

        if (nextThread == 0) {
            RT_LOG(RT_TAG_OS) << "SelectThread: ERROR - pending bit set but no thread in queue!" << std::endl;
            const uint32_t mask = ~(1u << (31 - priorityLevel));
            ::Memory::Write32(kSchedulerPendingFlagAddr, pendingMask & mask);
            pendingMask = ::Memory::Read32(kSchedulerPendingFlagAddr);
            if (pendingMask == 0) {
                cpu->gpr[3] = 0;
                return;
            }
            continue;
        }

        const uint16_t state = ::Memory::Read16(nextThread + kThreadStateOffset);
        if (ThreadStateIsTerminated(state) || Fiber::GuestFiberManager::IsTerminated(nextThread)) {
            RemoveThreadFromQueue(nextThread);
            pendingMask = ::Memory::Read32(kSchedulerPendingFlagAddr);
            if (pendingMask == 0) {
                cpu->gpr[3] = 0;
                return;
            }
            continue;
        }

        break;
    }

    // Remove thread from queue head
    const uint32_t threadNext = PopThreadQueueHead(queueEntry, nextThread);

    // If queue is now empty, clear the pending bit
    if (threadNext == 0) {
        const uint32_t mask = ~(1u << (31 - priorityLevel));
        ::Memory::Write32(kSchedulerPendingFlagAddr, pendingMask & mask);
    }

    // Clear thread's queue pointer
    ::Memory::Write32(nextThread + 0x2DCu, 0);
    
    // Set thread to RUNNING state
    ::Memory::Write16(nextThread + 0x2C8u, 2);

    // Invoke switch callback
    TryInvokeSwitchCallback(runningContext, nextThread, cpu);

    // Update running context
    PhaseTimeCommit(nextThread);
    ::Memory::Write32(kOSRunningContextAddr, nextThread);
    
    // Set as current context
    OS__SetCurrentContext_801a1e70(nextThread);

    // Use fiber-based context switch if available
    if (Fiber::GuestFiberManager::IsInitialized()) {
        uint32_t currentGuestThread = Fiber::GuestFiberManager::GetCurrentGuestThread();
        
        // If we don't have a current guest fiber but we DO have a running context,
        // register the running context as the "main thread" fiber.
        // This allows the main thread to participate in fiber switching.
        if (currentGuestThread == 0 && runningContext != 0) {
            if (!Fiber::GuestFiberManager::HasFiber(runningContext)) {
                Fiber::GuestFiberManager::RegisterMainThreadAsFiber(runningContext, cpu);
            }
            // Whether we registered it or it already existed, use it as the current thread
            currentGuestThread = runningContext;
        }
        
        // Check if target thread has a fiber
        if (!Fiber::GuestFiberManager::HasFiber(nextThread)) {
            // No fiber for target thread - this can happen if the thread was created
            // by translated code that didn't go through our HLE. Just continue.
            cpu->gpr[3] = nextThread;
            OS__LoadContext_801a1f58(cpu);
            return;
        }
        
        // If we still don't have a current guest thread (no running context),
        // we can't do a proper fiber switch. Just return and let the caller handle it.
        if (currentGuestThread == 0) {
            Fiber::GuestFiberManager::SwitchToThread(nextThread, cpu);
            return;
        }
        
        // Perform the fiber switch!
        Fiber::GuestFiberManager::SwitchToThread(nextThread, cpu);
        // When we return here, we've been switched back
        return;
    }
    
    // Fallback to blocking InvokeIndirectJump (original behavior)
    cpu->gpr[3] = nextThread;
    OS__LoadContext_801a1f58(cpu);
}

PPC_NATIVE_OVERRIDE_VOID(80107EE8, SelectThread_801a9c08, (CpuContext* ctx), (ctx));

// ============================================================================
// OSWakeupThread HLE - drain a thread queue and mark threads runnable
// Address: 0x801B5D80
// ============================================================================
static void WakeupThreadQueue(CpuContext* ctx, bool allowImmediateReschedule)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t queueAddr = cpu->gpr[3];
    if (queueAddr == 0) {
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();
    bool resched = false;
    constexpr int kMaxWake = 256;
    int woke = 0;

    // tracewake: for the display-sync queue only, capture the head and the sync
    // snapshot at entry so the exit line below can flag a wake that found no
    // waiter. Reads only - the wake itself is unchanged.
    const bool traceThis = OS_HLE_TraceWakeArmed() && queueAddr == kTraceWakeQueueAddr;
    uint32_t headAtEntry = 0;
    TraceWakeSync snapAtEntry{};
    if (traceThis) {
        MemoryInline::TryReadGuestScalar(queueAddr, headAtEntry);
        snapAtEntry = OS_HLE_TraceWakeReadSync();
    }

    while (woke < kMaxWake) {
        const uint32_t thread = ::Memory::Read32(queueAddr);
        if (thread == 0) {
            break;
        }

        PopThreadQueueHead(queueAddr, thread);

        const uint16_t state = ::Memory::Read16(thread + kThreadStateOffset);
        if (ThreadStateIsTerminated(state) || Fiber::GuestFiberManager::IsTerminated(thread)) {
            ::Memory::Write32(thread + kThreadQueueOffset, 0);
            ::Memory::Write32(thread + kThreadNextOffset, 0);
            ::Memory::Write32(thread + kThreadPrevOffset, 0);
            woke++;
            continue;
        }

        // Mark READY
        ::Memory::Write16(thread + 0x2C8u, 1);

        const int32_t suspend = static_cast<int32_t>(::Memory::Read32(thread + 0x2CCu));
        if (suspend < 1) {
            int32_t prio = static_cast<int32_t>(::Memory::Read32(thread + 0x2D0u));
            if (prio < 0) prio = 0;
            if (prio > 31) prio = 31;
            LinkThreadOnRunQueue(thread, prio);

            if (Fiber::GuestFiberManager::IsInitialized()) {
                if (thread == kDefaultThreadContextAddr && !Fiber::GuestFiberManager::HasFiber(thread)) {
                    Fiber::GuestFiberManager::RegisterMainThreadAsFiber(thread, cpu);
                }
                Fiber::GuestFiberManager::ResumeGuestThread(thread);
            }

            MarkRunQueuePending(prio);
            resched = true;
        }

        woke++;
    }

    if (woke >= kMaxWake) {
        RT_LOG(RT_TAG_OS) << "OSWakeupThread: hit safety limit, possible queue cycle at 0x"
                  << std::hex << queueAddr << std::dec << std::endl;
    }

    // tracewake: one line per wake on the display-sync queue. head is the queue
    // head captured at entry; head == 0 means the wake found no waiter and was
    // discarded - the lost-wakeup shape - flagged literally as wake-to-empty.
    // inrestore marks a wake executed inside DeliverPendingInterruptsOnEnable
    // (the waiter's own OSRestoreInterrupts delivery), read through
    // OS_HLE_InRestoreDelivery(). The snapshot is the entry snapshot: counter A
    // already holds the value the advancer advanced it to before calling here.
    if (traceThis) {
        static int wakeN = 0;
        if (wakeN++ < 100000) {
            RT_LOGF(RT_TAG_OS,
                    "[wake] wakeup q=0x%08X head=0x%08X woke=%d result=%s lr=0x%08X inrestore=%d A=%u B=%u arm=%u latch=%u guest=%u d4=%u b8=%u e0=%u flag=%u\n",
                    queueAddr, headAtEntry, woke, headAtEntry == 0 ? "wake-to-empty" : "woke",
                    cpu->lr, OS_HLE_InRestoreDelivery() ? 1 : 0,
                    snapAtEntry.counterA, snapAtEntry.counterB, snapAtEntry.arm,
                    snapAtEntry.latch, snapAtEntry.guest,
                    snapAtEntry.d4, snapAtEntry.b8, snapAtEntry.e0, snapAtEntry.flag);
        }
    }

    if (allowImmediateReschedule && resched && !VI_HLE_IsAdvancingRetrace()) {
        cpu->gpr[3] = 0;
        SelectThread_801a9c08(cpu);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}

extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx)
{
    WakeupThreadQueue(ctx, true);
}

void OS_HLE_WakeupThreadNoReschedule(CpuContext* ctx, uint32_t waitQueue)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t savedR3 = cpu->gpr[3];
    cpu->gpr[3] = waitQueue;
    WakeupThreadQueue(cpu, false);
    cpu->gpr[3] = savedR3;
}

PPC_NATIVE_OVERRIDE_VOID(801087F4, OSWakeupThread_HLE_801aaaa4, (CpuContext* ctx), (ctx));

extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx);

extern "C" void OSLockMutex_HLE_801a7ee4(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t mutexPtr = cpu->gpr[3];
    if (mutexPtr == 0) {
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        const uint32_t currentThread = ::Memory::Read32(kOSRunningContextAddr);
        if (currentThread == 0) {
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        while (true) {
            const uint32_t ownerThread = ::Memory::Read32(mutexPtr + kMutexOwnerOffset);
            if (ownerThread == 0) {
                ::Memory::Write32(mutexPtr + kMutexOwnerOffset, currentThread);
                const uint32_t count = ::Memory::Read32(mutexPtr + kMutexCountOffset);
                ::Memory::Write32(mutexPtr + kMutexCountOffset, count + 1u);
                LinkMutexToThread(currentThread, mutexPtr);
                break;
            }

            if (ownerThread == currentThread) {
                const uint32_t count = ::Memory::Read32(mutexPtr + kMutexCountOffset);
                ::Memory::Write32(mutexPtr + kMutexCountOffset, count + 1u);
                break;
            }

            ::Memory::Write32(currentThread + kThreadMutexOffset, mutexPtr);
            const int32_t priority =
                static_cast<int32_t>(::Memory::Read32(currentThread + kThreadPriorityOffset));
            PromoteThreadPriority(ownerThread, priority);

            cpu->gpr[3] = mutexPtr;
            OSSleepThread_HLE_801aa9b8(cpu);
            ::Memory::Write32(currentThread + kThreadMutexOffset, 0);
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSLockMutex", e);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}

extern "C" void OSUnlockMutex_HLE_801a7fc0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t mutexPtr = cpu->gpr[3];
    if (mutexPtr == 0) {
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        const uint32_t currentThread = ::Memory::Read32(kOSRunningContextAddr);
        if (currentThread == 0) {
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        if (::Memory::Read32(mutexPtr + kMutexOwnerOffset) == currentThread) {
            const uint32_t count = ::Memory::Read32(mutexPtr + kMutexCountOffset) - 1u;
            ::Memory::Write32(mutexPtr + kMutexCountOffset, count);
            if (count == 0) {
                UnlinkMutexFromThread(currentThread, mutexPtr);
                ::Memory::Write32(mutexPtr + kMutexOwnerOffset, 0);

                const int32_t effectivePriority =
                    static_cast<int32_t>(::Memory::Read32(currentThread + kThreadPriorityOffset));
                const int32_t basePriority =
                    static_cast<int32_t>(::Memory::Read32(currentThread + kThreadBasePriorityOffset));
                if (effectivePriority < basePriority) {
                    const int32_t recomputedPriority = ComputeThreadEffectivePriority(currentThread);
                    ::Memory::Write32(currentThread + kThreadPriorityOffset,
                                      static_cast<uint32_t>(recomputedPriority));
                }

                cpu->gpr[3] = mutexPtr;
                OSWakeupThread_HLE_801aaaa4(cpu);
            }
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSUnlockMutex", e);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}

